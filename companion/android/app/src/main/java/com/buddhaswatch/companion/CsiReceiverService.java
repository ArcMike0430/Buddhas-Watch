package com.buddhaswatch.companion;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import androidx.core.app.NotificationCompat;

/**
 * CsiReceiverService — foreground service that keeps UDP listener alive
 * even when the app is in the background.
 *
 * The actual UDP reception is handled by CsiViewerViewModel.
 * This service exists only to hold the foreground notification so Android
 * does not kill the process during continuous monitoring sessions.
 */
public class CsiReceiverService extends Service {

    private static final String TAG         = "CsiReceiverSvc";
    private static final String CHANNEL_ID  = "csi_receiver";
    private static final int    NOTIF_ID    = 1;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        Notification notif = new NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Buddhas-Watch")
            .setContentText("CSI receiver running")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build();
        startForeground(NOTIF_ID, notif);
        Log.i(TAG, "Foreground service started");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "Service stopped");
    }

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID, "CSI Receiver", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Background CSI data reception from Buddhas-Watch");
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(channel);
    }
}
