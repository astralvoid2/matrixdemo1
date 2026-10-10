/*
 * synth.c — procedural cyberpunk techno loop.
 *
 * HOW THIS SYNTH IS ORGANISED
 * ---------------------------
 * Real-time synths keep "voices" alive and compute one sample at a time.
 * We don't need real time: the music is a fixed 15-second loop, so we render
 * it once at startup into memory, and the audio thread just streams it.
 *
 * That lets us use the simplest possible structure, "event rendering":
 *
 *   for every step of the sequencer:
 *       for every instrument that plays on this step:
 *           render that one note from start to finish, ADDING it into
 *           the instrument's bus (a mono float buffer the length of the loop)
 *
 * Every note is rendered independently, with its own oscillator phase and
 * filter state starting from zero. Overlapping notes just sum. Afterwards the
 * buses are mixed into stereo with panning, sidechain ducking and a delay.
 *
 * Writes go through put(), which wraps the index modulo the loop length. So
 * a note that starts near the end of the loop and rings past it keeps
 * sounding at the beginning, which is what makes the loop seamless.
 *
 * INSTRUMENTS (all classic techno recipes)
 *   kick  - sine wave whose pitch sweeps quickly from ~160 Hz to ~42 Hz
 *   clap  - band-passed noise with three rapid bursts and a short tail
 *   hats  - high-passed noise; short "closed" and longer "open" variants
 *   bass  - two detuned saws + sub sine through a resonant low-pass filter
 *           whose cutoff snaps open on each note (the "acid"/"rolling" sound)
 *   pad   - 3-note chords of detuned saws, softly filtered, for atmosphere
 *   arp   - pulse-wave arpeggio with a dotted-8th ping-pong delay
 *
 * TIMING MATH
 *   samples per beat = 44100 * 60 / 128 = 20671.875
 *   samples per step = 20671.875 / 4    = 5167.96875
 * These aren't integers, so each event's start sample is rounded on its own
 * ( llround(step * STEP_LEN) ) and rounding errors never pile up over the loop.
 */
#include "synth.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SR        DEMO_SAMPLE_RATE
#define TAU       6.283185307179586
#define BARS      8
#define STEPS     (BARS * 16)                      /* 16th notes in the loop */
#define BEAT_LEN  ((double)SR * 60.0 / DEMO_BPM)   /* samples per beat       */
#define STEP_LEN  (BEAT_LEN / 4.0)                 /* samples per 16th       */

static long g_len; /* loop length in frames, set in synth_render() */

/* Add v to buf at index i, wrapping around the loop (seamless tails). */
static void put(float *buf, long i, double v)
{
    i %= g_len;
    if (i < 0) i += g_len;
    buf[i] += (float)v;
}

/* MIDI note number to frequency in Hz. 69 = A4 = 440 Hz, 12 notes per octave. */
static double mtof(double m) { return 440.0 * pow(2.0, (m - 69.0) / 12.0); }

/* xorshift32: a tiny, fast pseudo-random generator, used as a noise source.
 * Returns white noise in [-1, 1]. A fixed seed makes the music identical
 * on every run. */
static uint32_t rng = 0x12345678u;
static double noise(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (double)rng / 2147483648.0 - 1.0;
}

/*
 * Chamberlin state-variable filter (SVF): one small loop that gives low-pass,
 * band-pass and high-pass outputs at once. Two parameters:
 *   f = 2*sin(pi * cutoff / SR)  (cutoff frequency; stable while f is well below 1)
 *   q = damping, 2 = no resonance, ~0.2 = screaming resonance
 */
typedef struct { double low, band; } SVF;
static void svf_tick(SVF *s, double in, double cutoff, double q)
{
    double f = 2.0 * sin(M_PI * fmin(cutoff, SR * 0.16) / SR); /* clamp for stability */
    s->low += f * s->band;
    double high = in - s->low - q * s->band;
    s->band += f * high;
}

/* ------------------------------------------------------------------------- */
/* Instruments. Each takes the bus to write into and a start sample.         */
/* ------------------------------------------------------------------------- */

/* KICK: pitch-swept sine. The fast downward sweep gives the "punch", the
 * slow amplitude decay gives the "boom". tanh() overdrive squares the wave
 * off a little for extra weight on small speakers. */
static void kick(float *bus, long start)
{
    double phase = 0.0;
    long len = (long)(SR * 0.45);
    for (long n = 0; n < len; n++) {
        double t    = (double)n / SR;
        double freq = 42.0 + 118.0 * exp(-t * 28.0);    /* 160 Hz to 42 Hz */
        phase += TAU * freq / SR;                        /* integrate frequency */
        double amp  = exp(-t * 5.5);
        double click = exp(-t * 600.0) * 0.3 * noise();  /* beater transient */
        put(bus, start + n, tanh(1.8 * sin(phase) * amp) + click);
    }
}

/* CLAP: a real hand clap is several people/hands slightly out of sync, so we
 * fire three short noise bursts 10 ms apart and then a softer tail, all
 * band-passed around 1.2 kHz. */
static void clap(float *bus, long start, double gain)
{
    SVF f = {0};
    long len = (long)(SR * 0.3);
    for (long n = 0; n < len; n++) {
        double t = (double)n / SR, env = 0.0;
        for (int b = 0; b < 3; b++) {                     /* the three bursts */
            double tb = t - b * 0.010;
            if (tb >= 0.0) env += exp(-tb * 220.0);
        }
        if (t > 0.02) env += 0.55 * exp(-(t - 0.02) * 16.0); /* the tail */
        svf_tick(&f, noise(), 1200.0, 0.9);
        put(bus, start + n, f.band * env * 2.2 * gain);
    }
}

/* HI-HAT: white noise minus a low-passed copy of itself gives high-passed noise
 * (one-pole high-pass). We do it twice for a steeper, sizzlier slope.
 * `decay` controls closed (fast) vs open (slow) hats. */
static void hat(float *bus, long start, double decay, double gain)
{
    double lp1 = 0.0, lp2 = 0.0;
    long len = (long)(SR * 6.0 / decay);   /* stop once env is ~e^-6 */
    for (long n = 0; n < len; n++) {
        double t  = (double)n / SR;
        double x  = noise();
        lp1 += 0.55 * (x - lp1);  double h1 = x - lp1;
        lp2 += 0.55 * (h1 - lp2); double h2 = h1 - lp2;
        put(bus, start + n, h2 * exp(-t * decay) * gain);
    }
}

/* BASS: rich oscillator (2 detuned saws + sub sine) into a resonant low-pass.
 * The cutoff starts high and quickly closes ("filter envelope"). Accented
 * notes open further. That squelch is what makes it "drive". */
static void bass(float *bus, long start, double note, double accent)
{
    SVF f = {0};
    double f0 = mtof(note);
    double p1 = 0.0, p2 = 0.0, ps = 0.0;
    long len = (long)(STEP_LEN * 0.92);       /* slightly staccato */
    for (long n = 0; n < len; n++) {
        double t = (double)n / SR;
        /* Naive sawtooth: phase ramps 0..1, mapped to -1..1. It aliases a bit
         * at high pitch, but bass notes are low enough that nobody will hear it. */
        p1 = fmod(p1 + f0 * 1.000 / SR, 1.0);
        p2 = fmod(p2 + f0 * 1.007 / SR, 1.0);   /* +0.7% detune = thickness */
        ps = fmod(ps + f0 * 0.5   / SR, 1.0);   /* one octave down          */
        double osc = (2*p1 - 1) + (2*p2 - 1) + 0.8 * sin(TAU * ps);
        double cutoff = 90.0 + (900.0 + 1600.0 * accent) * exp(-t * 22.0);
        svf_tick(&f, osc, cutoff, 0.35);
        /* amplitude: 2 ms attack, hold, 8 ms release (prevents clicks) */
        double amp = fmin(1.0, t / 0.002) * fmin(1.0, (double)(len - n) / (SR * 0.008));
        put(bus, start + n, f.low * amp * 0.5);
    }
}

/* PAD: one chord per bar. Each chord note is 3 saws detuned around the pitch
 * (a "supersaw"-lite), softened by a one-pole low-pass. Slow attack/release
 * envelopes overlap neighbouring bars, so chords crossfade smoothly. */
static void pad(float *bus, long start, const double *notes, int count)
{
    long len = (long)(BEAT_LEN * 4 + SR * 0.6);  /* bar + release tail */
    for (int k = 0; k < count; k++) {
        double f0 = mtof(notes[k]);
        double ph[3] = {0.13 * k, 0.51 * k, 0.77 * k}; /* decorrelated starts */
        const double det[3] = {0.996, 1.0, 1.005};
        double lp = 0.0;
        for (long n = 0; n < len; n++) {
            double t = (double)n / SR, s = 0.0;
            for (int v = 0; v < 3; v++) {
                ph[v] = fmod(ph[v] + f0 * det[v] / SR, 1.0);
                s += 2*ph[v] - 1;
            }
            lp += 0.035 * (s - lp);              /* dark one-pole filter */
            double env = fmin(1.0, t / 0.25) *   /* 250 ms fade in        */
                         fmin(1.0, (double)(len - n) / (SR * 0.6)); /* fade out */
            put(bus, start + n, lp * env * 0.12);
        }
    }
}

/* ARP: thin pulse wave (25% duty cycle) with a plucky envelope. */
static void arp(float *bus, long start, double note)
{
    double f0 = mtof(note), ph = 0.0;
    long len = (long)(STEP_LEN * 1.5);
    for (long n = 0; n < len; n++) {
        double t = (double)n / SR;
        ph = fmod(ph + f0 / SR, 1.0);
        double sq = ph < 0.25 ? 1.0 : -0.33;
        double amp = fmin(1.0, t / 0.001) * exp(-t * 16.0);
        put(bus, start + n, sq * amp * 0.5);
    }
}

/* ------------------------------------------------------------------------- */
/* The song                                                                   */
/* ------------------------------------------------------------------------- */

/* Chord progression in A minor, one chord per bar:  Am Am F G | Am Am F E
 * (the E major at the end uses G#, the "harmonic minor" leading tone, which
 * pulls strongly back to Am when the loop restarts). MIDI roots: A1=33. */
static const int    ROOT[BARS]  = {33, 33, 29, 31, 33, 33, 29, 28};
static const int    MINOR[BARS] = { 1,  1,  0,  0,  1,  1,  0,  0};

/* Rolling bass, 16 steps per bar. -1 = rest. The downbeat (where the kick
 * is) stays empty so kick and bass don't fight for the same low frequencies.
 * Values are semitones above the bar's root. */
static const int BASS_PAT[16] = {-1, 0, 0, 12, -1, 0, 0, 12, -1, 0, 12, 0, -1, 0, 7, 12};
static const int BASS_ACC[16] = { 0, 0, 1,  0,  0, 0, 1,  0,  0, 0,  1, 0,  0, 1, 0,  1};

/* Arp: index into the chord tones stacked over several octaves. */
static const int ARP_PAT[16] = {0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 4, 3, 2, 1};

float *synth_render(long *out_frames)
{
    g_len = (long)llround(STEPS * STEP_LEN); /* 661500 for 128 BPM */
    *out_frames = g_len;

    /* One mono bus per instrument group, so we can process them differently. */
    enum { B_KICK, B_CLAP, B_HAT, B_OHAT, B_BASS, B_PAD, B_ARP, B_COUNT };
    float *bus[B_COUNT];
    for (int i = 0; i < B_COUNT; i++) {
        bus[i] = calloc((size_t)g_len, sizeof(float));
        if (!bus[i]) { while (i--) free(bus[i]); return NULL; }
    }

    /* ---- sequencer: walk every 16th-note step of the loop ---- */
    for (int s = 0; s < STEPS; s++) {
        int  bar   = s / 16;       /* 0..7                    */
        int  st    = s % 16;       /* step inside the bar     */
        long t0    = (long)llround(s * STEP_LEN);
        int  root  = ROOT[bar];
        int  third = MINOR[bar] ? 3 : 4;
        int  chord[3] = {0, third, 7};

        /* Mini break: the last beat of the loop drops the kick and plays a
         * clap roll instead, a tiny moment of tension before the loop restarts. */
        int is_break = (bar == BARS - 1 && st >= 12);

        if (st % 4 == 0 && !is_break) kick(bus[B_KICK], t0);  /* four-on-the-floor */
        if (st == 4 || st == 12)       clap(bus[B_CLAP], t0, 1.0); /* beats 2 and 4 */
        if (is_break)                  clap(bus[B_CLAP], t0, 0.35 + 0.15 * (st - 12));

        /* Hats: open hat on the off-beat "and", quiet closed hats on 16ths,
         * with a bit of velocity variation so they don't sound robotic. */
        if (st % 4 == 2) hat(bus[B_OHAT], t0, 14.0, 0.9);
        else             hat(bus[B_HAT],  t0, 70.0, (st % 2) ? 0.35 : 0.55);

        if (BASS_PAT[st] >= 0)
            bass(bus[B_BASS], t0, root + BASS_PAT[st], BASS_ACC[st]);

        if (st == 0) {
            /* Pad voicing two octaves above the bass root. */
            double notes[3] = {root + 24 + chord[0], root + 24 + chord[1], root + 24 + chord[2]};
            pad(bus[B_PAD], t0, notes, 3);
        }

        /* The arp only plays in the second half of the loop (bars 5-8), so the
         * 15-second loop has an A/B feel instead of being one flat pattern. */
        if (bar >= 4) {
            int k = ARP_PAT[st];
            arp(bus[B_ARP], t0, root + 36 + chord[k % 3] + 12 * (k / 3));
        }
    }

    /* ---- delay for the arp: dotted 8th = 3 steps, with feedback ----
     * dl[i] = arp[i - D] + fb * dl[i - D]. Indices wrap around the loop, and
     * we run 3 passes so the echoes that wrap past the loop end settle into
     * their steady state. */
    float *dl = calloc((size_t)g_len, sizeof(float));
    if (dl) {
        long D = (long)llround(3 * STEP_LEN);
        for (int pass = 0; pass < 3; pass++)
            for (long i = 0; i < g_len; i++) {
                long j = (i - D + g_len) % g_len;
                dl[i] = bus[B_ARP][j] + 0.45f * dl[j];
            }
    }

    /* ---- final stereo mix ---- */
    float *out = malloc((size_t)g_len * 2 * sizeof(float));
    if (out) {
        for (long i = 0; i < g_len; i++) {
            /* SIDECHAIN DUCKING: the kick hits on every beat, so "time since the
             * last beat" tells us how loud to let the bass/pad/arp be. They dip
             * hard on each kick and swell back. That pumping is the
             * signature groove of modern techno and house. */
            double tb   = fmod((double)i, BEAT_LEN) / SR;
            /* min(1, tb/3ms) ramps INTO the duck instead of jumping, since an
             * instant gain change is itself an audible click. */
            double duck = 1.0 - 0.75 * exp(-tb * 9.0) * fmin(1.0, tb / 0.003);

            double d = dl ? dl[i] : 0.0;
            double center = bus[B_KICK][i] * 0.95
                          + bus[B_CLAP][i] * 0.45
                          + bus[B_BASS][i] * 0.60 * duck
                          + bus[B_PAD][i]  * 0.90 * duck;
            double hats = bus[B_HAT][i] * 0.22 + bus[B_OHAT][i] * 0.20;
            double a    = bus[B_ARP][i] * 0.16 * duck;

            /* Simple panning: hats lean right, the dry arp leans left and its
             * echo leans right, which gives a wide ping-pong effect cheaply. */
            double L = center + hats * 0.7 + a * 1.0 + d * 0.06 * duck;
            double R = center + hats * 1.0 + a * 0.5 + d * 0.14 * duck;

            /* Master "limiter": tanh soft-clips peaks instead of hard-clipping. */
            out[2*i + 0] = (float)(tanh(L * 1.1) * 0.85);
            out[2*i + 1] = (float)(tanh(R * 1.1) * 0.85);
        }
    }

    for (int i = 0; i < B_COUNT; i++) free(bus[i]);
    free(dl);
    return out;
}
