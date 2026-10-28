package com.melastore.twinslot;

import android.os.Bundle;
import android.widget.LinearLayout;
import android.widget.TextView;

/** The guide: what TwinSlot does and how to go from one Android to two, in short sections. */
public class GuideActivity extends Screen {

    /** Title, then paragraphs; a paragraph starting with a digit and ". " is a numbered step. */
    private static final String[][] SECTIONS = {
        {"Alpha software",
            "TwinSlot is alpha: tested on the Tecno CK7n, not yet on most other MediaTek phones. It rewrites "
                + "partitions, so a bug or a cut-off flash can erase your data or stop the phone from starting. "
                + "Back up first; you use it at your own risk."},
        {"How it works",
            "An A/B phone keeps two copies of Android, in slot A and slot B. Normally the second copy only "
                + "receives updates. TwinSlot puts a different Android there, gives it its own data, and lets "
                + "you choose which one starts.",
            "Only one Android runs at a time. Switching is a reboot.",
            "Each Android has its own apps, accounts and files. They share nothing but the phone."},
        {"What you need",
            "1. Root in the Android you use now (Magisk, KernelSU, APatch or similar), with TwinSlot allowed.",
            "2. An A/B phone with dynamic partitions (super). TwinSlot's device check tells you.",
            "3. A full ROM zip for your phone: the kind recovery sideloads, with payload.bin inside.",
            "4. A recovery that can ADB sideload (most can) or install from an SD card or USB stick.",
            "5. A backup, and a way to flash the phone from scratch if something goes badly wrong."},
        {"Step 1: split the storage",
            "Two Androids cannot share one /data, so the data partition is split in two. This erases the data "
                + "of both Androids, so back up first.",
            "1. On the main screen, under Storage split, pick the size for the second Android.",
            "2. Tap Make the setup zip. Copy it to a computer (or an SD card), together with the TwinSlot APK "
                + "and your root manager's APK: they are erased too.",
            "3. Reboot to recovery and flash it: Apply update › Apply from ADB, then on the computer run "
                + "adb sideload with the zip's name.",
            "4. Reboot. Android starts fresh: set it up, install your root manager and TwinSlot again, and "
                + "allow TwinSlot root."},
        {"Step 2: install the second ROM",
            "1. Put the ROM zip on internal storage (Download is fine).",
            "2. Tap Choose ROM zip and pick it. TwinSlot installs into the slot you are not using.",
            "3. Wait for it: checking the zip and writing take a few minutes. You can leave the app; the "
                + "notification shows the progress. Do not restart the phone meanwhile.",
            "When it says Installed, every partition was read back and matches the ROM."},
        {"Step 3: switch",
            "Tap Reboot to … on the other Android's card. Or add the Other Android tile to Quick Settings and "
                + "switch from there, from either Android.",
            "If a slot fails to start a few times in a row, the bootloader goes back to the other slot by itself.",
            "From recovery, a switch zip does the same. TwinSlot offers one when an install is done: keep it on "
                + "an SD card or a computer, since each Android has its own internal storage."},
        {"Root and TwinSlot on both",
            "Each Android has its own data, so root and TwinSlot have to be set up in each one. Until then, "
                + "use the switch zips or the other Android's TwinSlot to go back."},
        {"Updates",
            "Keep system updates (OTA) off on both Androids: an update is written into the other slot, over "
                + "the other Android.",
            "To update the other ROM, install its newer full zip here with Start with empty data turned off."},
        {"If something is wrong",
            "Pointed at the wrong data: the card says so. Tap Fix. This happens when a recovery is flashed over "
                + "a slot's boot partitions.",
            "Install failed: the slot is left switched off, so the phone never boots half a ROM. Read the "
                + "message, fix the cause, install again.",
            "Something else: Settings › Diagnostic log, and send that file when you ask for help."},
        {"If a flash is cut off",
            "Setup, resize and remove zips rewrite the partition table. Before the zip is made, TwinSlot saves "
                + "a copy of the table next to it (twinslot-gpt-….bin) and puts the same copy inside the zip "
                + "(table.bin), so a zip you sideload from a computer leaves a copy there. From an SD card or "
                + "USB stick the zip saves one more into its own folder. On phones without a backup table "
                + "(MediaTek), the zip refuses to run when no copy would outlive a power cut.",
            "A write that fails while the phone is on is undone at once: TwinSlot writes the old table back "
                + "itself. A copy is for when the power went.",
            "If the phone still reaches a recovery with a shell (TWRP, OrangeFox): run twinslot table-restore "
                + "with the copy's path. It finds the disk from the copy's name or size; if it asks, add "
                + "--disk with the disk twinslot status showed (disk=).",
            "If it does not start at all: write the copy back from a computer with a MediaTek tool such as "
                + "mtkclient, at offset 0 of the storage that holds userdata. The README has the exact steps."},
        {"Remove the split",
            "Under Storage split, Change or remove the split › Make the remove zip. It gives all the space back "
                + "to the main data and erases both Androids' data. The second slot stops booting until a ROM "
                + "is installed there again."},
        {"The app lock",
            "Settings › App lock asks for a PIN or pattern when TwinSlot opens, and fingerprint unlock can "
                + "stand in for it. The Quick Settings tile then opens the app instead of switching directly.",
            "Forgot it? Android's Settings › Apps › TwinSlot › Storage › Clear storage removes the lock along "
                + "with TwinSlot's settings. Your Androids are not affected."},
    };

    private int open = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs.setGuideOffered();
    }

    @Override
    void ready() {
        render();
    }

    private void render() {
        content.removeAllViews();
        pageHeader("Guide");
        for (int i = 0; i < SECTIONS.length; i++) {
            content.addView(section(i), ui.top(i == 0 ? 12 : 10));
        }
    }

    private LinearLayout section(int index) {
        String[] s = SECTIONS[index];
        boolean expanded = index == open;
        LinearLayout card = ui.card(expanded ? p.surface : Palette.blend(p.background, p.surface, 0.6f));
        card.setPadding(ui.dp(20), ui.dp(4), ui.dp(12), ui.dp(expanded ? 18 : 4));
        LinearLayout head = ui.actionRow(0, s[0], null, v -> {
            open = expanded ? -1 : index;
            render();
        });
        head.addView(ui.iconView(expanded ? R.drawable.ic_collapse : R.drawable.ic_expand, p.textDim, 22));
        head.setContentDescription(s[0] + (expanded ? ", expanded" : ", collapsed"));
        card.addView(head);
        if (!expanded) {
            return card;
        }
        for (int k = 1; k < s.length; k++) {
            String para = s[k];
            boolean step = para.length() > 3 && Character.isDigit(para.charAt(0)) && para.startsWith(". ", 1);
            if (step) {
                LinearLayout row = ui.step(para.charAt(0) - '0', para.substring(3));
                row.setPadding(0, 0, ui.dp(8), 0);
                card.addView(row, ui.top(k == 1 ? 4 : 10));
            } else {
                TextView t = ui.text(para, 14.5f, p.text);
                t.setPadding(0, 0, ui.dp(8), 0);
                card.addView(t, ui.top(k == 1 ? 4 : 10));
            }
        }
        return card;
    }
}
