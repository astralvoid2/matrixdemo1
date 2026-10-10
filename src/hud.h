/*
 * hud.h — a small text overlay (toggle with H) for FPS, GPU, audio sync and
 *         beat position.
 *
 * It's deliberately separate from the art: the rain shader knows nothing
 * about it. The HUD is a second, tiny draw call blended on top.
 */
#ifndef HUD_H
#define HUD_H

typedef struct {
    double      fps, frame_ms;
    const char *gl_renderer, *gl_version;
    const char *audio_source;   /* "procedural" or the WAV file name, or NULL */
    const char *audio_backend;
    unsigned    audio_rate;     /* Hz, 0 = no audio */
    double      alsa_delay_ms;  /* what ALSA reports as queued */
    double      sync_ms;        /* how far visuals run behind the audio clock */
    const char *sync_source;    /* "auto", "saved" or "--sync-ms"            */
    int         muted;
    double      bpm, beats;     /* beats = the beat position the VISUALS use */
    int         other_gpu;      /* 1 if G can switch to another GPU */
} HudInfo;

int  hud_init(void);                                   /* 0 on success */
void hud_draw(const HudInfo *info, int fb_w, int fb_h);
void hud_shutdown(void);

#endif
