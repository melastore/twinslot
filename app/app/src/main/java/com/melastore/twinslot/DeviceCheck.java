package com.melastore.twinslot;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * What TwinSlot needs from a phone, read without root: system properties any
 * app may read. It answers "can this phone dual boot at all?" before root is
 * even asked for. The diagnostic log reads it too.
 */
final class DeviceCheck {

    enum State { OK, NOTE, PROBLEM }

    static final class Item {
        final String label;
        final String value;
        final State state;
        final String note;

        Item(String label, String value, State state, String note) {
            this.label = label;
            this.value = value;
            this.state = state;
            this.note = note;
        }
    }

    /** A root manager app TwinSlot knows, with where its per-app switch lives. */
    static final class RootManager {
        final String pkg;
        final String name;
        final String howTo;

        RootManager(String pkg, String name, String howTo) {
            this.pkg = pkg;
            this.name = name;
            this.howTo = howTo;
        }
    }

    private static final String KSU_HOW = "Open it, go to Superuser, find TwinSlot and turn it on. Not in the "
            + "list? Pull the list down to refresh it.";
    private static final String MAGISK_HOW = "Open it, go to Superuser and turn TwinSlot on. If Magisk asks "
            + "when TwinSlot opens, tap Grant.";

    /** The managers people use. Each is also listed under &lt;queries&gt; in the manifest. */
    static final RootManager[] ROOT_MANAGERS = {
        new RootManager("com.topjohnwu.magisk", "Magisk", MAGISK_HOW),
        new RootManager("io.github.vvb2060.magisk", "Magisk Alpha", MAGISK_HOW),
        new RootManager("io.github.huskydg.magisk", "Kitsune Mask", MAGISK_HOW),
        new RootManager("me.weishu.kernelsu", "KernelSU", KSU_HOW),
        new RootManager("com.rifsxd.ksunext", "KernelSU Next", KSU_HOW),
        new RootManager("com.sukisu.ultra", "SukiSU Ultra", KSU_HOW),
        new RootManager("com.kowx712.supermanager", "KowSU", KSU_HOW),
        new RootManager("me.bmax.apatch", "APatch", "Open it, go to Superuser and turn TwinSlot on."),
    };

    final Map<String, String> props;
    final List<Item> items = new ArrayList<>();
    /** False when the phone cannot dual boot whatever is done about root. */
    boolean capable = true;

    private DeviceCheck(Map<String, String> props) {
        this.props = props;
    }

    static DeviceCheck run() {
        DeviceCheck d = new DeviceCheck(readProps());
        d.check();
        return d;
    }

    private String prop(String key) {
        String v = props.get(key);
        return v == null ? "" : v.trim();
    }

    private void check() {
        String suffix = prop("ro.boot.slot_suffix");
        if (suffix.isEmpty()) {
            suffix = prop("ro.boot.slot");
        }
        boolean ab = !suffix.isEmpty() || "true".equals(prop("ro.build.ab_update"));
        if (ab) {
            String running = suffix.replace("_", "").toUpperCase(Locale.ROOT);
            items.add(new Item("Two system slots (A/B)", running.isEmpty() ? "Yes" : "Yes, running " + running,
                    State.OK, null));
        } else {
            capable = false;
            items.add(new Item("Two system slots (A/B)", "No", State.PROBLEM,
                    "This phone keeps one copy of Android, so there is no second slot to put another one in."));
        }

        boolean dynamic = "true".equals(prop("ro.boot.dynamic_partitions"));
        if (dynamic) {
            items.add(new Item("Dynamic partitions (super)", "Yes", State.OK, null));
        } else {
            capable = false;
            items.add(new Item("Dynamic partitions (super)", "No", State.PROBLEM,
                    "TwinSlot places the second Android inside the super partition, which phones that "
                            + "launched with Android 10 or later have."));
        }

        if ("true".equals(prop("ro.virtual_ab.enabled"))) {
            items.add(new Item("Virtual A/B updates", "Yes", State.NOTE,
                    "Keep system updates (OTA) off on both Androids: an update writes into the other slot."));
        }

        String release = prop("ro.build.version.release");
        items.add(new Item("Android", (release.isEmpty() ? Build.VERSION.RELEASE : release) + " (API "
                + Build.VERSION.SDK_INT + ")", State.OK, null));

        String soc = (prop("ro.soc.manufacturer") + " " + prop("ro.soc.model")).trim();
        if (soc.isEmpty()) {
            soc = prop("ro.board.platform");
        }
        if (soc.isEmpty()) {
            soc = prop("ro.hardware");
        }
        // Slot switching goes through the boot record in misc, which MediaTek's bootloader reads. Qualcomm keeps
        // the slots in the partition table, Pixels and Exynos in their own places: TwinSlot cannot switch there.
        String chip = (soc + " " + prop("ro.board.platform") + " " + prop("ro.hardware")).toLowerCase(Locale.ROOT);
        if (chip.matches(".*\\b(qcom|qti|qualcomm|msm|sdm|sm\\d|lahaina|taro|kalama|pineapple|holi|bengal|lito|"
                + "kona|parrot)\\w*.*")) {
            capable = false;
            items.add(new Item("Chipset", soc.isEmpty() ? "Qualcomm" : soc, State.PROBLEM,
                    "Qualcomm phones keep the boot slots in the partition table, which TwinSlot does not "
                            + "switch yet."));
        } else if (chip.matches(".*\\b(gs\\d|zuma|tensor|google|exynos|s5e|samsung)\\w*.*")) {
            capable = false;
            items.add(new Item("Chipset", soc.isEmpty() ? "Unknown" : soc, State.PROBLEM,
                    "This chipset keeps its boot slots in its own place, which TwinSlot does not switch."));
        } else if (chip.matches(".*\\b(mt\\d|mediatek)\\w*.*")) {
            items.add(new Item("Chipset", soc, State.OK, null));
        } else {
            items.add(new Item("Chipset", soc.isEmpty() ? "Unknown" : soc, State.NOTE,
                    "TwinSlot is tested on MediaTek phones only."));
        }

        String vbs = prop("ro.boot.verifiedbootstate");
        if (!vbs.isEmpty()) {
            boolean locked = "green".equals(vbs);
            items.add(new Item("Verified boot", vbs + (locked ? " (locked)" : "orange".equals(vbs) ? " (unlocked)" : ""),
                    locked ? State.NOTE : State.OK, locked ? "A locked bootloader starts only ROMs signed with a "
                    + "key it accepts. That is true for either slot." : null));
        }
    }

    /** The first known root manager installed on the phone, or null (or one hidden under another name). */
    static RootManager installedManager(Context c) {
        PackageManager pm = c.getPackageManager();
        for (RootManager m : ROOT_MANAGERS) {
            try {
                pm.getPackageInfo(m.pkg, 0);
                return m;
            } catch (PackageManager.NameNotFoundException ignored) {
                // Not this one.
            }
        }
        return null;
    }

    static Intent launch(Context c, RootManager m) {
        return c.getPackageManager().getLaunchIntentForPackage(m.pkg);
    }

    /** Every property `getprop` shows an app: "[key]: [value]" lines. */
    private static Map<String, String> readProps() {
        Map<String, String> map = new HashMap<>();
        try {
            Process p = new ProcessBuilder("getprop").redirectErrorStream(true).start();
            p.getOutputStream().close();
            try (BufferedReader in = new BufferedReader(new InputStreamReader(p.getInputStream(),
                    StandardCharsets.UTF_8))) {
                String line;
                while ((line = in.readLine()) != null) {
                    int mid = line.indexOf("]: [");
                    if (line.startsWith("[") && mid > 0 && line.endsWith("]")) {
                        map.put(line.substring(1, mid), line.substring(mid + 4, line.length() - 1));
                    }
                }
            }
            p.waitFor();
        } catch (IOException e) {
            // No getprop: every check reads as unknown.
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return map;
    }
}
