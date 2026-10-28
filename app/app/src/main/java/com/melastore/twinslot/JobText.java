package com.melastore.twinslot;

import java.util.List;
import java.util.Locale;

/**
 * Plain words for what the engine reports: its progress steps ("writing
 * system_a") and, when a job fails, what went wrong and what to do about it.
 * The engine's own lines stay in the log for anyone who wants them.
 */
final class JobText {

    private JobText() {
    }

    /** The headline for a step: what is happening, in a few words. */
    static String phase(String step) {
        String s = step.toLowerCase(Locale.ROOT);
        if (s.isEmpty()) {
            return "Starting";
        }
        if (s.startsWith("checking ")) {
            return "Checking the ROM";
        }
        if (s.startsWith("reading the rom zip")) {
            return "Opening the ROM";
        }
        if (s.startsWith("marking slot") || s.startsWith("updating the super")) {
            return "Preparing the slot";
        }
        if (s.startsWith("writing the partition table") || s.startsWith("blanking")) {
            return "Splitting the storage";
        }
        if (s.startsWith("writing ")) {
            return "Installing";
        }
        return "Finishing";
    }

    /** The line under the headline: the part being worked on. */
    static String detail(String step) {
        String s = step.toLowerCase(Locale.ROOT);
        if (s.isEmpty()) {
            return "Getting ready…";
        }
        if (s.startsWith("checking ")) {
            return "Reading " + part(step.substring(9)) + " to make sure the file is whole";
        }
        if (s.startsWith("reading the rom zip")) {
            return "Reading the list of partitions";
        }
        if (s.startsWith("marking slot")) {
            return "It will not boot until everything is written";
        }
        if (s.startsWith("updating the super")) {
            return "Making room for the new system";
        }
        if (s.startsWith("writing the partition table")) {
            return "Writing the new partition table";
        }
        if (s.startsWith("blanking")) {
            return "Clearing the new data partitions";
        }
        if (s.startsWith("writing ")) {
            return "Writing " + part(step.substring(8));
        }
        if (s.startsWith("hashing ")) {
            return "Signing " + part(step.substring(8)) + " for the bootloader";
        }
        if (s.startsWith("pointing the rom")) {
            return "Pointing it at its own data";
        }
        if (s.startsWith("wiping")) {
            return "Clearing its old data";
        }
        if (s.equals("done")) {
            return "Checking that it all landed";
        }
        return Character.toUpperCase(step.charAt(0)) + step.substring(1);
    }

    /** "system_a" → "system": the slot is a detail people need not read. */
    private static String part(String name) {
        String n = name.trim();
        return n.endsWith("_a") || n.endsWith("_b") ? n.substring(0, n.length() - 2) : n;
    }

    /** What went wrong, in plain words, from the engine's messages; null if nothing known matches. */
    static String explain(List<String> lines) {
        for (int i = lines.size() - 1; i >= 0; i--) {
            String why = explain(lines.get(i).toLowerCase(Locale.ROOT));
            if (why != null) {
                return why;
            }
        }
        return null;
    }

    private static String explain(String l) {
        if (l.contains("damaged download") || l.contains("truncated download") || l.contains("does not decompress")
                || l.contains("bad header") || l.contains("manifest is malformed")) {
            return "The ROM file is damaged or not fully downloaded. Download it again; if the site lists a "
                    + "checksum, compare it.";
        }
        if (l.contains("not a zip file")) {
            return "That file is not a ROM zip.";
        }
        if (l.contains("is it an a/b rom zip")) {
            return "This zip has no payload.bin, so it is not a full A/B ROM. TwinSlot installs the kind of ROM "
                    + "zip that recovery sideloads (with payload.bin inside).";
        }
        if (l.contains("incremental ota") || l.contains("partial update payload")
                || l.contains("is not in a full ota")) {
            return "This is an update for one exact build (an incremental OTA). Use the full ROM zip instead.";
        }
        if (l.contains("compressed inside the zip")) {
            return "This zip was repacked with compression. Use the zip exactly as the ROM's site gives it.";
        }
        if (l.contains("unsupported payload") || l.contains("unexpected payload")) {
            return "This ROM uses a payload format TwinSlot does not read yet.";
        }
        if (l.contains("super cannot hold") || l.contains("not enough free space in super")
                || l.contains("does not fit in group")) {
            return "There is not enough room in the phone's system area (super) for this ROM next to the one "
                    + "you run now. A smaller ROM (for example one without Google apps) may fit.";
        }
        if (l.contains(") does not fit in ")) {
            return "A part of this ROM is bigger than the same part on this phone. It is probably made for "
                    + "another phone.";
        }
        if (l.contains("is not a/b on this device")) {
            return "This ROM writes a partition this phone has only once, shared by both Androids. Installing "
                    + "it would change the Android you run now too, so TwinSlot stopped.";
        }
        if (l.contains("dual data is not set up")) {
            return "Split the storage first, so each Android gets its own data.";
        }
        if (l.contains("is the running rom")) {
            return "That slot holds the Android you are using. Switch to the other one first.";
        }
        if (l.contains("still merging")) {
            return "A system update is still finishing in the background. Keep using this Android until it is "
                    + "done, then try again.";
        }
        if (l.contains("cannot open /storage") || l.contains("cannot open /sdcard")
                || l.contains("cannot open /data/media")) {
            return "TwinSlot could not read the zip. Keep it on internal storage (for example in Download) and "
                    + "pick it again.";
        }
        if (l.contains("write failed") || l.contains("fsync failed") || l.contains("did not read back")) {
            return "Writing to the phone's storage failed. The slot was left switched off, so nothing boots "
                    + "from half a ROM. Try once more; if it fails again, share the log.";
        }
        if (l.contains("does not match the rom (hash mismatch)")) {
            return "What was written does not read back the same. The slot was left switched off. Try once "
                    + "more; if it fails again, share the log.";
        }
        if (l.contains("mounts") && l.contains("expected")) {
            return "The new ROM could not be pointed at its own data, so it was left switched off. Use “Fix” "
                    + "on its card, or share the log.";
        }
        return null;
    }

    /**
     * Time left, from how fast the bar moved so far. The zip check and the
     * install each run 0 to 100%, so the rate restarts when the phase changes.
     */
    static final class Eta {
        private String phase = "";
        private long t0;
        private int p0;
        private long remainingMs = -1;

        void sample(String step, int permille) {
            String ph = phase(step);
            long now = android.os.SystemClock.elapsedRealtime();
            if (!ph.equals(phase) || permille < p0) {
                phase = ph;
                t0 = now;
                p0 = permille;
                remainingMs = -1;
                return;
            }
            long elapsed = now - t0;
            int moved = permille - p0;
            // Too little to go on yet: the first seconds are mostly opening files.
            if (elapsed < 8000 || moved < 15) {
                return;
            }
            long estimate = elapsed * (1000 - permille) / moved;
            remainingMs = remainingMs < 0 ? estimate : (remainingMs * 3 + estimate) / 4;
        }

        /** "About 3 min left", or null while there is no fair guess. */
        String text() {
            if (remainingMs < 0) {
                return null;
            }
            long min = Math.round(remainingMs / 60000.0);
            return min < 1 ? "Less than a minute left" : "About " + min + " min left";
        }
    }
}
