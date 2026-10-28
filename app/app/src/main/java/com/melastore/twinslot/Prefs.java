package com.melastore.twinslot;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.os.Build;

/** The user's choices. Each Android has its own copy: they live in this app's data. */
final class Prefs {

    enum Theme { SYSTEM, LIGHT, DARK }

    /** WALLPAPER follows Android 12+'s wallpaper colors; the rest are fixed. */
    enum Accent { WALLPAPER, BLUE, TEAL, GREEN, PURPLE, AMBER, ROSE }

    enum ScreenOn { JOBS, ALWAYS, NEVER }

    private final SharedPreferences sp;

    Prefs(Context context) {
        sp = context.getApplicationContext().getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    // ---- appearance ----

    Theme theme() {
        return get("theme", Theme.SYSTEM);
    }

    Accent accent() {
        Accent a = get("accent", Accent.WALLPAPER);
        return a == Accent.WALLPAPER && !wallpaperColors() ? Accent.BLUE : a;
    }

    boolean pureBlack() {
        return sp.getBoolean("pure_black", false);
    }

    void setTheme(Theme t) {
        sp.edit().putString("theme", t.name()).putInt("look", look() + 1).apply();
    }

    void setAccent(Accent a) {
        sp.edit().putString("accent", a.name()).putInt("look", look() + 1).apply();
    }

    void setPureBlack(boolean on) {
        sp.edit().putBoolean("pure_black", on).putInt("look", look() + 1).apply();
    }

    /** Bumped on every change of the look, so open screens know to redraw themselves. */
    int look() {
        return sp.getInt("look", 0);
    }

    static boolean wallpaperColors() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.S;
    }

    /**
     * The configuration override that forces day or night on a context, or null to follow the system.
     * Applied before the context's resources are made, so the theme, dialogs and system bars all follow it.
     */
    static Configuration nightOverride(Context base) {
        Theme t = new Prefs(base).theme();
        if (t == Theme.SYSTEM) {
            return null;
        }
        Configuration c = new Configuration();
        c.uiMode = (base.getResources().getConfiguration().uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                | (t == Theme.DARK ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        return c;
    }

    // ---- screen and power ----

    ScreenOn screenOn() {
        return get("screen_on", ScreenOn.JOBS);
    }

    void setScreenOn(ScreenOn s) {
        sp.edit().putString("screen_on", s.name()).apply();
    }

    /** Hold a wake lock while a job runs, so it does not pause when the screen goes off. */
    boolean stayAwake() {
        return sp.getBoolean("stay_awake", true);
    }

    void setStayAwake(boolean on) {
        sp.edit().putBoolean("stay_awake", on).apply();
    }

    // ---- install defaults ----

    boolean wipeByDefault() {
        return sp.getBoolean("install_wipe", true);
    }

    boolean verifyByDefault() {
        return sp.getBoolean("install_verify", true);
    }

    boolean switchByDefault() {
        return sp.getBoolean("install_switch", true);
    }

    /** Off: the zip's lk/preloader are skipped and the phone's own stay. */
    boolean bootloaderByDefault() {
        return sp.getBoolean("install_bootloader", false);
    }

    void setWipeByDefault(boolean on) {
        sp.edit().putBoolean("install_wipe", on).apply();
    }

    void setVerifyByDefault(boolean on) {
        sp.edit().putBoolean("install_verify", on).apply();
    }

    void setSwitchByDefault(boolean on) {
        sp.edit().putBoolean("install_switch", on).apply();
    }

    void setBootloaderByDefault(boolean on) {
        sp.edit().putBoolean("install_bootloader", on).apply();
    }

    // ---- the running job ----

    /** What the running job does, as a title: "Installing crDroid into slot A". */
    String jobTitle() {
        return sp.getString("job_title", "Last job");
    }

    /** The zip and target slot of the last install, so an interrupted one can be started again. */
    String jobZip() {
        return sp.getString("job_zip", null);
    }

    int jobSlot() {
        return sp.getInt("job_slot", -1);
    }

    void setJob(String title, String zip, int slot) {
        sp.edit().putString("job_title", title).putString("job_zip", zip).putInt("job_slot", slot).apply();
    }

    /** Asked for the notification permission once already: Android shows its prompt only so often. */
    boolean askedNotifications() {
        return sp.getBoolean("asked_notifications", false);
    }

    void setAskedNotifications() {
        sp.edit().putBoolean("asked_notifications", true).apply();
    }

    // ---- ROM names ----

    /**
     * The name the user gave the ROM in slot, "" for the ROM's own. Keyed by the ROM's own name too, so a
     * newer build of it keeps the name and a different ROM installed there does not inherit it.
     */
    String romName(int slot, String rom) {
        return sp.getString(romKey(slot, rom), "");
    }

    void setRomName(int slot, String rom, String name) {
        String n = name == null ? "" : name.trim();
        if (n.isEmpty()) {
            sp.edit().remove(romKey(slot, rom)).apply();
        } else {
            sp.edit().putString(romKey(slot, rom), n).apply();
        }
    }

    private static String romKey(int slot, String rom) {
        return "rom_name_" + slot + "_" + rom;
    }

    // ---- first run ----

    /** The first-open warning's version: raised when its text changes in a way everyone must read again. */
    static final int DISCLAIMER_VERSION = 1;

    boolean disclaimerAccepted() {
        return sp.getInt("disclaimer", 0) >= DISCLAIMER_VERSION;
    }

    void setDisclaimerAccepted() {
        sp.edit().putInt("disclaimer", DISCLAIMER_VERSION).apply();
    }

    boolean guideOffered() {
        return sp.getBoolean("guide_offered", false);
    }

    void setGuideOffered() {
        sp.edit().putBoolean("guide_offered", true).apply();
    }

    private <E extends Enum<E>> E get(String key, E fallback) {
        String v = sp.getString(key, null);
        if (v != null) {
            try {
                return Enum.valueOf(fallback.getDeclaringClass(), v);
            } catch (IllegalArgumentException ignored) {
                // Written by another version: use the default.
            }
        }
        return fallback;
    }
}
