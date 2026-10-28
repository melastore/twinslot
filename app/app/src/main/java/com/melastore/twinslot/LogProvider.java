package com.melastore.twinslot;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Hands the diagnostic log file to the app the user shares it with, read-only
 * and only for the one share (the share grants that file's URI). Nothing else
 * in the app's storage can be reached through it.
 */
public class LogProvider extends ContentProvider {

    static final String AUTHORITY = "com.melastore.twinslot.logs";
    /** Under the cache directory. */
    static final String DIR = "shared-logs";

    static Uri uriFor(File f) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(f.getName()).build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The file a URI names, only if it is one of ours: one plain name inside the log directory. */
    private File file(Uri uri) throws FileNotFoundException {
        Context c = getContext();
        String name = uri.getLastPathSegment();
        if (c == null || name == null || uri.getPathSegments().size() != 1 || name.contains("/")
                || name.startsWith(".") || !name.endsWith(".txt")) {
            throw new FileNotFoundException(uri.toString());
        }
        File f = new File(new File(c.getCacheDir(), DIR), name);
        if (!f.isFile()) {
            throw new FileNotFoundException(uri.toString());
        }
        return f;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new SecurityException("read-only");
        }
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f;
        try {
            f = file(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        // Chat and mail apps ask for the name and size to show the attachment.
        String[] cols = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) {
                row[i] = f.getName();
            } else if (OpenableColumns.SIZE.equals(cols[i])) {
                row[i] = f.length();
            }
        }
        MatrixCursor cursor = new MatrixCursor(cols, 1);
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }
}
