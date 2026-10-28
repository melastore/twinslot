package com.melastore.twinslot;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Runs the bundled twinslot binary as root and reads its answers.
 *
 * Everything shown about the other Android (its name, version, which data it
 * mounts) is read by the binary from that slot's own partitions, so both
 * Androids show the same picture without storing anything.
 */
final class Twinslot {

    static final class Result {
        final int code;
        final String out;
        final String err;

        Result(int code, String out, String err) {
            this.code = code;
            this.out = out;
            this.err = err;
        }

        boolean ok() {
            return code == 0;
        }

        String message() {
            String s = err.trim().isEmpty() ? out.trim() : err.trim();
            return s.replace("twinslot: ", "").isEmpty() ? "exit " + code : s.replace("twinslot: ", "");
        }
    }

    static final class Slot {
        int priority;
        int tries;
        boolean successful;
        boolean bootable;
        /** "yes", "no" or "unknown" (super unreadable). */
        String installed = "unknown";
        /** Data the ROM mounts / must mount: "primary", "twin", "mixed", "none" or "unknown". */
        String data = "unknown";
        /** "primary", "twin", or "unknown" (the partition table could not be read). */
        String expected = "primary";
        String rom = "";
        /** The user's own name for it, "" for the ROM's: see {@link Prefs#romName}. */
        String customName = "";
        String android = "";
        long buildUtc;
        /** Size of its system partition: Settings adds it to /data for the storage total. */
        long systemMib;

        boolean isInstalled() {
            return "yes".equals(installed);
        }

        /**
         * May be switched to, as the engine sees it: bootable, or parked (switched off with its tries left,
         * images whole). Half written (no priority, no tries) is not: an install did not finish there.
         */
        boolean usable() {
            return bootable || (priority == 0 && tries > 0);
        }

        /** The ROM mounts the data that belongs to it. */
        boolean dataOk() {
            return dataKnown() && data.equals(expected);
        }

        /** Both sides of {@link #dataOk()} could be read: only then is a mismatch worth showing. */
        boolean dataKnown() {
            return !"unknown".equals(data) && !"unknown".equals(expected);
        }

        String title(int index) {
            if (isInstalled() && !customName.isEmpty()) {
                return customName;
            }
            if (!rom.isEmpty()) {
                return rom;
            }
            return isInstalled() ? "Android in slot " + Character.toUpperCase(letter(index)) : "Empty";
        }
    }

    static final class Status {
        /** 0 = a, 1 = b, -1 = unknown. */
        int current = -1;
        int next = -1;
        String merge = "unknown";
        /** "ready", "off" or "unknown". */
        String dualData = "unknown";
        int owner = -1;
        long userdataMib;
        long twinDataMib;
        String version = "";
        final Slot[] slots = {new Slot(), new Slot()};

        int other() {
            return current < 0 ? -1 : 1 - current;
        }

        /** Both data partitions together: what a split divides. */
        long poolMib() {
            return userdataMib + (dualReady() ? twinDataMib : 0);
        }

        /** Size of slot i's /data partition, 0 if unknown. */
        long dataMib(int i) {
            if (!dualReady()) {
                return userdataMib;
            }
            return i == owner ? twinDataMib : userdataMib;
        }

        boolean dualReady() {
            return "ready".equals(dualData);
        }

        boolean mergePending() {
            return "snapshotted".equals(merge) || "merging".equals(merge);
        }

        /**
         * The other slot may be booted. Without the split both slots mount userdata, so the other ROM would
         * start on this one's data. Not bootable means an install did not finish (or is still writing), a
         * remove switched it off, or it failed to start too often: switching would undo that.
         */
        boolean canSwitch() {
            int o = other();
            return o >= 0 && dualReady() && !mergePending() && slots[o].isInstalled() && slots[o].usable()
                    && slots[o].dataOk();
        }
    }

    /** One long command (install, verify) running as root in the background. */
    static final class Job {
        /** The job has written its log or pid: until then it is still starting. */
        boolean started;
        boolean running;
        boolean finished;
        int code = -1;
        int permille;
        String step = "";
        final List<String> lines = new ArrayList<>();

        /** The zip check ran or runs first (install then is step 2 of 2). */
        boolean checks() {
            if (step.startsWith("checking")) {
                return true;
            }
            for (String l : lines) {
                if (l.startsWith("verified:") || l.startsWith("ok ")) {
                    return true;
                }
            }
            return false;
        }

        boolean installed() {
            for (String l : lines) {
                if (l.startsWith("installed:")) {
                    return true;
                }
            }
            return false;
        }
    }

    /**
     * Root's own directory (Magisk, KernelSU and APatch all keep /data/adb at 0700 root). Not /data/local/tmp:
     * the shell user owns that, and could swap the job script between the app writing it and root running it.
     */
    static final String DIR = "/data/adb/twinslot";
    private static final String LOG = DIR + "/job.log";
    private static final String PID = DIR + "/job.pid";
    private static final String JOB = DIR + "/job.sh";
    /** The log of the job before, kept when a finished job is cleared, for the diagnostic log. */
    private static final String LAST_LOG = DIR + "/last-job.log";
    private static final String WAKE_LOCK = "/sys/power/wake_lock";
    private static final String WAKE_UNLOCK = "/sys/power/wake_unlock";
    /** Recovery zips made by the app go here, where any recovery's file browser finds them. */
    static final String ZIP_DIR = "/sdcard/TwinSlot";

    private final Context context;
    private final String binary;

    Twinslot(Context context) {
        this.context = context.getApplicationContext();
        binary = context.getApplicationInfo().nativeLibraryDir + "/libtwinslot.so";
    }

    static char letter(int slot) {
        return (char) ('a' + slot);
    }

    /** Whether root is there for this app: granted, refused, or no su at all. */
    enum Root { GRANTED, DENIED, MISSING }

    /**
     * Asks su for uid 0. Blocks while a root manager shows its prompt. With
     * KernelSU-style root, su is not even visible to an app that was not
     * allowed, so MISSING can also mean "not allowed yet".
     */
    static Root checkRoot() {
        try {
            Result r = run(new ProcessBuilder("su", "-c", "id -u"));
            return r.ok() && r.out.trim().equals("0") ? Root.GRANTED : Root.DENIED;
        } catch (IOException e) {
            return Root.MISSING;
        }
    }

    Status status() throws IOException {
        Result r = su(quote(binary) + " status");
        if (!r.ok()) {
            throw new IOException(r.message());
        }
        Status st = new Status();
        for (String line : r.out.split("\n")) {
            int eq = line.indexOf('=');
            if (eq < 0) {
                continue;
            }
            String key = line.substring(0, eq);
            String value = line.substring(eq + 1).trim();
            switch (key) {
                case "version": st.version = value; break;
                case "current": st.current = slotIndex(value); break;
                case "next": st.next = slotIndex(value); break;
                case "merge_status": st.merge = value; break;
                case "dual_data": st.dualData = value; break;
                case "dual_owner": st.owner = slotIndex(value); break;
                case "userdata_mib": st.userdataMib = parseLong(value); break;
                case "twindata_mib": st.twinDataMib = parseLong(value); break;
                case "slot_a": parseSlot(st.slots[0], value); break;
                case "slot_b": parseSlot(st.slots[1], value); break;
                case "rom_a": parseRom(st.slots[0], value); break;
                case "rom_b": parseRom(st.slots[1], value); break;
                default: break;
            }
        }
        Prefs prefs = new Prefs(context);
        for (int i = 0; i < 2; i++) {
            st.slots[i].customName = prefs.romName(i, st.slots[i].rom);
        }
        return st;
    }

    Result switchTo(int slot) throws IOException {
        return su(quote(binary) + " switch " + letter(slot));
    }

    Result rebind(int slot) throws IOException {
        return su(quote(binary) + " rebind " + letter(slot));
    }

    /** Erases the data of a slot that is not running (dual data only: the engine refuses shared data). */
    Result wipe(int slot) throws IOException {
        return su(quote(binary) + " wipe " + letter(slot));
    }

    /** Throws when the reboot was refused; success never returns for long, the phone goes down. */
    void reboot() throws IOException {
        check(su("svc power reboot || reboot"));
    }

    void rebootRecovery() throws IOException {
        check(su("svc power reboot recovery || reboot recovery"));
    }

    private static void check(Result r) throws IOException {
        if (!r.ok()) {
            throw new IOException(r.message());
        }
    }

    /** The size of a file as root sees it; 0 if unknown. */
    long size(String path) throws IOException {
        return parseLong(su("stat -c %s " + quote(path)).out.trim());
    }

    /** Whether root sees a file at path. */
    boolean exists(String path) throws IOException {
        return su("test -f " + quote(path)).ok();
    }

    // ---- background jobs ----

    /**
     * Starts twinslot commands (each an argument list) detached from the app,
     * so they keep going if the app is closed. Each runs only if the one before
     * succeeded. The output goes to a log that {@link #poll()} reads.
     *
     * The commands go into a script that one exec'd command starts in its own
     * session with every descriptor on /dev/null. Backgrounding a list instead
     * (`a && b &`) leaves a subshell holding su's stdout until the job ends, and
     * this call, which reads that stdout to its end, would return only then.
     */
    void start(List<String[]> commands, boolean wakeLock) throws IOException {
        StringBuilder cmd = new StringBuilder();
        for (String[] args : commands) {
            cmd.append(cmd.length() == 0 ? "" : " && ").append(quote(binary));
            for (String a : args) {
                cmd.append(' ').append(quote(a));
            }
        }
        // The kernel wake lock keeps the phone from suspending (and pausing the job) when the screen goes off.
        // The trap releases it however the job ends.
        // First it leaves the app's cgroup. With root that runs inside the app (KernelSU, APatch), the job
        // starts there, and Android freezes that cgroup while the app is in the background and kills all of
        // it with the app: the install would stall, then die half-written. The root cgroup is neither.
        // /acct is where Android 11 and older kept the per-app groups.
        String script = "for g in /sys/fs/cgroup /acct; do [ -f $g/cgroup.procs ] && echo $$ > $g/cgroup.procs; "
                + "done 2>/dev/null\n"
                + "echo $$ > " + PID + "\n"
                + ": > " + LOG + "\n"
                + (wakeLock ? "echo twinslot > " + WAKE_LOCK + " 2>/dev/null\n"
                        + "trap 'echo twinslot > " + WAKE_UNLOCK + " 2>/dev/null' EXIT\n" : "")
                + "{ " + cmd + "; } >> " + LOG + " 2>&1\n"
                + "echo \"exit $?\" >> " + LOG + "\n";
        // The directory is made root's alone before anything is written into it, whoever made it before.
        String launch = "rm -f " + LOG + " " + PID + " && mkdir -p " + DIR + " && chown 0:0 "
                + DIR + " && chmod 0700 " + DIR + " && printf '%s' " + quote(script) + " > " + JOB + " && cd / "
                + "|| exit 1\n"
                + "( if command -v setsid > /dev/null; then exec setsid sh " + JOB + "; else exec nohup sh " + JOB
                + "; fi ) < /dev/null > /dev/null 2>&1 &\n";
        Result r = su(launch);
        if (!r.ok()) {
            throw new IOException(r.message());
        }
    }

    Job poll() throws IOException {
        // Only the last progress line matters, and an install writes thousands.
        // The pid file outlives a crash or power loss: after the reboot another process may have that pid, so
        // it counts only while it is still our job script.
        Result r = su("if [ -f " + LOG + " ]; then grep -v '^progress ' " + LOG + " | tail -n 300; grep '^progress ' "
                + LOG + " | tail -n 1; echo @log; fi; "
                + "p=$(cat " + PID + " 2>/dev/null); if [ -n \"$p\" ] && tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null "
                + "| grep -q " + quote(JOB) + "; then echo @running; fi; true");
        if (!r.ok()) {
            // su itself failed: say so, rather than report a job that is still writing as interrupted.
            throw new IOException(r.message());
        }
        Job job = new Job();
        for (String line : r.out.split("\n")) {
            if (line.equals("@running")) {
                job.running = true;
                job.started = true;
            } else if (line.equals("@log")) {
                job.started = true;
            } else if (line.startsWith("exit ")) {
                job.finished = true;
                job.code = (int) parseLong(line.substring(5).trim());
            } else if (line.startsWith("progress ")) {
                String[] parts = line.split(" ", 3);
                job.permille = (int) parseLong(parts[1]);
                job.step = parts.length > 2 ? parts[2] : "";
            } else if (!line.isEmpty()) {
                job.lines.add(line.replace("twinslot: ", ""));
            }
        }
        job.running &= !job.finished;
        return job;
    }

    void clearJob() throws IOException {
        su("if [ -f " + LOG + " ]; then mv -f " + LOG + " " + LAST_LOG + "; fi; rm -f " + PID + " " + JOB);
    }

    /** The current (or the previous) job's log for the diagnostic log: its end, and its last progress line. */
    String jobLog(boolean previous, int lines) {
        String log = previous ? LAST_LOG : LOG;
        try {
            Result r = su("if [ -f " + log + " ]; then grep -v '^progress ' " + log + " | tail -n " + lines
                    + "; grep '^progress ' " + log + " | tail -n 1; else echo '(none)'; fi");
            return r.out.trim() + "\n";
        } catch (IOException e) {
            return "(could not read: " + e.getMessage() + ")\n";
        }
    }

    /** One read-only engine command's whole output, errors included, for the diagnostic log. */
    String diagnose(String command) {
        try {
            Result r = su(quote(binary) + " " + command + " 2>&1");
            return r.out.trim() + (r.ok() ? "" : "\n(exit " + r.code + ")") + "\n";
        } catch (IOException e) {
            return "(could not run: " + e.getMessage() + ")\n";
        }
    }

    // ---- recovery zips ----

    /**
     * Writes a recovery zip that runs `twinslot args...` into {@link #ZIP_DIR}
     * and returns its path there.
     */
    String makeRecoveryZip(String name, String... args) throws IOException {
        return makeRecoveryZip(name, Collections.singletonList(args));
    }

    /**
     * A copy of the partition table as it is now, into {@link #ZIP_DIR}, for a zip that rewrites the table.
     * Returns the copy's file name.
     */
    String saveTable() throws IOException {
        Result r = su("mkdir -p " + ZIP_DIR + " && " + quote(binary) + " table-save " + quote(ZIP_DIR) + " 2>&1");
        check(r);
        for (String line : r.out.split("\n")) {
            if (line.startsWith("table_copy=")) {
                String path = line.substring("table_copy=".length()).trim();
                check(su("chmod 0644 " + quote(path)));
                return path.substring(path.lastIndexOf('/') + 1);
            }
        }
        throw new IOException("TwinSlot made no copy of the partition table");
    }

    /** Copies an installed app's APK into {@link #ZIP_DIR} as fileName and returns its path there. */
    String saveApk(String apk, String fileName) throws IOException {
        String dest = ZIP_DIR + "/" + fileName;
        check(su("mkdir -p " + ZIP_DIR + " && cp " + quote(apk) + " " + quote(dest) + " && chmod 0644 "
                + quote(dest)));
        return dest;
    }

    /** The same with several commands, one per line; recovery stops at the first that fails. */
    String makeRecoveryZip(String name, List<String[]> commands) throws IOException {
        return makeRecoveryZip(name, commands, null);
    }

    /**
     * With tableCopy (a {@link #saveTable()} name), the zip also carries that copy as table.bin. Sideloaded, the
     * zip stays on the computer, so the copy survives a power cut that loses the table; setup and remove in
     * recovery count it only while it still matches the table byte for byte.
     */
    String makeRecoveryZip(String name, List<String[]> commands, String tableCopy) throws IOException {
        File tmp = new File(context.getCacheDir(), name);
        File table = tableCopy == null ? null : new File(context.getCacheDir(), "table.bin");
        if (table != null) {
            check(su("cp " + quote(ZIP_DIR + "/" + tableCopy) + " " + quote(table.getPath()) + " && chmod 0644 "
                    + quote(table.getPath())));
        }
        StringBuilder command = new StringBuilder();
        for (String[] args : commands) {
            StringBuilder line = new StringBuilder();
            for (String a : args) {
                line.append(line.length() == 0 ? "" : " ").append(quote(a));
            }
            command.append(command.length() == 0 ? "" : "\n").append(line);
        }
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(tmp))) {
            zip.putNextEntry(new ZipEntry("META-INF/com/google/android/update-binary"));
            try (InputStream in = context.getAssets().open("update-binary")) {
                copy(in, zip);
            }
            zip.putNextEntry(new ZipEntry("META-INF/com/google/android/updater-script"));
            zip.write("#twinslot\n".getBytes(StandardCharsets.UTF_8));
            zip.putNextEntry(new ZipEntry("twinslot"));
            try (InputStream in = new java.io.FileInputStream(binary)) {
                copy(in, zip);
            }
            zip.putNextEntry(new ZipEntry("command"));
            zip.write((command + "\n").getBytes(StandardCharsets.UTF_8));
            if (table != null) {
                zip.putNextEntry(new ZipEntry("table.bin"));
                try (InputStream in = new java.io.FileInputStream(table)) {
                    copy(in, zip);
                }
            }
        } finally {
            if (table != null) {
                //noinspection ResultOfMethodCallIgnored
                table.delete();
            }
        }
        String dest = ZIP_DIR + "/" + name;
        Result r = su("mkdir -p " + ZIP_DIR + " && cp " + quote(tmp.getPath()) + " " + quote(dest)
                + " && chmod 0644 " + quote(dest));
        //noinspection ResultOfMethodCallIgnored
        tmp.delete();
        if (!r.ok()) {
            throw new IOException(r.message());
        }
        return dest;
    }

    // ---- plumbing ----

    /** Single-quotes for sh. */
    static String quote(String s) {
        return "'" + s.replace("'", "'\\''") + "'";
    }

    private static int slotIndex(String v) {
        return v.equals("a") ? 0 : v.equals("b") ? 1 : -1;
    }

    private static long parseLong(String v) {
        try {
            return Long.parseLong(v);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static void parseSlot(Slot s, String value) {
        for (String field : value.split(" ")) {
            int colon = field.indexOf(':');
            if (colon < 0) {
                continue;
            }
            String k = field.substring(0, colon);
            String v = field.substring(colon + 1);
            switch (k) {
                case "priority": s.priority = (int) parseLong(v); break;
                case "tries": s.tries = (int) parseLong(v); break;
                case "successful": s.successful = v.equals("1"); break;
                case "bootable": s.bootable = v.equals("yes"); break;
                case "installed": s.installed = v; break;
                case "data": s.data = v; break;
                case "expected": s.expected = v; break;
                case "system_mib": s.systemMib = parseLong(v); break;
                default: break;
            }
        }
    }

    /** "android|build utc|name" (the name may contain anything but newlines). */
    private static void parseRom(Slot s, String value) {
        String[] parts = value.split("\\|", 3);
        s.android = parts[0];
        s.buildUtc = parts.length > 1 ? parseLong(parts[1]) : 0;
        s.rom = parts.length > 2 ? parts[2].trim() : "";
    }

    private static Result su(String command) throws IOException {
        return run(new ProcessBuilder("su", "-c", command));
    }

    static Result run(ProcessBuilder pb) throws IOException {
        Process p = pb.start();
        p.getOutputStream().close();
        ByteArrayOutputStream err = new ByteArrayOutputStream();
        Thread errReader = new Thread(() -> drain(p.getErrorStream(), err));
        errReader.start();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        drain(p.getInputStream(), out);
        try {
            errReader.join();
            int code = p.waitFor();
            return new Result(code, out.toString(StandardCharsets.UTF_8.name()),
                    err.toString(StandardCharsets.UTF_8.name()));
        } catch (InterruptedException e) {
            p.destroy();
            Thread.currentThread().interrupt();
            throw new IOException("interrupted", e);
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
    }

    private static void drain(InputStream in, ByteArrayOutputStream out) {
        try (InputStream is = in) {
            copy(is, out);
        } catch (IOException ignored) {
            // The process ended; keep what was read.
        }
    }
}
