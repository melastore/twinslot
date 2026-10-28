package com.melastore.twinslot;

import android.app.NotificationManager;
import android.app.usage.UsageStatsManager;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.Process;
import android.provider.Settings;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The diagnostic log: what someone needs to fix TwinSlot on a phone they do
 * not have, with nothing in it that says whose phone it is.
 *
 * The app notes its own events (root answers, jobs, errors, crashes) in a
 * small file of its own as it goes. When asked, that is put together with the
 * phone's layout, the root setup, the engine's status and the last jobs'
 * logs, and every serial number, address and name is taken out of the text.
 * Nothing leaves the phone unless the user shares the file.
 */
final class Diagnostics {

    /** Events are kept to about this much; the older half rolls into one previous file. */
    private static final long EVENTS_MAX = 96 * 1024;
    private static final int JOB_LINES = 400;
    private static final int LIST_LINES = 300;
    private static final Object LOCK = new Object();

    private Diagnostics() {
    }

    // ---- the event log ----

    /** Notes one event. Never throws: a log that cannot be written is not worth failing for. */
    static void note(Context c, String event) {
        String line = stamp(new Date()) + "  " + event.replace('\n', ' ') + "\n";
        synchronized (LOCK) {
            File dir = dir(c);
            File now = new File(dir, "events.log");
            try {
                if (now.length() > EVENTS_MAX) {
                    File old = new File(dir, "events.1.log");
                    //noinspection ResultOfMethodCallIgnored
                    old.delete();
                    //noinspection ResultOfMethodCallIgnored
                    now.renameTo(old);
                }
                try (Writer w = new OutputStreamWriter(new FileOutputStream(now, true), StandardCharsets.UTF_8)) {
                    w.write(line);
                }
            } catch (IOException ignored) {
                // Full or unwritable storage: the event is lost, the app goes on.
            }
        }
    }

    /** Notes a failure with where it came from: the top of its stack and of each cause. */
    static void error(Context c, String where, Throwable t) {
        StringBuilder s = new StringBuilder(where).append(": ").append(t);
        for (Throwable x = t; x != null; x = x.getCause()) {
            if (x != t) {
                s.append(" | caused by ").append(x);
            }
            StackTraceElement[] st = x.getStackTrace();
            for (int i = 0; i < Math.min(6, st.length); i++) {
                s.append(" | at ").append(st[i]);
            }
        }
        note(c, s.toString());
    }

    /** Crashes go into the log before the app dies, so the next report still has them. */
    static void catchCrashes(Context c) {
        Context app = c.getApplicationContext();
        Thread.UncaughtExceptionHandler next = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, e) -> {
            StringWriter trace = new StringWriter();
            e.printStackTrace(new PrintWriter(trace));
            note(app, "CRASH in thread " + thread.getName() + ": " + trace.toString().replace("\n", " | "));
            if (next != null) {
                next.uncaughtException(thread, e);
            }
        });
    }

    static void clearEvents(Context c) {
        synchronized (LOCK) {
            //noinspection ResultOfMethodCallIgnored
            new File(dir(c), "events.log").delete();
            //noinspection ResultOfMethodCallIgnored
            new File(dir(c), "events.1.log").delete();
        }
    }

    private static File dir(Context c) {
        File d = new File(c.getFilesDir(), "diagnostics");
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        return d;
    }

    // ---- the report ----

    /** A finished report: the text to share, with the private parts taken out. */
    static final class Report {
        final String text;

        Report(String text) {
            this.text = text;
        }
    }

    /** Puts the report together. Slow (getprop, root, logcat): call it off the main thread. */
    static Report build(Context c) {
        DeviceCheck check = DeviceCheck.run();
        StringBuilder r = new StringBuilder();
        r.append("TwinSlot diagnostic log\n");
        r.append("Made ").append(stamp(new Date())).append(" (UTC)\n");

        section(r, "App");
        r.append("TwinSlot ").append(MainActivity.appVersion(c)).append('\n');
        appState(c, r);

        section(r, "Phone");
        r.append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE)
                .append(", ").append(Build.PRODUCT).append(")\n");
        r.append("Build: ").append(Build.DISPLAY).append('\n');
        r.append("Fingerprint: ").append(Build.FINGERPRINT).append('\n');
        r.append("Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT)
                .append("), security patch ").append(Build.VERSION.SECURITY_PATCH).append('\n');
        r.append("Kernel: ").append(System.getProperty("os.version")).append('\n');
        r.append("ABIs: ").append(String.join(", ", Build.SUPPORTED_ABIS)).append('\n');
        r.append('\n');
        for (DeviceCheck.Item i : check.items) {
            r.append(i.state == DeviceCheck.State.OK ? "[ok]   " : i.state == DeviceCheck.State.NOTE ? "[note] " : "[no]   ")
                    .append(i.label).append(": ").append(i.value).append('\n');
        }
        r.append('\n');
        for (Map.Entry<String, String> e : new TreeMap<>(check.props).entrySet()) {
            if (wanted(e.getKey())) {
                r.append(e.getKey()).append('=').append(e.getValue()).append('\n');
            }
        }

        section(r, "Root");
        DeviceCheck.RootManager m = DeviceCheck.installedManager(c);
        r.append("Manager: ").append(m == null ? "none of the known ones" : m.name + " " + versionOf(c, m.pkg))
                .append('\n');
        Twinslot.Root root = Twinslot.checkRoot();
        r.append("su answers: ").append(root).append('\n');
        List<String> serials = new ArrayList<>();
        if (root == Twinslot.Root.GRANTED) {
            // Apps cannot read the serial number, but root tools may print it: it is fetched only to be taken out.
            for (String line : su("for k in ro.serialno ro.boot.serialno ro.boot.ap_serialno; do getprop $k; done; "
                    + "cat /sys/devices/soc0/serial_number 2>/dev/null; true").split("\n")) {
                serials.add(line.trim());
            }
            // Where su runs decides whether a job outlives the app (see Twinslot.start), so it is shown.
            r.append(su("echo \"su version: $(su -v 2>&1) / $(su -V 2>&1)\"\n"
                    + "echo \"context: $(cat /proc/self/attr/current 2>/dev/null | tr -d '\\0')\"\n"
                    + "echo \"cgroup: $(grep -m1 '^0::' /proc/self/cgroup)\"\n"
                    + "echo \"SELinux: $(getenforce 2>&1)\"\n"
                    + "for d in /data/adb/magisk /data/adb/ksu /data/adb/ap; do [ -e $d ] && echo \"found $d\"; done\n"
                    + "true"));

            Twinslot ts = new Twinslot(c);
            section(r, "Engine");
            r.append(ts.diagnose("version"));
            r.append(ts.diagnose("status"));

            section(r, "Super partition (lpdump)");
            r.append(su("lpdump 2>&1 | head -n " + LIST_LINES));

            section(r, "Partitions by name");
            r.append(su("cd /dev/block/by-name 2>/dev/null && for p in *; do echo \"$p -> $(readlink $p)\"; done "
                    + "| head -n " + LIST_LINES));

            section(r, "Mounts");
            r.append(su("grep -E ' /(data|metadata|system|vendor|product|system_ext) ' /proc/mounts"));

            Prefs prefs = new Prefs(c);
            section(r, "Current job");
            r.append("Title: ").append(prefs.jobTitle()).append('\n');
            r.append(ts.jobLog(false, JOB_LINES));
            section(r, "Previous job");
            r.append(ts.jobLog(true, JOB_LINES));

            section(r, "Recent crashes (crash buffer)");
            r.append(su("logcat -d -b crash -t 400 2>/dev/null | grep -i -A 25 twinslot | tail -n 150"));
        }

        section(r, "App events");
        r.append(read(new File(dir(c), "events.1.log"))).append(read(new File(dir(c), "events.log")));

        section(r, "App messages (logcat)");
        r.append(run(new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "300",
                "--pid=" + Process.myPid())));

        Privacy privacy = new Privacy(c, check.props, serials);
        String text = privacy.clean(r.toString());
        return new Report(text);
    }

    /** How the app is set up, and what Android lets it do in the background. */
    private static void appState(Context c, StringBuilder r) {
        Prefs prefs = new Prefs(c);
        AppLock lock = new AppLock(c);
        r.append("Theme ").append(prefs.theme()).append(", colour ").append(prefs.accent())
                .append(", screen on ").append(prefs.screenOn()).append(", stay awake ").append(prefs.stayAwake())
                .append('\n');
        r.append("Install defaults: check ").append(prefs.verifyByDefault()).append(", wipe ")
                .append(prefs.wipeByDefault()).append(", switch ").append(prefs.switchByDefault()).append('\n');
        r.append("App lock ").append(lock.type()).append(lock.fingerprint() ? " + fingerprint" : "").append('\n');
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        PowerManager pm = c.getSystemService(PowerManager.class);
        r.append("Notifications ").append(nm.areNotificationsEnabled() ? "on" : "OFF");
        r.append(", battery optimisation ").append(pm.isIgnoringBatteryOptimizations(c.getPackageName())
                ? "off (unrestricted)" : "on");
        UsageStatsManager us = c.getSystemService(UsageStatsManager.class);
        if (us != null) {
            r.append(", standby bucket ").append(us.getAppStandbyBucket());
        }
        r.append('\n');
    }

    /** Properties that tell how the phone boots and is partitioned; never the ones that identify it. */
    private static boolean wanted(String key) {
        if (key.contains("serial") || key.contains("imei") || key.contains("iccid") || key.contains("bt.name")
                || key.contains("device_name") || key.contains("wifi") || key.contains("mac")) {
            return false;
        }
        return key.startsWith("ro.boot.slot") || key.startsWith("ro.boot.dynamic") || key.startsWith("ro.virtual_ab")
                || key.startsWith("ro.boot.verifiedbootstate") || key.startsWith("ro.boot.flash")
                || key.startsWith("ro.boot.vbmeta.") || key.startsWith("ro.build.ab_update")
                || key.startsWith("ro.product.first_api_level") || key.startsWith("ro.board.")
                || key.startsWith("ro.soc.") || key.equals("ro.hardware") || key.startsWith("ro.boot.hardware")
                || key.endsWith(".build.version.release") || key.endsWith(".build.fingerprint")
                || key.startsWith("ro.crypto.") || key.startsWith("ro.treble.") || key.startsWith("ro.vndk.")
                || key.startsWith("ro.apex.updatable") || key.startsWith("ro.boot.bootdevice")
                || key.startsWith("ro.boot.mode") || key.startsWith("sys.boot.reason")
                || key.startsWith("ro.boot.bootreason") || key.startsWith("persist.sys.usb.config")
                || key.startsWith("ro.build.type") || key.startsWith("ro.build.tags");
    }

    private static String versionOf(Context c, String pkg) {
        try {
            PackageInfo i = c.getPackageManager().getPackageInfo(pkg, 0);
            return i.versionName + " (" + i.getLongVersionCode() + ")";
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    private static void section(StringBuilder r, String title) {
        r.append("\n== ").append(title).append(" ==\n");
    }

    private static String su(String script) {
        return run(new ProcessBuilder("su", "-c", script));
    }

    /** A command's output, or why there is none: the report is made of whatever could be read. */
    private static String run(ProcessBuilder pb) {
        try {
            Twinslot.Result res = Twinslot.run(pb.redirectErrorStream(true));
            String out = res.out.trim();
            return (out.isEmpty() ? "(nothing)" : out) + (res.ok() ? "" : "\n(exit " + res.code + ")") + "\n";
        } catch (IOException e) {
            return "(could not run: " + e.getMessage() + ")\n";
        }
    }

    private static String read(File f) {
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    private static String stamp(Date d) {
        SimpleDateFormat f = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return f.format(d);
    }

    /** Writes a report where {@link LogProvider} serves it from, and returns the file. */
    static File save(Context c, Report report) throws IOException {
        File dir = new File(c.getCacheDir(), LogProvider.DIR);
        File[] old = dir.listFiles();
        if (old != null) {
            for (File f : old) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("cannot create " + dir);
        }
        SimpleDateFormat f = new SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        File out = new File(dir, "twinslot-log-" + Build.DEVICE.replaceAll("[^A-Za-z0-9_-]", "") + "-"
                + f.format(new Date()) + ".txt");
        Files.write(out.toPath(), report.text.getBytes(StandardCharsets.UTF_8));
        return out;
    }

    /**
     * Takes out of a text what could point to a person or a single phone. The
     * phone's own serials and names are replaced wherever they appear; patterns
     * catch the rest (numbers as long as an IMEI, addresses, e-mail).
     */
    static final class Privacy {

        private static final Pattern EMAIL = Pattern.compile("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}");
        private static final Pattern MAC = Pattern.compile("\\b[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}\\b");
        private static final Pattern IPV4 = Pattern.compile("\\b(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)"
                + "(?:\\.(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)){3}\\b");
        // Three colons at least, so clock times (12:34:56) are not taken for addresses.
        private static final Pattern IPV6 = Pattern.compile("\\b(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}\\b"
                + "|\\b(?:[0-9A-Fa-f]{1,4}:){1,6}:[0-9A-Fa-f]{1,4}\\b");
        private static final Pattern PHONE = Pattern.compile("(?<![\\w+])\\+\\d[\\d ()-]{7,}\\d");
        // IMEI, IMSI and SIM numbers; sizes in bytes stay shorter than this.
        private static final Pattern LONG_NUMBER = Pattern.compile("(?<![\\w.])\\d{14,20}(?![\\w.])");

        private final List<String[]> known = new ArrayList<>();

        Privacy(Context c, Map<String, String> props, List<String> serials) {
            for (Map.Entry<String, String> e : props.entrySet()) {
                if (e.getKey().contains("serial")) {
                    know(e.getValue(), "serial number");
                }
            }
            for (String s : serials) {
                know(s, "serial number");
            }
            // The phone's name says whose it is once someone renames it ("Anna's phone"). Left as it came, it is
            // the model's name, which stays.
            List<String> models = new ArrayList<>();
            models.add(Build.MODEL);
            for (Map.Entry<String, String> e : props.entrySet()) {
                if (e.getKey().contains("model") || e.getKey().contains("marketname")) {
                    models.add(e.getValue());
                }
            }
            String[] names = {
                setting(() -> Settings.Global.getString(c.getContentResolver(), Settings.Global.DEVICE_NAME)),
                // Usually the same; newer Androids keep it from apps, then it is not read.
                setting(() -> Settings.Secure.getString(c.getContentResolver(), "bluetooth_name")),
            };
            for (String name : names) {
                if (name != null && !containsIgnoreCase(models, name.trim())) {
                    know(name, "device name");
                }
            }
        }

        private static boolean containsIgnoreCase(List<String> list, String s) {
            for (String x : list) {
                if (x != null && x.trim().equalsIgnoreCase(s)) {
                    return true;
                }
            }
            return false;
        }

        private interface Read {
            String read();
        }

        private static String setting(Read r) {
            try {
                return r.read();
            } catch (SecurityException | IllegalArgumentException e) {
                return null;
            }
        }

        private void know(String value, String what) {
            // Very short values ("1", "ab") would replace half the report.
            if (value != null && value.trim().length() >= 4 && !value.trim().equalsIgnoreCase("unknown")) {
                known.add(new String[] {value.trim(), what});
            }
        }

        String clean(String text) {
            for (String[] k : known) {
                text = replace(text, Pattern.compile(Pattern.quote(k[0]), Pattern.CASE_INSENSITIVE), k[1]);
            }
            text = replace(text, EMAIL, "e-mail");
            text = replace(text, MAC, "MAC address");
            text = replace(text, IPV6, "IP address");
            text = replace(text, IPV4, "IP address");
            text = replace(text, PHONE, "phone number");
            text = replace(text, LONG_NUMBER, "long number");
            return text;
        }

        private String replace(String text, Pattern p, String what) {
            Matcher m = p.matcher(text);
            StringBuffer out = new StringBuffer();
            int n = 0;
            while (m.find()) {
                n++;
                m.appendReplacement(out, Matcher.quoteReplacement("<" + what + ">"));
            }
            if (n == 0) {
                return text;
            }
            m.appendTail(out);
            return out.toString();
        }
    }
}
