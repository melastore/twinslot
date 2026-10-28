package com.melastore.twinslot;

import android.content.res.ColorStateList;
import android.graphics.drawable.RippleDrawable;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.security.GeneralSecurityException;
import java.util.List;

/**
 * The full-screen PIN or pattern pad, in three uses: unlocking the app,
 * confirming the current secret before it is changed, and choosing a new one
 * (entered twice).
 */
final class LockScreen {

    interface Created {
        void created(AppLock.Type type, String secret);
    }

    private enum Mode { UNLOCK, VERIFY, CREATE }

    private final Screen screen;
    private final Palette p;
    private final Ui ui;
    private final AppLock lock;
    private final Mode mode;
    private final AppLock.Type type;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private Runnable onSuccess;
    private Created onCreated;
    private Runnable onCancel;

    private final FrameLayout view;
    private TextView title;
    private TextView message;
    private TextView pinDots;
    private LinearLayout keypad;
    private PatternView pattern;
    private Button fingerprintButton;
    private final StringBuilder pin = new StringBuilder();
    /** CREATE: the first entry, waiting to be confirmed. */
    private String firstEntry;
    private boolean busy;
    private CancellationSignal fingerprintCancel;

    private LockScreen(Screen screen, AppLock lock, Mode mode, AppLock.Type type) {
        this.screen = screen;
        this.p = screen.p;
        this.ui = screen.ui;
        this.lock = lock;
        this.mode = mode;
        this.type = type;
        view = new FrameLayout(screen);
        build();
    }

    /** The lock shown when the app opens; {@code onUnlocked} runs once it is open. */
    static LockScreen unlock(Screen screen, AppLock lock, Runnable onUnlocked) {
        LockScreen l = new LockScreen(screen, lock, Mode.UNLOCK, lock.type());
        l.onSuccess = onUnlocked;
        return l;
    }

    /** Asks for the current secret before a change; {@code onOk} runs if it was right. */
    static LockScreen verify(Screen screen, AppLock lock, String why, Runnable onOk, Runnable onCancel) {
        LockScreen l = new LockScreen(screen, lock, Mode.VERIFY, lock.type());
        l.onSuccess = onOk;
        l.onCancel = onCancel;
        l.title.setText(why);
        return l;
    }

    /** Asks for a new secret twice and hands it over once both entries match. */
    static LockScreen create(Screen screen, AppLock lock, AppLock.Type type, Created onCreated, Runnable onCancel) {
        LockScreen l = new LockScreen(screen, lock, Mode.CREATE, type);
        l.onCreated = onCreated;
        l.onCancel = onCancel;
        return l;
    }

    View view() {
        return view;
    }

    /** Offers the fingerprint right away when it is set up; the pad stays as the way out. */
    void start() {
        if (mode == Mode.UNLOCK && lock.fingerprint() && lock.waitMs() == 0) {
            askFingerprint();
        }
        tickWait();
    }

    /** The screen went away: the system prompt goes with it. */
    void stop() {
        if (fingerprintCancel != null) {
            fingerprintCancel.cancel();
            fingerprintCancel = null;
        }
        handler.removeCallbacksAndMessages(null);
    }

    // ---- building ----

    private void build() {
        view.setBackgroundColor(p.background);
        view.setClickable(true);
        LinearLayout col = ui.column();
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        view.addView(col, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        view.setOnApplyWindowInsetsListener((v, in) -> {
            android.graphics.Insets bars = Ui.bars(in);
            v.setPadding(bars.left + ui.dp(24), bars.top + ui.dp(16), bars.right + ui.dp(24), bars.bottom + ui.dp(16));
            return in;
        });

        TextView badge = new TextView(screen);
        badge.setBackground(ui.shape(p.primaryContainer, 100));
        badge.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_lock, p.onPrimaryContainer, 28), null, null, null);
        badge.setPadding(ui.dp(18), 0, 0, 0);
        badge.setGravity(Gravity.CENTER_VERTICAL);
        col.addView(badge, new LinearLayout.LayoutParams(ui.dp(64), ui.dp(64)));

        title = ui.headline(initialTitle(), 24, p.text);
        title.setGravity(Gravity.CENTER);
        col.addView(title, ui.top(18));
        message = ui.body(initialMessage());
        message.setGravity(Gravity.CENTER);
        message.setMinLines(2);
        col.addView(message, ui.top(6));

        if (type == AppLock.Type.PATTERN) {
            pattern = new PatternView(screen, p.primary, p.outline, p.error, ui.dp(300));
            pattern.setListener(this::patternDrawn);
            col.addView(pattern, ui.top(20));
        } else {
            pinDots = ui.headline("", 28, p.text);
            pinDots.setGravity(Gravity.CENTER);
            pinDots.setLetterSpacing(0.3f);
            pinDots.setMinHeight(ui.dp(40));
            col.addView(pinDots, ui.top(14));
            keypad = keypad();
            col.addView(keypad, ui.wrapTop(12));
        }

        LinearLayout actions = ui.row();
        actions.setGravity(Gravity.CENTER);
        if (mode == Mode.UNLOCK && lock.fingerprint()) {
            fingerprintButton = ui.button("Use fingerprint", Ui.Style.TONAL, R.drawable.ic_fingerprint);
            fingerprintButton.setOnClickListener(v -> askFingerprint());
            actions.addView(fingerprintButton);
        }
        if (mode != Mode.UNLOCK) {
            Button cancel = ui.button("Cancel", Ui.Style.TEXT, 0);
            cancel.setOnClickListener(v -> cancel());
            actions.addView(cancel);
        }
        if (actions.getChildCount() > 0) {
            col.addView(actions, ui.wrapTop(18));
        }
    }

    private String noun() {
        return type == AppLock.Type.PATTERN ? "pattern" : "PIN";
    }

    private String initialTitle() {
        switch (mode) {
            case UNLOCK: return "TwinSlot is locked";
            case VERIFY: return "Confirm your " + noun();
            default: return type == AppLock.Type.PATTERN ? "Draw an unlock pattern" : "Choose a PIN";
        }
    }

    private String initialMessage() {
        switch (mode) {
            case UNLOCK:
                return type == AppLock.Type.PATTERN ? "Draw your pattern to open it." : "Enter your PIN to open it.";
            case VERIFY:
                return "Enter the current " + noun() + " first.";
            default:
                return type == AppLock.Type.PATTERN ? "Join at least " + AppLock.MIN_PATTERN + " dots."
                        : AppLock.MIN_PIN + " to " + AppLock.MAX_PIN + " digits.";
        }
    }

    /** 1 to 9, then delete, 0 and enter: a phone keypad. */
    private LinearLayout keypad() {
        LinearLayout pad = ui.column();
        String[] keys = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", ">"};
        for (int r = 0; r < 4; r++) {
            LinearLayout row = ui.row();
            for (int c = 0; c < 3; c++) {
                String k = keys[r * 3 + c];
                TextView key = key(k);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ui.dp(72), ui.dp(64));
                lp.setMargins(ui.dp(8), ui.dp(5), ui.dp(8), ui.dp(5));
                row.addView(key, lp);
            }
            pad.addView(row);
        }
        return pad;
    }

    private TextView key(String k) {
        TextView t = ui.headline(k, 26, p.text);
        t.setGravity(Gravity.CENTER);
        int bg = p.surfaceHigh;
        if (k.equals("<")) {
            t.setText("");
            t.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_backspace, p.text, 26), null, null, null);
            t.setPadding(ui.dp(23), 0, 0, 0);
            t.setContentDescription("Delete");
            bg = 0;
            t.setOnLongClickListener(v -> {
                pin.setLength(0);
                showPin();
                return true;
            });
        } else if (k.equals(">")) {
            t.setText("");
            t.setCompoundDrawablesRelative(ui.icon(R.drawable.ic_check, p.onPrimary, 26), null, null, null);
            t.setPadding(ui.dp(23), 0, 0, 0);
            t.setContentDescription("Enter");
            bg = p.primary;
        }
        t.setBackground(new RippleDrawable(ColorStateList.valueOf(Palette.alpha(p.text, 0.16f)),
                bg == 0 ? null : ui.shape(bg, 100), ui.shape(0xFFFFFFFF, 100)));
        t.setOnClickListener(v -> {
            v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            if (k.equals("<")) {
                if (pin.length() > 0) {
                    pin.setLength(pin.length() - 1);
                }
            } else if (k.equals(">")) {
                submitPin();
                return;
            } else if (pin.length() < AppLock.MAX_PIN) {
                pin.append(k);
            }
            showPin();
        });
        return t;
    }

    private void showPin() {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < pin.length(); i++) {
            s.append('●');
        }
        pinDots.setText(s);
        pinDots.setTextColor(p.text);
    }

    // ---- input ----

    private void submitPin() {
        if (busy || pin.length() == 0) {
            return;
        }
        String entry = pin.toString();
        if (mode == Mode.CREATE && firstEntry == null && entry.length() < AppLock.MIN_PIN) {
            say("A PIN needs at least " + AppLock.MIN_PIN + " digits.", true);
            return;
        }
        submit(entry);
    }

    private void patternDrawn(List<Integer> dots) {
        if (busy) {
            return;
        }
        if (mode == Mode.CREATE && firstEntry == null && dots.size() < AppLock.MIN_PATTERN) {
            pattern.showError();
            say("Join at least " + AppLock.MIN_PATTERN + " dots. Try again.", true);
            return;
        }
        submit(AppLock.pattern(dots));
    }

    private void submit(String entry) {
        if (mode == Mode.CREATE) {
            if (firstEntry == null) {
                firstEntry = entry;
                title.setText(type == AppLock.Type.PATTERN ? "Draw it again" : "Enter it again");
                say("To make sure it is the one you meant.", false);
                resetInput();
            } else if (firstEntry.equals(entry)) {
                onCreated.created(type, entry);
            } else {
                firstEntry = null;
                title.setText(initialTitle());
                say("Those did not match. Start over.", true);
                resetInput();
            }
            return;
        }
        busy = true;
        new Thread(() -> {
            boolean ok;
            try {
                ok = lock.check(entry);
            } catch (GeneralSecurityException e) {
                ok = false;
            }
            boolean right = ok;
            screen.runOnUiThread(() -> {
                busy = false;
                if (right) {
                    succeed();
                } else {
                    wrong();
                }
            });
        }).start();
    }

    private void succeed() {
        stop();
        onSuccess.run();
    }

    private void wrong() {
        if (pattern != null) {
            pattern.showError();
        } else {
            pinDots.setTextColor(p.error);
            view.performHapticFeedback(android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R
                    ? HapticFeedbackConstants.REJECT : HapticFeedbackConstants.LONG_PRESS);
        }
        if (lock.waitMs() > 0) {
            tickWait();
            return;
        }
        int left = AppLock.FREE_TRIES - lock.failures();
        say("Wrong " + noun() + "." + (left > 0 && left < 3 ? " " + left + (left == 1 ? " try" : " tries")
                + " before a wait." : ""), true);
        handler.postDelayed(this::resetInput, 900);
    }

    private void resetInput() {
        pin.setLength(0);
        if (pinDots != null) {
            showPin();
        }
        if (pattern != null) {
            pattern.clear();
        }
    }

    /** Too many wrong guesses: count the wait down with the pad off. */
    private void tickWait() {
        if (mode == Mode.CREATE) {
            return;
        }
        long ms = lock.waitMs();
        boolean waiting = ms > 0;
        setInputEnabled(!waiting);
        if (!waiting) {
            return;
        }
        long s = (ms + 999) / 1000;
        say("Too many wrong tries. Try again in " + (s >= 60 ? s / 60 + " min " + s % 60 + " s" : s + " s") + ".",
                true);
        handler.postDelayed(() -> {
            tickWait();
            if (lock.waitMs() == 0) {
                say(initialMessage(), false);
                resetInput();
            }
        }, 1000);
    }

    private void setInputEnabled(boolean on) {
        if (keypad != null) {
            Ui.enable(keypad, on);
            for (int r = 0; r < keypad.getChildCount(); r++) {
                LinearLayout row = (LinearLayout) keypad.getChildAt(r);
                for (int c = 0; c < row.getChildCount(); c++) {
                    row.getChildAt(c).setEnabled(on);
                }
            }
        }
        if (pattern != null) {
            pattern.setInputEnabled(on);
        }
        if (fingerprintButton != null) {
            Ui.enable(fingerprintButton, on);
        }
    }

    private void say(String text, boolean error) {
        message.setText(text);
        message.setTextColor(error ? p.error : p.textDim);
    }

    private void cancel() {
        stop();
        if (onCancel != null) {
            onCancel.run();
        }
    }

    /** Back on the pad: cancels a change; on the app lock, it leaves the app. */
    void back() {
        if (mode == Mode.UNLOCK) {
            screen.moveTaskToBack(true);
        } else {
            cancel();
        }
    }

    // ---- fingerprint ----

    private void askFingerprint() {
        if (fingerprintCancel != null) {
            return;
        }
        fingerprintCancel = new CancellationSignal();
        FingerprintUnlock.authenticate(screen, "Unlock TwinSlot", "Use " + noun(), fingerprintCancel,
                new FingerprintUnlock.Callback() {
                    @Override
                    public void succeeded() {
                        fingerprintCancel = null;
                        succeed();
                    }

                    @Override
                    public void fallback() {
                        fingerprintCancel = null;
                    }

                    @Override
                    public void failed(CharSequence why) {
                        fingerprintCancel = null;
                        say(why.toString(), true);
                    }

                    @Override
                    public void invalidated() {
                        fingerprintCancel = null;
                        lock.setFingerprint(false);
                        if (fingerprintButton != null) {
                            fingerprintButton.setVisibility(View.GONE);
                        }
                        say("The phone's fingerprints changed, so fingerprint unlock was turned off. Use your "
                                + noun() + ", then turn it on again in Settings.", true);
                    }
                });
    }
}
