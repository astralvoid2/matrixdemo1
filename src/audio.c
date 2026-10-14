/*
 * audio.c — ALSA output thread, audio clock, WAV load/save.
 *
 * LINUX AUDIO IN ONE PARAGRAPH
 *   ALSA is the kernel-level sound API on Linux. libasound (the "alsa-lib"
 *   userspace library) has a "default" device. On a modern Ubuntu desktop
 *   that device is routed into PipeWire (or PulseAudio on older releases),
 *   so writing to ALSA "default" plays nicely alongside your browser, volume
 *   control and Bluetooth headphones. We use the blocking API from a separate
 *   thread: snd_pcm_writei() sleeps until the device has room for more
 *   samples, so the thread wakes up at exactly the pace audio is consumed
 *   and burns no CPU spinning.
 *
 * THREADING
 *   main thread  : renders graphics, calls audio_time() once per frame.
 *   audio thread : loops forever: copy a chunk of the loop, then writei() it.
 *   Shared state : the clock (protected by a mutex), plus the mute/run flags
 *                  (C11 atomics, so reads and writes can't tear).
 *
 * ENDIANNESS NOTE: WAV files are little-endian, and so are x86-64 and
 *   (normally) ARM64, so we read/write sample data directly without byte-
 *   swapping. That is fine for any machine running Ubuntu desktop.
 */
#define _GNU_SOURCE
#include "audio.h"
#include "audio_backend.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHUNK_FRAMES 512      /* frames per writei(): ~11.6 ms at 44.1 kHz    */
#define LATENCY_US   60000    /* requested total device buffer: 60 ms. Lower
                                 = tighter sync but more underrun risk.       */

static snd_pcm_t  *g_pcm;
static int16_t    *g_samples;   /* interleaved stereo loop, 16-bit             */
static long        g_frames;    /* loop length in frames                      */
static unsigned    g_rate;      /* sample rate of g_samples                    */
static pthread_t   g_thread;
static atomic_int  g_running;
static atomic_int  g_mute;

/* The clock: "frames heard" measured at a known moment (monotonic time). */
static pthread_mutex_t g_clk_mtx = PTHREAD_MUTEX_INITIALIZER;
static long long       g_clk_frames;
static double          g_clk_stamp;  /* CLOCK_MONOTONIC seconds at measurement */
static int             g_clk_valid;
static long            g_last_delay;  /* frames ALSA said were queued (for the HUD) */
static double          g_delay_ema;   /* the same, smoothed (frames); <0 = not yet set */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  /* never jumps, unlike wall-clock time */
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------- */
/* Audio thread                                                               */
/* ------------------------------------------------------------------------- */
static void *audio_thread(void *arg)
{
    (void)arg;
    int16_t   chunk[CHUNK_FRAMES * 2];
    long      pos = 0;          /* read position inside the loop      */
    long long written = 0;      /* total frames handed to ALSA, ever  */

    while (atomic_load(&g_running)) {
        /* 1. Copy the next CHUNK_FRAMES of the loop, wrapping at the end. */
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            long p = (pos + i) % g_frames;
            chunk[2*i + 0] = g_samples[2*p + 0];
            chunk[2*i + 1] = g_samples[2*p + 1];
        }
        /* Mute writes silence, but we keep advancing, so the music clock
         * (and therefore the visuals) carry on as if you could hear it. */
        if (atomic_load(&g_mute)) memset(chunk, 0, sizeof chunk);

        /* 2. Hand it to ALSA. Blocks until there is room in the device buffer. */
        snd_pcm_sframes_t r = snd_pcm_writei(g_pcm, chunk, CHUNK_FRAMES);
        if (r < 0) {
            /* -EPIPE = underrun (we were too slow), -ESTRPIPE = system suspend.
             * snd_pcm_recover() handles both by re-preparing the device. */
            if (snd_pcm_recover(g_pcm, (int)r, 1) < 0) {
                fprintf(stderr, "audio: unrecoverable ALSA error: %s\n", snd_strerror((int)r));
                break;
            }
            continue;
        }
        /* writei may write fewer frames than asked; only advance by what it took. */
        pos = (pos + r) % g_frames;
        written += r;

        /* 3. Update the clock. "delay" = frames written but not yet audible
         *    (still sitting in the buffers between us and the speaker), so
         *    frames actually heard = written - delay. */
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(g_pcm, &delay) < 0) continue;
        /* Some plugins return garbage (a huge number) on the very first call,
         * before the stream is really running. A queue longer than a few
         * seconds is impossible here, so skip the update. */
        if (delay < 0 || delay > (snd_pcm_sframes_t)g_rate * 2) continue;
        long long heard = written - delay;
        if (heard < 0) heard = 0;

        pthread_mutex_lock(&g_clk_mtx);
        g_last_delay = delay;
        /* The raw delay jitters by a chunk or two from write to write. An
         * exponential moving average (each new reading moves the average 2% of
         * the way, about a half-second time constant) gives a steady value
         * the visuals can follow without trembling. Switching outputs
         * (speakers to Bluetooth) still settles within a second or so. */
        g_delay_ema = g_delay_ema < 0 ? (double)delay : g_delay_ema + 0.02 * ((double)delay - g_delay_ema);
        g_clk_frames = heard;
        g_clk_stamp  = now_sec();
        g_clk_valid  = 1;
        pthread_mutex_unlock(&g_clk_mtx);
    }
    return NULL;
}

/* Open the device and start the thread. Takes ownership of `samples`. */
int audio_backend_start(int16_t *samples, long frames, unsigned rate)
{
    g_samples = samples;
    g_frames  = frames;
    g_rate    = rate;

    int err = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "audio: cannot open ALSA 'default' device: %s\n", snd_strerror(err));
        goto fail;
    }
    /* snd_pcm_set_params is the "easy mode" setup call: format, access type,
     * channels, rate, allow-resampling, total latency. ALSA (or PipeWire
     * behind it) resamples if the hardware runs at another rate (e.g. 48 kHz). */
    err = snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, rate, 1, LATENCY_US);
    if (err < 0) {
        fprintf(stderr, "audio: snd_pcm_set_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        goto fail;
    }

    g_delay_ema = -1.0;
    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, audio_thread, NULL) != 0) {
        atomic_store(&g_running, 0);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        goto fail;
    }
    fprintf(stderr, "audio: playing %.2f s loop at %u Hz via ALSA 'default'\n",
            (double)frames / rate, rate);
    return 0;

fail:
    free(samples);
    g_samples = NULL;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Clock + control                                                            */
/* ------------------------------------------------------------------------- */
double audio_time(void)
{
    if (!g_pcm) return -1.0;

    pthread_mutex_lock(&g_clk_mtx);
    int       valid  = g_clk_valid;
    long long frames = g_clk_frames;
    double    stamp  = g_clk_stamp;
    pthread_mutex_unlock(&g_clk_mtx);
    if (!valid) return 0.0;

    /* The audio thread only updates the clock about once per chunk (~12 ms),
     * so reading the raw value at 60+ fps gives a staircase (some frames see
     * the same time twice), which makes motion stutter. So we EXTRAPOLATE:
     * time at the last measurement + how long ago that was. The extrapolation
     * is capped at 100 ms so a stalled device makes the visuals pause rather
     * than run away from the music. */
    double t = (double)frames / g_rate + fmin(now_sec() - stamp, 0.1);

    /* Each new measurement can land slightly before our extrapolated guess.
     * Never let time run backwards, or the rain would twitch upward. */
    static double last = 0.0;
    if (t < last) t = last;
    last = t;
    return t;
}

double audio_reported_delay(void)
{
    if (!g_pcm) return -1.0;
    pthread_mutex_lock(&g_clk_mtx);
    double d = (double)g_last_delay / g_rate;
    pthread_mutex_unlock(&g_clk_mtx);
    return d;
}

/*
 * WHICH CLOCK SHOULD THE VISUALS USE?
 * In theory, the moment you HEAR sample n is (n - delay), with delay from
 * snd_pcm_delay(), and audio_time() reports exactly that. In practice the
 * visuals have latency too: compositor, display scan-out, and the way the eye
 * reads a fading flash as happening around the middle of its glow.
 * Tested by ear on a laptop, on built-in speakers (~70 ms reported) and on
 * Bluetooth headphones (~240 ms reported), the best sync in BOTH cases
 * was visuals running exactly `delay` AHEAD of audio_time(), i.e. aligned to
 * the moment audio is handed to ALSA. main.c therefore adds this smoothed
 * delay back on, which means it follows output changes automatically, and
 * the user's [ / ] trim sits on top.
 */
double audio_smoothed_delay(void)
{
    if (!g_pcm) return 0.0;
    pthread_mutex_lock(&g_clk_mtx);
    double d = g_delay_ema < 0 ? 0.0 : g_delay_ema / g_rate;
    pthread_mutex_unlock(&g_clk_mtx);
    return d;
}

double audio_visual_lead(void) { return audio_smoothed_delay(); }

const char *audio_backend_name(void)
{
    if (!g_pcm) return "off";
    switch (snd_pcm_type(g_pcm)) {
    case SND_PCM_TYPE_IOPLUG: return "ALSA->plugin (PulseAudio/PipeWire)";
    case SND_PCM_TYPE_HW:     return "ALSA hw";
    default:                  return "ALSA";
    }
}

unsigned audio_rate(void) { return g_pcm ? g_rate : 0; }

void audio_set_mute(int mute) { atomic_store(&g_mute, mute); }
int  audio_get_mute(void)     { return atomic_load(&g_mute); }

void audio_stop(void)
{
    if (!g_pcm) return;
    atomic_store(&g_running, 0);
    /* The thread notices g_running = 0 after its current writei() returns
     * (at most one chunk later), so a plain join is enough. */
    pthread_join(g_thread, NULL);
    snd_pcm_drop(g_pcm);   /* discard what's still buffered: instant silence */
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
    free(g_samples);
    g_samples = NULL;
}
