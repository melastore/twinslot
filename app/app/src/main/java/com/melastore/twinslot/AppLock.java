package com.melastore.twinslot;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Base64;

import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;

import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;

/**
 * The app lock: a PIN or a pattern, optionally unlocked by fingerprint.
 *
 * Only a salted PBKDF2 hash of the secret is stored. Wrong guesses are counted
 * and, past a few, answered with a growing wait that a reboot does not reset.
 * Whether the app is unlocked is kept in memory only, so a killed app starts
 * locked again.
 */
final class AppLock {

    enum Type { NONE, PIN, PATTERN }

    static final int MIN_PIN = 4;
    static final int MAX_PIN = 16;
    static final int MIN_PATTERN = 4;
    /** Seconds the app stays unlocked after it leaves the screen. */
    static final int[] TIMEOUTS = {0, 30, 60, 300};

    private static final int ITERATIONS = 60000;
    /** Wrong guesses allowed before the first wait. */
    static final int FREE_TRIES = 5;
    private static final long FIRST_WAIT_MS = 30_000;
    private static final long MAX_WAIT_MS = 15 * 60_000;
    private static final long PICKER_GRACE_MS = 5 * 60_000;

    // The session: shared by every screen of the app.
    private static boolean unlocked;
    private static int visibleScreens;
    private static long leftAt = -1;
    private static boolean returning;

    private final Context context;
    private final SharedPreferences sp;

    AppLock(Context context) {
        this.context = context.getApplicationContext();
        sp = this.context.getSharedPreferences("lock", Context.MODE_PRIVATE);
    }

    Type type() {
        try {
            return Type.valueOf(sp.getString("type", Type.NONE.name()));
        } catch (IllegalArgumentException e) {
            return Type.NONE;
        }
    }

    boolean enabled() {
        return type() != Type.NONE && sp.contains("hash");
    }

    boolean fingerprint() {
        return enabled() && sp.getBoolean("fingerprint", false);
    }

    void setFingerprint(boolean on) {
        sp.edit().putBoolean("fingerprint", on).apply();
    }

    int timeoutSeconds() {
        return sp.getInt("timeout", 0);
    }

    void setTimeoutSeconds(int s) {
        sp.edit().putInt("timeout", s).apply();
    }

    /** Sets a new secret. Slow (key stretching): call it off the main thread. */
    void set(Type type, String secret) throws GeneralSecurityException {
        byte[] salt = new byte[16];
        new SecureRandom().nextBytes(salt);
        byte[] hash = hash(type, secret, salt, ITERATIONS);
        sp.edit()
                .putString("type", type.name())
                .putString("salt", Base64.encodeToString(salt, Base64.NO_WRAP))
                .putString("hash", Base64.encodeToString(hash, Base64.NO_WRAP))
                .putInt("iterations", ITERATIONS)
                .remove("failures")
                .remove("wait_until")
                .remove("wait_boot")
                .commit();
        unlocked = true;
    }

    void disable() {
        sp.edit().clear().commit();
        FingerprintUnlock.deleteKey();
        unlocked = false;
    }

    /**
     * Checks a guess. Slow: call it off the main thread. A guess made while
     * {@link #waitMs()} is above zero is refused without being checked.
     */
    boolean check(String secret) throws GeneralSecurityException {
        if (!enabled() || waitMs() > 0) {
            return false;
        }
        byte[] salt = Base64.decode(sp.getString("salt", ""), Base64.NO_WRAP);
        byte[] want = Base64.decode(sp.getString("hash", ""), Base64.NO_WRAP);
        byte[] got = hash(type(), secret, salt, sp.getInt("iterations", ITERATIONS));
        if (MessageDigest.isEqual(want, got)) {
            sp.edit().remove("failures").remove("wait_until").remove("wait_boot").commit();
            return true;
        }
        int failures = sp.getInt("failures", 0) + 1;
        SharedPreferences.Editor e = sp.edit().putInt("failures", failures);
        if (failures >= FREE_TRIES) {
            long wait = Math.min(MAX_WAIT_MS, FIRST_WAIT_MS << Math.min(10, failures - FREE_TRIES));
            e.putLong("wait_until", SystemClock.elapsedRealtime() + wait).putLong("wait_ms", wait)
                    .putInt("wait_boot", bootCount());
        }
        e.commit();
        return false;
    }

    /** How long until the next guess is taken; 0 if now. */
    long waitMs() {
        if (!sp.contains("wait_until")) {
            return 0;
        }
        if (sp.getInt("wait_boot", -1) != bootCount()) {
            // The phone restarted: the clock the wait was measured on restarted too. Wait the whole time again.
            sp.edit().putLong("wait_until", SystemClock.elapsedRealtime() + sp.getLong("wait_ms", FIRST_WAIT_MS))
                    .putInt("wait_boot", bootCount()).apply();
        }
        return Math.max(0, sp.getLong("wait_until", 0) - SystemClock.elapsedRealtime());
    }

    int failures() {
        return sp.getInt("failures", 0);
    }

    // ---- the session ----

    boolean needsUnlock() {
        if (!enabled()) {
            return false;
        }
        if (!unlocked) {
            return true;
        }
        return leftAt >= 0 && SystemClock.elapsedRealtime() - leftAt >= timeoutSeconds() * 1000L;
    }

    void unlocked() {
        unlocked = true;
        leftAt = -1;
    }

    /** A screen of the app came into view. */
    static void screenStarted() {
        if (visibleScreens++ == 0 && returning) {
            // Back from a picker the app opened itself: that is not leaving the app, unless the picker
            // was only the way out and the phone sat elsewhere for long.
            returning = false;
            if (leftAt >= 0 && SystemClock.elapsedRealtime() - leftAt < PICKER_GRACE_MS) {
                leftAt = -1;
            }
        }
    }

    /**
     * A screen of the app left the view; when the last one goes, the app has been left. A screen that is
     * only rebuilt (rotation, a theme change) comes straight back, so it does not count as leaving.
     */
    static void screenStopped(boolean rebuilding) {
        if (--visibleScreens <= 0) {
            visibleScreens = 0;
            if (leftAt < 0 && !rebuilding) {
                leftAt = SystemClock.elapsedRealtime();
            }
        }
    }

    /** The app is about to open a system screen (the file picker) and come back from it. */
    static void openingOwnPicker() {
        returning = true;
    }

    private int bootCount() {
        return Settings.Global.getInt(context.getContentResolver(), Settings.Global.BOOT_COUNT, 0);
    }

    private static byte[] hash(Type type, String secret, byte[] salt, int iterations) throws GeneralSecurityException {
        // The type is part of what is hashed: a PIN never opens a pattern lock that spells the same digits.
        char[] input = (type.name() + ":" + secret).toCharArray();
        PBEKeySpec spec = new PBEKeySpec(input, salt, iterations, 256);
        try {
            return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
        } finally {
            spec.clearPassword();
        }
    }

    /** A pattern as the string that is hashed: the dots' numbers, 1 to 9, in the order drawn. */
    static String pattern(java.util.List<Integer> dots) {
        StringBuilder s = new StringBuilder();
        for (int d : dots) {
            s.append((char) ('1' + d));
        }
        return s.toString();
    }
}
