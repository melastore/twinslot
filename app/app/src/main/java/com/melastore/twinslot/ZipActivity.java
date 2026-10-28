package com.melastore.twinslot;

import android.content.Context;
import android.content.Intent;
import android.view.Gravity;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Makes a recovery zip (setup, resize, remove) and explains how to flash it.
 *
 * These zips change the partition table, so they run in recovery with /data
 * unmounted. The page walks through that: what the zip does, a backup check
 * when it erases data, then the ways to get it into recovery.
 */
public class ZipActivity extends Screen {

    private static final String EXTRA_NAME = "name";
    private static final String EXTRA_TITLE = "title";
    private static final String EXTRA_WHAT = "what";
    private static final String EXTRA_ERASES = "erases";
    private static final String EXTRA_COMMANDS = "commands";
    private static final String EXTRA_FIRST = "first";
    /** Separates arguments inside one command of {@link #EXTRA_COMMANDS}. */
    private static final String ARG_SEP = "\u001f";

    private enum State { PLAN, MAKING, READY, FAILED }

    private State state = State.PLAN;
    private boolean backedUp;
    private String path;
    private long size;
    private String error;
    /** The APKs saved next to the zip (file names), for an erasing zip. */
    private ArrayList<String> apks = new ArrayList<>();
    /** The copy of the partition table saved next to a zip that rewrites the table (file name), or null. */
    private String table;

    /**
     * Opens the page for one zip. {@code what} says what it does; {@code erases}
     * says whether it wipes the data of both Androids.
     */
    static void open(Context c, String name, String title, String what, boolean erases, List<String[]> commands) {
        open(c, name, title, what, erases, false, commands);
    }

    /** {@code first}: the first split, on a phone with one Android in use; its warning says so. */
    static void open(Context c, String name, String title, String what, boolean erases, boolean first,
            List<String[]> commands) {
        ArrayList<String> lines = new ArrayList<>();
        for (String[] args : commands) {
            lines.add(String.join(ARG_SEP, args));
        }
        c.startActivity(new Intent(c, ZipActivity.class)
                .putExtra(EXTRA_NAME, name)
                .putExtra(EXTRA_TITLE, title)
                .putExtra(EXTRA_WHAT, what)
                .putExtra(EXTRA_ERASES, erases)
                .putExtra(EXTRA_FIRST, first)
                .putStringArrayListExtra(EXTRA_COMMANDS, lines));
    }

    @Override
    protected void onCreate(android.os.Bundle saved) {
        super.onCreate(saved);
        // A rotation recreates the page: it stays where it was. A zip still being made is made again on tap.
        if (saved != null) {
            state = State.values()[saved.getInt("state", 0)];
            if (state == State.MAKING) {
                state = State.PLAN;
            }
            backedUp = saved.getBoolean("backedUp");
            path = saved.getString("path");
            size = saved.getLong("size");
            error = saved.getString("error");
            ArrayList<String> kept = saved.getStringArrayList("apks");
            apks = kept != null ? kept : new ArrayList<>();
            table = saved.getString("table");
        }
    }

    @Override
    protected void onSaveInstanceState(android.os.Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("state", state.ordinal());
        out.putBoolean("backedUp", backedUp);
        out.putString("path", path);
        out.putLong("size", size);
        out.putString("error", error);
        out.putStringArrayList("apks", apks);
        out.putString("table", table);
    }

    @Override
    void ready() {
        render();
    }

    private String name() {
        return getIntent().getStringExtra(EXTRA_NAME);
    }

    private boolean erases() {
        return getIntent().getBooleanExtra(EXTRA_ERASES, false);
    }

    private void render() {
        content.removeAllViews();
        pageHeader(getIntent().getStringExtra(EXTRA_TITLE));
        switch (state) {
            case PLAN: plan(); break;
            case MAKING: making(); break;
            case READY: made(); break;
            default: failed(); break;
        }
    }

    private void plan() {
        LinearLayout card = ui.card(p.surface);
        card.addView(ui.overline("Recovery zip", p.textDim));
        card.addView(ui.headline(name(), 20, p.text), ui.top(4));
        card.addView(ui.body(getIntent().getStringExtra(EXTRA_WHAT)), ui.top(10));
        content.addView(card, ui.top(16));

        if (erases()) {
            boolean first = getIntent().getBooleanExtra(EXTRA_FIRST, false);
            content.addView(first
                    ? ui.banner(R.drawable.ic_warning, "Everything on this phone is erased",
                            "Splitting the storage resets the phone: apps, photos, Download and accounts on internal "
                                    + "storage are erased. Android itself stays installed. Copy what you want to "
                                    + "keep to a computer or an SD card first.", p.errorContainer, p.onErrorContainer)
                    : ui.banner(R.drawable.ic_warning, "Both Androids start empty",
                            "It erases the data of BOTH Androids: apps, photos, everything on internal storage. The "
                                    + "ROMs stay installed. Copy what you want to keep to a computer or a card first.",
                            p.errorContainer, p.onErrorContainer), ui.top(12));
            // Found in the phone run: after the flash the phone had no TwinSlot, no root manager and no APKs.
            content.addView(ui.banner(R.drawable.ic_info, "The apps go too",
                    "TwinSlot and your root manager (Magisk, KernelSU…) are erased as well, and so is a ROM zip "
                            + "left in Download. TwinSlot puts its own APK and the root manager's next to the zip: "
                            + "copy that whole folder off the phone.", p.surfaceHigh, p.text), ui.top(10));
            CheckBox ok = new CheckBox(this);
            ok.setText(first ? "I have copied my files off the phone" : "I have a backup of both Androids");
            ok.setTextColor(p.text);
            ok.setTextSize(15);
            ok.setButtonTintList(android.content.res.ColorStateList.valueOf(p.primary));
            ok.setChecked(backedUp);
            content.addView(ok, ui.top(8));
            Button make = ui.button("Make the zip", Ui.Style.DANGER, R.drawable.ic_download);
            Ui.enable(make, backedUp);
            ok.setOnCheckedChangeListener((b, v) -> {
                backedUp = v;
                Ui.enable(make, v);
            });
            make.setOnClickListener(v -> make());
            content.addView(make, ui.top(12));
        } else {
            Button make = ui.button("Make the zip", Ui.Style.FILLED, R.drawable.ic_download);
            make.setOnClickListener(v -> make());
            content.addView(make, ui.top(16));
        }
        TextView where = ui.text("It is saved to " + Twinslot.ZIP_DIR + ". Nothing changes until you flash it.",
                12.5f, p.textDim);
        where.setGravity(Gravity.CENTER);
        content.addView(where, ui.top(12));
    }

    private void making() {
        LinearLayout card = ui.card(p.surface);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        card.setPadding(ui.dp(20), ui.dp(36), ui.dp(20), ui.dp(36));
        ProgressBar spin = new ProgressBar(this);
        spin.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(p.primary));
        card.addView(spin, new LinearLayout.LayoutParams(ui.dp(40), ui.dp(40)));
        TextView t = ui.body("Making " + name() + "…");
        t.setGravity(Gravity.CENTER);
        card.addView(t, ui.top(16));
        content.addView(card, ui.top(16));
    }

    private void make() {
        state = State.MAKING;
        render();
        List<String[]> commands = new ArrayList<>();
        for (String line : getIntent().getStringArrayListExtra(EXTRA_COMMANDS)) {
            commands.add(line.split(ARG_SEP, -1));
        }
        boolean repartitions = false;
        for (String[] c : commands) {
            repartitions |= c.length > 0 && (c[0].equals("setup") || c[0].equals("remove"));
        }
        boolean saveTable = repartitions;
        background(() -> {
            Twinslot ts = new Twinslot(this);
            // Before the zip: a zip that rewrites the partition table is only made once a copy of it is safe.
            String copy = saveTable ? ts.saveTable() : null;
            String made = ts.makeRecoveryZip(name(), commands, copy);
            long bytes = ts.size(made);
            ArrayList<String> saved = erases() ? saveApks(ts) : new ArrayList<>();
            runOnUiThread(() -> {
                path = made;
                size = bytes;
                apks = saved;
                table = copy;
                state = State.READY;
                render();
            });
        }, e -> {
            error = e;
            state = State.FAILED;
            render();
        });
    }

    /**
     * TwinSlot's own APK and the root manager's, next to the zip: the zip erases both apps, and these are what
     * puts them back. Best effort; a copy that fails is noted and left out.
     */
    private ArrayList<String> saveApks(Twinslot ts) {
        ArrayList<String> saved = new ArrayList<>();
        try {
            String name = "TwinSlot-" + MainActivity.appVersion(this) + ".apk";
            ts.saveApk(getApplicationInfo().sourceDir, name);
            saved.add(name);
        } catch (IOException e) {
            Diagnostics.note(this, "could not save the TwinSlot APK: " + e.getMessage());
        }
        DeviceCheck.RootManager m = DeviceCheck.installedManager(this);
        if (m != null) {
            try {
                String apk = getPackageManager().getApplicationInfo(m.pkg, 0).sourceDir;
                String name = m.name.replaceAll("[^A-Za-z0-9._-]", "") + ".apk";
                ts.saveApk(apk, name);
                saved.add(name);
            } catch (IOException | android.content.pm.PackageManager.NameNotFoundException e) {
                Diagnostics.note(this, "could not save the " + m.name + " APK: " + e.getMessage());
            }
        }
        return saved;
    }

    private void made() {
        LinearLayout card = ui.card(p.primaryContainer);
        LinearLayout head = ui.row();
        head.addView(ui.iconView(R.drawable.ic_check, p.onPrimaryContainer, 28));
        TextView title = ui.headline("Zip ready", 24, p.onPrimaryContainer);
        title.setPadding(ui.dp(12), 0, 0, 0);
        head.addView(title);
        card.addView(head);
        TextView file = ui.medium(name(), 15, p.onPrimaryContainer);
        card.addView(file, ui.top(12));
        TextView info = ui.text(path + (size > 0 ? " · " + android.text.format.Formatter.formatShortFileSize(this,
                size) : ""), 13, p.onPrimaryContainer);
        info.setAlpha(0.8f);
        info.setTextIsSelectable(true);
        card.addView(info, ui.top(2));
        content.addView(card, ui.top(16));

        content.addView(ui.sectionTitle("Flash it in recovery"), ui.top(24));
        LinearLayout how = ui.card(p.surface);
        String file1 = name();
        boolean folder = !apks.isEmpty() || table != null;
        how.addView(ui.medium("From a computer (ADB sideload)", 15.5f, p.text));
        how.addView(ui.step(1, folder ? "Copy the folder (the zip and the APKs) to the computer now, while Android "
                + "runs:" : "Copy the zip to the computer now, while Android runs:"), ui.top(10));
        how.addView(ui.codeBlock("adb pull " + (folder ? Twinslot.ZIP_DIR : path)), ui.top(8));
        how.addView(ui.step(2, "Reboot to recovery and choose Apply update › Apply from ADB (TWRP: Advanced › "
                + "ADB Sideload)." + (erases() ? " If recovery asks for your PIN to decrypt the data, cancel: "
                + "the data must stay closed for the zip to change it." : "")), ui.top(12));
        how.addView(ui.step(3, "On the computer:"), ui.top(10));
        how.addView(ui.codeBlock("adb sideload " + file1), ui.top(8));
        how.addView(ui.divider(), ui.top(18));
        how.addView(ui.medium("From an SD card or USB stick", 15.5f, p.text), ui.top(16));
        how.addView(ui.body("Copy the " + (folder ? "folder" : "zip") + " there, then in recovery choose Apply "
                + "update › Choose from SD card (or USB). Not from internal storage: it lives on the data "
                + "partition this zip changes."), ui.top(6));
        content.addView(how);
        if (!apks.isEmpty()) {
            content.addView(ui.banner(R.drawable.ic_info, "Next to the zip",
                    String.join("\n", apks) + "\n\nInstall them again after the flash: TwinSlot first, then the "
                            + "root manager, and allow TwinSlot root.", p.surfaceHigh, p.text), ui.top(12));
        }
        if (table != null) {
            content.addView(ui.banner(R.drawable.ic_shield, "A copy of the partition table",
                    table + "\n\nThe partition table as it is now, saved before the zip changes it. Keep it on "
                            + "the computer. If the flash is cut off (battery, cable) and the phone no longer "
                            + "starts, it puts the old table back: see “If a flash is cut off” in the guide.",
                    p.surfaceHigh, p.text), ui.top(12));
        }

        content.addView(ui.banner(R.drawable.ic_info, "“Signature verification failed”?",
                "Recoveries that check signatures (LineageOS and similar) warn about zips made on the phone. "
                        + "Choose Yes / Install anyway: the zip only runs TwinSlot's commands.",
                p.surfaceHigh, p.text), ui.top(12));

        Button reboot = ui.button("Reboot to recovery", Ui.Style.FILLED, R.drawable.ic_reboot);
        reboot.setOnClickListener(v -> show(new android.app.AlertDialog.Builder(this)
                .setTitle("Reboot to recovery?")
                .setMessage("Copy the zip to a computer or card first if you have not.")
                .setPositiveButton("Reboot", (d, w) -> background(() -> new Twinslot(this).rebootRecovery(),
                        e -> SwitchFlow.toast(this, "Could not reboot: " + e)))
                .setNegativeButton(android.R.string.cancel, null)));
        content.addView(reboot, ui.top(16));
        Button done = ui.button("Done", Ui.Style.OUTLINED, 0);
        done.setOnClickListener(v -> finish());
        content.addView(done, ui.top(10));
    }

    private void failed() {
        content.addView(ui.banner(R.drawable.ic_error, "The zip was not made", error,
                p.errorContainer, p.onErrorContainer), ui.top(16));
        Button again = ui.button("Try again", Ui.Style.TONAL, R.drawable.ic_reboot);
        again.setOnClickListener(v -> {
            state = State.PLAN;
            render();
        });
        content.addView(again, ui.top(14));
    }
}
