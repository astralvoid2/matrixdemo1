/* Device-independent regression checks for the shared WAV/synthesis layer. */
#include "audio.h"
#include "audio_backend.h"
#include "platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int16_t *received;
static long received_frames;
static unsigned received_rate;
static int failures;

int audio_backend_start(int16_t *samples, long frames, unsigned rate)
{
    free(received);
    received = samples;
    received_frames = frames;
    received_rate = rate;
    return 0;
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)
static void wr16(FILE *f, unsigned value) { fputc(value & 255, f); fputc((value >> 8) & 255, f); }
static void wr32(FILE *f, uint32_t value) { wr16(f, value & 65535); wr16(f, value >> 16); }

static void fixture(const char *path, int channels, int floating, int extensible)
{
    FILE *f = platform_fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    unsigned bits = floating ? 32 : 16, data_bytes = (unsigned)channels * bits / 8 * 2;
    unsigned fmt_bytes = extensible ? 40 : 16;
    fwrite("RIFF", 1, 4, f); wr32(f, 20 + fmt_bytes + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, fmt_bytes); wr16(f, extensible ? 0xfffe : floating ? 3 : 1);
    wr16(f, (unsigned)channels); wr32(f, 48000); wr32(f, 48000 * (unsigned)channels * bits / 8);
    wr16(f, (unsigned)channels * bits / 8); wr16(f, bits);
    if (extensible) {
        wr16(f, 22); wr16(f, bits); wr32(f, channels == 2 ? 3 : 4);
        wr32(f, floating ? 3 : 1);
        const uint8_t tail[12] = {0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        fwrite(tail, 1, sizeof tail, f);
    }
    fwrite("data", 1, 4, f); wr32(f, data_bytes);
    for (int frame = 0; frame < 2; frame++) {
        for (int channel = 0; channel < channels; channel++) {
            if (floating) {
                float sample = frame ? -0.5f : 0.5f;
                if (channels == 2 && channel) sample = -sample;
                fwrite(&sample, sizeof sample, 1, f);
            } else {
                int sample = frame ? -12000 : 12000;
                if (channels == 2 && channel) sample = -sample;
                wr16(f, (uint16_t)sample);
            }
        }
    }
    CHECK(fclose(f) == 0);
}

int main(void)
{
    for (int channels = 1; channels <= 2; channels++)
        for (int floating = 0; floating <= 1; floating++)
            for (int extensible = 0; extensible <= 1; extensible++) {
                fixture("audio-fixture.wav", channels, floating, extensible);
                CHECK(audio_start_wav("audio-fixture.wav") == 0);
                CHECK(received_frames == 2 && received_rate == 48000);
                CHECK(received != NULL);
                if (received) {
                    CHECK(abs(received[0] - (floating ? 16384 : 12000)) <= 1);
                    CHECK(received[0] == -received[2]);
                    CHECK(received[1] == (channels == 1 ? received[0] : -received[0]));
                }
            }

    /* Truncated data, incomplete extensible format, unknown format and
     * partial sample frames must be rejected before the backend sees them. */
    fixture("audio-fixture.wav", 2, 0, 0);
    FILE *f = platform_fopen("audio-fixture.wav", "r+b");
    CHECK(f != NULL);
    if (f) { fseek(f, 40, SEEK_SET); wr32(f, 1000000); fclose(f); }
    CHECK(audio_start_wav("audio-fixture.wav") == -1);
    fixture("audio-fixture.wav", 2, 0, 0);
    f = platform_fopen("audio-fixture.wav", "r+b");
    if (f) { fseek(f, 20, SEEK_SET); wr16(f, 0xfffe); fclose(f); }
    CHECK(audio_start_wav("audio-fixture.wav") == -1);
    fixture("audio-fixture.wav", 2, 0, 0);
    f = platform_fopen("audio-fixture.wav", "r+b");
    if (f) { fseek(f, 40, SEEK_SET); wr32(f, 7); fclose(f); }
    CHECK(audio_start_wav("audio-fixture.wav") == -1);

    CHECK(audio_export_procedural("audio-export-test.wav") == 0);
    CHECK(audio_start_wav("audio-export-test.wav") == 0);
    CHECK(received_frames == 661500 && received_rate == 44100);
    int peak = 0;
    if (received) for (size_t i = 0; i < (size_t)received_frames * 2; i++)
        if (abs(received[i]) > peak) peak = abs(received[i]);
    CHECK(peak > 1000);
    CHECK(strcmp(platform_basename("C:\\music\\track.wav"), "track.wav") == 0);
    CHECK(strcmp(platform_basename("music/track.wav"), "track.wav") == 0);
    free(received);
    platform_remove_file("audio-fixture.wav");
    platform_remove_file("audio-export-test.wav");
    fprintf(stderr, "audio data tests: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
