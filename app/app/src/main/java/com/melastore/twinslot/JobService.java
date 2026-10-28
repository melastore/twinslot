package com.melastore.twinslot;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.IOException;

/**
 * Watches a running job from outside the screen: its progress in a
 * notification, a wake lock so the phone does not sleep through it, and a
 * last notification with the result.
 *
 * The job itself runs as root, detached; this service only reads its log. If
 * the app is killed the job goes on, and opening the app again restarts the watch.
 */
public class JobService extends Service {

    private static final String CHANNEL_RUNNING = "job";
    private static final String CHANNEL_RESULT = "result";
    private static final int ID_RUNNING = 1;
    static final int ID_RESULT = 2;
    private static final long POLL_MS = 1000;
    /** Consecutive failed polls before the watch gives up on the job. */
    private static final int MAX_POLL_FAILURES = 10;
    /** Longest an install has any business taking; the wake lock lets go after it regardless. */
    private static final long WAKE_LIMIT_MS = 2 * 60 * 60 * 1000L;

    private Thread watcher;
    private PowerManager.WakeLock wakeLock;

    /** Starts watching the job, if nothing watches it yet. Call while the app is on screen. */
    static void start(Context context) {
        context.startForegroundService(new Intent(context, JobService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        channels(this);
        Prefs prefs = new Prefs(this);
        startForeground(ID_RUNNING, running(prefs.jobTitle(), null, 0, true),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        if (watcher == null) {
            if (prefs.stayAwake()) {
                PowerManager pm = getSystemService(PowerManager.class);
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "TwinSlot:job");
                wakeLock.setReferenceCounted(false);
                wakeLock.acquire(WAKE_LIMIT_MS);
            }
            watcher = new Thread(this::watch, "job-watch");
            watcher.start();
        }
        return START_NOT_STICKY;
    }

    private void watch() {
        Twinslot ts = new Twinslot(this);
        Prefs prefs = new Prefs(this);
        NotificationManager nm = getSystemService(NotificationManager.class);
        JobText.Eta eta = new JobText.Eta();
        long since = System.currentTimeMillis();
        Twinslot.Job job = null;
        int failures = 0;
        while (!Thread.currentThread().isInterrupted()) {
            try {
                job = ts.poll();
                failures = 0;
            } catch (IOException e) {
                // One failed su call is no reason to drop the wake lock of a job that is still writing.
                if (++failures < MAX_POLL_FAILURES && job != null) {
                    try {
                        Thread.sleep(POLL_MS);
                    } catch (InterruptedException ie) {
                        break;
                    }
                    continue;
                }
                job = null;
            }
            boolean starting = job != null && !job.started && !job.finished
                    && System.currentTimeMillis() - since < 15000;
            if (job == null || !job.running && !starting) {
                break;
            }
            eta.sample(job.step, job.permille);
            nm.notify(ID_RUNNING, running(prefs.jobTitle(), job, job.permille, !job.started, eta.text()));
            try {
                Thread.sleep(POLL_MS);
            } catch (InterruptedException e) {
                break;
            }
        }
        finish(job, prefs.jobTitle());
    }

    private void finish(Twinslot.Job job, String title) {
        stopForeground(STOP_FOREGROUND_REMOVE);
        if (job == null) {
            Diagnostics.note(this, "job watch ended: the job could not be read");
        } else {
            Diagnostics.note(this, "job end: " + (job.finished ? "exit " + job.code : "no exit code (stopped)")
                    + ", last step \"" + job.step + "\", " + job.permille / 10 + "%"
                    + (job.lines.isEmpty() ? "" : ", last line: " + job.lines.get(job.lines.size() - 1)));
        }
        if (job != null && (job.finished || job.started)) {
            getSystemService(NotificationManager.class).notify(ID_RESULT, result(title, job));
        }
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (watcher != null) {
            watcher.interrupt();
        }
        if (wakeLock != null && wakeLock.isHeld()) {
            wakeLock.release();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- notifications ----

    static void channels(Context c) {
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        NotificationChannel run = new NotificationChannel(CHANNEL_RUNNING, "Install progress",
                NotificationManager.IMPORTANCE_LOW);
        run.setDescription("While a ROM installs or the zip is checked");
        run.setShowBadge(false);
        NotificationChannel done = new NotificationChannel(CHANNEL_RESULT, "Install result",
                NotificationManager.IMPORTANCE_DEFAULT);
        done.setDescription("When an install finishes or fails");
        nm.createNotificationChannel(run);
        nm.createNotificationChannel(done);
    }

    private PendingIntent openApp() {
        Intent i = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification running(String title, Twinslot.Job job, int permille, boolean indeterminate) {
        return running(title, job, permille, indeterminate, null);
    }

    private Notification running(String title, Twinslot.Job job, int permille, boolean indeterminate, String eta) {
        String step = job == null ? "" : job.step;
        String text = JobText.phase(step) + (indeterminate ? "" : " · " + permille / 10 + "%");
        Notification.Builder b = new Notification.Builder(this, CHANNEL_RUNNING)
                .setSmallIcon(R.drawable.ic_twinslot)
                .setContentTitle(title)
                .setContentText(text)
                .setSubText(eta)
                .setStyle(new Notification.BigTextStyle().bigText(text + "\n" + JobText.detail(step)
                        + "\nDo not restart the phone."))
                .setProgress(1000, permille, indeterminate)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(openApp());
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        }
        return b.build();
    }

    private Notification result(String title, Twinslot.Job job) {
        boolean ok = job.finished && job.code == 0;
        String head;
        String text;
        if (ok) {
            head = job.installed() ? "Installed" : "Done";
            text = job.installed() ? "Every partition matches the ROM. Open TwinSlot to reboot into it."
                    : "Finished without errors.";
        } else {
            head = job.finished ? "Install failed" : "Install stopped";
            String why = JobText.explain(job.lines);
            text = why != null ? why : job.finished ? "Open TwinSlot to see what happened."
                    : "It ended without a result. Open TwinSlot and check the slot before booting it.";
        }
        return new Notification.Builder(this, CHANNEL_RESULT)
                .setSmallIcon(ok ? R.drawable.ic_check : R.drawable.ic_error)
                .setContentTitle(head + ": " + title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setAutoCancel(true)
                .setContentIntent(openApp())
                .build();
    }
}
