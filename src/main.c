/*
 * main.c — "MATRIX RAIN", a 4K-intro-style audiovisual demo for Linux.
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
#define GL_GLEXT_PROTOTYPES   /* declare glCreateShader & co. (see notes above) */
#define GLFW_INCLUDE_GLEXT    /* make GLFW also include <GL/glext.h>           */
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "synth.h"

/* -------------------------------------------------------------------------
 * The fragment shader source is EMBEDDED in the executable, so ./demo is a
 * single self-contained file. The Makefile runs
 *     ld -r -b binary shader.frag -o shader_frag.o
 * which wraps the raw bytes of shader.frag in an object file and defines
 * these two symbols marking the start and end of the data. (Not
 * NUL-terminated! That's why we pass an explicit length to glShaderSource.)
 * ------------------------------------------------------------------------- */
extern const char _binary_shader_frag_start[];
extern const char _binary_shader_frag_end[];

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
static GLuint g_program;
static GLint  u_resolution, u_time, u_rain_time, u_beat, u_beat_count;

static double g_bpm    = DEMO_BPM;  /* overridable with --bpm for custom tracks */
static double g_offset = 0.0;       /* seconds before the first beat in a WAV  */

static int g_fullscreen;
static int g_win_x, g_win_y, g_win_w = 1280, g_win_h = 720; /* saved windowed geometry */
static int g_reload_requested;

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

/* Make `prog` current and look up its uniforms by name.
 * A uniform the compiler optimised away (unused in the shader) returns -1,
 * and glUniform* calls with location -1 are silently ignored, so it's safe
 * to experiment with shaders that don't use every uniform. */
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

/* Hot reload: read the shader from disk and swap it in if it compiles. If it
 * doesn't, keep the old one running and print the errors, so you can iterate
 * on the visuals without restarting the music. We look in src/ (running
 * bin/demo from the repo root) and then the current directory (running from
 * inside src/, or a copy of the shader placed next to you). */
static void reload_shader_from_disk(void)
{
    const char *path = "src/shader.frag";
    FILE *f = fopen(path, "rb");
    if (!f) f = fopen(path = "shader.frag", "rb");
    if (!f) { fprintf(stderr, "reload: src/shader.frag not found (run from the repo root)\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n);
    if (src && fread(src, 1, (size_t)n, f) == (size_t)n) {
        GLuint prog = build_program(src, (GLint)n, path);
        if (prog) { use_program(prog); fprintf(stderr, "reload: %s OK\n", path); }
        else      fprintf(stderr, "reload: keeping previous shader\n");
    }
    free(src);
    fclose(f);
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
    glfwSwapInterval(1); /* some drivers reset vsync on a mode change */
}

/* GLFW calls this from glfwPollEvents() for every key press/release/repeat.
 * GLFW_KEY_* codes follow the US keyboard layout. */
static void on_key(GLFWwindow *win, int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_ESCAPE:
    case GLFW_KEY_Q:   glfwSetWindowShouldClose(win, GLFW_TRUE); break;
    case GLFW_KEY_F:
    case GLFW_KEY_F11: toggle_fullscreen(win); break;
    case GLFW_KEY_M:   audio_set_mute(!audio_get_mute()); break;
    case GLFW_KEY_R:   g_reload_requested = 1; break; /* do GL work in the main loop */
    }
}

static void on_glfw_error(int code, const char *msg)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, msg);
}

/* ------------------------------------------------------------------------- */
static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "  --fullscreen        start fullscreen (toggle anytime with F / F11)\n"
           "  --wav FILE          play FILE (looped) instead of the built-in synth\n"
           "  --bpm N             tempo of --wav track for beat sync (default %.0f)\n"
           "  --offset S          seconds until the first beat in --wav track (default 0)\n"
           "  --nosound           no audio (visuals run on the wall clock)\n"
           "  --export FILE.wav   write the built-in track to a WAV file and exit\n"
           "keys: ESC/Q quit, F/F11 fullscreen, M mute, R reload shader.frag\n",
           argv0, DEMO_BPM);
}

int main(int argc, char **argv)
{
    const char *wav = NULL;
    int nosound = 0, start_fullscreen = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if      (!strcmp(a, "--fullscreen"))       start_fullscreen = 1;
        else if (!strcmp(a, "--nosound"))          nosound = 1;
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

    GLFWwindow *win = glfwCreateWindow(g_win_w, g_win_h, "MATRIX RAIN // 128 BPM", NULL, NULL);
    if (!win) {
        fprintf(stderr, "Could not create an OpenGL 3.3 core window.\n"
                        "Check `glxinfo -B` (package mesa-utils) for your GL version.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);  /* GL calls on this thread now go to this context */
    glfwSetKeyCallback(win, on_key);

    /* VSYNC: wait for the monitor's vertical blank before swapping. That caps
     * us at the refresh rate (60/144/... Hz), avoids tearing, and stops
     * the GPU from rendering thousands of frames per second for no reason. */
    glfwSwapInterval(1);

    /* Print which GPU we got. Useful on hybrid laptops (see README: PRIME). */
    fprintf(stderr, "GL_VENDOR   : %s\nGL_RENDERER : %s\nGL_VERSION  : %s\n",
            glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

    if (start_fullscreen) toggle_fullscreen(win);

    /* ---- Shaders ----------------------------------------------------------- */
    GLint frag_len = (GLint)(_binary_shader_frag_end - _binary_shader_frag_start);
    GLuint prog = build_program(_binary_shader_frag_start, frag_len, "shader.frag (embedded)");
    if (!prog) { glfwTerminate(); return 1; }
    use_program(prog);

    /* An empty VAO: no vertex attributes, but core profile refuses to draw
     * without one bound. */
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* ---- Audio ------------------------------------------------------------- *
     * Started after the window exists, so the music doesn't begin while the
     * screen is still black. */
    if (!nosound) {
        int rc = wav ? audio_start_wav(wav) : audio_start_procedural();
        if (rc != 0) fprintf(stderr, "audio unavailable, continuing silently\n");
    }
    double wall_start = glfwGetTime();

    /* ---- Main loop --------------------------------------------------------- */
    double prev_t = 0.0, rain_t = 0.0;
    double fps_t0 = glfwGetTime();
    int    fps_frames = 0;

    while (!glfwWindowShouldClose(win)) {
        if (g_reload_requested) { g_reload_requested = 0; reload_shader_from_disk(); }

        /* MASTER CLOCK: the music if we have it, the wall clock otherwise. */
        double t = audio_time();
        if (t < 0.0) t = glfwGetTime() - wall_start;

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
        glViewport(0, 0, fb_w, fb_h);

        /* UNIFORMS: how time and resolution reach the shader. A uniform is a
         * read-only global variable for one draw call: every pixel sees the same
         * value. We set them right before drawing. */
        glUniform2f(u_resolution, (float)fb_w, (float)fb_h);
        glUniform1f(u_time,       (float)t);
        glUniform1f(u_rain_time,  (float)rain_t);
        glUniform1f(u_beat,       (float)pulse);
        glUniform1f(u_beat_count, (float)beats);

        /* No glClear needed: the triangle overwrites every pixel. */
        glDrawArrays(GL_TRIANGLES, 0, 3);

        /* Show the finished frame (blocks for vsync), then handle input. */
        glfwSwapBuffers(win);
        glfwPollEvents();

        /* FPS counter in the window title, updated once a second. */
        fps_frames++;
        double now = glfwGetTime();
        if (now - fps_t0 >= 1.0) {
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
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(g_program);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
