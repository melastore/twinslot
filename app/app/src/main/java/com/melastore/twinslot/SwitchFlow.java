package com.melastore.twinslot;

import android.app.AlertDialog;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.ContextThemeWrapper;
import android.widget.Toast;

import java.io.IOException;
import java.util.function.Consumer;

/** "Reboot to the other Android": shared by the app screen and the Quick Settings tile. */
final class SwitchFlow {

    private SwitchFlow() {
    }

    /**
     * Builds the confirmation dialog for switching to {@code st}'s other slot.
     * {@code show} displays it (Activity: dialog.show(); tile: showDialog()).
     */
    static void confirm(Context context, Twinslot.Status st, Consumer<AlertDialog> show) {
        int target = st.other();
        if (target < 0) {
            toast(context, "Cannot tell which Android is running");
            return;
        }
        Twinslot.Slot slot = st.slots[target];
        String name = slot.title(target);
        char letter = Character.toUpperCase(Twinslot.letter(target));
        if (!slot.isInstalled()) {
            toast(context, "Nothing is installed in slot " + letter);
            return;
        }
        if (!st.dualReady()) {
            toast(context, "Set up the storage split first: " + name + " would start on this Android's data");
            return;
        }
        if (!slot.usable()) {
            toast(context, name + " is switched off (an install did not finish, or it failed to start); "
                    + "install it again");
            return;
        }
        if (st.mergePending()) {
            toast(context, "A system update is still finishing; reboot this Android first");
            return;
        }
        if (!slot.dataOk()) {
            toast(context, name + " is not pointed at its own data yet; open TwinSlot to fix it");
            return;
        }
        AlertDialog dialog = new AlertDialog.Builder(themed(context))
                .setTitle("Reboot to " + name + "?")
                .setMessage("The phone restarts into the Android in slot " + letter
                        + ". Open apps will close.")
                .setPositiveButton("Reboot", (d, w) -> run(context, target))
                .setNegativeButton(android.R.string.cancel, null)
                .create();
        show.accept(dialog);
    }

    private static void run(Context context, int target) {
        Context app = context.getApplicationContext();
        new Thread(() -> {
            Twinslot ts = new Twinslot(app);
            try {
                Twinslot.Result r = ts.switchTo(target);
                if (!r.ok()) {
                    Diagnostics.note(app, "switch to slot " + Twinslot.letter(target) + " failed (exit " + r.code
                            + "): " + r.message());
                    toast(app, "Not switched: " + r.message());
                    return;
                }
            } catch (IOException e) {
                Diagnostics.error(app, "switch", e);
                toast(app, "Not switched: " + e.getMessage());
                return;
            }
            try {
                ts.reboot();
            } catch (IOException e) {
                // The record is already changed: the next restart, whenever it comes, goes to the other slot.
                Diagnostics.error(app, "reboot after switch", e);
                toast(app, "Switched, but the phone did not restart (" + e.getMessage() + "). Restart it to "
                        + "start slot " + Character.toUpperCase(Twinslot.letter(target)) + ".");
            }
        }).start();
    }

    /** The app's day/night theme, also for the tile's dialog (a service has none of its own). */
    static Context themed(Context context) {
        ContextThemeWrapper themed = new ContextThemeWrapper(context, R.style.Theme_TwinSlot);
        android.content.res.Configuration night = Prefs.nightOverride(context);
        if (night != null) {
            themed.applyOverrideConfiguration(night);
        }
        return themed;
    }

    static void toast(Context context, String text) {
        new Handler(Looper.getMainLooper()).post(
                () -> Toast.makeText(context, text, Toast.LENGTH_LONG).show());
    }
}
