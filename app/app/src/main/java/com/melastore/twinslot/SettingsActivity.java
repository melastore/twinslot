package com.melastore.twinslot;

import android.app.AlertDialog;
import android.content.Intent;
import android.os.CancellationSignal;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.security.GeneralSecurityException;

/** Look, screen, install defaults, the app lock and help. Each Android keeps its own settings. */
public class SettingsActivity extends Screen {

    static final String BOOTLOADER_NOTE = "Off keeps this phone's own lk. Turn on only for an lk made for this "
            + "exact model: one that does not start leaves the phone at the logo, with no fallback to the other "
            + "slot, until it is flashed over USB.";

    @Override
    void ready() {
        render();
    }

    private void render() {
        content.removeAllViews();
        pageHeader("Settings");
        appearance();
        screen();
        installDefaults();
        security();
        help();
    }

    private LinearLayout group(String title) {
        content.addView(ui.sectionTitle(title), ui.top(20));
        LinearLayout card = ui.card(p.surface);
        card.setPadding(ui.dp(20), ui.dp(6), ui.dp(20), ui.dp(6));
        content.addView(card);
        return card;
    }

    // ---- appearance ----

    private void appearance() {
        LinearLayout g = group("Appearance");
        String[] themes = {"Follow the system", "Light", "Dark"};
        g.addView(ui.actionRow(R.drawable.ic_theme, "Theme", themes[prefs.theme().ordinal()], v ->
                choose("Theme", themes, prefs.theme().ordinal(), i -> {
                    prefs.setTheme(Prefs.Theme.values()[i]);
                    recreate();
                })));
        g.addView(ui.divider());

        LinearLayout colors = ui.column();
        colors.setPadding(0, ui.dp(12), 0, ui.dp(12));
        LinearLayout titleRow = ui.row();
        titleRow.addView(ui.iconView(R.drawable.ic_palette, p.textDim, 22));
        TextView t = ui.medium("Color", 15.5f, p.text);
        t.setPadding(ui.dp(16), 0, 0, 0);
        titleRow.addView(t);
        colors.addView(titleRow);
        LinearLayout swatches = ui.row();
        swatches.setPadding(ui.dp(38), ui.dp(10), 0, 0);
        for (Prefs.Accent a : Prefs.Accent.values()) {
            if (a == Prefs.Accent.WALLPAPER && !Prefs.wallpaperColors()) {
                continue;
            }
            swatches.addView(swatch(a));
        }
        android.widget.HorizontalScrollView scroll = new android.widget.HorizontalScrollView(this);
        scroll.setHorizontalScrollBarEnabled(false);
        scroll.addView(swatches);
        colors.addView(scroll);
        g.addView(colors);

        if (p.dark) {
            g.addView(ui.divider());
            g.addView(ui.switchRow("Pure black", "A black background in dark mode, easy on OLED screens",
                    prefs.pureBlack(), on -> {
                        prefs.setPureBlack(on);
                        recreate();
                    }));
        }
    }

    private View swatch(Prefs.Accent a) {
        boolean chosen = prefs.accent() == a;
        int color = Palette.swatch(this, a, p.dark);
        TextView s = new TextView(this);
        s.setGravity(Gravity.CENTER);
        android.graphics.drawable.GradientDrawable bg = ui.shape(color, 100);
        if (chosen) {
            bg.setStroke(ui.dp(3), p.text);
            s.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_check, contrastOn(color), 20), null, null, null);
            s.setPadding(ui.dp(10), 0, 0, 0);
        }
        s.setBackground(bg);
        String label = a == Prefs.Accent.WALLPAPER ? "Wallpaper colors"
                : a.name().charAt(0) + a.name().substring(1).toLowerCase(java.util.Locale.ROOT);
        s.setContentDescription(label + (chosen ? ", chosen" : ""));
        s.setOnClickListener(v -> {
            prefs.setAccent(a);
            recreate();
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ui.dp(40), ui.dp(40));
        lp.setMarginEnd(ui.dp(12));
        s.setLayoutParams(lp);
        if (a == Prefs.Accent.WALLPAPER) {
            // The wallpaper's swatch carries a small mark, so it reads as "from the wallpaper".
            s.setTooltipText(label);
            if (!chosen) {
                s.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_wallpaper, contrastOn(color), 20), null, null, null);
                s.setPadding(ui.dp(10), 0, 0, 0);
            }
        }
        return s;
    }

    /** Black or white, whichever reads on {@code color}. */
    private static int contrastOn(int color) {
        double l = 0.2126 * ((color >> 16) & 0xFF) + 0.7152 * ((color >> 8) & 0xFF) + 0.0722 * (color & 0xFF);
        return l > 140 ? 0xFF000000 : 0xFFFFFFFF;
    }

    // ---- screen ----

    private void screen() {
        LinearLayout g = group("Screen and power");
        String[] modes = {"While a job runs", "Whenever TwinSlot is open", "Never"};
        String[] subs = {"On while installing or checking a zip", "On whenever TwinSlot is on screen",
            "Turns off as usual"};
        g.addView(ui.actionRow(R.drawable.ic_screen, "Keep the screen on", subs[prefs.screenOn().ordinal()], v ->
                choose("Keep the screen on", modes, prefs.screenOn().ordinal(), i -> {
                    prefs.setScreenOn(Prefs.ScreenOn.values()[i]);
                    updateScreenOn();
                    render();
                })));
        g.addView(ui.divider());
        g.addView(ui.switchRow("Keep the phone awake during jobs", "So an install never pauses when the screen "
                + "turns off. Shows a notification while it runs.", prefs.stayAwake(), prefs::setStayAwake));
    }

    // ---- install defaults ----

    private void installDefaults() {
        LinearLayout g = group("Install defaults");
        g.addView(ui.switchRow("Start with empty data", "Needed when the ROM changes",
                prefs.wipeByDefault(), prefs::setWipeByDefault));
        g.addView(ui.divider());
        g.addView(ui.switchRow("Check the zip first", "Reads the whole zip before writing anything",
                prefs.verifyByDefault(), prefs::setVerifyByDefault));
        g.addView(ui.divider());
        g.addView(ui.switchRow("Boot it next", "The new ROM starts on the next reboot",
                prefs.switchByDefault(), prefs::setSwitchByDefault));
        g.addView(ui.divider());
        g.addView(ui.switchRow("Write the zip's bootloader", BOOTLOADER_NOTE,
                prefs.bootloaderByDefault(), prefs::setBootloaderByDefault));
    }

    // ---- security ----

    private void security() {
        LinearLayout g = group("Security");
        AppLock.Type type = lock.enabled() ? lock.type() : AppLock.Type.NONE;
        String[] types = {"Off", "PIN", "Pattern"};
        g.addView(ui.actionRow(R.drawable.ic_lock, "App lock", type == AppLock.Type.NONE ? "Off"
                : types[type.ordinal()] + " · asked when TwinSlot opens", v -> chooseLock()));
        if (!lock.enabled()) {
            return;
        }
        g.addView(ui.divider());
        g.addView(ui.actionRow(R.drawable.ic_key, "Change " + (type == AppLock.Type.PIN ? "PIN" : "pattern"), null,
                v -> verifyThen("Confirm your " + noun(type), () -> create(type))));
        g.addView(ui.divider());

        String why = FingerprintUnlock.unavailable(this);
        View fp = ui.switchRow("Unlock with fingerprint", why != null ? why
                : "Your " + noun(type) + " still works, and is needed after a new fingerprint is added",
                lock.fingerprint(), on -> {
                    if (on) {
                        enableFingerprint();
                    } else {
                        lock.setFingerprint(false);
                        FingerprintUnlock.deleteKey();
                    }
                });
        if (why != null && !lock.fingerprint()) {
            Ui.enable(fp, false);
        }
        g.addView(fp);
        g.addView(ui.divider());

        int[] timeouts = AppLock.TIMEOUTS;
        String[] labels = new String[timeouts.length];
        int current = 0;
        for (int i = 0; i < timeouts.length; i++) {
            labels[i] = timeouts[i] == 0 ? "Right away" : timeouts[i] < 60 ? "After " + timeouts[i] + " seconds"
                    : "After " + timeouts[i] / 60 + (timeouts[i] == 60 ? " minute" : " minutes");
            if (timeouts[i] == lock.timeoutSeconds()) {
                current = i;
            }
        }
        int chosen = current;
        g.addView(ui.actionRow(R.drawable.ic_timer, "Lock again", labels[chosen] + " after leaving TwinSlot", v ->
                choose("Lock again", labels, chosen, i -> {
                    lock.setTimeoutSeconds(timeouts[i]);
                    render();
                })));
    }

    private static String noun(AppLock.Type t) {
        return t == AppLock.Type.PATTERN ? "pattern" : "PIN";
    }

    private void chooseLock() {
        String[] types = {"Off", "PIN", "Pattern"};
        int current = lock.enabled() ? lock.type().ordinal() : 0;
        choose("App lock", types, current, i -> {
            if (i == current) {
                return;
            }
            AppLock.Type t = AppLock.Type.values()[i];
            if (!lock.enabled()) {
                create(t);
            } else if (t == AppLock.Type.NONE) {
                verifyThen("Turn off the app lock", () -> {
                    lock.disable();
                    render();
                    SwitchFlow.toast(this, "App lock is off");
                });
            } else {
                verifyThen("Confirm your " + noun(lock.type()), () -> create(t));
            }
        });
    }

    /** Asks for the current secret, then runs {@code then}. */
    private void verifyThen(String why, Runnable then) {
        showPad(LockScreen.verify(this, lock, why, () -> {
            hidePad();
            then.run();
        }, this::hidePad));
    }

    private void create(AppLock.Type t) {
        showPad(LockScreen.create(this, lock, t, (type, secret) -> {
            hidePad();
            background(() -> {
                lock.set(type, secret);
                runOnUiThread(() -> {
                    render();
                    SwitchFlow.toast(this, (type == AppLock.Type.PIN ? "PIN" : "Pattern") + " set. TwinSlot asks "
                            + "for it when it opens.");
                });
            }, e -> SwitchFlow.toast(this, "Not set: " + e));
        }, this::hidePad));
    }

    /** A fresh key, and one touch to prove the sensor and the key work together, before it is relied on. */
    private void enableFingerprint() {
        try {
            FingerprintUnlock.createKey();
        } catch (GeneralSecurityException e) {
            SwitchFlow.toast(this, "Fingerprint unlock is not available: " + e.getMessage());
            render();
            return;
        }
        FingerprintUnlock.authenticate(this, "Turn on fingerprint unlock", "Cancel", new CancellationSignal(),
                new FingerprintUnlock.Callback() {
                    @Override
                    public void succeeded() {
                        lock.setFingerprint(true);
                        render();
                    }

                    @Override
                    public void fallback() {
                        FingerprintUnlock.deleteKey();
                        render();
                    }

                    @Override
                    public void failed(CharSequence why) {
                        FingerprintUnlock.deleteKey();
                        SwitchFlow.toast(SettingsActivity.this, why.toString());
                        render();
                    }

                    @Override
                    public void invalidated() {
                        render();
                    }
                });
    }

    // ---- help ----

    private void help() {
        LinearLayout g = group("Help");
        g.addView(ui.actionRow(R.drawable.ic_guide, "Guide", "How dual boot works, step by step",
                v -> startActivity(new Intent(this, GuideActivity.class))));
        g.addView(ui.divider());
        g.addView(ui.actionRow(R.drawable.ic_bug, "Diagnostic log",
                "For a bug report: this phone's layout, root and TwinSlot's logs",
                v -> startActivity(new Intent(this, DiagnosticsActivity.class))));
        g.addView(ui.divider());
        g.addView(ui.actionRow(R.drawable.ic_notification, "Notifications", "Install progress and results",
                v -> {
                    AppLock.openingOwnPicker();
                    startActivity(new Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, getPackageName()));
                }));

        TextView about = ui.text("TwinSlot " + MainActivity.appVersion(this) + "\nTwo Androids on one A/B phone. "
                + "Free software; it changes partitions, so keep a backup and a way to flash the phone from "
                + "scratch.", 12.5f, p.textDim);
        about.setGravity(Gravity.CENTER);
        content.addView(about, ui.top(24));
    }

    // ---- plumbing ----

    private interface Chosen {
        void chosen(int index);
    }

    private void choose(String title, String[] items, int current, Chosen chosen) {
        show(new AlertDialog.Builder(this)
                .setTitle(title)
                .setSingleChoiceItems(items, current, (d, i) -> {
                    d.dismiss();
                    chosen.chosen(i);
                })
                .setNegativeButton(android.R.string.cancel, null));
    }
}
