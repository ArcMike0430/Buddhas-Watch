/**
 * audio.h — ES7210 dual-microphone ADC driver
 *
 * The ES7210 is a 4-channel audio ADC connected over I²S.
 * Channels 0 and 1 are used for dual-mic acoustic correlation
 * with CSI anomalies to detect acoustoelectric coupling.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BITS          16
#define AUDIO_BUF_SAMPLES   4096

/** Initialise the ES7210 ADC and I²S peripheral. */
void audio_init(void);

/**
 * Read a block of PCM samples into buf (interleaved stereo, 16-bit signed).
 * Returns the number of bytes actually read.
 */
size_t audio_read(int16_t *buf, size_t samples);

/**
 * Compute RMS amplitude of the last audio frame.
 * Returns value in range [0.0, 1.0].
 */
float audio_get_rms(void);

/** Returns true if an acoustic transient above threshold_rms was detected. */
bool audio_spike_detected(float threshold_rms);
