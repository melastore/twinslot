package com.melastore.twinslot;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.text.format.Formatter;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * The diagnostic log page: what goes into the log, what is kept out, a
 * preview of the exact text, and sharing it as a file.
 */
public class DiagnosticsActivity extends Screen {

    /** Above this, the clipboard (one binder call) may refuse the text: sharing the file still works. */
    private static final int COPY_MAX = 400 * 1024;

    private Diagnostics.Report report;
    private File file;
    private boolean building;
    private boolean preview;
    private String error;

    @Override
    void ready() {
        if (report == null && !building && error == null) {
            build();
        }
        render();
    }

    private void build() {
        building = true;
        error = null;
        background(() -> {
            Diagnostics.Report r = Diagnostics.build(this);
            File f = Diagnostics.save(this, r);
            runOnUiThread(() -> {
                building = false;
                report = r;
                file = f;
                render();
            });
        }, e -> {
            building = false;
            error = e;
            render();
        });
    }

    private void render() {
        content.removeAllViews();
        pageHeader("Diagnostic log");
        content.addView(ui.body("If TwinSlot does something wrong on your phone, send this file to the "
                + "developer. It shows how the phone is laid out and what TwinSlot did, so the problem can be "
                + "found without having the phone."), ui.top(4));

        LinearLayout what = ui.card(p.surface);
        what.addView(ui.overline("What is in it", p.textDim));
        String[] parts = {
            "The phone model, its Android build and how its partitions are laid out",
            "The root app, and how it runs TwinSlot's commands",
            "TwinSlot's settings, its status, and the logs of the last two installs",
            "What the app itself noted: errors, crashes, jobs started and ended",
        };
        for (int i = 0; i < parts.length; i++) {
            what.addView(ui.step(i + 1, parts[i]), ui.top(i == 0 ? 10 : 8));
        }
        content.addView(what, ui.top(16));


        if (building) {
            LinearLayout card = ui.card(p.surface);
            card.setGravity(Gravity.CENTER_HORIZONTAL);
            card.setPadding(ui.dp(20), ui.dp(28), ui.dp(20), ui.dp(28));
            ProgressBar spin = new ProgressBar(this);
            spin.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(p.primary));
            card.addView(spin, new LinearLayout.LayoutParams(ui.dp(36), ui.dp(36)));
            TextView t = ui.body("Collecting… root may take a few seconds to answer.");
            t.setGravity(Gravity.CENTER);
            card.addView(t, ui.top(14));
            content.addView(card, ui.top(16));
            return;
        }
        if (report == null) {
            content.addView(ui.banner(R.drawable.ic_error, "The log could not be made", error,
                    p.errorContainer, p.onErrorContainer), ui.top(16));
            Button again = ui.button("Try again", Ui.Style.FILLED, 0);
            again.setOnClickListener(v -> {
                build();
                render();
            });
            content.addView(again, ui.top(12));
            return;
        }

        LinearLayout ready = ui.card(p.surface);
        ready.addView(ui.overline("Ready", p.textDim));
        ready.addView(ui.medium(file.getName(), 15, p.text), ui.top(4));
        int lines = report.text.split("\n", -1).length;
        TextView facts = ui.text(lines + " lines · " + Formatter.formatShortFileSize(this, file.length()), 13,
                p.textDim);
        ready.addView(facts, ui.top(6));
        content.addView(ready, ui.top(16));

        Button share = ui.button("Share the file", Ui.Style.FILLED, R.drawable.ic_share);
        share.setOnClickListener(v -> share());
        content.addView(share, ui.top(12));
        Button copy = ui.button("Copy as text", Ui.Style.OUTLINED, R.drawable.ic_copy);
        copy.setOnClickListener(v -> copy());
        content.addView(copy, ui.top(8));

        LinearLayout more = ui.row();
        Button toggle = ui.button(preview ? "Hide the log" : "See what is sent", Ui.Style.TEXT,
                preview ? R.drawable.ic_collapse : R.drawable.ic_expand);
        toggle.setOnClickListener(v -> {
            preview = !preview;
            render();
        });
        more.addView(toggle);
        Button refresh = ui.button("Make it again", Ui.Style.TEXT, R.drawable.ic_reboot);
        refresh.setOnClickListener(v -> {
            report = null;
            build();
            render();
        });
        more.addView(refresh);
        content.addView(more, ui.wrapTop(8));

        if (preview) {
            TextView log = ui.mono(report.text, p.textDim);
            log.setTextIsSelectable(true);
            log.setTextSize(11.5f);
            log.setBackground(ui.shape(p.surface, 20));
            log.setPadding(ui.dp(14), ui.dp(12), ui.dp(14), ui.dp(12));
            content.addView(log, ui.top(4));
        }

        Button clear = ui.button("Clear the app's own notes", Ui.Style.TEXT, R.drawable.ic_delete);
        clear.setOnClickListener(v -> {
            Diagnostics.clearEvents(this);
            report = null;
            build();
            render();
            Toast.makeText(this, "Cleared. The phone's details and install logs are read fresh each time.",
                    Toast.LENGTH_LONG).show();
        });
        content.addView(clear, ui.wrapTop(16));
    }

    private void share() {
        Uri uri = LogProvider.uriFor(file);
        Intent send = new Intent(Intent.ACTION_SEND)
                .setType("text/plain")
                .putExtra(Intent.EXTRA_STREAM, uri)
                .putExtra(Intent.EXTRA_SUBJECT, "TwinSlot diagnostic log")
                .putExtra(Intent.EXTRA_TEXT, "TwinSlot " + MainActivity.appVersion(this) + " diagnostic log · "
                        + Build.MANUFACTURER + " " + Build.MODEL + " · Android " + Build.VERSION.RELEASE)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        // The grant travels with the clip to whichever app is picked in the chooser.
        send.setClipData(ClipData.newRawUri(file.getName(), uri));
        AppLock.openingOwnPicker();
        startActivity(Intent.createChooser(send, "Share the diagnostic log"));
    }

    private void copy() {
        if (report.text.length() > COPY_MAX) {
            Toast.makeText(this, "The log is too long to copy. Share the file instead.", Toast.LENGTH_LONG).show();
            return;
        }
        getSystemService(ClipboardManager.class).setPrimaryClip(ClipData.newPlainText("TwinSlot log", report.text));
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            // Android 13+ confirms the copy itself.
            Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show();
        }
    }
}
