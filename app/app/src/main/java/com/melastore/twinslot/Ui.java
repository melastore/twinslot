package com.melastore.twinslot;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/** The few building blocks the screen is made of, in the palette's colors. */
final class Ui {

    enum Style { FILLED, TONAL, OUTLINED, TEXT, DANGER, DANGER_OUTLINED }

    final Context context;
    final Palette p;
    private final Typeface regular = Typeface.create("sans-serif", Typeface.NORMAL);
    private final Typeface medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    private final Typeface bold = Typeface.create("sans-serif", Typeface.BOLD);

    Ui(Context context, Palette palette) {
        this.context = context;
        this.p = palette;
    }

    int dp(float v) {
        return Math.round(v * context.getResources().getDisplayMetrics().density);
    }

    // ---- text ----

    TextView text(CharSequence s, float sp, int color) {
        TextView t = new TextView(context);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTypeface(regular);
        t.setTextColor(color);
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    TextView body(CharSequence s) {
        return text(s, 14, p.textDim);
    }

    TextView medium(CharSequence s, float sp, int color) {
        TextView t = text(s, sp, color);
        t.setTypeface(medium);
        t.setLineSpacing(0, 1f);
        return t;
    }

    TextView headline(CharSequence s, float sp, int color) {
        TextView t = text(s, sp, color);
        t.setTypeface(bold);
        t.setLineSpacing(0, 1f);
        t.setLetterSpacing(-0.01f);
        return t;
    }

    /** Small caps label above a block. */
    TextView overline(CharSequence s, int color) {
        TextView t = medium(s.toString().toUpperCase(java.util.Locale.ROOT), 11.5f, color);
        t.setLetterSpacing(0.08f);
        return t;
    }

    TextView sectionTitle(CharSequence s) {
        TextView t = medium(s, 15, p.primary);
        t.setPadding(dp(4), 0, 0, dp(10));
        return t;
    }

    TextView mono(CharSequence s, int color) {
        TextView t = text(s, 11.5f, color);
        t.setTypeface(Typeface.MONOSPACE);
        t.setLineSpacing(0, 1.1f);
        return t;
    }

    /** The system bars and cutout a window must keep clear of. */
    @SuppressWarnings("deprecation")
    static android.graphics.Insets bars(android.view.WindowInsets in) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            return in.getInsets(android.view.WindowInsets.Type.systemBars()
                    | android.view.WindowInsets.Type.displayCutout());
        }
        return in.getSystemWindowInsets();
    }

    // ---- shapes ----

    GradientDrawable shape(int color, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    LinearLayout column() {
        LinearLayout l = new LinearLayout(context);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    LinearLayout row() {
        LinearLayout l = new LinearLayout(context);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    LinearLayout card(int color) {
        LinearLayout c = column();
        c.setBackground(shape(color, 28));
        c.setPadding(dp(20), dp(20), dp(20), dp(20));
        return c;
    }

    /** A rounded square holding a slot's letter. */
    TextView badge(char letter, int bg, int fg, int sizeDp) {
        TextView t = headline(String.valueOf(letter), sizeDp * 0.45f, fg);
        t.setGravity(Gravity.CENTER);
        t.setBackground(shape(bg, sizeDp * 0.32f));
        t.setLayoutParams(new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp)));
        return t;
    }

    /** A small rounded label, optionally with an icon. */
    TextView pill(CharSequence s, int bg, int fg, int icon) {
        TextView t = medium(s, 12, fg);
        t.setBackground(shape(bg, 100));
        t.setPadding(dp(icon != 0 ? 8 : 10), dp(4), dp(10), dp(4));
        t.setGravity(Gravity.CENTER_VERTICAL);
        if (icon != 0) {
            t.setCompoundDrawablesRelative(icon(icon, fg, 14), null, null, null);
            t.setCompoundDrawablePadding(dp(4));
        }
        return t;
    }

    Drawable icon(int res, int color, int sizeDp) {
        Drawable d = context.getDrawable(res).mutate();
        d.setTint(color);
        d.setBounds(0, 0, dp(sizeDp), dp(sizeDp));
        return d;
    }

    ImageView iconView(int res, int color, int sizeDp) {
        ImageView v = new ImageView(context);
        v.setImageDrawable(icon(res, color, sizeDp));
        v.setLayoutParams(new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp)));
        return v;
    }

    // ---- controls ----

    Button button(CharSequence label, Style style, int icon) {
        int bg;
        int fg;
        int stroke = 0;
        switch (style) {
            case FILLED: bg = p.primary; fg = p.onPrimary; break;
            case TONAL: bg = p.primaryContainer; fg = p.onPrimaryContainer; break;
            case DANGER: bg = p.errorContainer; fg = p.onErrorContainer; break;
            case DANGER_OUTLINED: bg = 0; fg = p.error; stroke = p.outlineVariant; break;
            case OUTLINED: bg = 0; fg = p.primary; stroke = p.outlineVariant; break;
            default: bg = 0; fg = p.primary; break;
        }
        Button b = new Button(context);
        b.setText(label);
        b.setAllCaps(false);
        b.setTypeface(medium);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setTextColor(fg);
        b.setStateListAnimator(null);
        b.setElevation(0);
        b.setMinHeight(dp(style == Style.TEXT ? 40 : 52));
        b.setMinimumHeight(dp(style == Style.TEXT ? 40 : 52));
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setGravity(Gravity.CENTER);
        b.setSingleLine(true);
        b.setEllipsize(TextUtils.TruncateAt.END);
        GradientDrawable shape = shape(bg, 100);
        if (stroke != 0) {
            shape.setStroke(dp(1), stroke);
        }
        GradientDrawable mask = shape(0xFFFFFFFF, 100);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(Palette.alpha(fg, 0.16f)), shape, mask));
        setLabel(b, label, icon, fg);
        b.setPadding(dp(style == Style.TEXT ? 12 : 22), 0, dp(style == Style.TEXT ? 12 : 22), 0);
        return b;
    }

    /** The label with its icon in front, so both stay centered together. */
    void setLabel(Button b, CharSequence label, int icon, int color) {
        if (icon == 0) {
            b.setText(label);
            return;
        }
        android.text.SpannableString s = new android.text.SpannableString("\u00A0\u00A0\u00A0" + label);
        s.setSpan(new android.text.style.ImageSpan(icon(icon, color, 18), android.text.style.ImageSpan.ALIGN_CENTER),
                0, 1, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        b.setText(s);
    }

    /** Disabled buttons fade, as Material does. */
    static void enable(View v, boolean enabled) {
        v.setEnabled(enabled);
        v.setAlpha(enabled ? 1f : 0.38f);
    }

    interface Toggle {
        void set(boolean value);
    }

    /** A title, a line under it and a switch; the whole row toggles. */
    View switchRow(CharSequence title, CharSequence sub, boolean checked, Toggle toggle) {
        LinearLayout row = row();
        row.setPadding(0, dp(10), 0, dp(10));
        LinearLayout texts = column();
        texts.addView(medium(title, 15.5f, p.text));
        if (sub != null) {
            TextView s = body(sub);
            s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            texts.addView(s);
        }
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Switch sw = new Switch(context);
        sw.setChecked(checked);
        int[][] states = {{android.R.attr.state_checked}, {}};
        sw.setThumbTintList(new ColorStateList(states, new int[] {p.primary, p.outline}));
        sw.setTrackTintList(new ColorStateList(states, new int[] {p.primary, p.surfaceHigh}));
        sw.setOnCheckedChangeListener((b, v) -> toggle.set(v));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMarginStart(dp(12));
        row.addView(sw, lp);
        row.setOnClickListener(v -> sw.toggle());
        row.setBackground(new RippleDrawable(ColorStateList.valueOf(Palette.alpha(p.text, 0.08f)), null,
                shape(0xFFFFFFFF, 12)));
        return row;
    }

    /** A round, borderless icon button. */
    android.widget.ImageButton iconButton(int icon, int color, CharSequence description) {
        android.widget.ImageButton b = new android.widget.ImageButton(context);
        b.setImageDrawable(icon(icon, color, 24));
        b.setContentDescription(description);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(Palette.alpha(color, 0.14f)), null,
                shape(0xFFFFFFFF, 100)));
        b.setLayoutParams(new LinearLayout.LayoutParams(dp(48), dp(48)));
        b.setScaleType(ImageView.ScaleType.CENTER);
        return b;
    }

    /** A settings row: an optional icon, a title, a line under it and an action for the whole row. */
    LinearLayout actionRow(int icon, CharSequence title, CharSequence sub, View.OnClickListener click) {
        LinearLayout row = row();
        row.setPadding(0, dp(12), 0, dp(12));
        if (icon != 0) {
            ImageView i = iconView(icon, p.textDim, 22);
            ((LinearLayout.LayoutParams) i.getLayoutParams()).setMarginEnd(dp(16));
            row.addView(i);
        }
        LinearLayout texts = column();
        texts.addView(medium(title, 15.5f, p.text));
        if (sub != null) {
            TextView s = body(sub);
            s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            texts.addView(s);
        }
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        if (click != null) {
            row.setOnClickListener(click);
            row.setBackground(new RippleDrawable(ColorStateList.valueOf(Palette.alpha(p.text, 0.08f)), null,
                    shape(0xFFFFFFFF, 12)));
        }
        return row;
    }

    /** A block of text in a fixed-width font, with a button that copies it. */
    LinearLayout codeBlock(String code) {
        LinearLayout b = row();
        b.setBackground(shape(p.surfaceHigh, 14));
        b.setPadding(dp(14), dp(4), dp(4), dp(4));
        TextView t = mono(code, p.text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setTextIsSelectable(true);
        b.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        android.widget.ImageButton copy = iconButton(R.drawable.ic_copy, p.primary, "Copy");
        copy.setOnClickListener(v -> {
            android.content.ClipboardManager cm = context.getSystemService(android.content.ClipboardManager.class);
            cm.setPrimaryClip(android.content.ClipData.newPlainText("TwinSlot", code));
            if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
                // Android 13+ confirms the copy itself.
                android.widget.Toast.makeText(context, "Copied", android.widget.Toast.LENGTH_SHORT).show();
            }
        });
        b.addView(copy);
        return b;
    }

    /** A numbered step: a small badge and its text. */
    LinearLayout step(int number, CharSequence text) {
        LinearLayout row = row();
        row.setGravity(Gravity.TOP);
        row.addView(badge((char) ('0' + number), p.surfaceHigh, p.text, 24));
        TextView t = text(text, 14, p.text);
        t.setPadding(dp(12), dp(2), 0, 0);
        row.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    /** An icon, a bold line and text, on a tinted block. */
    LinearLayout banner(int icon, CharSequence title, CharSequence body, int bg, int fg) {
        LinearLayout b = row();
        b.setGravity(Gravity.TOP);
        b.setBackground(shape(bg, 20));
        b.setPadding(dp(16), dp(14), dp(16), dp(14));
        ImageView i = iconView(icon, fg, 20);
        LinearLayout.LayoutParams ilp = (LinearLayout.LayoutParams) i.getLayoutParams();
        ilp.setMarginEnd(dp(12));
        ilp.topMargin = dp(1);
        b.addView(i);
        LinearLayout texts = column();
        if (title != null) {
            texts.addView(medium(title, 14.5f, fg));
        }
        if (body != null) {
            TextView t = text(body, 13.5f, fg);
            t.setAlpha(0.9f);
            if (title != null) {
                t.setPadding(0, dp(2), 0, 0);
            }
            texts.addView(t);
        }
        b.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return b;
    }

    /** Left text, right text, on one line. */
    LinearLayout pair(View left, View right) {
        LinearLayout r = row();
        r.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        r.addView(right);
        return r;
    }

    View dot(int color) {
        View v = new View(context);
        v.setBackground(shape(color, 100));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(10), dp(10));
        lp.setMarginEnd(dp(10));
        v.setLayoutParams(lp);
        return v;
    }

    View divider() {
        View v = new View(context);
        v.setBackgroundColor(p.outlineVariant);
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(1) / 2)));
        return v;
    }

    void tint(SeekBar bar, int color) {
        bar.setProgressTintList(ColorStateList.valueOf(color));
        bar.setThumbTintList(ColorStateList.valueOf(color));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(p.outlineVariant));
    }

    LinearLayout.LayoutParams top(int topDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topDp);
        return lp;
    }

    LinearLayout.LayoutParams wrapTop(int topDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topDp);
        return lp;
    }

    /** A vertical column that grows no wider than its maximum. */
    static final class Column extends LinearLayout {
        private final int maxWidth;

        Column(Context context, int maxWidthPx) {
            super(context);
            setOrientation(VERTICAL);
            maxWidth = maxWidthPx;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int w = MeasureSpec.getSize(widthSpec);
            if (w > maxWidth) {
                widthSpec = MeasureSpec.makeMeasureSpec(maxWidth, MeasureSpec.EXACTLY);
            }
            super.onMeasure(widthSpec, heightSpec);
        }
    }

    /** A rounded bar of colored parts over a track: storage use, the split, progress. */
    static final class Bar extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final int track;
        private float[] parts = {};
        private int[] colors = {};
        private final float gap;
        private final int height;

        Bar(Context context, int track, float gapPx, int heightPx) {
            super(context);
            this.track = track;
            this.gap = gapPx;
            this.height = heightPx;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            setMeasuredDimension(MeasureSpec.getSize(widthSpec), height);
        }

        void set(float[] fractions, int[] partColors) {
            parts = fractions;
            colors = partColors;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight();
            float w = getWidth();
            float r = h / 2;
            paint.setColor(track);
            rect.set(0, 0, w, h);
            c.drawRoundRect(rect, r, r, paint);
            float x = 0;
            for (int i = 0; i < parts.length; i++) {
                float pw = Math.max(0, Math.min(1, parts[i])) * w;
                if (pw <= 0) {
                    continue;
                }
                float left = x + (i > 0 ? gap / 2 : 0);
                float right = Math.min(w, x + pw - (i < parts.length - 1 ? gap / 2 : 0));
                if (right - left < h) {
                    right = Math.min(w, left + h);
                }
                paint.setColor(colors[i]);
                rect.set(left, 0, right, h);
                c.drawRoundRect(rect, r, r, paint);
                x += pw;
            }
        }
    }

    Bar bar(int track, int heightDp, float gapDp) {
        Bar b = new Bar(context, track, dp(gapDp), dp(heightDp));
        b.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(heightDp)));
        return b;
    }
}
