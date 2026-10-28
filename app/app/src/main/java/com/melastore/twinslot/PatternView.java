package com.melastore.twinslot;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/** A 3 by 3 grid of dots to draw an unlock pattern on, like Android's own lock screen. */
final class PatternView extends View {

    interface Listener {
        /** The finger lifted; {@code dots} holds the numbers 0..8 in the order they were joined. */
        void drawn(List<Integer> dots);
    }

    private final Paint dotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Integer> dots = new ArrayList<>();
    private final boolean[] used = new boolean[9];
    private final int color;
    private final int dim;
    private final int errorColor;
    private final int maxSize;
    private Listener listener;
    private float fingerX = -1;
    private float fingerY = -1;
    private boolean error;
    private boolean enabled = true;

    PatternView(Context context, int color, int dim, int errorColor, int maxSizePx) {
        super(context);
        this.color = color;
        this.dim = dim;
        this.errorColor = errorColor;
        this.maxSize = maxSizePx;
        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeCap(Paint.Cap.ROUND);
        linePaint.setStrokeJoin(Paint.Join.ROUND);
        setContentDescription("Pattern area. Use the PIN instead if you cannot draw.");
    }

    void setListener(Listener l) {
        listener = l;
    }

    /** Shows the last pattern in the error color until the next touch. */
    void showError() {
        error = true;
        invalidate();
    }

    void clear() {
        dots.clear();
        java.util.Arrays.fill(used, false);
        error = false;
        fingerX = fingerY = -1;
        invalidate();
    }

    void setInputEnabled(boolean on) {
        enabled = on;
        setAlpha(on ? 1f : 0.38f);
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int w = MeasureSpec.getSize(widthSpec);
        int size = Math.min(w, maxSize);
        setMeasuredDimension(w, size);
    }

    private float cell() {
        return Math.min(getWidth(), getHeight()) / 3f;
    }

    private float cx(int dot) {
        float side = Math.min(getWidth(), getHeight());
        return (getWidth() - side) / 2f + cell() * (dot % 3 + 0.5f);
    }

    private float cy(int dot) {
        return cell() * (dot / 3 + 0.5f);
    }

    /** The dot under (x, y), if the finger is close enough to its center. */
    private int hit(float x, float y) {
        float reach = cell() * 0.36f;
        for (int d = 0; d < 9; d++) {
            float dx = x - cx(d);
            float dy = y - cy(d);
            if (dx * dx + dy * dy <= reach * reach) {
                return d;
            }
        }
        return -1;
    }

    private void add(int d) {
        if (used[d]) {
            return;
        }
        if (!dots.isEmpty()) {
            // Passing straight over a dot joins it, as on Android's lock screen.
            int last = dots.get(dots.size() - 1);
            int mid = middle(last, d);
            if (mid >= 0 && !used[mid]) {
                used[mid] = true;
                dots.add(mid);
            }
        }
        used[d] = true;
        dots.add(d);
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    /** The dot halfway between two others on the grid, or -1. */
    private static int middle(int a, int b) {
        int ar = a / 3;
        int ac = a % 3;
        int br = b / 3;
        int bc = b % 3;
        if ((ar + br) % 2 != 0 || (ac + bc) % 2 != 0) {
            return -1;
        }
        int m = (ar + br) / 2 * 3 + (ac + bc) / 2;
        return m == a || m == b ? -1 : m;
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!enabled) {
            return false;
        }
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                clear();
                getParent().requestDisallowInterceptTouchEvent(true);
                // fall through
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getHistorySize(); i++) {
                    track(e.getHistoricalX(i), e.getHistoricalY(i));
                }
                track(e.getX(), e.getY());
                invalidate();
                return true;
            case MotionEvent.ACTION_UP:
                fingerX = fingerY = -1;
                invalidate();
                if (!dots.isEmpty() && listener != null) {
                    listener.drawn(new ArrayList<>(dots));
                }
                return true;
            case MotionEvent.ACTION_CANCEL:
                clear();
                return true;
            default:
                return false;
        }
    }

    private void track(float x, float y) {
        int d = hit(x, y);
        if (d >= 0) {
            add(d);
        }
        fingerX = x;
        fingerY = y;
    }

    @Override
    protected void onDraw(Canvas c) {
        float cell = cell();
        int ink = error ? errorColor : color;
        linePaint.setColor(Palette.alpha(ink, 0.55f));
        linePaint.setStrokeWidth(cell * 0.07f);
        for (int i = 1; i < dots.size(); i++) {
            c.drawLine(cx(dots.get(i - 1)), cy(dots.get(i - 1)), cx(dots.get(i)), cy(dots.get(i)), linePaint);
        }
        if (!dots.isEmpty() && fingerX >= 0) {
            int last = dots.get(dots.size() - 1);
            c.drawLine(cx(last), cy(last), fingerX, fingerY, linePaint);
        }
        for (int d = 0; d < 9; d++) {
            boolean on = used[d];
            dotPaint.setColor(on ? ink : dim);
            c.drawCircle(cx(d), cy(d), cell * (on ? 0.085f : 0.06f), dotPaint);
            if (on) {
                dotPaint.setColor(Palette.alpha(ink, 0.16f));
                c.drawCircle(cx(d), cy(d), cell * 0.22f, dotPaint);
            }
        }
    }
}
