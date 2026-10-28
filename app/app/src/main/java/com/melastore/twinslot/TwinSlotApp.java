package com.melastore.twinslot;

import android.app.Application;

/** Starts before any screen, service or tile: crashes anywhere in the app are noted for the diagnostic log. */
public class TwinSlotApp extends Application {

    @Override
    public void onCreate() {
        super.onCreate();
        Diagnostics.catchCrashes(this);
    }
}
