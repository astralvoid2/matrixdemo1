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
#include "synth.h"

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
        g_clk_frames = heard;
        g_clk_stamp  = now_sec();
        g_clk_valid  = 1;
        pthread_mutex_unlock(&g_clk_mtx);
    }
    return NULL;
}

/* Open the device and start the thread. Takes ownership of `samples`. */
static int start_playback(int16_t *samples, long frames, unsigned rate)
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

static int16_t to_s16(float x)
{
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)lrintf(x * 32767.0f);
}

/* Render the procedural loop and convert it to 16-bit stereo. */
static int16_t *render_s16(long *frames)
{
    float *f = synth_render(frames);
    if (!f) return NULL;
    int16_t *s = malloc((size_t)*frames * 2 * sizeof(int16_t));
    if (s)
        for (long i = 0; i < *frames * 2; i++) s[i] = to_s16(f[i]);
    free(f);
    return s;
}

int audio_start_procedural(void)
{
    long frames;
    int16_t *s = render_s16(&frames);
    if (!s) return -1;
    return start_playback(s, frames, DEMO_SAMPLE_RATE);
}

/* ------------------------------------------------------------------------- */
/* WAV reading. A WAV file is a RIFF container: a 12-byte header ("RIFF",     */
/* size, "WAVE") followed by chunks, each = 4-char id + 32-bit size + data.  */
/* We need "fmt " (sample format) and "data" (the samples); others are       */
/* skipped. Chunks are padded to an even size.                               */
/* ------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int audio_start_wav(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp); free(buf);
        fprintf(stderr, "%s: read error\n", path);
        return -1;
    }
    fclose(fp);

    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        free(buf);
        return -1;
    }

    int fmt_tag = 0, channels = 0, bits = 0;
    unsigned rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;

    for (long off = 12; off + 8 <= size; ) {
        const uint8_t *ck = buf + off;
        uint32_t len = rd32(ck + 4);
        if (off + 8 + (long)len > size) len = (uint32_t)(size - off - 8); /* truncated file */
        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            fmt_tag  = rd16(ck + 8);   /* 1 = integer PCM, 3 = IEEE float, 0xFFFE = extensible */
            channels = rd16(ck + 10);
            rate     = rd32(ck + 12);
            bits     = rd16(ck + 22);
            if (fmt_tag == 0xFFFE && len >= 26) fmt_tag = rd16(ck + 32); /* real format in SubFormat GUID */
        } else if (!memcmp(ck, "data", 4)) {
            data = ck + 8;
            data_len = len;
        }
        off += 8 + len + (len & 1);
    }

    int is_s16 = (fmt_tag == 1 && bits == 16);
    int is_f32 = (fmt_tag == 3 && bits == 32);
    if (!data || (channels != 1 && channels != 2) || !(is_s16 || is_f32) || rate == 0) {
        fprintf(stderr, "%s: unsupported WAV (need 16-bit PCM or 32-bit float, mono/stereo).\n"
                        "  Convert with: ffmpeg -i in.ext -ac 2 -ar 44100 -sample_fmt s16 out.wav\n", path);
        free(buf);
        return -1;
    }

    long frames = data_len / (channels * (bits / 8));
    int16_t *s = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!s || frames == 0) { free(buf); free(s); return -1; }
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            int src_c = channels == 2 ? c : 0;          /* mono to both speakers */
            long idx  = i * channels + src_c;
            if (is_s16) s[2*i + c] = (int16_t)rd16(data + idx * 2);
            else {
                float v;
                memcpy(&v, data + idx * 4, 4);           /* memcpy avoids unaligned access */
                s[2*i + c] = to_s16(v);
            }
        }
    free(buf);
    return start_playback(s, frames, rate);
}

/* ------------------------------------------------------------------------- */
/* WAV writing (16-bit stereo PCM, the most compatible format there is).     */
/* ------------------------------------------------------------------------- */
static void wr32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void wr16(FILE *f, uint16_t v) { uint8_t b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }

int audio_export_procedural(const char *path)
{
    long frames;
    int16_t *s = render_s16(&frames);
    if (!s) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); free(s); return -1; }
    uint32_t data_len = (uint32_t)(frames * 4);
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + data_len); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16);
    wr16(f, 1);                          /* PCM            */
    wr16(f, 2);                          /* stereo         */
    wr32(f, DEMO_SAMPLE_RATE);
    wr32(f, DEMO_SAMPLE_RATE * 4);       /* bytes / second */
    wr16(f, 4);                          /* bytes / frame  */
    wr16(f, 16);                         /* bits / sample  */
    fwrite("data", 1, 4, f); wr32(f, data_len);
    fwrite(s, 4, (size_t)frames, f);
    fclose(f);
    free(s);
    fprintf(stderr, "wrote %s (%.2f s, %d Hz, %.0f BPM)\n",
            path, (double)frames / DEMO_SAMPLE_RATE, DEMO_SAMPLE_RATE, DEMO_BPM);
    return 0;
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
 * HIDDEN LATENCY. snd_pcm_delay() is only as honest as the layer below us.
 * Talking straight to hardware (type HW), it's exact. On a desktop, "default"
 * is usually an I/O PLUGIN (type IOPLUG) that forwards to PulseAudio, which on
 * Ubuntu 22.04+ is really PipeWire's PulseAudio emulation. That emulation
 * reports 0 us of latency for every stream, so the plugin's delay only
 * counts its OWN buffer and misses PipeWire's queue, the graph quantum
 * (1024 samples = 21 ms) and the sound card's buffer.
 *
 * Measured on a Meteor Lake laptop by recording the screen and the speaker
 * monitor together: visuals led the kicks by ~116 ms. So for plugin outputs
 * we assume this much extra, unreported latency by default. The user can fine-tune
 * it live with [ and ] (Bluetooth headphones add another 100-200 ms).
 */
double audio_hidden_latency_guess(void)
{
    if (!g_pcm) return 0.0;
    return snd_pcm_type(g_pcm) == SND_PCM_TYPE_IOPLUG ? 0.110 : 0.0;
}

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
