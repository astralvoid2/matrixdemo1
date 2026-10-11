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
    double      gpu_ms;         /* GPU time for the rain, from timer queries */
    double      gpu_peak_ms;    /* worst single frame in the last second     */
    int         vsync, fb_w, fb_h;
    int         rt_w, rt_h, scale_pc; /* scene render size and render scale (%) */
    const char *gl_renderer, *gl_version;
    const char *audio_source;   /* "procedural" or the WAV file name, or NULL */
    const char *audio_backend;
    unsigned    audio_rate;     /* Hz, 0 = no audio */
    double      alsa_delay_ms;  /* what ALSA reports as queued */
    double      auto_sync_ms;   /* automatic part: minus the smoothed ALSA delay */
    double      sync_ms;        /* the user's trim on top (+ = visuals later)   */
    const char *sync_source;    /* "default", "saved" or "--sync-ms"            */
    long        rss_kb, anon_kb, file_kb, shmem_kb;  /* from /proc/self/status */
    long        exe_bytes;      /* size of the executable file                  */
    int         muted;
    double      bpm, beats;     /* beats = the beat position the VISUALS use */
    int         other_gpu;      /* 1 if G can switch to another GPU */
} HudInfo;

int  hud_init(void);                                   /* 0 on success */
void hud_draw(const HudInfo *info, int fb_w, int fb_h);
void hud_shutdown(void);

#endif
