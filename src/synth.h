/*
 * synth.h — tiny procedural music generator for the Matrix rain intro.
 *
 * The whole soundtrack is *computed* at startup (a few milliseconds of CPU)
 * instead of being loaded from a file. That is the classic 4K-intro trick:
 * a WAV of this loop would be ~2.6 MB, the code that generates it is a few KB.
 *
 * TEMPO / SYNC CONTRACT
 * ---------------------
 *   DEMO_BPM = 128 beats per minute, 4/4 time, 16th-note step sequencer.
 *   One beat  = 60 / 128        = 0.46875 s
 *   One step  = one beat / 4    = 0.1171875 s  (a 16th note)
 *   One bar   = 4 beats         = 1.875 s
 *   The loop  = 8 bars          = 15.0 s exactly (661500 frames @ 44.1 kHz)
 *   The kick drum lands on every beat, starting at t = 0.
 *
 * main.c uses DEMO_BPM to turn "seconds of audio played" into "beats elapsed",
 * which is what drives the visual pulse. If you change the BPM here, the
 * visuals follow automatically.
 */
#ifndef SYNTH_H
#define SYNTH_H

#define DEMO_BPM         128.0
#define DEMO_SAMPLE_RATE 44100

/*
 * Render the full music loop.
 *   out_frames receives the loop length in frames (1 frame = 1 L + 1 R sample).
 *   Returns a malloc'ed buffer of interleaved stereo floats in [-1, 1]
 *   (L0 R0 L1 R1 ...), or NULL on allocation failure. Caller frees it.
 *
 * The loop is seamless: sounds that ring past the end of the loop (reverb-ish
 * tails, delay echoes) are wrapped around to the start, so playing the buffer
 * back-to-back forever never clicks.
 */
float *synth_render(long *out_frames);

#endif
