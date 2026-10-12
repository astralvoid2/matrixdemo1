/*
 * hud.c — text overlay rendered with a CPU "canvas" plus a tiny shader.
 *
 * HOW IT WORKS
 *   1. The CPU draws text into an 8-bit image (one byte per pixel, 0 = empty,
 *      255 = brightest) using a classic 5x7 bitmap font stored in this file.
 *   2. That image is uploaded to a GL texture every frame. It's ~40 KB,
 *      nothing for a GPU.
 *   3. A second full-screen-triangle pass runs a small fragment shader. Inside
 *      the panel rectangle it reads the texture with texelFetch() (exact texel,
 *      no filtering) at an integer scale for crisp pixel text, and blends
 *      green text over a translucent black panel. Outside the panel it
 *      discards the pixel, leaving the rain untouched.
 *
 * Why not a font library? It'd add a dependency (FreeType) for 60 lines of
 * text. A bitmap font is also very "demoscene".
 */
#define _GNU_SOURCE
#include "gl_api.h"

#include "hud.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Canvas geometry (in texels; scaled up by an integer on screen) ---- */
#define COLS     64          /* characters per line           */
#define ROWS     12          /* lines                         */
#define CW       6           /* char cell: 5 px glyph + 1 gap */
#define CH       9           /* line height: 7 px glyph + 2   */
#define PAD      4           /* border inside the panel       */
#define TEX_W    (COLS * CW + 2 * PAD)
#define TEX_H    (ROWS * CH + 2 * PAD)

static unsigned char g_canvas[TEX_H][TEX_W];
static GLuint g_tex, g_prog, g_vao;
static GLint  u_res, u_scale, u_origin, u_size;

/*
 * THE FONT: the widely used 5x7 "glcdfont" layout, ASCII 32..126, plus a
 * filled block at 127 for meters. Each character is 5 bytes, one per COLUMN,
 * left to right. Bit 0 is the TOP pixel and bit 6 the bottom one.
 * Example, 'L' = 7F 40 40 40 40: a full first column, then only the bottom bit.
 */
static const unsigned char FONT[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14}, /*  !"# */
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00}, /* $%&' */
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08}, /* ()*+ */
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02}, /* ,-./ */
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31}, /* 0123 */
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03}, /* 4567 */
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00}, /* 89:; */
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06}, /* <=>? */
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22}, /* @ABC */
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A}, /* DEFG */
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41}, /* HIJK */
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E}, /* LMNO */
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31}, /* PQRS */
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F}, /* TUVW */
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00}, /* XYZ[ */
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40}, /* \]^_ */
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20}, /* `abc */
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E}, /* defg */
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00}, /* hijk */
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38}, /* lmno */
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20}, /* pqrs */
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C}, /* tuvw */
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00}, /* xyz{ */
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},{0x7F,0x7F,0x7F,0x7F,0x7F}, /* |}~ block */
};
#define BLOCK 127

/* Draw one character at text position (col,row) with brightness v (0..255). */
static void put_char(int col, int row, unsigned char c, unsigned char v)
{
    if (c < 32 || c > 127 || col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
    const unsigned char *g = FONT[c - 32];
    int x0 = PAD + col * CW, y0 = PAD + row * CH;
    for (int x = 0; x < 5; x++)
        for (int y = 0; y < 7; y++)
            if (g[x] >> y & 1) g_canvas[y0 + y][x0 + x] = v;
}

/* printf into the canvas at (col,row). Returns the column after the text. */
static int text(int col, int row, unsigned char v, const char *fmt, ...)
{
    char buf[COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (const char *p = buf; *p && col < COLS; p++) put_char(col++, row, (unsigned char)*p, v);
    return col;
}

/* ---- Shaders: the same full-screen triangle trick as the main demo ---- */
static const char *HUD_VS =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *HUD_FS =
    "#version 330 core\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uText;\n"
    "uniform vec2  uRes;     // framebuffer size\n"
    "uniform vec2  uOrigin;  // panel top-left, pixels from the TOP-left corner\n"
    "uniform vec2  uSize;    // canvas size in texels\n"
    "uniform float uScale;   // integer magnification\n"
    "void main() {\n"
    "    // gl_FragCoord has y up; the canvas has y down, so flip.\n"
    "    vec2 p = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y) - uOrigin;\n"
    "    vec2 t = floor(p / uScale);\n"
    "    if (any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, uSize))) discard;\n"
    "    float v = texelFetch(uText, ivec2(t), 0).r;\n"
    "    // Translucent dark-green panel, text from dim green to near-white.\n"
    "    vec3 c = mix(vec3(0.15, 0.85, 0.35), vec3(0.85, 1.0, 0.9), v * v);\n"
    "    fragColor = v > 0.0 ? vec4(c * (0.12 + 0.88 * v), 1.0) : vec4(0.0, 0.03, 0.01, 0.78);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "hud shader: %s\n", log);
    }
    return sh;
}

int hud_init(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, HUD_VS), fs = compile(GL_FRAGMENT_SHADER, HUD_FS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "hud: shader link failed\n"); return -1; }
    u_res    = glGetUniformLocation(g_prog, "uRes");
    u_scale  = glGetUniformLocation(g_prog, "uScale");
    u_origin = glGetUniformLocation(g_prog, "uOrigin");
    u_size   = glGetUniformLocation(g_prog, "uSize");

    /* One-channel 8-bit texture (GL_R8). NEAREST filtering, since we fetch
     * exact texels anyway. UNPACK_ALIGNMENT 1 because rows of TEX_W bytes
     * aren't necessarily a multiple of 4, GL's default row alignment. */
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TEX_W, TEX_H, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenVertexArrays(1, &g_vao);
    return 0;
}

/* ------------------------------------------------------------------------- */
void hud_draw(const HudInfo *in, int fb_w, int fb_h)
{
    enum { DIM = 110, MID = 190, HI = 255 };
    memset(g_canvas, 0, sizeof g_canvas);

    int r = 0, c;
    c = text(0, r, HI, "MATRIX RAIN");
    c = text(c, r, DIM, " // HUD          executable ");
    text(c, r++, HI, "%.1f KB", in->exe_bytes / 1024.0);

    /* FPS is capped by vsync. GPU ms is the real cost: at 60 Hz there are
     * 16.7 ms per frame, so "GPU 4 ms" means about 4x headroom. */
    c = text(0, r, DIM, "FPS   ");
    c = text(c, r, HI, "%5.1f", in->fps);
    c = text(c, r, MID, " %5.2f ms", in->frame_ms);
    c = text(c, r, DIM, "  GPU ");
    c = text(c, r, HI, "%.1f", in->gpu_ms);
    c = text(c, r, DIM, " peak ");
    c = text(c, r, HI, "%.1f ms", in->gpu_peak_ms);
    text(c, r++, in->vsync ? DIM : HI, "  vsync %s", in->vsync ? "on" : "OFF");

    /* Render scale: the scene is drawn at rt_w x rt_h, then stretched to the
     * display. Fewer pixels = less GPU time; S cycles 100/75/50%. */
    c = text(0, r, DIM, "RES   ");
    c = text(c, r, HI, "%dx%d", in->rt_w, in->rt_h);
    c = text(c, r, in->scale_pc == 100 ? DIM : HI, " (%d%%)", in->scale_pc);
    c = text(c, r, DIM, " of ");
    c = text(c, r, MID, "%dx%d", in->fb_w, in->fb_h);
    text(c, r++, DIM, "   S scale");

    /* Memory, in MB. RSS = what's actually in RAM. "own" is this process's
     * private allocations, "libs" the shared-library pages mapped from disk,
     * "shm" shared memory (GPU drivers keep buffers there). */
    c = text(0, r, DIM, "MEM   ");
    c = text(c, r, HI, "%5.1f MB", in->rss_kb / 1024.0);
#ifdef _WIN32
    /* Windows reports working set and private committed memory. */
    c = text(c, r, DIM, "  private commit ");
    text(c, r++, MID, "%.1f MB", in->anon_kb / 1024.0);
#else
    c = text(c, r, DIM, "  own ");
    c = text(c, r, MID, "%.1f", in->anon_kb / 1024.0);
    c = text(c, r, DIM, "  libs ");
    c = text(c, r, MID, "%.1f", in->file_kb / 1024.0);
    c = text(c, r, DIM, "  shm ");
    text(c, r++, MID, "%.1f", in->shmem_kb / 1024.0);
#endif

    c = text(0, r, DIM, "GPU   ");
    text(c, r++, HI, "%.58s", in->gl_renderer ? in->gl_renderer : "?");
    c = text(0, r, DIM, "GL    ");
    text(c, r++, MID, "%.58s", in->gl_version ? in->gl_version : "?");

    c = text(0, r, DIM, "AUDIO ");
    if (in->audio_rate) {
        c = text(c, r, MID, "%.20s %.1f kHz", in->audio_source, in->audio_rate / 1000.0);
        if (in->muted) text(c, r, HI, "  MUTED");
        r++;
        c = text(0, r, DIM, "      ");
        text(c, r++, MID, "%.36s  queue %3.0f ms", in->audio_backend, in->audio_delay_ms);
    } else {
        text(c, r++, MID, "off (wall clock)");
        r++;
    }

    /* Total visual offset = automatic part (minus the ALSA delay) + trim. */
    c = text(0, r, DIM, "SYNC  ");
    c = text(c, r, HI, "%+4.0f ms", in->auto_sync_ms + in->sync_ms);
    c = text(c, r, DIM, " = auto ");
    c = text(c, r, MID, "%+.0f", in->auto_sync_ms);
    c = text(c, r, DIM, " + trim ");
    c = text(c, r, HI, "%+.0f", in->sync_ms);
    text(c, r++, DIM, " (%.9s)", in->sync_source ? in->sync_source : "");

    /* Musical position: bar.beat, a 4-box beat meter and a 16-step phrase bar. */
    double b = in->beats < 0 ? 0 : in->beats;
    long   beat_i = (long)b;
    int    in_bar = (int)(beat_i % 4), in_phrase = (int)(beat_i % 16);
    double frac   = b - (double)beat_i;
    c = text(0, r, DIM, "BPM   ");
    c = text(c, r, HI, "%3.0f", in->bpm);
    text(c, r++, MID, "   bar %ld  beat %d   phrase %ld", beat_i / 4 + 1, in_bar + 1, beat_i / 16 + 1);

    c = text(0, r, DIM, "BEAT  ");
    for (int i = 0; i < 4; i++) {
        /* The current beat's box flashes on the kick and fades, like the visuals. */
        unsigned char v = (unsigned char)(i == in_bar ? 120 + 135 * (1.0 - frac) : 30);
        put_char(c + i * 2, r, BLOCK, v);
    }
    c += 9;
    c = text(c, r, DIM, " PHRASE ");
    for (int i = 0; i < 16; i++)
        put_char(c + i, r, i <= in_phrase ? BLOCK : '-', i == in_phrase ? HI : i < in_phrase ? MID : DIM);
    r++;

    /* Footer: the keys. */
    text(0, r, DIM, "keys: H hud  V vsync  M mute  [ ] trim  BkSp 0  F full%s",
         in->other_gpu ? "  G GPU" : "");

    /* Upload and draw. Scale 2x at 720p, 3x at 1080p, and so on. */
    int scale = fb_h / 360 < 1 ? 1 : fb_h / 360;
    if (TEX_W * scale > fb_w - 20 && scale > 1) scale--;
    int margin = 6 * scale;

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H, GL_RED, GL_UNSIGNED_BYTE, g_canvas);

    glUseProgram(g_prog);
    glUniform2f(u_res, (float)fb_w, (float)fb_h);
    glUniform2f(u_origin, (float)margin, (float)margin);
    glUniform2f(u_size, (float)TEX_W, (float)TEX_H);
    glUniform1f(u_scale, (float)scale);

    /* Standard "over" alpha blending, on just for this draw. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

void hud_shutdown(void)
{
    glDeleteTextures(1, &g_tex);
    glDeleteVertexArrays(1, &g_vao);
    glDeleteProgram(g_prog);
}
