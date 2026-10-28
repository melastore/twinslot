package com.melastore.twinslot;

import android.Manifest;
import android.app.AlertDialog;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.IOException;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.List;

/** The main screen: the two Androids, switching, installing a ROM, and the storage split. */
public class MainActivity extends Screen {

    /** From the Quick Settings tile when the app lock is on: unlock, then offer the switch. */
    static final String ACTION_SWITCH = "com.melastore.twinslot.SWITCH";

    private static final int PICK_ZIP = 1;
    private static final int ASK_NOTIFICATIONS = 2;
    private static final long POLL_MS = 700;
    private static final int MIN_GIB = 8;
    /** Room for a carved twinmeta (32 to 64 MiB, aligned) when sizing the split. */
    private static final long META_ROOM = 256 * SettingsSize.MIB;
    /** Right after the start the job may not have written anything yet: it is starting, not gone. */
    private static final long JOB_GRACE_MS = 15000;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable poll = this::pollJob;
    private Twinslot twinslot;
    private Twinslot.Status status;
    private final JobText.Eta eta = new JobText.Eta();

    // Choices, kept across re-renders.
    private boolean optWipe;
    private boolean optVerify;
    private boolean optSwitch;
    private boolean optBootloader;
    private int setupGiB;
    private boolean editorOpen;
    private boolean logOpen;
    private long jobStartedAt;
    private boolean pendingSwitch;
    private boolean visible;
    /** The job service watches the running job. */
    private boolean watching;
    /** The last root answer noted in the diagnostic log, so each change is noted once. */
    private static Twinslot.Root lastRoot;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        twinslot = new Twinslot(this);
        optWipe = prefs.wipeByDefault();
        optVerify = prefs.verifyByDefault();
        optSwitch = prefs.switchByDefault();
        optBootloader = prefs.bootloaderByDefault();
        pendingSwitch = ACTION_SWITCH.equals(getIntent().getAction());
        // A rotation or a theme change recreates the screen: the choices made on it stay, whatever the defaults.
        if (savedInstanceState != null) {
            optWipe = savedInstanceState.getBoolean("optWipe", optWipe);
            optVerify = savedInstanceState.getBoolean("optVerify", optVerify);
            optSwitch = savedInstanceState.getBoolean("optSwitch", optSwitch);
            optBootloader = savedInstanceState.getBoolean("optBootloader", optBootloader);
            setupGiB = savedInstanceState.getInt("setupGiB");
            editorOpen = savedInstanceState.getBoolean("editorOpen");
            logOpen = savedInstanceState.getBoolean("logOpen");
            jobStartedAt = savedInstanceState.getLong("jobStartedAt");
            pendingSwitch = false;
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("optWipe", optWipe);
        out.putBoolean("optVerify", optVerify);
        out.putBoolean("optSwitch", optSwitch);
        out.putBoolean("optBootloader", optBootloader);
        out.putInt("setupGiB", setupGiB);
        out.putBoolean("editorOpen", editorOpen);
        out.putBoolean("logOpen", logOpen);
        out.putLong("jobStartedAt", jobStartedAt);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (ACTION_SWITCH.equals(intent.getAction())) {
            pendingSwitch = true;
        }
    }

    @Override
    void ready() {
        if (!prefs.disclaimerAccepted()) {
            showDisclaimer();
            return;
        }
        refresh();
    }

    // Visible, not resumed: a permission dialog pauses the screen while the progress stays in view behind it.
    @Override
    protected void onStart() {
        visible = true;
        super.onStart();
    }

    @Override
    protected void onStop() {
        super.onStop();
        visible = false;
        handler.removeCallbacks(poll);
    }

    // ---- loading ----

    private void refresh() {
        if (status == null) {
            showLoading("Reading both slots…");
        }
        background(() -> {
            Twinslot.Root root = Twinslot.checkRoot();
            if (root != lastRoot) {
                Diagnostics.note(this, "root: " + root);
                lastRoot = root;
            }
            if (root != Twinslot.Root.GRANTED) {
                DeviceCheck check = DeviceCheck.run();
                DeviceCheck.RootManager manager = DeviceCheck.installedManager(this);
                runOnUiThread(() -> showNoRoot(root, check, manager));
                return;
            }
            Twinslot.Status st = twinslot.status();
            Twinslot.Job job = twinslot.poll();
            runOnUiThread(() -> {
                status = st;
                if (job.running || job.finished || job.started) {
                    renderJob(job);
                } else {
                    render(st);
                }
            });
        }, error -> {
            Diagnostics.note(this, "could not read the phone: " + error);
            showUnreadable(error);
        });
    }

    private void pollJob() {
        handler.removeCallbacks(poll);
        if (!visible) {
            return;
        }
        background(() -> {
            Twinslot.Job job = twinslot.poll();
            runOnUiThread(() -> renderJob(job));
        }, error -> showError("Lost track of the job", error + "\n\nThe job itself goes on; open TwinSlot "
                + "again in a moment."));
    }

    // ---- simple screens ----

    private void header() {
        LinearLayout row = ui.row();
        TextView logo = new TextView(this);
        logo.setBackground(ui.shape(p.primaryContainer, 16));
        logo.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_twinslot, p.onPrimaryContainer, 26), null, null,
                null);
        logo.setGravity(Gravity.CENTER);
        logo.setPadding(ui.dp(11), 0, 0, 0);
        row.addView(logo, new LinearLayout.LayoutParams(ui.dp(48), ui.dp(48)));
        LinearLayout texts = ui.column();
        texts.setPadding(ui.dp(14), 0, 0, 0);
        texts.addView(ui.headline("TwinSlot", 26, p.text));
        texts.addView(ui.body("Two Androids, one phone"));
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        ImageButton guide = ui.iconButton(R.drawable.ic_guide, p.textDim, "Guide");
        guide.setOnClickListener(v -> startActivity(new Intent(this, GuideActivity.class)));
        row.addView(guide);
        ImageButton settings = ui.iconButton(R.drawable.ic_settings, p.textDim, "Settings");
        settings.setOnClickListener(v -> startActivity(new Intent(this, SettingsActivity.class)));
        row.addView(settings);
        row.setPadding(ui.dp(4), ui.dp(8), 0, 0);
        content.addView(row);
    }

    /**
     * The first open: TwinSlot is alpha and can cost the user's data or a bootable phone. Nothing else
     * (not even the root request) happens until they tick the box and continue.
     */
    private void showDisclaimer() {
        content.removeAllViews();
        header();

        LinearLayout hero = ui.card(p.surface);
        TextView badge = ui.medium("ALPHA", 12, p.onErrorContainer);
        badge.setBackground(ui.shape(p.errorContainer, 100));
        badge.setPadding(ui.dp(12), ui.dp(4), ui.dp(12), ui.dp(4));
        hero.addView(badge, ui.wrapTop(0));
        hero.addView(ui.headline("Read this first", 24, p.text), ui.top(14));
        hero.addView(ui.body("TwinSlot is alpha software: an early version that is still being tested. It is "
                + "tested on the Tecno CK7n (MediaTek). Other A/B MediaTek phones should work, but most of "
                + "them have not been tried yet."), ui.top(6));
        content.addView(hero, ui.top(16));

        content.addView(ui.banner(R.drawable.ic_warning, "It can erase your data or stop your phone from starting",
                "TwinSlot changes the partition table and writes the boot and system partitions. A bug, a wrong "
                        + "ROM, a flat battery or a cut cable in the middle of it can erase everything on the "
                        + "phone, or leave it stuck at the logo. Getting it back may need a computer and "
                        + "MediaTek flashing tools.", p.errorContainer, p.onErrorContainer), ui.top(12));
        content.addView(ui.banner(R.drawable.ic_info, "Before you use it",
                "• Back up everything you want to keep.\n"
                        + "• Keep a full ROM zip for this phone, and know how to flash it from scratch.\n"
                        + "• Charge the battery to at least half before any flash.",
                p.surfaceHigh, p.text), ui.top(10));

        LinearLayout terms = ui.card(p.surface);
        terms.addView(ui.medium("No warranty", 16, p.text));
        terms.addView(ui.body("TwinSlot is free software under the MIT License, provided “as is”, without "
                + "warranty of any kind. You use it at your own risk. The author is not responsible for any "
                + "damage to your phone, lost data, or any other loss that comes from using TwinSlot or from "
                + "not being able to use it."), ui.top(6));
        content.addView(terms, ui.top(10));

        RadioButton agree = new RadioButton(this);
        agree.setText("I understand. I use TwinSlot at my own risk, and I alone am responsible for my phone "
                + "and my data.");
        agree.setTextColor(p.text);
        agree.setTextSize(15);
        agree.setButtonTintList(android.content.res.ColorStateList.valueOf(p.primary));
        agree.setPadding(ui.dp(8), ui.dp(4), 0, ui.dp(4));
        content.addView(agree, ui.top(14));

        Button go = ui.button("Continue", Ui.Style.FILLED, R.drawable.ic_check);
        Ui.enable(go, false);
        agree.setOnCheckedChangeListener((b, on) -> Ui.enable(go, on));
        go.setOnClickListener(v -> {
            prefs.setDisclaimerAccepted();
            Diagnostics.note(this, "first-open warning accepted (version " + Prefs.DISCLAIMER_VERSION + ")");
            refresh();
        });
        content.addView(go, ui.top(12));
        Button close = ui.button("Close TwinSlot", Ui.Style.TEXT, 0);
        close.setOnClickListener(v -> finish());
        content.addView(close, ui.top(6));
    }

    private void showLoading(String text) {
        content.removeAllViews();
        header();
        LinearLayout card = ui.card(p.surface);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        card.setPadding(ui.dp(20), ui.dp(36), ui.dp(20), ui.dp(36));
        ProgressBar spin = new ProgressBar(this);
        spin.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(p.primary));
        card.addView(spin, new LinearLayout.LayoutParams(ui.dp(40), ui.dp(40)));
        TextView t = ui.body(text);
        t.setGravity(Gravity.CENTER);
        card.addView(t, ui.top(16));
        content.addView(card, ui.top(24));
    }

    private void showError(String title, String detail) {
        setJobRunning(false);
        content.removeAllViews();
        header();
        content.addView(ui.banner(R.drawable.ic_error, title, detail, p.errorContainer, p.onErrorContainer),
                ui.top(24));
        Button again = ui.button("Try again", Ui.Style.TONAL, R.drawable.ic_reboot);
        again.setOnClickListener(v -> {
            status = null;
            refresh();
        });
        content.addView(again, ui.top(16));
    }

    /** Root works but the engine could not read the phone: an unusual layout, most likely. */
    private void showUnreadable(String error) {
        showError("TwinSlot could not read this phone", error + "\n\nIf this phone is A/B with a super "
                + "partition, send a diagnostic log (Settings › Diagnostic log) so it can be supported.");
        Button settings = ui.button("Open settings", Ui.Style.TEXT, R.drawable.ic_settings);
        settings.setOnClickListener(v -> startActivity(new Intent(this, SettingsActivity.class)));
        content.addView(settings, ui.wrapTop(8));
    }

    /** No root yet: what root is for, how to allow it in the manager found, and whether the phone fits at all. */
    private void showNoRoot(Twinslot.Root root, DeviceCheck check, DeviceCheck.RootManager manager) {
        content.removeAllViews();
        header();

        LinearLayout hero = ui.card(check.capable ? p.surface : p.errorContainer);
        int fg = check.capable ? p.text : p.onErrorContainer;
        TextView badge = new TextView(this);
        badge.setBackground(ui.shape(check.capable ? p.primaryContainer : p.error, 100));
        badge.setCompoundDrawablesRelative(ui.icon(check.capable ? R.drawable.ic_shield : R.drawable.ic_error,
                check.capable ? p.onPrimaryContainer : p.errorContainer, 26), null, null, null);
        badge.setPadding(ui.dp(15), 0, 0, 0);
        badge.setGravity(Gravity.CENTER_VERTICAL);
        hero.addView(badge, new LinearLayout.LayoutParams(ui.dp(56), ui.dp(56)));
        String title;
        String body;
        if (!check.capable) {
            title = "This phone cannot dual boot";
            body = "TwinSlot puts a second Android in the second system slot of an A/B phone. The check below "
                    + "shows what this phone lacks; root would not change that.";
        } else if (root == Twinslot.Root.DENIED) {
            title = "Root access was refused";
            body = "TwinSlot asked for root and the root manager said no. Allow it there, then tap Try again.";
        } else {
            title = "Root access needed";
            body = "TwinSlot writes the phone's boot and system partitions, which only root may change. It "
                    + "asks for nothing else.";
        }
        hero.addView(ui.headline(title, 24, fg), ui.top(16));
        TextView b = ui.text(body, 14.5f, fg);
        b.setAlpha(0.9f);
        hero.addView(b, ui.top(6));
        content.addView(hero, ui.top(24));

        if (check.capable) {
            content.addView(ui.sectionTitle("How to allow it"), ui.top(24));
            LinearLayout how = ui.card(p.surface);
            if (manager != null) {
                how.addView(ui.medium(manager.name + " is installed", 15.5f, p.text));
                how.addView(ui.body(manager.howTo), ui.top(4));
                Intent launch = DeviceCheck.launch(this, manager);
                if (launch != null) {
                    Button open = ui.button("Open " + manager.name, Ui.Style.TONAL, R.drawable.ic_shield);
                    open.setOnClickListener(v -> {
                        AppLock.openingOwnPicker();
                        startActivity(launch);
                    });
                    how.addView(open, ui.top(14));
                }
            } else {
                how.addView(ui.medium("No root manager found", 15.5f, p.text));
                how.addView(ui.body("TwinSlot works with Magisk, KernelSU, APatch and their forks. With KernelSU "
                        + "and APatch, apps cannot see root until they are allowed, so open your manager's "
                        + "Superuser list and turn TwinSlot on. A manager installed under a hidden name works "
                        + "the same way."), ui.top(4));
            }
            content.addView(how);
        }

        Button again = ui.button("Try again", Ui.Style.FILLED, R.drawable.ic_reboot);
        again.setOnClickListener(v -> {
            status = null;
            refresh();
        });
        content.addView(again, ui.top(16));

        content.addView(ui.sectionTitle("This phone"), ui.top(24));
        LinearLayout list = ui.card(p.surface);
        list.setPadding(ui.dp(20), ui.dp(8), ui.dp(20), ui.dp(8));
        for (int i = 0; i < check.items.size(); i++) {
            if (i > 0) {
                list.addView(ui.divider());
            }
            list.addView(checkRow(check.items.get(i)));
        }
        content.addView(list);

        Button guide = ui.button("Read the guide", Ui.Style.TEXT, R.drawable.ic_guide);
        guide.setOnClickListener(v -> startActivity(new Intent(this, GuideActivity.class)));
        content.addView(guide, ui.wrapTop(12));
    }

    private View checkRow(DeviceCheck.Item item) {
        LinearLayout row = ui.row();
        row.setGravity(Gravity.TOP);
        row.setPadding(0, ui.dp(12), 0, ui.dp(12));
        int icon = item.state == DeviceCheck.State.OK ? R.drawable.ic_check
                : item.state == DeviceCheck.State.NOTE ? R.drawable.ic_info : R.drawable.ic_error;
        int color = item.state == DeviceCheck.State.OK ? p.good
                : item.state == DeviceCheck.State.NOTE ? p.textDim : p.error;
        android.widget.ImageView i = ui.iconView(icon, color, 20);
        ((LinearLayout.LayoutParams) i.getLayoutParams()).setMarginEnd(ui.dp(14));
        row.addView(i);
        LinearLayout texts = ui.column();
        texts.addView(ui.pair(ui.medium(item.label, 14.5f, p.text), ui.text(item.value, 13.5f, p.textDim)));
        if (item.note != null) {
            TextView n = ui.text(item.note, 13, item.state == DeviceCheck.State.PROBLEM ? p.error : p.textDim);
            texts.addView(n, ui.top(4));
        }
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    // ---- main screen ----

    private void render(Twinslot.Status st) {
        setJobRunning(false);
        content.removeAllViews();
        header();

        if (!prefs.guideOffered()) {
            LinearLayout intro = ui.banner(R.drawable.ic_guide, "New to TwinSlot?",
                    "The guide explains the three steps (split, install, switch) in two minutes.",
                    p.primaryContainer, p.onPrimaryContainer);
            LinearLayout buttons = ui.row();
            Button open = ui.button("Open the guide", Ui.Style.TEXT, 0);
            open.setTextColor(p.onPrimaryContainer);
            open.setOnClickListener(v -> startActivity(new Intent(this, GuideActivity.class)));
            Button later = ui.button("Not now", Ui.Style.TEXT, 0);
            later.setTextColor(p.onPrimaryContainer);
            later.setOnClickListener(v -> {
                prefs.setGuideOffered();
                render(status);
            });
            buttons.addView(open);
            buttons.addView(later);
            ((LinearLayout) intro.getChildAt(1)).addView(buttons, ui.wrapTop(6));
            content.addView(intro, ui.top(20));
        }

        int cur = st.current;
        if (cur >= 0) {
            content.addView(runningCard(st, cur), ui.top(24));
            content.addView(otherCard(st, 1 - cur, true), ui.top(12));
        } else {
            content.addView(ui.banner(R.drawable.ic_warning, "Cannot tell which slot is running", null,
                    p.warnContainer, p.onWarnContainer), ui.top(24));
            for (int i = 0; i < 2; i++) {
                content.addView(otherCard(st, i, false), ui.top(12));
            }
        }

        if (st.mergePending()) {
            content.addView(ui.banner(R.drawable.ic_warning, "A system update is still merging",
                    "Keep using this Android until it finishes (" + st.merge + "). Switching or installing now "
                            + "would lose the update.", p.warnContainer, p.onWarnContainer), ui.top(12));
        }

        // Before the split, splitting is step 1: it comes first.
        if (!st.dualReady()) {
            content.addView(ui.sectionTitle("Storage split"), ui.top(28));
            content.addView(storageCard(st));
        }
        content.addView(ui.sectionTitle("Install a ROM"), ui.top(28));
        content.addView(installCard(st));
        if (st.dualReady()) {
            content.addView(ui.sectionTitle("Storage split"), ui.top(28));
            content.addView(storageCard(st));
        }

        content.addView(ui.banner(R.drawable.ic_info, null, "Keep system updates (OTA) off on both Androids: an "
                + "update writes into the other slot.", p.surface, p.textDim), ui.top(28));
        String version = appVersion(this);
        // The engine ships inside the app, so the two only differ if the engine came from elsewhere.
        TextView foot = ui.text("TwinSlot " + version + (st.version.equals(version) ? "" : " · engine " + st.version),
                12, p.textDim);
        foot.setGravity(Gravity.CENTER);
        foot.setAlpha(0.7f);
        content.addView(foot, ui.top(16));

        if (pendingSwitch) {
            pendingSwitch = false;
            SwitchFlow.confirm(this, st, this::present);
        }
    }

    /** The Android in use right now, with its own storage as Android sees it. */
    private View runningCard(Twinslot.Status st, int i) {
        Twinslot.Slot s = st.slots[i];
        int fg = p.onSlotContainer(i);
        LinearLayout card = ui.card(p.slotContainer(i));
        card.addView(slotHead(st, i, "Running now", p.slot(i), p.onSlot(i), fg));

        LinearLayout pills = ui.row();
        pills.addView(ui.pill(s.successful ? "Boots fine" : "First boot", Palette.alpha(fg, 0.12f), fg,
                R.drawable.ic_check));
        pills.addView(spacer(6));
        pills.addView(ui.pill(dataName(s), Palette.alpha(fg, 0.12f), fg, R.drawable.ic_storage));
        card.addView(pills, ui.top(14));

        // Usage of the mounted /data, as an app sees it.
        StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
        long total = fs.getTotalBytes();
        long free = fs.getAvailableBytes();
        long used = Math.max(0, total - free);
        Ui.Bar bar = ui.bar(Palette.alpha(fg, 0.14f), 10, 0);
        bar.set(new float[] {total > 0 ? (float) used / total : 0}, new int[] {p.slot(i)});
        card.addView(ui.pair(ui.medium("Storage", 14, fg),
                ui.text(SettingsSize.format(used) + " used of " + SettingsSize.format(total), 13.5f, fg)),
                ui.top(18));
        card.addView(bar, ui.top(8));

        long data = st.dataMib(i) * SettingsSize.MIB;
        long system = systemBytes(s);
        if (data > 0) {
            long shown = SettingsSize.total(data, system);
            TextView t = ui.text(SettingsSize.wrong(data, system) ? "Settings › Storage rounds this to "
                    + SettingsSize.format(shown) + " and reads “used” too low" : settingsLine(data, system), 12.5f, fg);
            t.setAlpha(0.7f);
            card.addView(t, ui.top(8));
        }
        return card;
    }

    /**
     * Factory reset of the other slot, offered on its card whatever state it is in: the engine erases that
     * slot's own data partitions. The running Android has none of this: its data is mounted.
     */
    private void addFactoryReset(LinearLayout card, Twinslot.Status st, int i) {
        Button reset = ui.button("Factory reset slot " + upper(i), Ui.Style.DANGER_OUTLINED, R.drawable.ic_delete);
        reset.setOnClickListener(v -> confirmWipe(st, i));
        Ui.enable(reset, st.dualReady());
        card.addView(reset, ui.top(st.slots[i].isInstalled() ? 10 : 16));
        if (!st.dualReady()) {
            TextView why = ui.text("Needs the storage split: without it slot " + upper(i) + " uses this "
                    + "Android's data.", 12.5f, p.textDim);
            card.addView(why, ui.top(6));
        }
    }

    private void confirmWipe(Twinslot.Status st, int i) {
        Twinslot.Slot s = st.slots[i];
        show(new AlertDialog.Builder(this)
                .setTitle("Factory reset slot " + upper(i) + "?")
                .setMessage((s.isInstalled() ? s.title(i) + " starts like after a factory reset: its apps, "
                        + "accounts, settings and internal storage are erased. The ROM itself stays installed."
                        : "Slot " + upper(i) + "'s data is erased, so the next ROM there starts clean.")
                        + "\n\nThis Android is not touched.")
                .setPositiveButton("Erase", (d, w) -> runQuick("Factory resetting slot " + upper(i) + "…",
                        () -> twinslot.wipe(i)))
                .setNegativeButton(android.R.string.cancel, null));
    }

    /** The other Android: what it is and the way to boot it. */
    private View otherCard(Twinslot.Status st, int i, boolean withSwitch) {
        Twinslot.Slot s = st.slots[i];
        LinearLayout card = ui.card(p.surface);
        String label = i == st.next && i != st.current ? "Next boot" : withSwitch ? "Other Android" : "";
        card.addView(slotHead(st, i, label, p.slotContainer(i), p.onSlotContainer(i), p.text));

        if (!s.isInstalled()) {
            card.addView(ui.body("Nothing is installed in this slot. Install a ROM below."), ui.top(10));
            addFactoryReset(card, st, i);
            return card;
        }

        // Without the split this is whatever the slot held before (an older build, or a ROM a remove left
        // behind): it shares this Android's data, so it is shown as switched off, not as broken.
        boolean shared = !st.dualReady();
        // Half written, or failed to start: only an install brings it back, so which data it names is moot.
        // Parked (what the split leaves a slot that held a ROM before) is not this: its images are whole,
        // so pointing it at its own data is enough, and it then starts like after a factory reset.
        boolean off = !shared && !s.usable();
        LinearLayout pills = ui.row();
        if (!s.usable() && shared) {
            pills.addView(ui.pill("Switched off", p.surfaceHigh, p.text, R.drawable.ic_info));
        } else if (!s.usable()) {
            pills.addView(ui.pill("Not bootable", p.errorContainer, p.onErrorContainer, R.drawable.ic_error));
        } else if (s.successful) {
            pills.addView(ui.pill("Boots fine", p.surfaceHigh, p.text, R.drawable.ic_check));
        } else {
            // Bootable but never started: the bootloader's try counter is no news to anyone.
            pills.addView(ui.pill("Not started yet", p.surfaceHigh, p.text, R.drawable.ic_info));
        }
        pills.addView(spacer(6));
        long data = st.dataMib(i) * SettingsSize.MIB;
        pills.addView(ui.pill(shared ? "Shares this data" : dataName(s)
                + (data > 0 && s.dataOk() ? " · " + SettingsSize.format(data) : ""), p.surfaceHigh, p.text,
                R.drawable.ic_storage));
        card.addView(pills, ui.top(14));
        if (shared) {
            card.addView(ui.body("Switched off so it cannot touch this Android's files: without the storage split "
                    + "all the data belongs to this Android. You can ignore this slot, or split the storage to "
                    + "dual-boot again."), ui.top(12));
        } else if (off) {
            card.addView(ui.body("An install did not finish here, or this Android failed to start several times "
                    + "in a row. Install a ROM into slot " + upper(i) + " again to use it."), ui.top(12));
        }

        if (!shared && !off && s.dataKnown() && !s.dataOk()) {
            card.addView(ui.banner(R.drawable.ic_error, "Pointed at the wrong data",
                    "It must use " + ("twin".equals(s.expected) ? "its own data (twindata)" : "the main data (userdata)")
                            + ", or it would boot on the other Android's data.",
                    p.errorContainer, p.onErrorContainer), ui.top(14));
            if (i != st.current) {
                Button fix = ui.button("Fix: point it at " + ("twin".equals(s.expected) ? "its own data"
                        : "the main data"), Ui.Style.TONAL, R.drawable.ic_wrench);
                fix.setOnClickListener(v -> runQuick("Pointing slot " + upper(i) + " at its data…",
                        () -> twinslot.rebind(i)));
                card.addView(fix, ui.top(12));
            }
        }

        if (withSwitch && !shared) {
            boolean ready = st.canSwitch();
            Button go = ui.button("Reboot to " + s.title(i), Ui.Style.FILLED, R.drawable.ic_reboot);
            Ui.enable(go, ready);
            go.setOnClickListener(v -> SwitchFlow.confirm(this, status, this::present));
            card.addView(go, ui.top(18));
        }
        addFactoryReset(card, st, i);
        return card;
    }

    /** Badge, small label and the ROM's name with its version line. */
    private View slotHead(Twinslot.Status st, int i, String label, int badgeBg, int badgeFg, int fg) {
        Twinslot.Slot s = st.slots[i];
        LinearLayout row = ui.row();
        row.setGravity(Gravity.TOP);
        row.addView(ui.badge(upper(i), badgeBg, badgeFg, 48));
        LinearLayout texts = ui.column();
        texts.setPadding(ui.dp(14), 0, 0, 0);
        TextView over = ui.overline("Slot " + upper(i) + (label.isEmpty() ? "" : " · " + label), fg);
        over.setAlpha(0.75f);
        texts.addView(over);
        texts.addView(ui.headline(s.isInstalled() ? s.title(i) : "Empty", 21, fg), ui.top(2));
        StringBuilder detail = new StringBuilder();
        if (!s.android.isEmpty()) {
            detail.append("Android ").append(s.android);
        }
        if (s.buildUtc > 0) {
            detail.append(detail.length() > 0 ? " · built " : "Built ")
                    .append(DateFormat.getDateInstance(DateFormat.MEDIUM).format(new Date(s.buildUtc * 1000)));
        }
        if (detail.length() > 0) {
            TextView d = ui.text(detail, 13.5f, fg);
            d.setAlpha(0.8f);
            texts.addView(d, ui.top(2));
        }
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        if (s.isInstalled()) {
            ImageButton rename = ui.iconButton(R.drawable.ic_edit, fg, "Rename " + s.title(i));
            rename.setOnClickListener(v -> rename(st, i));
            row.addView(rename);
        }
        return row;
    }

    /** The user's own name for a ROM. Kept in this Android's settings: the other Android keeps its own. */
    private void rename(Twinslot.Status st, int i) {
        Twinslot.Slot s = st.slots[i];
        String own = s.rom.isEmpty() ? "Android in slot " + upper(i) : s.rom;
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(40)});
        input.setText(s.title(i));
        input.setSelectAllOnFocus(true);
        input.setHint(own);
        FrameLayout box = new FrameLayout(this);
        box.setPadding(ui.dp(24), ui.dp(8), ui.dp(24), 0);
        box.addView(input);
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle("Rename slot " + upper(i))
                .setView(box)
                .setPositiveButton("Save", (d, w) -> setRomName(st, i, input.getText().toString()))
                .setNegativeButton(android.R.string.cancel, null);
        if (!s.customName.isEmpty()) {
            b.setNeutralButton("Use “" + own + "”", (d, w) -> setRomName(st, i, ""));
        }
        AlertDialog dialog = show(b);
        input.requestFocus();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        }
    }

    private void setRomName(Twinslot.Status st, int i, String name) {
        Twinslot.Slot s = st.slots[i];
        String n = name.trim();
        // Typing the ROM's own name back is the same as no name of one's own.
        new Prefs(this).setRomName(i, s.rom, n.equals(s.rom) ? "" : n);
        s.customName = n.equals(s.rom) ? "" : n;
        render(st);
    }

    private static String dataName(Twinslot.Slot s) {
        return "twin".equals(s.data) ? "Own data" : "primary".equals(s.data) ? "Main data" : "Data: " + s.data;
    }

    /** twinslot-setup-121GB.zip: the size the screen shows (decimal GB), not the GiB the engine is given. */
    private String zipName(String kind) {
        return "twinslot-" + kind + "-" + SettingsSize.format(setupGiB * SettingsSize.GIB).replace(" ", "") + ".zip";
    }

    // ---- install ----

    private View installCard(Twinslot.Status st) {
        int target = st.other();
        LinearLayout card = ui.card(p.surface);
        if (target < 0) {
            card.addView(ui.body("Cannot tell which slot is running."));
            return card;
        }
        char t = upper(target);
        card.addView(ui.body("Pick a full ROM zip made for this phone (usually 1.5 to 4 GB, with payload.bin "
                + "inside). It goes into slot " + t + "; the Android you use now is not touched."));
        if (!st.dualReady()) {
            card.addView(ui.banner(R.drawable.ic_info, "Split the storage first",
                    "Two different ROMs cannot share one /data. Set up the split above.",
                    p.surfaceHigh, p.text), ui.top(14));
            return card;
        }
        LinearLayout opts = ui.column();
        // An empty slot has no data to keep: the choice would only be a trap.
        if (st.slots[target].isInstalled()) {
            opts.addView(ui.switchRow("Erase slot " + t + "'s data", "Keep on when the ROM changes. Turn off only "
                    + "to update the ROM already in slot " + t + " to a newer build.", optWipe, v -> optWipe = v));
            opts.addView(ui.divider());
        }
        opts.addView(ui.switchRow("Check the zip first", "Reads the whole zip before writing (a few minutes).",
                optVerify, v -> optVerify = v));
        opts.addView(ui.divider());
        opts.addView(ui.switchRow("Boot it next", "Slot " + t + " starts on the next reboot.", optSwitch,
                v -> optSwitch = v));
        opts.addView(ui.divider());
        opts.addView(ui.switchRow("Write the zip's bootloader", SettingsActivity.BOOTLOADER_NOTE, optBootloader,
                v -> optBootloader = v));
        card.addView(opts, ui.top(8));
        Button pick = ui.button("Choose ROM zip", Ui.Style.TONAL, R.drawable.ic_download);
        Ui.enable(pick, !st.mergePending());
        pick.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            AppLock.openingOwnPicker();
            startActivityForResult(i, PICK_ZIP);
        });
        card.addView(pick, ui.top(12));
        return card;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ZIP || resultCode != RESULT_OK || data == null || status == null) {
            return;
        }
        Uri uri = data.getData();
        background(() -> {
            String path = pathOf(uri);
            runOnUiThread(() -> confirmInstall(path));
        }, error -> showError("Cannot read that zip", error));
    }

    private void confirmInstall(String path) {
        if (path == null) {
            show(new AlertDialog.Builder(this)
                    .setTitle("Pick a file on the phone")
                    .setMessage("TwinSlot reads the zip as root, so it needs a file on internal storage "
                            + "(Download, or any folder there) or on an SD card or USB stick, not a cloud or app "
                            + "provider.")
                    .setPositiveButton(android.R.string.ok, null));
            return;
        }
        int target = status.other();
        Twinslot.Slot s = status.slots[target];
        String name = path.substring(path.lastIndexOf('/') + 1);
        String msg = name + "\n\ngoes into slot " + upper(target)
                + (s.isInstalled() ? ", replacing " + s.title(target) : "") + "."
                + (wipes(target) ? "\n\nThat slot's data is erased." : "\n\nThat slot's data is kept (same ROM, "
                + "newer build only).")
                + (optBootloader ? "\n\nThe zip's lk is written into slot " + upper(target) + ". If it was not "
                + "made for this model, the phone stays at the logo until it is flashed over USB." : "")
                + "\n\nThe Android you use now is not touched.";
        show(new AlertDialog.Builder(this)
                .setTitle("Install?")
                .setMessage(msg)
                .setPositiveButton("Install", (d, w) -> startInstall(path, target))
                .setNegativeButton(android.R.string.cancel, null));
    }

    /** An empty slot always starts with empty data; the switch is offered only over an installed ROM. */
    private boolean wipes(int target) {
        return optWipe || status == null || !status.slots[target].isInstalled();
    }

    private void startInstall(String zip, int target) {
        List<String[]> commands = new ArrayList<>();
        if (optVerify) {
            commands.add(new String[] {"verify", zip});
        }
        List<String> install = new ArrayList<>(Arrays.asList(
                "install", zip, "--slot", String.valueOf(Twinslot.letter(target))));
        if (wipes(target)) {
            install.add("--wipe");
        }
        if (!optSwitch) {
            install.add("--no-switch");
        }
        if (optBootloader) {
            install.add("--bootloader");
        }
        commands.add(install.toArray(new String[0]));
        String name = zip.substring(zip.lastIndexOf('/') + 1).replaceFirst("(?i)\\.zip$", "");
        prefs.setJob("Installing " + name + " into slot " + upper(target), zip, target);
        askForNotifications();
        logOpen = false;
        jobStartedAt = System.currentTimeMillis();
        // The progress screen comes up at once; the job fills it in.
        renderJob(new Twinslot.Job());
        Diagnostics.note(this, "job start: " + (optVerify ? "verify, " : "") + "install " + name + ".zip into slot "
                + Twinslot.letter(target) + (wipes(target) ? ", wipe" : "") + (optSwitch ? "" : ", no switch")
                + (optBootloader ? ", bootloader" : "")
                + ", stay awake " + prefs.stayAwake());
        background(() -> {
            twinslot.start(commands, prefs.stayAwake());
            runOnUiThread(() -> {
                watching = true;
                JobService.start(this);
                pollJob();
            });
        }, error -> showError("Could not start", error));
    }

    /** The progress and result notifications need this permission on Android 13 and later; asked once. */
    private void askForNotifications() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !prefs.askedNotifications()
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            prefs.setAskedNotifications();
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, ASK_NOTIFICATIONS);
        }
    }

    private void renderJob(Twinslot.Job job) {
        if (!job.started && !job.finished && System.currentTimeMillis() - jobStartedAt < JOB_GRACE_MS) {
            job.running = true;
        }
        setJobRunning(job.running);
        content.removeAllViews();
        header();
        boolean ok = !job.running && job.code == 0;
        boolean interrupted = !job.running && !job.finished;
        boolean checking = job.step.startsWith("checking");
        int bg = job.running ? p.surface : ok ? p.primaryContainer : interrupted ? p.warnContainer : p.errorContainer;
        int fg = job.running ? p.text : ok ? p.onPrimaryContainer : interrupted ? p.onWarnContainer : p.onErrorContainer;
        LinearLayout card = ui.card(bg);
        int slot = prefs.jobSlot();
        String slotName = slot >= 0 ? "slot " + upper(slot) : "the other slot";
        // The title names the zip, which is long and must not be upper-cased: the overline carries only the
        // slot, the zip goes on its own line below.
        TextView over = ui.overline((slot >= 0 ? "Slot " + upper(slot) + " · " : "") + "Install", fg);
        over.setAlpha(0.8f);
        card.addView(over);
        String zip = prefs.jobZip();
        if (zip != null && !zip.isEmpty()) {
            TextView file = ui.text(zip.substring(zip.lastIndexOf('/') + 1), 13, fg);
            file.setAlpha(0.75f);
            file.setMaxLines(2);
            file.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            card.addView(file, ui.top(2));
        }
        if (job.running) {
            eta.sample(job.step, job.permille);
            String phase = JobText.phase(job.step);
            card.addView(ui.headline(phase, 26, fg), ui.top(4));
            if (job.checks()) {
                TextView stepOf = ui.text((checking ? "Step 1 of 2" : "Step 2 of 2"), 13, p.textDim);
                card.addView(stepOf, ui.top(2));
            }
            LinearLayout nums = ui.row();
            nums.setGravity(Gravity.BOTTOM);
            nums.addView(ui.headline(job.started ? job.permille / 10 + "%" : "…", 44, p.primary));
            String etaText = eta.text();
            TextView left = ui.body(etaText == null ? "" : etaText);
            left.setGravity(Gravity.END);
            left.setPadding(ui.dp(12), 0, 0, ui.dp(8));
            nums.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            card.addView(nums, ui.top(12));
            Ui.Bar bar = ui.bar(p.surfaceHigh, 10, 0);
            bar.set(new float[] {job.permille / 1000f}, new int[] {p.primary});
            card.addView(bar, ui.top(8));
            card.addView(ui.text(JobText.detail(job.step), 14, fg), ui.top(12));
            card.addView(ui.banner(R.drawable.ic_info, null, "You can leave TwinSlot: the notification shows how it "
                    + "goes. Do not restart the phone until it is done.", p.surfaceHigh, p.text), ui.top(16));
        } else if (ok) {
            card.addView(ui.headline(job.installed() ? "Installed" : "Done", 26, fg), ui.top(4));
            card.addView(ui.text(job.installed() ? "Every partition was read back and matches the ROM. "
                    + (lastLineHas(job, "boots next") ? "It starts on the next reboot."
                    : "Switch to it whenever you like.") : lastLine(job), 14.5f, fg), ui.top(8));
        } else if (interrupted) {
            card.addView(ui.headline("Install interrupted", 26, fg), ui.top(4));
            card.addView(ui.text("It stopped before it could finish: the phone restarted, or the job was closed. "
                    + (wrote(job) ? "TwinSlot had started writing, so " + slotName + " is switched off and "
                    + "cannot boot half a ROM. " : "Nothing had been written yet. ")
                    + "The Android you use now is fine. Install it again to finish.", 14.5f, fg), ui.top(8));
        } else {
            card.addView(ui.headline(checking ? "The zip did not pass the check" : "Install failed", 26, fg),
                    ui.top(4));
            String why = JobText.explain(job.lines);
            card.addView(ui.text(why != null ? why : lastLine(job), 14.5f, fg), ui.top(8));
            TextView safe = ui.text(wrote(job) ? capitalize(slotName) + " is switched off, so the phone never boots "
                    + "half a ROM. The Android you use now was not touched."
                    : "Nothing was written. Both Androids are as they were.", 13.5f, fg);
            safe.setAlpha(0.85f);
            card.addView(safe, ui.top(10));
        }
        content.addView(card, ui.top(24));

        if (!job.lines.isEmpty()) {
            LinearLayout logButtons = ui.row();
            Button toggle = ui.button(logOpen ? "Hide the details" : "Show the details", Ui.Style.TEXT,
                    logOpen ? R.drawable.ic_collapse : R.drawable.ic_expand);
            toggle.setOnClickListener(v -> {
                logOpen = !logOpen;
                renderJob(job);
            });
            logButtons.addView(toggle);
            if (!job.running && !ok) {
                Button share = ui.button("Share the log", Ui.Style.TEXT, R.drawable.ic_share);
                share.setOnClickListener(v -> startActivity(new Intent(this, DiagnosticsActivity.class)));
                logButtons.addView(share);
            }
            content.addView(logButtons, ui.wrapTop(8));
            if (logOpen) {
                List<String> lines = job.lines;
                StringBuilder log = new StringBuilder();
                for (int i = Math.max(0, lines.size() - 200); i < lines.size(); i++) {
                    log.append(lines.get(i)).append('\n');
                }
                TextView logView = ui.mono(log.toString().trim(), p.textDim);
                logView.setTextIsSelectable(true);
                logView.setBackground(ui.shape(p.surface, 20));
                logView.setPadding(ui.dp(16), ui.dp(14), ui.dp(16), ui.dp(14));
                content.addView(logView, ui.top(4));
            }
        }

        if (job.running) {
            if (job.started && !watching) {
                // Found running when the app opened (it was closed meanwhile): the notification and the
                // wake lock come back with the watch.
                watching = true;
                JobService.start(this);
            }
            handler.removeCallbacks(poll);
            handler.postDelayed(poll, POLL_MS);
            return;
        }
        watching = false;
        // The result is on screen: its notification has nothing more to say.
        getSystemService(NotificationManager.class).cancel(JobService.ID_RESULT);
        // Only when the new ROM boots next: after "Boot it next" was off, a reboot just comes back here.
        if (ok && job.installed() && lastLineHas(job, "boots next")) {
            Button reboot = ui.button("Reboot into slot " + (slot >= 0 ? upper(slot) : '?') + " now", Ui.Style.FILLED,
                    R.drawable.ic_reboot);
            // The job is done with: back in this Android later, TwinSlot opens on its main screen.
            reboot.setOnClickListener(v -> background(() -> {
                twinslot.clearJob();
                twinslot.reboot();
            }, error -> showError("Could not reboot", error)));
            content.addView(reboot, ui.top(16));
        }
        if (ok && job.installed()) {
            // The new Android starts without root or TwinSlot: this zip is the way back until it has them.
            Button switchBack = ui.button("Make a zip to switch back", Ui.Style.TONAL, R.drawable.ic_download);
            switchBack.setOnClickListener(v -> ZipActivity.open(this, "twinslot-switch-other.zip", "Switch back",
                    "Flashed in recovery, it makes the other Android start next. Slot "
                            + (slot >= 0 ? upper(slot) : '?') + " starts without root or TwinSlot: keep this zip on "
                            + "an SD card or a computer to get back before setting them up there.", false,
                    Collections.singletonList(new String[] {"switch", "other"})));
            content.addView(switchBack, ui.top(10));
        }
        if (!ok && zip != null && slot >= 0 && status != null && status.other() == slot) {
            Button again = ui.button("Install again", Ui.Style.FILLED, R.drawable.ic_download);
            again.setOnClickListener(v -> background(() -> {
                twinslot.clearJob();
                runOnUiThread(() -> confirmInstall(zip));
            }, error -> showError("Could not clear the job", error)));
            content.addView(again, ui.top(16));
        }
        Button back = ui.button("Back", Ui.Style.OUTLINED, 0);
        back.setOnClickListener(v -> background(() -> {
            twinslot.clearJob();
            runOnUiThread(this::refresh);
        }, error -> showError("Could not clear the job", error)));
        content.addView(back, ui.top(10));
    }

    /**
     * The job got as far as changing the slot: the last step is past checking the zip and planning, the
     * points where the engine has written nothing yet.
     */
    private static boolean wrote(Twinslot.Job job) {
        String s = job.step;
        return !(s.isEmpty() || s.startsWith("checking") || s.startsWith("reading the ROM zip"));
    }

    private static String lastLine(Twinslot.Job job) {
        return job.lines.isEmpty() ? (job.code >= 0 ? "Exit code " + job.code : "") : job.lines.get(job.lines.size() - 1);
    }

    private static boolean lastLineHas(Twinslot.Job job, String s) {
        for (String l : job.lines) {
            if (l.startsWith("installed:") && l.contains(s)) {
                return true;
            }
        }
        return false;
    }

    // ---- storage split ----

    private View storageCard(Twinslot.Status st) {
        LinearLayout card = ui.card(p.surface);
        long pool = st.poolMib() * SettingsSize.MIB;
        if (st.dualReady()) {
            int owner = st.owner;
            Ui.Bar split = ui.bar(p.surfaceHigh, 14, 4);
            long[] data = new long[2];
            for (int i = 0; i < 2; i++) {
                data[i] = st.dataMib(i) * SettingsSize.MIB;
            }
            split.set(new float[] {(float) data[0] / pool, (float) data[1] / pool},
                    new int[] {p.slot(0), p.slot(1)});
            card.addView(split);
            List<String> wrongOn = new ArrayList<>();
            for (int i = 0; i < 2; i++) {
                Twinslot.Slot s = st.slots[i];
                long sys = systemBytes(s);
                card.addView(legend(i, (s.isInstalled() ? s.title(i) : "Empty") + (i == owner ? " · own data"
                        : " · main data"), data[i], sys), ui.top(i == 0 ? 16 : 10));
                if (SettingsSize.wrong(data[i], sys)) {
                    wrongOn.add(s.isInstalled() ? s.title(i) : "slot " + upper(i));
                }
            }
            if (setupGiB == 0) {
                setupGiB = (int) (st.twinDataMib / 1024);
            }
            if (!wrongOn.isEmpty()) {
                int best = bestGiB(st, owner, pool);
                if (best > 0 && !editorOpen) {
                    Button show = ui.button("Fix: " + SettingsSize.format(best * SettingsSize.GIB) + " / "
                            + SettingsSize.format(pool - best * SettingsSize.GIB) + " shows right on both",
                            Ui.Style.TEXT, R.drawable.ic_split);
                    show.setOnClickListener(v -> {
                        editorOpen = true;
                        setupGiB = best;
                        render(status);
                    });
                    card.addView(show, ui.wrapTop(8));
                }
            }

            Button toggle = ui.button(editorOpen ? "Hide" : "Change or remove the split", Ui.Style.TEXT,
                    editorOpen ? R.drawable.ic_collapse : R.drawable.ic_expand);
            toggle.setOnClickListener(v -> {
                editorOpen = !editorOpen;
                render(status);
            });
            card.addView(toggle, ui.wrapTop(wrongOn.isEmpty() ? 12 : 0));
            if (!editorOpen) {
                return card;
            }
            card.addView(ui.divider(), ui.top(8));
            card.addView(ui.medium("New split", 16, p.text), ui.top(16));
            card.addView(ui.body("Each Android's data is encrypted block by block where it lies, so the split "
                    + "cannot move without erasing BOTH. Both ROMs stay installed and start empty."), ui.top(4));
            Button resize = ui.button("Make the resize zip", Ui.Style.DANGER, R.drawable.ic_split);
            addSplitEditor(card, st, owner, pool, resize);
            resize.setOnClickListener(v -> ZipActivity.open(this, zipName("resize"),
                    "Resize the split", "Slot " + upper(owner) + " gets " + SettingsSize.format(setupGiB
                            * SettingsSize.GIB) + " of its own, slot " + upper(1 - owner) + " the rest. Both ROMs "
                            + "stay installed and start with empty data.", true,
                    concat(Collections.singletonList(new String[] {"remove", "--apply"}),
                            setupCommands(owner, st.current, st.slots[owner].isInstalled()
                                    && st.slots[owner].bootable))));
            card.addView(resize, ui.top(14));

            card.addView(ui.divider(), ui.top(20));
            card.addView(ui.medium("Remove the split", 16, p.text), ui.top(16));
            card.addView(ui.body("Gives all the space back to slot " + upper(1 - owner) + ". Slot " + upper(owner)
                    + " stops booting until a ROM is installed again."), ui.top(4));
            Button remove = ui.button("Make the remove zip", Ui.Style.DANGER_OUTLINED, R.drawable.ic_delete);
            remove.setOnClickListener(v -> ZipActivity.open(this, "twinslot-remove.zip", "Remove the split",
                    "All the space goes back to the main data, for slot " + upper(1 - owner) + ". Slot "
                            + upper(owner) + " stops booting until a ROM is installed there again.", true,
                    Collections.singletonList(new String[] {"remove", "--apply"})));
            card.addView(remove, ui.top(12));
            return card;
        }
        if (!"off".equals(st.dualData)) {
            card.addView(ui.body("Cannot read the partition table."));
            return card;
        }
        int other = st.other();
        card.addView(ui.body("Splits the main data partition so the other Android gets a /data of its own."));
        String[] steps = {"Back up: both Androids start empty afterwards (their ROMs stay installed).",
            "Pick the size below and make the setup zip.",
            "Flash it in recovery by ADB sideload (or from an SD card / USB stick).",
            "Reboot, then install a ROM into the other slot here."};
        for (int n = 0; n < steps.length; n++) {
            card.addView(ui.step(n + 1, steps[n]), ui.top(n == 0 ? 14 : 8));
        }
        if (other < 0 || st.current < 0) {
            return card;
        }
        if (setupGiB == 0) {
            int best = bestGiB(st, other, pool);
            setupGiB = best > 0 ? best : (int) Math.max(MIN_GIB, pool / SettingsSize.GIB / 2);
        }
        Button make = ui.button("Make the setup zip", Ui.Style.FILLED, R.drawable.ic_split);
        card.addView(ui.divider(), ui.top(16));
        addSplitEditor(card, st, other, pool, make);
        make.setOnClickListener(v -> ZipActivity.open(this, zipName("setup"),
                "Set up the split", "Slot " + upper(other) + " gets " + SettingsSize.format(setupGiB
                        * SettingsSize.GIB) + " of its own data; this Android keeps the rest. Then install a ROM "
                        + "into slot " + upper(other) + " from TwinSlot.", true, true,
                setupCommands(other, st.current, false)));
        card.addView(make, ui.top(14));
        return card;
    }

    /** One side of the split: color, name, size and what Settings will make of it. */
    private View legend(int slot, String name, long data, long system) {
        LinearLayout col = ui.column();
        LinearLayout row = ui.row();
        row.addView(ui.dot(p.slot(slot)));
        TextView title = ui.medium(upper(slot) + "  " + name, 14.5f, p.text);
        title.setSingleLine(true);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        lp.setMarginEnd(ui.dp(12));
        row.addView(title, lp);
        row.addView(ui.medium(SettingsSize.format(data), 14.5f, p.text));
        col.addView(row);
        col.addView(settingsCheck(data, system));
        return col;
    }

    /** "Settings shows 128 GB" with a check, or the warning. */
    private TextView settingsCheck(long data, long system) {
        boolean wrong = SettingsSize.wrong(data, system);
        int color = wrong ? p.error : p.textDim;
        TextView t = ui.text(wrong ? "Settings would show " + SettingsSize.format(SettingsSize.total(data, system))
                + " total, less than is free" : settingsLine(data, system), 12.5f, color);
        t.setCompoundDrawablesRelative(ui.icon(wrong ? R.drawable.ic_warning : R.drawable.ic_check,
                wrong ? p.error : p.good, 14), null, null, null);
        t.setCompoundDrawablePadding(ui.dp(6));
        t.setPadding(ui.dp(20), ui.dp(2), 0, 0);
        return t;
    }

    private static String settingsLine(long data, long system) {
        long extra = SettingsSize.error(data, system);
        return "Settings shows " + SettingsSize.format(SettingsSize.total(data, system)) + " total"
                + (extra >= 2 * SettingsSize.GB ? ", " + SettingsSize.format(extra) + " of it as System" : "");
    }

    /**
     * The size picker: slider, both sides' sizes and Settings' view of them. The action button is enabled only
     * for a split Settings shows right.
     */
    private void addSplitEditor(LinearLayout card, Twinslot.Status st, int owner, long pool, Button action) {
        int maxGiB = maxGiB(pool);
        setupGiB = Math.min(Math.max(setupGiB, MIN_GIB), maxGiB);
        int otherSlot = 1 - owner;
        long sysOwner = systemBytes(st.slots[owner]);
        long sysOther = systemBytes(st.slots[otherSlot]);

        Ui.Bar preview = ui.bar(p.surfaceHigh, 14, 4);
        TextView ownerSize = ui.headline("", 22, p.slot(owner));
        TextView otherSize = ui.headline("", 22, p.slot(otherSlot));
        otherSize.setGravity(Gravity.END);
        LinearLayout ownerCol = ui.column();
        LinearLayout otherCol = ui.column();
        otherCol.setGravity(Gravity.END);
        TextView ownerLabel = ui.overline("Slot " + upper(owner) + " · own data", p.textDim);
        TextView otherLabel = ui.overline("Slot " + upper(otherSlot) + " · main data", p.textDim);
        ownerCol.addView(ownerLabel);
        ownerCol.addView(ownerSize);
        otherCol.addView(otherLabel);
        otherCol.addView(otherSize);
        LinearLayout sizes = ui.row();
        sizes.addView(ownerCol, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        sizes.addView(otherCol, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        card.addView(sizes, ui.top(16));
        card.addView(preview, ui.top(10));

        SeekBar bar = new SeekBar(this);
        ui.tint(bar, p.slot(owner));
        bar.setMax(maxGiB - MIN_GIB);
        bar.setProgress(setupGiB - MIN_GIB);
        // The track lines up with the bars: the seek bar's own padding only makes room for the thumb.
        bar.setPadding(ui.dp(10), ui.dp(8), ui.dp(10), ui.dp(8));
        LinearLayout.LayoutParams barLp = ui.top(6);
        barLp.setMarginStart(-ui.dp(10));
        barLp.setMarginEnd(-ui.dp(10));
        card.addView(bar, barLp);

        LinearLayout checks = ui.column();
        card.addView(checks, ui.top(2));
        int best = SettingsSize.best(pool, sysOwner, sysOther, MIN_GIB, maxGiB);
        Button bestButton = ui.button("", Ui.Style.TEXT, R.drawable.ic_check);
        card.addView(bestButton, ui.wrapTop(4));

        Runnable update = () -> {
            long own = setupGiB * SettingsSize.GIB;
            long rest = pool - own;
            ownerSize.setText(SettingsSize.format(own));
            otherSize.setText(SettingsSize.format(rest));
            float f = (float) own / pool;
            preview.set(owner == 0 ? new float[] {f, 1 - f} : new float[] {1 - f, f},
                    new int[] {p.slot(0), p.slot(1)});
            checks.removeAllViews();
            TextView a = settingsCheck(own, sysOwner);
            TextView b = settingsCheck(rest, sysOther);
            a.setText(upper(owner) + ": " + a.getText());
            b.setText(upper(otherSlot) + ": " + b.getText());
            a.setPadding(0, ui.dp(2), 0, 0);
            b.setPadding(0, ui.dp(2), 0, 0);
            checks.addView(a);
            checks.addView(b);
            boolean ok = !SettingsSize.wrong(own, sysOwner) && !SettingsSize.wrong(rest, sysOther);
            Ui.enable(action, ok && st.current >= 0 && pool / SettingsSize.GIB >= 2 * MIN_GIB);
            bestButton.setVisibility(best > 0 && best != setupGiB ? View.VISIBLE : View.GONE);
            if (best > 0) {
                ui.setLabel(bestButton, "Use the best split: " + SettingsSize.format(best * SettingsSize.GIB)
                        + " / " + SettingsSize.format(pool - best * SettingsSize.GIB), R.drawable.ic_check, p.primary);
            }
        };
        update.run();
        bestButton.setOnClickListener(v -> {
            setupGiB = best;
            bar.setProgress(best - MIN_GIB);
            update.run();
        });
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean user) {
                setupGiB = progress + MIN_GIB;
                update.run();
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
            }
        });
    }

    private int bestGiB(Twinslot.Status st, int owner, long pool) {
        int maxGiB = maxGiB(pool);
        return SettingsSize.best(pool, systemBytes(st.slots[owner]), systemBytes(st.slots[1 - owner]), MIN_GIB,
                maxGiB);
    }

    /** The slot's system size, or a typical one when nothing is installed yet. */
    private static long systemBytes(Twinslot.Slot s) {
        return (s.systemMib > 0 ? s.systemMib : 1024) * SettingsSize.MIB;
    }

    /**
     * Splits the data at setupGiB for owner. With keepOwner (a resize under a ROM in use), it then points
     * owner's ROM at its new data and lets it boot again (setup marks it unbootable), and leaves the current
     * slot to boot next. A first setup leaves owner switched off: an empty slot has nothing to point, and
     * whatever an old slot holds should not come back to life on its own.
     */
    private List<String[]> setupCommands(int owner, int current, boolean keepOwner) {
        String[] setup = {"setup", "--size", setupGiB + "G", "--owner", String.valueOf(Twinslot.letter(owner)),
            "--apply"};
        if (!keepOwner) {
            return Collections.singletonList(setup);
        }
        return Arrays.asList(setup,
                new String[] {"rebind", String.valueOf(Twinslot.letter(owner))},
                new String[] {"switch", String.valueOf(Twinslot.letter(owner))},
                new String[] {"switch", String.valueOf(Twinslot.letter(current))});
    }

    /**
     * The largest split the slider offers: userdata keeps MIN_GIB, and room is left for a twinmeta carved
     * out of it on phones without a spare partition to adopt.
     */
    private static int maxGiB(long pool) {
        return (int) Math.max(MIN_GIB, (pool - META_ROOM) / SettingsSize.GIB - MIN_GIB);
    }

    private static char upper(int slot) {
        return Character.toUpperCase(Twinslot.letter(slot));
    }

    private static List<String[]> concat(List<String[]> a, List<String[]> b) {
        List<String[]> all = new ArrayList<>(a);
        all.addAll(b);
        return all;
    }

    // ---- helpers ----

    private interface Command {
        Twinslot.Result run() throws IOException;
    }

    private void runQuick(String what, Command command) {
        showLoading(what);
        background(() -> {
            Twinslot.Result r = command.run();
            if (!r.ok()) {
                Diagnostics.note(this, what + " failed (exit " + r.code + "): " + r.message());
                SwitchFlow.toast(this, r.message());
            }
            runOnUiThread(this::refresh);
        }, error -> {
            SwitchFlow.toast(this, error);
            refresh();
        });
    }

    /** A file path root can read, for a document on internal storage or a removable volume; else null. */
    private String pathOf(Uri uri) {
        if (uri == null) {
            return null;
        }
        if ("file".equals(uri.getScheme())) {
            return uri.getPath();
        }
        try {
            String id = DocumentsContract.getDocumentId(uri);
            String authority = uri.getAuthority();
            if ("com.android.externalstorage.documents".equals(authority) && id.startsWith("primary:")) {
                return "/storage/emulated/0/" + id.substring("primary:".length());
            }
            // An SD card or USB stick: "F31F-1E82:TwinSlot/rom.zip". Often where the ROM zip is right after
            // the split, since internal storage was just erased.
            int colon = id.indexOf(':');
            if ("com.android.externalstorage.documents".equals(authority) && colon > 0) {
                String path = removable(id.substring(0, colon), id.substring(colon + 1));
                if (path != null) {
                    return path;
                }
            }
            if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:")) {
                return id.substring("raw:".length());
            }
        } catch (IllegalArgumentException | IOException ignored) {
            // Not a document URI, or root could not look.
        }
        // Recent, Downloads (msf:N) and media documents carry no path: open the file and
        // ask the kernel which one it is.
        try (ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r")) {
            String path = fd == null ? null : Os.readlink("/proc/self/fd/" + fd.getFd());
            // The same file as the FUSE view, the app's view and the lower filesystem.
            String[][] views = {{"/storage/emulated/", "/storage/emulated/"},
                {"/mnt/user/0/emulated/", "/storage/emulated/"}, {"/mnt/pass_through/0/emulated/", "/storage/emulated/"},
                {"/data/media/", "/storage/emulated/"}, {"/sdcard/", "/storage/emulated/0/"}};
            for (String[] v : views) {
                if (path != null && path.startsWith(v[0])) {
                    return v[1] + path.substring(v[0].length());
                }
            }
            for (String prefix : new String[] {"/storage/", "/mnt/media_rw/"}) {
                if (path != null && path.startsWith(prefix) && !path.startsWith("/storage/emulated/")) {
                    String rest = path.substring(prefix.length());
                    int slash = rest.indexOf('/');
                    String found = slash > 0 ? removable(rest.substring(0, slash), rest.substring(slash + 1)) : null;
                    if (found != null) {
                        return found;
                    }
                }
            }
        } catch (IOException | ErrnoException | SecurityException | IllegalArgumentException ignored) {
            // Not a local file.
        }
        // Last resort: a file of that name in Download.
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null,
                null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null && !c.getString(0).contains("/")) {
                String path = "/storage/emulated/0/Download/" + c.getString(0);
                return twinslot.exists(path) ? path : null;
            }
        } catch (IOException | RuntimeException ignored) {
            // No name.
        }
        return null;
    }

    /**
     * A file on a removable volume as root sees it. The app's /storage/UUID is a FUSE view that root's mount
     * namespace may not have; /mnt/media_rw/UUID is the volume itself. Null if root sees neither.
     */
    private String removable(String volume, String rel) throws IOException {
        if (!volume.matches("[A-Za-z0-9-]+")) {
            return null;
        }
        for (String base : new String[] {"/mnt/media_rw/", "/storage/"}) {
            String path = base + volume + "/" + rel;
            if (twinslot.exists(path)) {
                return path;
            }
        }
        return null;
    }

    static String appVersion(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    private static String capitalize(String s) {
        return s.isEmpty() ? s : Character.toUpperCase(s.charAt(0)) + s.substring(1);
    }

    private View spacer(int widthDp) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(ui.dp(widthDp), 1));
        return v;
    }
}
