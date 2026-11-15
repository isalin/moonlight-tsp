/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_alsa.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <opus/opus_multistream.h>

/*
 * Same control and curve as the softvol in /etc/asound.conf, so the volume
 * keys (which set "Soft Volume Master") still apply. The system chain then
 * goes through dmix with 1024-frame periods; this skips dmix and talks to
 * the codec with 5 ms periods.
 */
static const char lowlat_conf[] =
    "pcm.tsp_lowlat {\n"
    "  type softvol\n"
    "  slave.pcm { type hw card audiocodec device 0 }\n"
    "  control { name \"Soft Volume Master\" card audiocodec }\n"
    "  min_dB -51.0\n"
    "  max_dB 0.0\n"
    "  resolution 256\n"
    "}\n";

static struct {
    snd_pcm_t *pcm;
    snd_config_t *conf;
    OpusMSDecoder *opus;
    short *pcm_buf;
    int channels;
    int samples_per_frame;
    snd_pcm_uframes_t drop_above;   /* frames */
    int latency_ms;
    const char *device;

    pthread_mutex_t lock;
    struct audio_stats st;
    unsigned long n_delay;
    uint64_t decode_ns, alsa_ns;    /* thread CPU, whole session */
} A = {
    .latency_ms = 20,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void audio_alsa_cpu(double *decode_s, double *alsa_s)
{
    pthread_mutex_lock(&A.lock);
    *decode_s = (double)A.decode_ns / 1e9;
    *alsa_s = (double)A.alsa_ns / 1e9;
    pthread_mutex_unlock(&A.lock);
}

void audio_alsa_set_latency(int ms)
{
    if (ms >= 5 && ms <= 200)
        A.latency_ms = ms;
}

void audio_alsa_set_device(const char *name)
{
    A.device = name;
}

static int open_lowlat(void)
{
    snd_input_t *in;
    int rc;

    if (A.conf == NULL) {
        if ((rc = snd_config_top(&A.conf)) < 0)
            return rc;
        rc = snd_input_buffer_open(&in, lowlat_conf, (ssize_t)strlen(lowlat_conf));
        if (rc < 0)
            return rc;
        rc = snd_config_load(A.conf, in);
        snd_input_close(in);
        if (rc < 0) {
            snd_config_delete(A.conf);
            A.conf = NULL;
            return rc;
        }
    }
    return snd_pcm_open_lconf(&A.pcm, "tsp_lowlat", SND_PCM_STREAM_PLAYBACK,
                              SND_PCM_NONBLOCK, A.conf);
}

static int configure(unsigned rate, unsigned channels, const char *name)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = rate * 5 / 1000;
    snd_pcm_uframes_t target = rate * (unsigned)A.latency_ms / 1000;
    /* Room only; start_threshold and drop_above bound the latency. */
    snd_pcm_uframes_t buffer = target * 3 > rate / 10 ? target * 3 : rate / 10;
    unsigned r = rate;
    int rc;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    if ((rc = snd_pcm_hw_params_any(A.pcm, hw)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(A.pcm, hw,
                SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (rc = snd_pcm_hw_params_set_format(A.pcm, hw,
                SND_PCM_FORMAT_S16_LE)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(A.pcm, hw, channels)) < 0 ||
        (rc = snd_pcm_hw_params_set_rate_near(A.pcm, hw, &r, NULL)) < 0 ||
        (rc = snd_pcm_hw_params_set_period_size_near(A.pcm, hw, &period,
                NULL)) < 0 ||
        (rc = snd_pcm_hw_params_set_buffer_size_near(A.pcm, hw,
                &buffer)) < 0 ||
        (rc = snd_pcm_hw_params(A.pcm, hw)) < 0)
        return rc;
    if (r != rate)
        return -EINVAL;
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    /*
     * Through dmix the period is the slave's (1024 frames), and starting
     * with less than two of them underruns at once.
     */
    if (target < 2 * period)
        target = 2 * period;
    if (target > buffer - period)
        target = buffer - period;

    if ((rc = snd_pcm_sw_params_current(A.pcm, sw)) < 0 ||
        (rc = snd_pcm_sw_params_set_start_threshold(A.pcm, sw, target)) < 0 ||
        (rc = snd_pcm_sw_params_set_avail_min(A.pcm, sw, period)) < 0 ||
        (rc = snd_pcm_sw_params(A.pcm, sw)) < 0)
        return rc;
    A.drop_above = target * 2 < buffer ? target * 2 : buffer - period;
    printf("audio: %s %u Hz %u ch, period %lu, buffer %lu, start %lu, "
           "drop above %lu frames\n", name, rate, channels,
           (unsigned long)period, (unsigned long)buffer,
           (unsigned long)target, (unsigned long)A.drop_above);
    fflush(stdout);
    return snd_pcm_prepare(A.pcm);
}

static void ar_cleanup(void)
{
    if (A.pcm != NULL) {
        snd_pcm_drop(A.pcm);
        snd_pcm_close(A.pcm);
        A.pcm = NULL;
    }
    if (A.opus != NULL) {
        opus_multistream_decoder_destroy(A.opus);
        A.opus = NULL;
    }
    free(A.pcm_buf);
    A.pcm_buf = NULL;
}

static int ar_init(int audio_configuration,
                   const POPUS_MULTISTREAM_CONFIGURATION oc, void *context,
                   int ar_flags)
{
    unsigned char mapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];
    const char *name;
    int rc;

    (void)audio_configuration;
    (void)context;
    (void)ar_flags;
    A.channels = oc->channelCount;
    A.samples_per_frame = oc->samplesPerFrame;
    memcpy(mapping, oc->mapping, sizeof(mapping));
    if (oc->channelCount >= 6) {
        /* Opus order FL FR C LFE RL RR to ALSA FL FR RL RR C LFE. */
        mapping[2] = oc->mapping[4];
        mapping[3] = oc->mapping[5];
        mapping[4] = oc->mapping[2];
        mapping[5] = oc->mapping[3];
    }
    A.pcm_buf = malloc(sizeof(short) * (size_t)oc->channelCount *
                       (size_t)oc->samplesPerFrame);
    if (A.pcm_buf == NULL)
        return -1;
    A.opus = opus_multistream_decoder_create(oc->sampleRate, oc->channelCount,
                                             oc->streams, oc->coupledStreams,
                                             mapping, &rc);
    if (A.opus == NULL) {
        fprintf(stderr, "audio: opus decoder: %d\n", rc);
        ar_cleanup();
        return -1;
    }

    rc = -ENODEV;
    name = A.device;
    if (name == NULL) {
        name = "hw:audiocodec,0 via softvol";
        rc = open_lowlat();
        if (rc == 0) {
            rc = configure((unsigned)oc->sampleRate,
                           (unsigned)oc->channelCount, name);
            if (rc < 0) {
                snd_pcm_close(A.pcm);
                A.pcm = NULL;
            }
        }
        if (rc < 0)
            fprintf(stderr, "audio: low-latency path unavailable (%s), "
                            "using default\n", snd_strerror(rc));
        name = "default";
    }
    if (rc < 0) {
        rc = snd_pcm_open(&A.pcm, name, SND_PCM_STREAM_PLAYBACK,
                          SND_PCM_NONBLOCK);
        if (rc == 0)
            rc = configure((unsigned)oc->sampleRate,
                           (unsigned)oc->channelCount, name);
        if (rc < 0) {
            fprintf(stderr, "audio: %s: %s\n", name, snd_strerror(rc));
            ar_cleanup();
            return -1;
        }
    }
    pthread_mutex_lock(&A.lock);
    memset(&A.st, 0, sizeof(A.st));
    A.n_delay = 0;
    A.decode_ns = A.alsa_ns = 0;
    pthread_mutex_unlock(&A.lock);
    return 0;
}

static void ar_play(char *data, int len)
{
    snd_pcm_sframes_t delay = 0, w;
    snd_pcm_state_t state;
    int n, lost = data == NULL, dropped = 0, xrun = 0;
    uint64_t t0 = thread_cpu_ns(), t, td = 0;

    state = snd_pcm_state(A.pcm);
    if (state == SND_PCM_STATE_XRUN) {
        snd_pcm_prepare(A.pcm);
        xrun = 1;
    } else if (state == SND_PCM_STATE_RUNNING &&
               snd_pcm_delay(A.pcm, &delay) == 0 && delay > 0 &&
               (snd_pcm_uframes_t)delay > A.drop_above) {
        /* Too much queued after a burst: drop to bound the latency. */
        dropped = 1;
        goto out;
    }

    t = thread_cpu_ns();
    n = opus_multistream_decode(A.opus, (const unsigned char *)data, len,
                                A.pcm_buf, A.samples_per_frame, 0);
    td = thread_cpu_ns() - t;
    if (n <= 0)
        goto out;
    w = snd_pcm_writei(A.pcm, A.pcm_buf, (snd_pcm_uframes_t)n);
    if (w == -EPIPE) {
        xrun = 1;
        snd_pcm_prepare(A.pcm);
        w = snd_pcm_writei(A.pcm, A.pcm_buf, (snd_pcm_uframes_t)n);
    } else if (w == -ESTRPIPE) {
        snd_pcm_recover(A.pcm, (int)w, 1);
        w = snd_pcm_writei(A.pcm, A.pcm_buf, (snd_pcm_uframes_t)n);
    }
    if (w == -EAGAIN)
        dropped = 1;

out:
    t = thread_cpu_ns();
    pthread_mutex_lock(&A.lock);
    A.decode_ns += td;
    A.alsa_ns += t - t0 - td;
    A.st.packets++;
    A.st.lost += (unsigned long)lost;
    A.st.dropped += (unsigned long)dropped;
    A.st.underruns += (unsigned long)xrun;
    if (state == SND_PCM_STATE_RUNNING) {
        A.st.delay_ms += delay * 1000.0 / 48000.0;
        A.n_delay++;
    }
    pthread_mutex_unlock(&A.lock);
}

void audio_alsa_take_stats(struct audio_stats *out)
{
    pthread_mutex_lock(&A.lock);
    *out = A.st;
    if (A.n_delay > 0)
        out->delay_ms /= A.n_delay;
    memset(&A.st, 0, sizeof(A.st));
    A.n_delay = 0;
    pthread_mutex_unlock(&A.lock);
}

AUDIO_RENDERER_CALLBACKS audio_alsa_callbacks = {
    .init = ar_init,
    .cleanup = ar_cleanup,
    .decodeAndPlaySample = ar_play,
    /* Queued: the receive thread never waits on ALSA. */
    .capabilities = 0,
};
