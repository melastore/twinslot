package com.melastore.twinslot;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Build;
import android.os.CancellationSignal;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.security.keystore.KeyProperties;

import java.io.IOException;
import java.security.GeneralSecurityException;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

/**
 * Fingerprint unlock, bound to a Keystore key.
 *
 * A successful touch is only taken when the key it releases actually encrypts:
 * the key needs a strong biometric for every use, and Android destroys it when
 * a fingerprint is added. Someone who enrolls their own finger therefore gets
 * the PIN or pattern, not the app.
 */
final class FingerprintUnlock {

    interface Callback {
        void succeeded();

        /** The user chose the PIN or pattern instead, or dismissed the prompt. */
        void fallback();

        void failed(CharSequence why);

        /** The fingerprints changed since unlock was set up: it is off now. */
        void invalidated();
    }

    private static final String KEY = "twinslot_unlock";
    private static final String STORE = "AndroidKeyStore";

    private FingerprintUnlock() {
    }

    /** Why fingerprint unlock cannot be used on this phone right now, or null if it can. */
    static String unavailable(Context c) {
        if (!c.getPackageManager().hasSystemFeature(PackageManager.FEATURE_FINGERPRINT)) {
            return "This phone has no fingerprint sensor.";
        }
        BiometricManager bm = c.getSystemService(BiometricManager.class);
        if (bm == null) {
            return "Fingerprint unlock is not available.";
        }
        int r = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                ? bm.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG) : canAuthenticateQ(bm);
        switch (r) {
            case BiometricManager.BIOMETRIC_SUCCESS:
                return null;
            case BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED:
                return "No fingerprint is set up. Add one in Android's Settings › Security first.";
            case BiometricManager.BIOMETRIC_ERROR_HW_UNAVAILABLE:
                return "The fingerprint sensor is busy or unavailable right now.";
            default:
                return "This phone's fingerprint sensor is not strong enough for app unlock.";
        }
    }

    @SuppressWarnings("deprecation")
    private static int canAuthenticateQ(BiometricManager bm) {
        return bm.canAuthenticate();
    }

    /** Makes a fresh key; the old one (if any) and what it unlocked are gone. */
    static void createKey() throws GeneralSecurityException {
        deleteKey();
        KeyGenParameterSpec.Builder spec = new KeyGenParameterSpec.Builder(KEY,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setUserAuthenticationRequired(true)
                .setInvalidatedByBiometricEnrollment(true);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            spec.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG);
        }
        KeyGenerator kg = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, STORE);
        kg.init(spec.build());
        kg.generateKey();
    }

    static void deleteKey() {
        try {
            KeyStore ks = KeyStore.getInstance(STORE);
            ks.load(null);
            ks.deleteEntry(KEY);
        } catch (GeneralSecurityException | IOException ignored) {
            // No key store or no key: nothing to delete.
        }
    }

    /**
     * Shows the system fingerprint prompt. {@code fallbackLabel} names the way
     * out ("Use PIN"). The callback runs on the main thread.
     */
    static void authenticate(Activity activity, String title, String fallbackLabel, CancellationSignal cancel,
                             Callback callback) {
        Cipher cipher;
        try {
            KeyStore ks = KeyStore.getInstance(STORE);
            ks.load(null);
            SecretKey key = (SecretKey) ks.getKey(KEY, null);
            if (key == null) {
                callback.invalidated();
                return;
            }
            cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key);
        } catch (KeyPermanentlyInvalidatedException e) {
            deleteKey();
            callback.invalidated();
            return;
        } catch (GeneralSecurityException | IOException e) {
            callback.failed(e.getMessage() == null ? e.toString() : e.getMessage());
            return;
        }

        BiometricPrompt.Builder b = new BiometricPrompt.Builder(activity)
                .setTitle(title)
                .setDescription("Touch the fingerprint sensor")
                .setConfirmationRequired(false)
                .setNegativeButton(fallbackLabel, activity.getMainExecutor(), (d, w) -> callback.fallback());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            b.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG);
        }
        b.build().authenticate(new BiometricPrompt.CryptoObject(cipher), cancel, activity.getMainExecutor(),
                new BiometricPrompt.AuthenticationCallback() {
                    @Override
                    public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
                        try {
                            // Only an authenticated key encrypts: this is the proof, not the callback itself.
                            result.getCryptoObject().getCipher().doFinal(new byte[16]);
                            callback.succeeded();
                        } catch (GeneralSecurityException e) {
                            callback.failed("The fingerprint was not accepted by the key store.");
                        }
                    }

                    @Override
                    public void onAuthenticationError(int code, CharSequence message) {
                        if (code == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED
                                || code == BiometricPrompt.BIOMETRIC_ERROR_CANCELED) {
                            callback.fallback();
                        } else {
                            callback.failed(message);
                        }
                    }
                });
    }
}
