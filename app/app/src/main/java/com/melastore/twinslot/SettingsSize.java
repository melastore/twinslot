package com.melastore.twinslot;

import java.util.Locale;

/**
 * The size Settings > Storage shows for one ROM.
 *
 * AOSP rounds /data + /system up to a power of two in GiB but hands it back in decimal GB
 * (FileUtils.roundStorageSize). With a split userdata that goes wrong: 125 GiB of /data shows as
 * "128 GB" with a negative "used". Pick sizes that land on a band.
 */
final class SettingsSize {

    static final long GB = 1_000_000_000L;
    static final long MIB = 1L << 20;
    static final long GIB = 1L << 30;
    /** The filesystems are a little smaller than their partitions: never trust a size this close to a band. */
    private static final long SLACK = 64 * MIB;

    private SettingsSize() {
    }

    /** FileUtils.roundStorageSize, as the framework has it. */
    static long round(long size) {
        long val = 1;
        long pow1024 = 1;
        long pow1000 = 1;
        while (val * pow1024 < size) {
            val <<= 1;
            if (val > 512) {
                val = 1;
                pow1024 *= 1024;
                pow1000 *= 1000;
            }
        }
        return val * pow1000;
    }

    /** The total Settings shows for /data of dataBytes and /system of systemBytes. */
    static long total(long dataBytes, long systemBytes) {
        return round(Math.max(0, dataBytes + systemBytes - SLACK));
    }

    /**
     * How far Settings' "used" is off: positive = that much extra shows as System; negative = it shows less than
     * is used (and below zero on an empty phone). The worse of the two readings on either side of the slack.
     */
    static long error(long dataBytes, long systemBytes) {
        long tight = round(Math.max(0, dataBytes + systemBytes - SLACK)) - dataBytes;
        long loose = round(dataBytes + systemBytes) - dataBytes;
        return Math.min(tight, loose);
    }

    static boolean wrong(long dataBytes, long systemBytes) {
        return error(dataBytes, systemBytes) < 0;
    }

    /**
     * The size in GiB for the owner's own data, out of poolBytes for both, that keeps both Settings screens right
     * with the least extra "System" on the worse side; -1 if none.
     */
    static int best(long poolBytes, long ownerSystem, long otherSystem, int minGiB, int maxGiB) {
        int best = -1;
        long bestError = Long.MAX_VALUE;
        for (int g = minGiB; g <= maxGiB; g++) {
            long owner = g * GIB;
            long other = poolBytes - owner;
            long a = error(owner, ownerSystem);
            long b = error(other, otherSystem);
            if (a < 0 || b < 0) {
                continue;
            }
            long worse = Math.max(a, b);
            if (worse < bestError) {
                bestError = worse;
                best = g;
            }
        }
        return best;
    }

    /** Decimal GB, the unit Settings and the box use. */
    static String format(long bytes) {
        double gb = bytes / (double) GB;
        return gb >= 10 || gb == Math.rint(gb) ? String.format(Locale.US, "%.0f GB", gb)
                : String.format(Locale.US, "%.1f GB", gb);
    }
}
