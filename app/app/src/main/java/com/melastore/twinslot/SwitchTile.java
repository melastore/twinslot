package com.melastore.twinslot;

import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

import java.io.IOException;

/** Quick Settings tile: tap, confirm, the phone reboots into the other Android. */
public class SwitchTile extends TileService {

    private volatile Twinslot.Status status;

    @Override
    public void onStartListening() {
        if (!new Prefs(this).disclaimerAccepted()) {
            // Nothing runs as root before the first-open warning was accepted: a tap opens the app, which shows it.
            status = null;
            Tile tile = getQsTile();
            if (tile != null) {
                tile.setState(Tile.STATE_INACTIVE);
                tile.setSubtitle(getString(R.string.tile_open_app));
                tile.updateTile();
            }
            return;
        }
        new Thread(() -> {
            Twinslot.Status st = null;
            try {
                st = new Twinslot(this).status();
            } catch (IOException ignored) {
                // No root or no record: the tile shows as unavailable.
            }
            status = st;
            Twinslot.Status shown = st;
            getMainExecutor().execute(() -> update(shown));
        }).start();
    }

    private void update(Twinslot.Status st) {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        int other = st == null ? -1 : st.other();
        boolean ready = st != null && st.canSwitch();
        tile.setState(ready ? Tile.STATE_INACTIVE : Tile.STATE_UNAVAILABLE);
        tile.setSubtitle(ready ? st.slots[other].title(other) : null);
        tile.updateTile();
    }

    @Override
    public void onClick() {
        if (!new Prefs(this).disclaimerAccepted()) {
            unlockAndRun(this::openApp);
            return;
        }
        Twinslot.Status st = status;
        if (st == null) {
            return;
        }
        if (new AppLock(this).enabled()) {
            // Not before the first-open warning was accepted, nor past the lock: the app asks, then offers the
            // same dialog.
            unlockAndRun(this::openApp);
            return;
        }
        unlockAndRun(() -> SwitchFlow.confirm(this, st, this::showDialog));
    }

    @SuppressWarnings("deprecation")
    private void openApp() {
        Intent i = new Intent(this, MainActivity.class).setAction(MainActivity.ACTION_SWITCH)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE));
        } else {
            startActivityAndCollapse(i);
        }
    }
}
