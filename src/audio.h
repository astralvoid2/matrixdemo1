/*
 * audio.h — ALSA playback of a looping stereo buffer, plus a precise
 *           "how many seconds of music has the listener actually heard" clock.
 *
 * Why a music clock rather than the wall clock?
 *   The visuals must line up with what you HEAR. Audio hardware plays from a
 *   buffer that is some tens of milliseconds behind what we've written, and
 *   audio can stall briefly (underruns, suspend). If the visuals use the
 *   audio device's own play position as their clock, they cannot drift
 *   from the music, however long the demo runs.
 */
#ifndef AUDIO_H
#define AUDIO_H

/* Play the built-in procedural loop (see synth.c). Returns 0 on success. */
int    audio_start_procedural(void);

/* Load a WAV file (16-bit PCM or 32-bit float, mono or stereo, any sample
 * rate) and play it in a loop. Returns 0 on success. */
int    audio_start_wav(const char *path);

/* Seconds of audio that have actually reached the speakers since playback
 * started. This value is unwrapped: it keeps growing across loop repeats.
 * Returns a negative number if audio isn't running (the caller should
 * fall back to a wall clock). */
double audio_time(void);

void   audio_set_mute(int mute);
int    audio_get_mute(void);
void   audio_stop(void);

/* Render the procedural loop and save it as a 16-bit stereo WAV file.
 * Handy to hear the track in another player, or as a reference when
 * generating a replacement track. Returns 0 on success. */
int    audio_export_procedural(const char *path);

#endif
