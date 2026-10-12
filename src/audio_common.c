/* Shared procedural synthesis and WAV load/save; playback lives in the platform backend. */
#include "audio.h"
#include "audio_backend.h"
#include "synth.h"
#include "platform.h"
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t to_s16(float x)
{
    if (!isfinite(x)) return 0;
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
    return audio_backend_start(s, frames, DEMO_SAMPLE_RATE);
}

/* ------------------------------------------------------------------------- */
/* WAV reading. A WAV file is a RIFF container: a 12-byte header ("RIFF",     */
/* size, "WAVE") followed by chunks, each = 4-char id + 32-bit size + data.  */
/* We need "fmt " (sample format) and "data" (the samples); others are       */
/* skipped. Chunks are padded to an even size.                               */
/* ------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int audio_start_wav(const char *path)
{
    FILE *fp = platform_fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    /* Windows long/ftell are 32-bit. Reject unsupported sizes explicitly. */
    if (size < 12 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
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

    for (size_t off = 12; off <= (size_t)size - 8; ) {
        const uint8_t *ck = buf + off;
        uint32_t len = rd32(ck + 4);
        if ((size_t)len > (size_t)size - off - 8) {
            fprintf(stderr, "%s: truncated WAV chunk\n", path);
            free(buf);
            return -1;
        }
        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            fmt_tag  = rd16(ck + 8);   /* 1 = integer PCM, 3 = IEEE float, 0xFFFE = extensible */
            channels = rd16(ck + 10);
            rate     = rd32(ck + 12);
            bits     = rd16(ck + 22);
            if (fmt_tag == 0xFFFE && len >= 40 && rd16(ck + 24) >= 22) {
                static const uint8_t wave_guid_tail[12] = {0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                if (!memcmp(ck + 36, wave_guid_tail, sizeof wave_guid_tail))
                    fmt_tag = (int)rd32(ck + 32);
            }
        } else if (!memcmp(ck, "data", 4)) {
            data = ck + 8;
            data_len = len;
        }
        off += 8 + (size_t)len + (len & 1);
    }

    int is_s16 = (fmt_tag == 1 && bits == 16);
    int is_f32 = (fmt_tag == 3 && bits == 32);
    if (!data || (channels != 1 && channels != 2) || !(is_s16 || is_f32) || rate == 0) {
        fprintf(stderr, "%s: unsupported WAV (need 16-bit PCM or 32-bit float, mono/stereo).\n"
                        "  Convert with: ffmpeg -i in.ext -ac 2 -ar 44100 -sample_fmt s16 out.wav\n", path);
        free(buf);
        return -1;
    }

    unsigned frame_bytes = (unsigned)(channels * (bits / 8));
    uint64_t frame_count = (uint64_t)data_len / frame_bytes;
    if (data_len % frame_bytes != 0 || frame_count > (uint64_t)LONG_MAX) { free(buf); return -1; }
    long frames = (long)frame_count;
    int16_t *s = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!s || frames == 0) { free(buf); free(s); return -1; }
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            int src_c = channels == 2 ? c : 0;          /* mono to both speakers */
            size_t idx = (size_t)i * (size_t)channels + (size_t)src_c;
            if (is_s16) s[(size_t)2*i + c] = (int16_t)rd16(data + idx * 2);
            else {
                float v;
                memcpy(&v, data + idx * 4, 4);           /* memcpy avoids unaligned access */
                s[(size_t)2*i + c] = to_s16(v);
            }
        }
    free(buf);
    return audio_backend_start(s, frames, rate);
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
    FILE *f = platform_fopen(path, "wb");
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
    int failed = ferror(f);
    if (fclose(f) != 0) failed = 1;
    free(s);
    if (failed) { fprintf(stderr, "%s: WAV write failed\n", path); return -1; }
    fprintf(stderr, "wrote %s (%.2f s, %d Hz, %.0f BPM)\n",
            path, (double)frames / DEMO_SAMPLE_RATE, DEMO_SAMPLE_RATE, DEMO_BPM);
    return 0;
}
