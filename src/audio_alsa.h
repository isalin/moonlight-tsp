/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_AUDIO_ALSA_H
#define MOONLIGHT_TSP_AUDIO_ALSA_H

#include <Limelight.h>

/*
 * Opus multistream decode to ALSA. Prefers a private low-latency chain,
 * softvol on the system's "Soft Volume Master" control (so the volume keys
 * keep working) straight to hw:audiocodec,0, and falls back to the system
 * "default" PCM (dmix, about 85 ms) when the hardware is busy.
 */
extern AUDIO_RENDERER_CALLBACKS audio_alsa_callbacks;

/* Target buffered audio in ms (default 20); set before the stream starts. */
void audio_alsa_set_latency(int ms);

/* Forces the system "default" PCM (for A/B tests). */
void audio_alsa_set_device(const char *name);

struct audio_stats {
    unsigned long packets;
    unsigned long lost;        /* concealed by Opus PLC */
    unsigned long dropped;     /* discarded to bound latency */
    unsigned long underruns;
    double delay_ms;           /* mean ALSA delay before each write */
};
void audio_alsa_take_stats(struct audio_stats *out);

/* Thread CPU seconds spent in Opus decoding and in ALSA calls so far. */
void audio_alsa_cpu(double *decode_s, double *alsa_s);

#endif
