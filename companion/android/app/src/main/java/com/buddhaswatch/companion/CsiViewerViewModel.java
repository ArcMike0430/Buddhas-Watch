package com.buddhaswatch.companion;

import android.content.Context;
import android.util.Log;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.google.gson.Gson;
import com.google.gson.JsonObject;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * CsiViewerViewModel — processes incoming CSI data from BLE or UDP,
 * maintains packet history, and provides export functionality.
 */
public class CsiViewerViewModel extends ViewModel {

    private static final String TAG          = "CsiViewerVM";
    private static final int    UDP_PORT     = 5500;
    private static final int    MAX_HISTORY  = 200;
    private static final int    MAX_SUBCARRIERS = 52;

    private final MutableLiveData<CsiPacket> mLatestPacket = new MutableLiveData<>();
    private final MutableLiveData<String>    mStatsText    = new MutableLiveData<>("Waiting...");
    private final List<CsiPacket>            mHistory      = new ArrayList<>();
    private final Gson                       mGson         = new Gson();
    private final ExecutorService            mExecutor     = Executors.newSingleThreadExecutor();

    private boolean mCapturing  = false;
    private boolean mUdpRunning = false;
    private DatagramSocket mUdpSocket;

    /* Reassembly buffer for BLE chunked data */
    private float[] mPendingMag   = new float[MAX_SUBCARRIERS];
    private float[] mPendingPhase = new float[MAX_SUBCARRIERS];
    private int     mExpectedChunks = 0;
    private int     mReceivedChunks = 0;

    /* Metadata from last BLE metadata characteristic */
    private int    mLastChannel  = 0;
    private int    mLastRssi     = 0;
    private long   mLastRateMbps = 0;
    private long   mLastTs       = 0;

    public LiveData<CsiPacket> getLatestPacket() { return mLatestPacket; }
    public LiveData<String>    getStatsText()    { return mStatsText;    }
    public boolean             isCapturing()     { return mCapturing;    }
    public void                toggleCapturing() { mCapturing = !mCapturing; }

    /* ---------------------------------------------------------------------- */
    /*  BLE chunk reassembly                                                    */
    /* ---------------------------------------------------------------------- */

    /**
     * processMetadata — decode 20-byte BLE metadata characteristic.
     * Format: [channel:1][rssi_s8:1][rate_lo:1][rate_hi:1][timestamp:8][subcarrier_count:1][pad:7]
     */
    public void processMetadata(byte[] data) {
        if (data.length < 12) return;
        mLastChannel  = data[0] & 0xFF;
        mLastRssi     = (int)(byte)data[1];
        mLastRateMbps = ((data[2] & 0xFF) | ((data[3] & 0xFF) << 8));
        ByteBuffer bb = ByteBuffer.wrap(data, 4, 8).order(ByteOrder.LITTLE_ENDIAN);
        mLastTs = bb.getLong();
    }

    /**
     * processBleChunk — decode a CSI data chunk from the BLE notify characteristic.
     * Format: [chunk_index:1][total_chunks:1][mag/phase pairs as uint8...]
     */
    public void processBleChunk(byte[] data) {
        if (data.length < 2) return;
        int  chunkIdx    = data[0] & 0xFF;
        int  totalChunks = data[1] & 0xFF;
        int  nValues     = (data.length - 2) / 2;

        if (chunkIdx == 0) {
            /* Start of new CSI frame */
            mExpectedChunks = totalChunks;
            mReceivedChunks = 0;
        }

        int base = chunkIdx * 60;
        for (int i = 0; i < nValues && (base + i) < MAX_SUBCARRIERS; i++) {
            int mv = data[2 + i * 2]     & 0xFF;
            int pv = data[2 + i * 2 + 1] & 0xFF;
            mPendingMag  [base + i] = mv;
            mPendingPhase[base + i] = (pv / 255.0f) * 2.0f * 3.14159f - 3.14159f;
        }
        mReceivedChunks++;

        if (mReceivedChunks >= mExpectedChunks) {
            /* Complete frame received */
            CsiPacket pkt = new CsiPacket(
                "ble_device", mLastTs, mLastChannel, mLastRssi,
                (int)mLastRateMbps,
                mPendingPhase.clone(), mPendingMag.clone());
            dispatchPacket(pkt);
        }
    }

    /* ---------------------------------------------------------------------- */
    /*  UDP listener                                                            */
    /* ---------------------------------------------------------------------- */

    public void startUdpListener() {
        if (mUdpRunning) return;
        mUdpRunning = true;
        mExecutor.submit(() -> {
            try {
                mUdpSocket = new DatagramSocket(UDP_PORT);
                byte[] buf = new byte[4096];
                Log.i(TAG, "UDP listener started on port " + UDP_PORT);
                while (mUdpRunning) {
                    DatagramPacket dp = new DatagramPacket(buf, buf.length);
                    mUdpSocket.receive(dp);
                    String json = new String(dp.getData(), 0, dp.getLength());
                    parseAndDispatchJson(json);
                }
            } catch (IOException e) {
                if (mUdpRunning) Log.e(TAG, "UDP error: " + e.getMessage());
            }
        });
    }

    public void stopUdpListener() {
        mUdpRunning = false;
        if (mUdpSocket != null && !mUdpSocket.isClosed()) {
            mUdpSocket.close();
        }
    }

    private void parseAndDispatchJson(String json) {
        try {
            JsonObject obj = mGson.fromJson(json, JsonObject.class);
            String nodeId  = obj.has("node_id")  ? obj.get("node_id").getAsString()  : "?";
            long   ts      = obj.has("timestamp") ? obj.get("timestamp").getAsLong()   : 0L;
            int    ch      = obj.has("channel")   ? obj.get("channel").getAsInt()     : 0;
            int    rssi    = obj.has("rssi")       ? obj.get("rssi").getAsInt()        : 0;
            int    rate    = obj.has("rate")       ? obj.get("rate").getAsInt()        : 0;

            com.google.gson.JsonArray phasesArr = obj.has("phases")
                ? obj.getAsJsonArray("phases") : null;
            com.google.gson.JsonArray magArr = obj.has("magnitudes")
                ? obj.getAsJsonArray("magnitudes") : null;

            float[] phases = parseFloatArray(phasesArr);
            float[] mags   = parseFloatArray(magArr);

            dispatchPacket(new CsiPacket(nodeId, ts, ch, rssi, rate, phases, mags));
        } catch (Exception e) {
            Log.w(TAG, "JSON parse error: " + e.getMessage());
        }
    }

    private float[] parseFloatArray(com.google.gson.JsonArray arr) {
        if (arr == null) return new float[MAX_SUBCARRIERS];
        float[] out = new float[Math.min(arr.size(), MAX_SUBCARRIERS)];
        for (int i = 0; i < out.length; i++) {
            out[i] = arr.get(i).getAsFloat();
        }
        return out;
    }

    /* ---------------------------------------------------------------------- */
    /*  Dispatch                                                                */
    /* ---------------------------------------------------------------------- */
    private void dispatchPacket(CsiPacket pkt) {
        mLatestPacket.postValue(pkt);

        synchronized (mHistory) {
            if (mHistory.size() >= MAX_HISTORY) mHistory.remove(0);
            mHistory.add(pkt);
        }

        mStatsText.postValue(String.format(
            "Node: %s  Ch: %d  RSSI: %d dBm\nRate: %d  Pkts: %d",
            pkt.nodeId, pkt.channel, pkt.rssi, pkt.rate, mHistory.size()));
    }

    /* ---------------------------------------------------------------------- */
    /*  Export                                                                  */
    /* ---------------------------------------------------------------------- */
    public void exportData(Context ctx) {
        mExecutor.submit(() -> {
            File dir = ctx.getExternalFilesDir(null);
            if (dir == null) { Log.e(TAG, "No external storage"); return; }

            String name = "csi_export_" + System.currentTimeMillis() + ".jsonl";
            File   out  = new File(dir, name);
            try (FileWriter fw = new FileWriter(out)) {
                synchronized (mHistory) {
                    for (CsiPacket p : mHistory) {
                        fw.write(mGson.toJson(p));
                        fw.write('\n');
                    }
                }
                Log.i(TAG, "Exported " + mHistory.size() + " packets to " + out.getAbsolutePath());
                mStatsText.postValue("Exported to " + out.getName());
            } catch (IOException e) {
                Log.e(TAG, "Export failed: " + e.getMessage());
            }
        });
    }

    @Override
    protected void onCleared() {
        super.onCleared();
        stopUdpListener();
        mExecutor.shutdown();
    }
}
