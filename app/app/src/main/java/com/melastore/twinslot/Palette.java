package com.melastore.twinslot;

import android.content.Context;
import android.content.res.Configuration;
import android.os.Build;

/**
 * Material 3 color roles, light or dark: the wallpaper's (Android 12+) or one of a few fixed accents,
 * as the settings choose.
 */
final class Palette {

    final boolean dark;
    final int background;
    final int surface;
    final int surfaceHigh;
    final int outline;
    final int outlineVariant;
    final int text;
    final int textDim;
    final int primary;
    final int onPrimary;
    final int primaryContainer;
    final int onPrimaryContainer;
    final int tertiary;
    final int onTertiary;
    final int tertiaryContainer;
    final int onTertiaryContainer;
    final int error;
    final int errorContainer;
    final int onErrorContainer;
    final int warnContainer;
    final int onWarnContainer;
    final int good;

    /**
     * Fixed accents: primary, on primary, primary container, on primary container, then the same four for
     * tertiary (slot B's color). Light first, then dark.
     */
    private static final int[][][] ACCENTS = {
        // BLUE
        {{0xFF3D5F90, 0xFFFFFFFF, 0xFFD5E3FF, 0xFF001B3C, 0xFF705574, 0xFFFFFFFF, 0xFFFAD8FD, 0xFF28132E},
         {0xFFA8C8FF, 0xFF07305F, 0xFF254777, 0xFFD5E3FF, 0xFFDDBCE0, 0xFF3F2844, 0xFF573E5C, 0xFFFAD8FD}},
        // TEAL
        {{0xFF006A6A, 0xFFFFFFFF, 0xFF9CF1F0, 0xFF002020, 0xFF4B607C, 0xFFFFFFFF, 0xFFD3E4FF, 0xFF041C35},
         {0xFF80D5D4, 0xFF003737, 0xFF004F4F, 0xFF9CF1F0, 0xFFB3C8E8, 0xFF1C314B, 0xFF334863, 0xFFD3E4FF}},
        // GREEN
        {{0xFF3B6939, 0xFFFFFFFF, 0xFFBCF0B4, 0xFF002204, 0xFF39656B, 0xFFFFFFFF, 0xFFBCEBF1, 0xFF001F23},
         {0xFFA1D39A, 0xFF0A390F, 0xFF235024, 0xFFBCF0B4, 0xFFA1CED5, 0xFF00363C, 0xFF1F4D53, 0xFFBCEBF1}},
        // PURPLE
        {{0xFF6750A4, 0xFFFFFFFF, 0xFFEADDFF, 0xFF21005D, 0xFF7D5260, 0xFFFFFFFF, 0xFFFFD8E4, 0xFF31111D},
         {0xFFD0BCFF, 0xFF381E72, 0xFF4F378B, 0xFFEADDFF, 0xFFEFB8C8, 0xFF492532, 0xFF633B48, 0xFFFFD8E4}},
        // AMBER
        {{0xFF845400, 0xFFFFFFFF, 0xFFFFDDB3, 0xFF291800, 0xFF4D6544, 0xFFFFFFFF, 0xFFCFEBC1, 0xFF0B2007},
         {0xFFFFB951, 0xFF452B00, 0xFF633F00, 0xFFFFDDB3, 0xFFB3CEA6, 0xFF1F361A, 0xFF354D2E, 0xFFCFEBC1}},
        // ROSE
        {{0xFF984061, 0xFFFFFFFF, 0xFFFFD9E2, 0xFF3E001D, 0xFF7C5635, 0xFFFFFFFF, 0xFFFFDCC2, 0xFF2E1500},
         {0xFFFFB0C8, 0xFF5E1133, 0xFF7B2949, 0xFFFFD9E2, 0xFFEFBD94, 0xFF48290C, 0xFF623F20, 0xFFFFDCC2}},
    };

    Palette(Context c) {
        Prefs prefs = new Prefs(c);
        dark = (c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
        Prefs.Accent accent = prefs.accent();
        int[] n;
        int[] a;
        if (accent == Prefs.Accent.WALLPAPER && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            n = wallpaperNeutrals(c, dark);
            a = wallpaperAccent(c, dark);
        } else {
            n = dark ? new int[] {0xFF0F1115, 0xFF1B1E24, 0xFF262A31, 0xFF8D9199, 0xFF43474E, 0xFFE2E2E9, 0xFFC3C6CF}
                    : new int[] {0xFFF9F9FF, 0xFFEFF0F7, 0xFFE2E2E9, 0xFF73777F, 0xFFC3C6CF, 0xFF191C20, 0xFF43474E};
            a = ACCENTS[accent.ordinal() - 1][dark ? 1 : 0];
        }
        if (dark && prefs.pureBlack()) {
            // For OLED panels: black page, cards just off it.
            n[0] = 0xFF000000;
            n[1] = blend(0xFF000000, n[2], 0.45f);
        }
        background = n[0];
        surface = n[1];
        surfaceHigh = n[2];
        outline = n[3];
        outlineVariant = n[4];
        text = n[5];
        textDim = n[6];
        primary = a[0];
        onPrimary = a[1];
        primaryContainer = a[2];
        onPrimaryContainer = a[3];
        tertiary = a[4];
        onTertiary = a[5];
        tertiaryContainer = a[6];
        onTertiaryContainer = a[7];
        if (dark) {
            error = 0xFFFFB4AB;
            errorContainer = 0xFF93000A;
            onErrorContainer = 0xFFFFDAD6;
            warnContainer = 0xFF4A3B00;
            onWarnContainer = 0xFFFFE08B;
            good = 0xFF8BD69C;
        } else {
            error = 0xFFBA1A1A;
            errorContainer = 0xFFFFDAD6;
            onErrorContainer = 0xFF410002;
            warnContainer = 0xFFFFEFC4;
            onWarnContainer = 0xFF3D2F00;
            good = 0xFF1E7A3C;
        }
    }

    /** The color that stands for an accent in the picker. */
    static int swatch(Context c, Prefs.Accent accent, boolean dark) {
        if (accent == Prefs.Accent.WALLPAPER) {
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? wallpaperAccent(c, dark)[0] : 0;
        }
        return ACCENTS[accent.ordinal() - 1][dark ? 1 : 0][0];
    }

    /** Background, surface, surface high, outline, outline variant, text, dim text. */
    private static int[] wallpaperNeutrals(Context c, boolean dark) {
        int n1_10 = c.getColor(android.R.color.system_neutral1_10);
        int n1_50 = c.getColor(android.R.color.system_neutral1_50);
        int n1_900 = c.getColor(android.R.color.system_neutral1_900);
        int n2_50 = c.getColor(android.R.color.system_neutral2_50);
        int n2_100 = c.getColor(android.R.color.system_neutral2_100);
        int n2_200 = c.getColor(android.R.color.system_neutral2_200);
        int n2_500 = c.getColor(android.R.color.system_neutral2_500);
        int n2_700 = c.getColor(android.R.color.system_neutral2_700);
        int n2_800 = c.getColor(android.R.color.system_neutral2_800);
        if (dark) {
            return new int[] {blend(n1_900, 0xFF000000, 0.35f), blend(n1_900, n2_800, 0.45f), n2_800, n2_500, n2_700,
                n1_50, n2_200};
        }
        return new int[] {n1_10, blend(n2_50, n1_10, 0.3f), n2_100, n2_500, n2_200, n1_900, n2_700};
    }

    /** The eight accent roles, in the order of {@link #ACCENTS}. */
    private static int[] wallpaperAccent(Context c, boolean dark) {
        if (dark) {
            return new int[] {c.getColor(android.R.color.system_accent1_200),
                c.getColor(android.R.color.system_accent1_800), c.getColor(android.R.color.system_accent1_700),
                c.getColor(android.R.color.system_accent1_100), c.getColor(android.R.color.system_accent3_200),
                c.getColor(android.R.color.system_accent3_800), c.getColor(android.R.color.system_accent3_700),
                c.getColor(android.R.color.system_accent3_100)};
        }
        return new int[] {c.getColor(android.R.color.system_accent1_600), 0xFFFFFFFF,
            c.getColor(android.R.color.system_accent1_100), c.getColor(android.R.color.system_accent1_900),
            c.getColor(android.R.color.system_accent3_600), 0xFFFFFFFF,
            c.getColor(android.R.color.system_accent3_100), c.getColor(android.R.color.system_accent3_900)};
    }

    /** Slot A wears the primary color, slot B the tertiary one. */
    int slot(int slot) {
        return slot == 0 ? primary : tertiary;
    }

    int onSlot(int slot) {
        return slot == 0 ? onPrimary : onTertiary;
    }

    int slotContainer(int slot) {
        return slot == 0 ? primaryContainer : tertiaryContainer;
    }

    int onSlotContainer(int slot) {
        return slot == 0 ? onPrimaryContainer : onTertiaryContainer;
    }

    static int blend(int a, int b, float t) {
        int r = Math.round(((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t);
        int g = Math.round(((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t);
        int bl = Math.round((a & 0xFF) * (1 - t) + (b & 0xFF) * t);
        return 0xFF000000 | (r << 16) | (g << 8) | bl;
    }

    static int alpha(int color, float a) {
        return (Math.round(a * 255) << 24) | (color & 0xFFFFFF);
    }
}
