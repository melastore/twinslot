package com.melastore.twinslot;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.util.ArrayList;
import java.util.List;

/**
 * What every TwinSlot screen shares: the chosen theme, an edge-to-edge
 * scrolling page, the keep-screen-on setting and the app lock in front of it all.
 */
abstract class Screen extends Activity {

    private static final int MAX_WIDTH_DP = 640;

    Palette p;
    Ui ui;
    Prefs prefs;
    AppLock lock;
    /** The page: subclasses fill it in {@link #ready()}. */
    LinearLayout content;

    private FrameLayout root;
    private int look;
    private boolean jobRunning;
    private final List<Dialog> dialogs = new ArrayList<>();
    private final List<Dialog> waiting = new ArrayList<>();

    private View overlay;
    private Runnable overlayBack;
    private LockScreen pad;
    private boolean unlockShown;
    private Object backCallback;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        Configuration night = Prefs.nightOverride(base);
        if (night != null) {
            applyOverrideConfiguration(night);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = new Prefs(this);
        lock = new AppLock(this);
        p = new Palette(this);
        ui = new Ui(this, p);
        look = prefs.look();

        View decor = getWindow().getDecorView();
        decor.setBackgroundColor(p.background);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
        } else {
            setLegacyBarFlags(decor);
        }

        content = new Ui.Column(this, ui.dp(MAX_WIDTH_DP));
        // On tablets and in landscape the page stays a readable column in the middle.
        FrameLayout page = new FrameLayout(this);
        page.addView(content, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER_HORIZONTAL));
        ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(page);
        // Edge to edge: the page scrolls behind the bars, its ends stay clear of them.
        scroll.setOnApplyWindowInsetsListener((v, in) -> {
            Insets i = Ui.bars(in);
            v.setPadding(i.left + ui.dp(16), i.top + ui.dp(12), i.right + ui.dp(16), i.bottom + ui.dp(28));
            return in;
        });
        // A veil under the status bar, so the page never runs into the clock.
        View veil = new View(this);
        veil.setBackgroundColor(Palette.alpha(p.background, 0.94f));
        root = new FrameLayout(this);
        root.addView(scroll);
        root.addView(veil, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, 0));
        veil.setOnApplyWindowInsetsListener((v, in) -> {
            v.getLayoutParams().height = Ui.bars(in).top;
            v.requestLayout();
            return in;
        });
        setContentView(root);
    }

    @SuppressWarnings("deprecation")
    private void setLegacyBarFlags(View decor) {
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | (p.dark ? 0
                : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR));
    }

    @Override
    protected void onStart() {
        super.onStart();
        AppLock.screenStarted();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (look != prefs.look()) {
            // The theme changed on another screen.
            recreate();
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // Locked or not, the Recents thumbnail must not show what is behind the lock.
            setRecentsScreenshotEnabled(!lock.enabled());
        }
        updateScreenOn();
        if (lock.needsUnlock()) {
            showUnlock();
            return;
        }
        lock.unlocked();
        if (!unlockShown) {
            ready();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        AppLock.screenStopped(isChangingConfigurations());
        if (lock.enabled()) {
            // A dialog would float above the lock when the app comes back.
            for (Dialog d : new ArrayList<>(dialogs)) {
                d.dismiss();
            }
            // Never shown, so dismiss() did not untrack them.
            dialogs.removeAll(waiting);
            waiting.clear();
            if (pad != null) {
                pad.stop();
            }
        }
    }

    /** Builds or refreshes the page: the app is unlocked and in view. */
    abstract void ready();

    // ---- the lock ----

    private void showUnlock() {
        if (unlockShown) {
            pad.start();
            return;
        }
        unlockShown = true;
        LockScreen l = LockScreen.unlock(this, lock, () -> {
            unlockShown = false;
            lock.unlocked();
            hideOverlay();
            ready();
            for (Dialog d : waiting) {
                d.show();
            }
            waiting.clear();
        });
        showPad(l);
    }

    /** Puts a PIN or pattern pad in front of the page. */
    void showPad(LockScreen l) {
        pad = l;
        showOverlay(l.view(), l::back);
        l.start();
    }

    void hidePad() {
        if (pad != null) {
            pad.stop();
            pad = null;
        }
        hideOverlay();
    }

    private void showOverlay(View v, Runnable onBack) {
        hideOverlay();
        overlay = v;
        overlayBack = onBack;
        root.addView(v, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        v.requestApplyInsets();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            OnBackInvokedCallback cb = () -> overlayBack.run();
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_OVERLAY, cb);
            backCallback = cb;
        }
    }

    private void hideOverlay() {
        if (overlay == null) {
            return;
        }
        root.removeView(overlay);
        overlay = null;
        overlayBack = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback((OnBackInvokedCallback) backCallback);
            backCallback = null;
        }
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        // Android 12 and older; newer ones call the registered callback instead.
        if (overlayBack != null) {
            overlayBack.run();
        } else {
            super.onBackPressed();
        }
    }

    // ---- screen on ----

    /** A job runs: the screen may stay on for it, as the settings say. */
    void setJobRunning(boolean running) {
        jobRunning = running;
        updateScreenOn();
    }

    void updateScreenOn() {
        Prefs.ScreenOn s = prefs.screenOn();
        boolean on = s == Prefs.ScreenOn.ALWAYS || s == Prefs.ScreenOn.JOBS && jobRunning;
        if (on) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    // ---- shared pieces ----

    /** Shows a dialog that is dismissed if the app locks while it is open. */
    AlertDialog show(AlertDialog.Builder b) {
        AlertDialog d = b.create();
        present(d);
        return d;
    }

    /** Shows d now, or after the unlock if the lock is up (a dialog would float above it). */
    void present(AlertDialog d) {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        track(d);
        if (unlockShown || lock.needsUnlock()) {
            waiting.add(d);
        } else {
            d.show();
        }
    }

    void track(AlertDialog d) {
        dialogs.add(d);
        d.setOnDismissListener(x -> dialogs.remove(d));
    }

    /** A back arrow and a title: the header of every page but the main one. */
    void pageHeader(String title) {
        LinearLayout row = ui.row();
        ImageButton back = ui.iconButton(R.drawable.ic_back, p.text, "Back");
        back.setOnClickListener(v -> finish());
        row.addView(back);
        TextView t = ui.headline(title, 24, p.text);
        t.setPadding(ui.dp(8), 0, 0, 0);
        row.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, ui.dp(4), 0, ui.dp(8));
        content.addView(row);
    }

    /** Runs work off the main thread and reports a failure's message on it. */
    void background(Work work, Failure failure) {
        new Thread(() -> {
            try {
                work.run();
            } catch (Exception e) {
                Diagnostics.error(this, getClass().getSimpleName(), e);
                String m = e.getMessage() == null ? e.toString() : e.getMessage();
                runOnUiThread(() -> failure.show(m));
            }
        }).start();
    }

    interface Work {
        void run() throws Exception;
    }

    interface Failure {
        void show(String message);
    }
}
