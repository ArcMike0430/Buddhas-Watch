package com.buddhaswatch.companion;

/**
 * CsiPacket — a single CSI measurement frame received from the watch.
 */
public class CsiPacket {
    public final String  nodeId;
    public final long    timestamp;
    public final int     channel;
    public final int     rssi;
    public final int     rate;
    public final float[] phases;
    public final float[] magnitudes;

    public CsiPacket(String nodeId, long timestamp, int channel, int rssi, int rate,
                     float[] phases, float[] magnitudes) {
        this.nodeId     = nodeId;
        this.timestamp  = timestamp;
        this.channel    = channel;
        this.rssi       = rssi;
        this.rate       = rate;
        this.phases     = phases;
        this.magnitudes = magnitudes;
    }
}
