/*
 * main.c — "MATRIX RAIN", a 4K-intro-style audiovisual demo for Linux and Windows.
 *
 * WHAT HAPPENS, IN ORDER
 *   1. Parse command-line options (custom WAV, BPM, fullscreen...).
 *   2. Start the music: render the procedural loop (synth.c) or load a WAV,
 *      and stream it to ALSA from a background thread (audio.c).
 *   3. Ask GLFW for a window with an OpenGL 3.3 *core profile* context.
 *   4. Compile a tiny vertex shader and the big fragment shader (shader.frag).
 *   5. Every frame: read the music clock, derive beat values, upload them as
 *      uniforms, draw ONE triangle that covers the screen, swap buffers.
 *   6. On ESC/Q: stop audio, free GL objects, destroy the window.
 *
 * WHY ONE FULL-SCREEN TRIANGLE?
 *   All the visuals are computed per pixel in the fragment shader. We just need
 *   the GPU to run that shader once for every pixel in the window, and the way
 *   to do that is to rasterise a shape that covers the whole window.
 *   A quad needs 2 triangles that share a diagonal edge. GPUs shade pixels in
 *   2x2 blocks, so pixels along that diagonal get shaded twice (wasted work).
 *   One oversized triangle with corners at (-1,-1), (3,-1), (-1,3) in clip
 *   space covers the [-1,1]^2 screen square entirely. The parts outside are
 *   clipped away for free, and there's no diagonal seam. It's also less code.
 *
 *   We don't even need a vertex buffer: the vertex shader computes the
 *   3 corners from gl_VertexID (0, 1, 2). Core profile still requires a
 *   Vertex Array Object to be bound when drawing, so we create an empty one.
 *
 * LINUX / OPENGL CONTEXT NOTES
 *   - GLFW talks to the window system for us: X11, or XWayland on a Wayland
 *     desktop (Ubuntu's GLFW 3.3 package uses X11 by default). It creates the
 *     GL context through GLX (X11) or EGL. We never touch those APIs directly.
 *   - "Core profile" = modern OpenGL with the deprecated fixed-function stuff
 *     (glBegin, matrix stacks...) removed. Mesa (Intel/AMD) and NVIDIA both
 *     support 3.3 core on any GPU from roughly 2010 onwards.
 *   - Function loading: on Windows you must fetch every GL >1.1 function
 *     pointer at runtime (hence GLAD/GLEW). On Linux, libGL.so (provided by
 *     libglvnd, which dispatches to Mesa or NVIDIA) exports all core GL entry
 *     points directly. So defining GL_GLEXT_PROTOTYPES before including the
 *     headers and linking with -lGL is enough. Strictly portable code would
 *     still use a loader; this is the simpler Linux path.
 */
#define _GNU_SOURCE
#include "gl_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "audio.h"
#include "hud.h"
#include "platform.h"
#include "synth.h"

/* -------------------------------------------------------------------------
 * The fragment shader source is EMBEDDED in the executable, so ./demo is a
 * single self-contained file. The Makefile runs
 *     ld -r -b binary shader.frag -o shader_frag.o
 * which wraps the raw bytes of shader.frag in an object file and defines
 * these two symbols marking the start and end of the data. (Not
 * NUL-terminated! That's why we pass an explicit length to glShaderSource.)
 * ------------------------------------------------------------------------- */
#ifdef DEMO_EMBEDDED_C
#include "embedded_shaders.h"
#else
extern const char _binary_shader_frag_start[];
extern const char _binary_shader_frag_end[];
extern const char _binary_post_frag_start[];   /* post.frag, embedded the same way */
extern const char _binary_post_frag_end[];
#endif

/* Vertex shader: generates the 3 corners of the full-screen triangle.
 *   gl_VertexID = 0 -> (0,0) -> clip (-1,-1)   bottom-left
 *   gl_VertexID = 1 -> (2,0) -> clip ( 3,-1)   far right
 *   gl_VertexID = 2 -> (0,2) -> clip (-1, 3)   far top
 * The bit tricks: (id << 1) & 2 is 0,2,0 and id & 2 is 0,0,2. */
static const char *VERTEX_SRC =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------------- */
/* Global state (a demo is one big singleton; globals keep it readable)      */
/* ------------------------------------------------------------------------- */
/* Two shader programs: the SCENE (shader.frag) draws the rain into an
 * off-screen texture, and POST (post.frag) puts it on screen with the
 * screen-space effects. See RENDER TARGET below. */
static GLuint g_program;
static GLint  u_resolution, u_time, u_rain_time, u_beat, u_beat_count;
static GLuint g_post;
static GLint  p_scene, p_resolution, p_time, p_beat_count;

/* RENDER TARGET: a framebuffer object (FBO) is an off-screen surface we
 * can draw into instead of the window. Its colour goes into g_scene_tex.
 * We use GL_RGBA16F (16-bit float per channel) so glow values above 1.0
 * survive until post.frag tone-maps them. 8-bit would clip them first.
 * Its size is the window size times the RENDER SCALE (S key): drawing the
 * expensive scene at 50% means a quarter of the pixels, and post.frag
 * stretches it back up. */
static GLuint g_fbo, g_scene_tex;
static int    g_rt_w, g_rt_h;
static const double SCALES[] = {1.0, 0.75, 0.5};
static int    g_scale_idx;

static double g_bpm    = DEMO_BPM;  /* overridable with --bpm for custom tracks */
static double g_offset = 0.0;       /* seconds before the first beat in a WAV  */

static int g_fullscreen;
static int g_win_x, g_win_y, g_win_w = 1280, g_win_h = 720; /* saved windowed geometry */
static int g_reload_requested;
static int g_hud_on;              /* H toggles the overlay                        */
#ifndef _WIN32
static int g_switch_gpu;          /* G: quit and relaunch on the other GPU         */
#endif
static int g_vsync = 1;           /* V toggles. See the VSYNC comment in main()    */

/* SYNC. The visual clock is
 *     audio_time() + audio_visual_lead() - g_sync
 * Linux retains its measured ALSA compensation. Windows follows the WASAPI
 * device clock without adding that Linux-specific compensation. The queued
 * audio delay is displayed separately. g_sync is a personal TRIM on top,
 * in seconds (positive = visuals later). Where it comes from, highest
 * priority first:
 *   --sync-ms N    command line
 *   saved          platform's per-user MatrixRain directory, written on [ or ]
 *   default        0
 * Backspace forgets the saved trim. */
static double      g_sync;
static int         g_sync_given;      /* 1 if --sync-ms was on the command line */
static const char *g_sync_source = "default";

/* ---- Remembering the sync offset between runs ----
 * Linux convention (the XDG Base Directory spec): per-user settings live in
 * $XDG_CONFIG_HOME, which defaults to ~/.config. One small file per setting
 * keeps the parsing trivial. */
static int sync_file_path(char *buf, size_t len, int make_dir)
{
    /* "trim" in the name: an older version saved an ABSOLUTE offset as
     * sync_ms. A new name means that value is never misread as a trim. */
    return platform_data_path(buf, len, "sync_trim_ms", make_dir);
}

static int load_saved_sync(double *sec)
{
    char path[DEMO_PATH_CAPACITY];
    if (sync_file_path(path, sizeof path, 0) != 0) return 0;
    FILE *f = platform_fopen(path, "r");
    if (!f) return 0;
    double ms;
    int ok = fscanf(f, "%lf", &ms) == 1;
    fclose(f);
    if (ok) *sec = ms / 1000.0;
    return ok;
}

static void save_sync(void)
{
    char path[DEMO_PATH_CAPACITY];
    if (sync_file_path(path, sizeof path, 1) != 0) return;
    FILE *f = platform_fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "%.0f\n", g_sync * 1000.0);
    fclose(f);
}

static void forget_saved_sync(void)
{
    char path[DEMO_PATH_CAPACITY];
    if (sync_file_path(path, sizeof path, 0) == 0) platform_remove_file(path);
}

/* ---- Memory use, from the kernel ----
 * /proc/self/status is a text file the kernel generates on the fly with
 * stats about the process reading it. The "Rss" lines say how much of our
 * memory is actually in RAM right now (in kB):
 *   VmRSS     total resident set
 *   RssAnon   private, "anonymous" memory: malloc, the music buffer, driver heaps
 *   RssFile   pages of files mapped into memory: our code and shared libraries
 *   RssShmem  shared memory (GPU drivers share buffers with the kernel/X)
 * Note that VRAM on a discrete GPU isn't counted, since it's not in system RAM. */
static void read_memory(long *rss, long *anon, long *file, long *shmem)
{
    platform_read_memory(rss, anon, file, shmem);
}

/* [ / ] handler: adjust, remember, report. */
static void nudge_sync(double delta)
{
    g_sync += delta;
    g_sync_source = "saved";
    save_sync();
    fprintf(stderr, "sync trim: %+.0f ms (saved)\n", g_sync * 1000.0);
}

/* ------------------------------------------------------------------------- */
/* Shader compilation                                                         */
/* ------------------------------------------------------------------------- */

/* Compile one shader stage. On failure print the driver's error log, which
 * usually has line numbers like "0:123(4): error: ...", and return 0. */
static GLuint compile(GLenum type, const char *src, GLint len, const char *name)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, &len);   /* len = -1 means NUL-terminated */
    glCompileShader(sh);

    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "%s: compile error:\n%s\n", name, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Build a program from the embedded vertex shader + given fragment source.
 * The fragment shader is where all the visuals live. Returns 0 on failure. */
static GLuint build_program(const char *frag_src, GLint frag_len, const char *frag_name)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SRC, -1, "vertex shader");
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag_src, frag_len, frag_name);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    /* Once linked, the program keeps its own compiled copy, so the shader
     * objects can be deleted right away. */
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* Install a scene/post program and look up its uniforms by name.
 * A uniform the compiler optimised away (unused in the shader) returns -1,
 * and glUniform* calls with location -1 are silently ignored, so it's safe
 * to experiment with shaders that don't use every uniform. */
static void set_post_program(GLuint prog)
{
    if (g_post) glDeleteProgram(g_post);
    g_post       = prog;
    p_scene      = glGetUniformLocation(prog, "uScene");
    p_resolution = glGetUniformLocation(prog, "uResolution");
    p_time       = glGetUniformLocation(prog, "uTime");
    p_beat_count = glGetUniformLocation(prog, "uBeatCount");
}

static void use_program(GLuint prog)
{
    if (g_program) glDeleteProgram(g_program);
    g_program = prog;
    glUseProgram(prog);
    u_resolution = glGetUniformLocation(prog, "uResolution");
    u_time       = glGetUniformLocation(prog, "uTime");
    u_rain_time  = glGetUniformLocation(prog, "uRainTime");
    u_beat       = glGetUniformLocation(prog, "uBeat");
    u_beat_count = glGetUniformLocation(prog, "uBeatCount");
}

/* Hot reload: read a shader from disk and build it. If it doesn't compile,
 * return 0 and the caller keeps the old one running, so you can iterate on
 * the visuals without restarting the music. We look in src/ (running bin/demo
 * from the repo root) and then the current directory. */
static GLuint build_from_disk(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "src/%s", name);
    FILE *f = platform_fopen(path, "rb");
    if (!f) f = platform_fopen(strcpy(path, name), "rb");
    if (!f) { fprintf(stderr, "reload: src/%s not found (run from the repo root)\n", name); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n);
    GLuint prog = 0;
    if (src && fread(src, 1, (size_t)n, f) == (size_t)n) prog = build_program(src, (GLint)n, path);
    free(src);
    fclose(f);
    fprintf(stderr, "reload: %s %s\n", path, prog ? "OK" : "FAILED, keeping previous");
    return prog;
}

static void reload_shaders_from_disk(void)
{
    GLuint scene = build_from_disk("shader.frag");
    if (scene) use_program(scene);
    GLuint post = build_from_disk("post.frag");
    if (post) set_post_program(post);
}

/* (Re)create the off-screen render target if the wanted size changed.
 * Called every frame, but only does work after a resize or scale change. */
static void ensure_render_target(int w, int h)
{
    if (w == g_rt_w && h == g_rt_h && g_fbo) return;
    g_rt_w = w;
    g_rt_h = h;
    if (!g_scene_tex) glGenTextures(1, &g_scene_tex);
    glBindTexture(GL_TEXTURE_2D, g_scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    /* LINEAR = bilinear filtering when post.frag stretches a scaled-down
     * scene. CLAMP_TO_EDGE: the RGB split reads slightly outside [0,1] at
     * the screen edges, so repeat the edge pixels instead of wrapping around. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!g_fbo) glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "render target %dx%d incomplete\n", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);   /* 0 = the window again */
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */
static void toggle_fullscreen(GLFWwindow *win)
{
    if (!g_fullscreen) {
        /* Remember the window geometry so we can restore it later. */
        glfwGetWindowPos(win, &g_win_x, &g_win_y);
        glfwGetWindowSize(win, &g_win_w, &g_win_h);
        /* Exclusive fullscreen on the primary monitor at its current video
         * mode. Using the current mode avoids a slow display mode switch. */
        GLFWmonitor *mon = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(win, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(win, NULL, g_win_x, g_win_y, g_win_w, g_win_h, GLFW_DONT_CARE);
    }
    g_fullscreen = !g_fullscreen;
    glfwSwapInterval(g_vsync); /* some drivers reset vsync on a mode change */
}

/* GLFW calls this from glfwPollEvents() for every key press/release/repeat.
 * GLFW_KEY_* codes follow the US keyboard layout. */
static void on_key(GLFWwindow *win, int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    /* Holding a key sends PRESS, then REPEATs. Only the sync keys act on
     * repeat (hold [ to sweep the offset); toggles like H and F must not
     * flicker on and off while held. */
    int repeatable = key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET;
    if (action == GLFW_RELEASE || (action == GLFW_REPEAT && !repeatable)) return;
    switch (key) {
    case GLFW_KEY_ESCAPE:
    case GLFW_KEY_Q:   glfwSetWindowShouldClose(win, GLFW_TRUE); break;
    case GLFW_KEY_F:
    case GLFW_KEY_F11: toggle_fullscreen(win); break;
    case GLFW_KEY_M:   audio_set_mute(!audio_get_mute()); break;
    case GLFW_KEY_R:   g_reload_requested = 1; break; /* do GL work in the main loop */
    case GLFW_KEY_S:   g_scale_idx = (g_scale_idx + 1) % 3; break;
    case GLFW_KEY_H:   g_hud_on = !g_hud_on; break;
    /* [ and ] by physical key position (US layout names). Visuals later / earlier. */
    case GLFW_KEY_LEFT_BRACKET:  nudge_sync(-0.010); break;
    case GLFW_KEY_RIGHT_BRACKET: nudge_sync(+0.010); break;
    case GLFW_KEY_BACKSPACE:
        forget_saved_sync();
        g_sync = 0.0;
        g_sync_source = "default";
        fprintf(stderr, "sync trim: reset to 0\n");
        break;
    case GLFW_KEY_G:
#ifdef _WIN32
        fprintf(stderr, "GPU preference: Windows Settings > System > Display > Graphics, then restart.\n");
#else
        g_switch_gpu = 1; glfwSetWindowShouldClose(win, GLFW_TRUE);
#endif
        break;
    case GLFW_KEY_V:   g_vsync = !g_vsync; glfwSwapInterval(g_vsync); break;
    }
}

static void on_glfw_error(int code, const char *msg)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, msg);
}

/* -------------------------------------------------------------------------
 * GPU SWITCHING (G key)
 * An OpenGL context belongs to one GPU for its whole life, and the choice is
 * made by libglvnd when the process creates its first context, based on
 * environment variables:
 *   NVIDIA (proprietary driver, "PRIME render offload"):
 *       __NV_PRIME_RENDER_OFFLOAD=1  __GLX_VENDOR_LIBRARY_NAME=nvidia
 *   Mesa (Intel/AMD, or a second AMD GPU with DRI_PRIME=1):
 *       __GLX_VENDOR_LIBRARY_NAME=mesa
 * So to "switch" we set those variables and exec() ourselves: the process is
 * replaced by a fresh copy that picks the other GPU. The music restarts.
 * The HUD/fullscreen/sync state is passed along as command-line options.
 * ------------------------------------------------------------------------- */
#ifndef _WIN32
static void relaunch_on_other_gpu(char **argv, int on_nvidia)
{
    /* MATRIXRAIN_GPU_SWITCH marks "we were relaunched onto NVIDIA", so that if
     * context creation then fails, main() knows to bounce back (once). */
    if (on_nvidia) {
        unsetenv("MATRIXRAIN_GPU_SWITCH");
        unsetenv("__NV_PRIME_RENDER_OFFLOAD");
        unsetenv("__VK_LAYER_NV_optimus");
        setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
    } else {
        setenv("MATRIXRAIN_GPU_SWITCH", "1", 1);
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
        setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    }

    /* Rebuild argv: keep the user's options except the ones we re-state. */
    static char *args[64];
    static char  sync_buf[32];
    int n = 0;
    args[n++] = argv[0];
    for (int i = 1; argv[i] && n < 56; i++) {
        if (!strcmp(argv[i], "--hud") || !strcmp(argv[i], "--fullscreen") ||
            !strcmp(argv[i], "--novsync")) continue;
        if (!strcmp(argv[i], "--sync-ms") || !strcmp(argv[i], "--scale")) { if (argv[i + 1]) i++; continue; }
        args[n++] = argv[i];
    }
    if (g_hud_on)     args[n++] = "--hud";
    if (!g_vsync)     args[n++] = "--novsync";
    static char scale_buf[16];
    if (g_scale_idx) {
        snprintf(scale_buf, sizeof scale_buf, "%.0f", SCALES[g_scale_idx] * 100.0);
        args[n++] = "--scale";
        args[n++] = scale_buf;
    }
    if (g_fullscreen) args[n++] = "--fullscreen";
    /* A saved or auto offset re-derives itself in the new process. Only an
     * explicit --sync-ms (possibly nudged since) needs passing on. */
    if (g_sync_given) {
        snprintf(sync_buf, sizeof sync_buf, "%.0f", g_sync * 1000.0);
        args[n++] = "--sync-ms";
        args[n++] = sync_buf;
    }
    args[n] = NULL;

    fprintf(stderr, "relaunching on the %s GPU...\n", on_nvidia ? "integrated (Mesa)" : "NVIDIA");
    execv("/proc/self/exe", args);    /* only returns on failure */
    perror("execv");
}
#endif

/* ------------------------------------------------------------------------- */
static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "  --fullscreen        start fullscreen (toggle anytime with F / F11)\n"
           "  --wav FILE          play FILE (looped) instead of the built-in synth\n"
           "  --bpm N             tempo of --wav track for beat sync (default %.0f)\n"
           "  --offset S          seconds until the first beat in --wav track (default 0)\n"
           "  --nosound           no audio (visuals run on the wall clock)\n"
           "  --sync-ms N         sync trim in ms, on top of the automatic latency\n"
           "                      compensation (default: last value set with [ ], else 0)\n"
           "  --hud               start with the HUD visible\n"
           "  --novsync           don't wait for the display refresh (benchmark)\n"
           "  --scale 100|75|50   render the scene at this %% of the display resolution\n"
           "  --export FILE.wav   write the built-in track to a WAV file and exit\n"
           "keys: ESC/Q quit, F/F11 fullscreen, M mute, R reload shaders,\n"
           "      H HUD, V vsync, S render scale, [ ] sync trim -/+ 10 ms (remembered),\n"
           "      Backspace trim 0"
#ifndef _WIN32
           ", G relaunch on the other GPU"
#endif
           "\n",
           argv0, DEMO_BPM);
}

int demo_main(int argc, char **argv)
{
    const char *wav = NULL;
    int nosound = 0, start_fullscreen = 0, smoke_test = 0, exit_code = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if      (!strcmp(a, "--fullscreen"))       start_fullscreen = 1;
        else if (!strcmp(a, "--nosound"))          nosound = 1;
        else if (!strcmp(a, "--smoke-test"))       { smoke_test = 1; nosound = 1; }
        else if (!strcmp(a, "--hud"))              g_hud_on = 1;
        else if (!strcmp(a, "--novsync"))          g_vsync = 0;
        else if (!strcmp(a, "--scale") && more)    { int pc = atoi(argv[++i]);
                                                     g_scale_idx = pc <= 50 ? 2 : pc <= 75 ? 1 : 0; }
        else if (!strcmp(a, "--sync-ms") && more)  { g_sync = atof(argv[++i]) / 1000.0; g_sync_given = 1;
                                                     g_sync_source = "--sync-ms"; }
        else if (!strcmp(a, "--wav")    && more)   wav = argv[++i];
        else if (!strcmp(a, "--bpm")    && more)   g_bpm = atof(argv[++i]);
        else if (!strcmp(a, "--offset") && more)   g_offset = atof(argv[++i]);
        else if (!strcmp(a, "--export") && more)   return audio_export_procedural(argv[++i]) ? 1 : 0;
        else { usage(argv[0]); return !strcmp(a, "-h") || !strcmp(a, "--help") ? 0 : 1; }
    }
    if (g_bpm <= 0.0) g_bpm = DEMO_BPM;

    /* ---- Window + GL context ---------------------------------------------- */
    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) return 1;

    /* Request OpenGL 3.3 core. FORWARD_COMPAT is only *required* on macOS,
     * but it's harmless on Linux and keeps the code portable. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    /* No depth/stencil buffer: we draw one flat triangle and never test depth. */
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    if (smoke_test) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *win = glfwCreateWindow(g_win_w, g_win_h, "MATRIX RAIN // 128 BPM", NULL, NULL);
    if (!win) {
        fprintf(stderr, "Could not create an OpenGL 3.3 core window.\n"
#ifdef _WIN32
                        "Install the Intel, AMD or NVIDIA graphics driver; OpenGL 3.3 is required.\n");
#else
                        "Check `glxinfo -B` (package mesa-utils) for your GL version.\n");
#endif
        glfwTerminate();
#ifndef _WIN32
        if (getenv("MATRIXRAIN_GPU_SWITCH")) {
            /* We got here by pressing G. The usual cause: the NVIDIA driver was
             * updated but not yet reloaded (`nvidia-smi` then says
             * "Driver/library version mismatch"), which a reboot fixes. Go back
             * to the integrated GPU rather than just quitting. */
            fprintf(stderr, "NVIDIA offload failed (if nvidia-smi reports a version mismatch, reboot).\n");
            g_fullscreen = start_fullscreen;
            relaunch_on_other_gpu(argv, 1);
        }
#endif
        return 1;
    }
    glfwMakeContextCurrent(win);  /* GL calls on this thread now go to this context */
#ifdef DEMO_USE_GLAD
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress) || !GLAD_GL_VERSION_3_3) {
        fprintf(stderr, "Could not load the required OpenGL 3.3 functions.\n");
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
#endif
    glfwSetKeyCallback(win, on_key);

    /* VSYNC: wait for the monitor's vertical blank before swapping. That caps
     * us at the refresh rate (60/144/... Hz), avoids tearing (a new frame
     * replacing the old one mid-scan), and stops the GPU rendering frames
     * the display can never show, which saves power, heat and fan noise.
     * The catch: FPS then can't show how much headroom you have, so the HUD
     * also shows GPU time per frame (see the TIMER QUERIES comment below).
     * V turns vsync off for benchmarking. Some drivers or desktop settings
     * force vsync regardless (Mesa: vblank_mode=0 ./bin/demo overrides it). */
    glfwSwapInterval(g_vsync);

    /* Print which GPU we got. Useful on hybrid laptops (see README: PRIME). */
    const char *gl_vendor   = (const char *)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version  = (const char *)glGetString(GL_VERSION);
    fprintf(stderr, "GL_VENDOR   : %s\nGL_RENDERER : %s\nGL_VERSION  : %s\n",
            gl_vendor, gl_renderer, gl_version);

    /* Is there another GPU we could switch to with G? If we're on NVIDIA, the
     * integrated GPU (Mesa) is always there. If we're on Mesa, an NVIDIA GPU is
     * usable when its kernel driver is loaded (this /proc file exists). */
#ifdef _WIN32
    int other_gpu = 0; /* GPU selection is a Windows per-application preference. */
#else
    int on_nvidia = gl_vendor && strstr(gl_vendor, "NVIDIA") != NULL;
    int other_gpu = on_nvidia || access("/proc/driver/nvidia/version", F_OK) == 0;
#endif

    if (start_fullscreen) toggle_fullscreen(win);

    /* ---- Shaders ----------------------------------------------------------- */
#ifdef DEMO_EMBEDDED_C
    GLint frag_len = (GLint)(sizeof embedded_shader - 1);
    GLuint prog = build_program((const char *)embedded_shader, frag_len, "shader.frag (embedded)");
    GLint post_len = (GLint)(sizeof embedded_post - 1);
    GLuint post = build_program((const char *)embedded_post, post_len, "post.frag (embedded)");
#else
    GLint frag_len = (GLint)(_binary_shader_frag_end - _binary_shader_frag_start);
    GLuint prog = build_program(_binary_shader_frag_start, frag_len, "shader.frag (embedded)");
    GLint post_len = (GLint)(_binary_post_frag_end - _binary_post_frag_start);
    GLuint post = build_program(_binary_post_frag_start, post_len, "post.frag (embedded)");
#endif
    if (!post) { glfwTerminate(); return 1; }
    set_post_program(post);
    if (!prog) { glfwTerminate(); return 1; }
    use_program(prog);
    if (hud_init() != 0) fprintf(stderr, "HUD unavailable\n");

    /* An empty VAO: no vertex attributes, but core profile refuses to draw
     * without one bound. */
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* TIMER QUERIES: ask the GPU itself how long our drawing took.
     * glBeginQuery(GL_TIME_ELAPSED) ... glEndQuery() brackets GPU work, and the
     * GPU writes the elapsed nanoseconds into the query object when it
     * finishes. The GPU runs a frame or two BEHIND the CPU, so asking for the
     * result immediately would stall until the GPU catches up. Instead we keep
     * a small ring of queries and read each one a few frames later, only when
     * GL_QUERY_RESULT_AVAILABLE says it's ready. Core OpenGL since 3.3. */
    enum { NQ = 4 };
    GLuint gpu_q[NQ];
    int    gpu_q_used[NQ] = {0};
    glGenQueries(NQ, gpu_q);
    unsigned frame_no = 0;
    double gpu_ms = 0.0;                       /* smoothed average             */
    double gpu_peak_ms = 0.0, gpu_peak_acc = 0.0; /* worst frame in the last second */

    /* ---- Audio ------------------------------------------------------------- *
     * Started after the window exists, so the music doesn't begin while the
     * screen is still black. */
    if (!nosound) {
        int rc = wav ? audio_start_wav(wav) : audio_start_procedural();
        if (rc != 0) fprintf(stderr, "audio unavailable, continuing silently\n");
    }
    /* Default visual delay: whatever latency the audio layer can't report. */
    if (!g_sync_given && load_saved_sync(&g_sync)) g_sync_source = "saved";
    fprintf(stderr, "sync: %s, trim %+.0f ms (%s; [ / ] to adjust)\n",
#ifdef _WIN32
            "WASAPI device clock",
#else
            "auto (follows ALSA delay)",
#endif
            g_sync * 1000.0, g_sync_source);
    double wall_start = glfwGetTime();

    /* ---- Main loop --------------------------------------------------------- */
    double prev_t = 0.0, rain_t = 0.0;
    double fps_t0 = glfwGetTime(), fps = 0.0, frame_ms = 0.0, last_frame = fps_t0;
    int    fps_frames = 0;
    long   mem_rss = 0, mem_anon = 0, mem_file = 0, mem_shmem = 0;

    uint64_t exe_bytes = platform_executable_size();

    while (!glfwWindowShouldClose(win)) {
        if (g_reload_requested) { g_reload_requested = 0; reload_shaders_from_disk(); }

        /* MASTER CLOCK: the music if we have it, the wall clock otherwise.
         * With audio: the estimated heard position, plus the (smoothed) ALSA
         * delay, minus the user's trim. See the SYNC comment at the top. */
        double t = audio_time();
        if (t < 0.0) t = glfwGetTime() - wall_start;
        else         t += audio_visual_lead() - g_sync;

        /* BEAT SYNC. The whole approach in three lines:
         *   beats = seconds * BPM / 60              e.g. 128 BPM is 2.133 beats/s
         *   phase = fract(beats)                    0 on the kick, rising to 1
         *   pulse = exp(-phase * 5)                 1 on the kick, decays fast
         * An exponential decay looks like a light being struck and fading,
         * which reads as "hit" to the eye. With a known BPM no audio analysis
         * (FFT, onset detection) is needed at all. */
        double beats = (t - g_offset) * g_bpm / 60.0;
        double pulse = beats < 0.0 ? 0.0 : exp(-(beats - floor(beats)) * 5.0);

        /* RAIN TIME. We want the rain to speed up on each beat. The naive
         * approach, position = time * speed(t), is WRONG: when speed changes,
         * time*speed jumps for the whole screen at once and everything
         * teleports. The correct approach is to INTEGRATE speed over time:
         *   rain_t += dt * speed(t)
         * so the position changes smoothly however the speed varies. dt is
         * clamped so a hiccup (window drag, breakpoint) doesn't cause a jump. */
        double dt = t - prev_t;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;
        prev_t = t;
        rain_t += dt * (1.0 + 0.6 * pulse);

        /* The framebuffer can be larger than the window size on HiDPI
         * screens, so always size the viewport from the framebuffer. */
        int fb_w, fb_h;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        if (fb_w < 1 || fb_h < 1) { glfwPollEvents(); continue; } /* minimised */

        /* Scene size = display size x render scale (at least 1 pixel). */
        int rt_w = (int)(fb_w * SCALES[g_scale_idx]), rt_h = (int)(fb_h * SCALES[g_scale_idx]);
        if (rt_w < 1) rt_w = 1;
        if (rt_h < 1) rt_h = 1;
        ensure_render_target(rt_w, rt_h);

        /* UNIFORMS: how time and resolution reach the shader. A uniform is a
         * read-only global variable for one draw call: every pixel sees the same
         * value. We set them right before drawing. The HUD uses its own program
         * and VAO, so re-bind ours every frame. */
        glBindVertexArray(vao);
        glUseProgram(g_program);
        glUniform2f(u_resolution, (float)rt_w, (float)rt_h);
        glUniform1f(u_time,       (float)t);
        glUniform1f(u_rain_time,  (float)rain_t);
        glUniform1f(u_beat,       (float)pulse);
        glUniform1f(u_beat_count, (float)beats);

        /* Collect the oldest timer result if it's ready (never wait for it). */
        int qi = frame_no % NQ;
        if (gpu_q_used[qi]) {
            GLint ready = 0;
            glGetQueryObjectiv(gpu_q[qi], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(gpu_q[qi], GL_QUERY_RESULT, &ns);
                gpu_ms += (ns / 1e6 - gpu_ms) * 0.05;   /* smooth like frame_ms */
                /* Averages hide spikes (virus hits are the heaviest
                 * frames), so track the worst frame too. */
                if (ns / 1e6 > gpu_peak_acc) gpu_peak_acc = ns / 1e6;
            }
        }

        /* Both passes are timed, but not the HUD, so the number is the
         * demo's own cost. No glClear needed: each full-screen triangle
         * overwrites every pixel of its target. */
        glBeginQuery(GL_TIME_ELAPSED, gpu_q[qi]);

        /* PASS 1: the scene, into the off-screen texture. */
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glViewport(0, 0, rt_w, rt_h);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* PASS 2: post-processing, from the texture onto the window. The
         * texture is bound to "texture unit 0", and the uScene sampler is
         * told to read unit 0. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fb_w, fb_h);
        glUseProgram(g_post);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_scene_tex);
        glUniform1i(p_scene, 0);
        glUniform2f(p_resolution, (float)fb_w, (float)fb_h);
        glUniform1f(p_time, (float)t);
        glUniform1f(p_beat_count, (float)beats);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glEndQuery(GL_TIME_ELAPSED);
        gpu_q_used[qi] = 1;
        frame_no++;

        if (smoke_test && frame_no >= 8) {
            /* Hidden-window integration check: compile the embedded shaders,
             * render both passes and verify actual RGB output and GL errors. */
            size_t bytes = (size_t)fb_w * (size_t)fb_h * 4;
            unsigned char *pixels = malloc(bytes);
            uint64_t rgb_sum = 0;
            if (pixels) {
                glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                for (size_t i = 0; i < bytes; i += 4)
                    rgb_sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            }
            GLenum error = glGetError();
            exit_code = !pixels || !rgb_sum || error != GL_NO_ERROR;
            fprintf(stderr, "render smoke test: %s (%dx%d, RGB sum %llu, GL error 0x%x)\n",
                    exit_code ? "FAILED" : "PASS", fb_w, fb_h, (unsigned long long)rgb_sum, error);
            free(pixels);
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        }

        if (g_hud_on) {
            HudInfo hi = {
                .fps = fps, .frame_ms = frame_ms, .gpu_ms = gpu_ms, .gpu_peak_ms = gpu_peak_ms,
                .vsync = g_vsync, .fb_w = fb_w, .fb_h = fb_h,
                .rt_w = rt_w, .rt_h = rt_h, .scale_pc = (int)(SCALES[g_scale_idx] * 100.0 + 0.5),
                .gl_renderer = gl_renderer, .gl_version = gl_version,
                .audio_source = wav ? wav : "procedural",
                .audio_backend = audio_backend_name(),
                .audio_rate = audio_rate(),
                .audio_delay_ms = audio_reported_delay() * 1000.0,
                .sync_ms = g_sync * 1000.0,
                .auto_sync_ms = -audio_visual_lead() * 1000.0,
                .rss_kb = mem_rss, .anon_kb = mem_anon, .file_kb = mem_file, .shmem_kb = mem_shmem,
                .exe_bytes = exe_bytes,
                .sync_source = g_sync_source,
                .muted = audio_get_mute(),
                .bpm = g_bpm, .beats = beats,
                .other_gpu = other_gpu,
            };
            /* Show only the file name of a WAV path, not the directories. */
            if (wav) hi.audio_source = platform_basename(wav);
            hud_draw(&hi, fb_w, fb_h);
        }

        /* Show the finished frame (blocks for vsync), then handle input. */
        glfwSwapBuffers(win);
        glfwPollEvents();

        /* FPS: frame_ms is smoothed per frame (exponential moving average),
         * the title-bar counter is averaged over a whole second. */
        fps_frames++;
        double now = glfwGetTime();
        frame_ms += ((now - last_frame) * 1000.0 - frame_ms) * 0.05;
        last_frame = now;
        if (now - fps_t0 >= 1.0) {
            fps = fps_frames / (now - fps_t0);
            read_memory(&mem_rss, &mem_anon, &mem_file, &mem_shmem); /* once a second is plenty */
            gpu_peak_ms = gpu_peak_acc;
            gpu_peak_acc = 0.0;
            char title[96];
            snprintf(title, sizeof title, "MATRIX RAIN // %.0f BPM // %d fps // beat %.0f%s",
                     g_bpm, (int)(fps_frames / (now - fps_t0) + 0.5), floor(beats),
                     audio_get_mute() ? " // muted" : "");
            glfwSetWindowTitle(win, title);
            fps_frames = 0;
            fps_t0 = now;
        }
    }

    /* ---- Cleanup ----------------------------------------------------------- *
     * The OS would reclaim all of this at exit anyway, but stopping the audio
     * thread cleanly avoids a tail of stale buffer, and tidy teardown is a good
     * habit (and keeps tools like valgrind quiet). */
    audio_stop();
    hud_shutdown();
    glDeleteQueries(NQ, gpu_q);
    glDeleteFramebuffers(1, &g_fbo);
    glDeleteTextures(1, &g_scene_tex);
    glDeleteProgram(g_post);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(g_program);
    glfwDestroyWindow(win);
    glfwTerminate();

#ifndef _WIN32
    if (g_switch_gpu && other_gpu) relaunch_on_other_gpu(argv, on_nvidia);
#endif
    return exit_code;
}
