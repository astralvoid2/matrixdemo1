#version 330 core
/*
 * shader.frag — Matrix digital rain, 100% procedural.
 *
 * THE BIG IDEA
 * ------------
 * A fragment shader runs once per pixel, in parallel, with no memory of
 * other pixels or of previous frames. So we can't "move characters down the
 * screen" the way a CPU program would. Instead each pixel asks:
 *
 *   1. Which character cell am I in?            (divide by the cell size)
 *   2. Where is the falling drop in my column right now?
 *                                               (a pure function of time)
 *   3. How far behind the drop's head is my cell?  (gives brightness)
 *   4. Which glyph lives in my cell, and is my pixel one of its "on" dots?
 *
 * Everything is a deterministic function of (pixel position, time). The
 * "randomness" (column speeds, glyph shapes) comes from HASH FUNCTIONS: they
 * turn integers (column number, row number...) into numbers that look
 * random but are always the same for the same input. That's how we get
 * per-column variety without storing any per-column data.
 *
 * Two copies of the rain are drawn: a small, dim, slow "far" layer and a
 * big, bright "near" layer. Different sizes and speeds fake depth (parallax).
 *
 * COORDINATES
 *   gl_FragCoord.xy = pixel centre in window pixels, origin BOTTOM-left,
 *   y goes up. Rain falls downward, so we count rows from the top.
 *
 * Edit this file and press R in the running demo to hot-reload it.
 */

out vec4 fragColor;

/* ---- Uniforms: values the CPU sets once per frame (see main.c) ---- */
uniform vec2  uResolution; // framebuffer size in pixels
uniform float uTime;       // seconds of music heard (the master clock)
uniform float uRainTime;   // "warped" time for falling motion (speeds up on beats)
uniform float uBeat;       // 1.0 at each kick drum, decays towards 0 before the next
uniform float uBeatCount;  // beats elapsed since the music started (fractional)

/* ---- Tunables. Units: rows per second, or rows (character cells). ---- */
const float SPEED_MIN  = 5.0;
const float SPEED_MAX  = 17.0;
const float TRAIL_MIN  = 6.0;
const float TRAIL_MAX  = 28.0;
const float CELL_ASPECT = 0.62;              // cell width / height (glyphs are tall)
const vec3  TRAIL_COLOR = vec3(0.10, 1.00, 0.35);
const vec3  HEAD_COLOR  = vec3(0.80, 1.00, 0.88);

/* ======================================================================== */
/* HASHING                                                                   */
/* ======================================================================== */
/*
 * Many shader tutorials use  fract(sin(x) * 43758.5453).  That trick depends
 * on how precisely each GPU computes sin() of large numbers, so it can give
 * different patterns (or visible banding) on different vendors' drivers.
 * GLSL 3.30 has real 32-bit unsigned integers, so we use an integer hash
 * instead. It's exact and identical on every GPU.
 *
 * hashU is "lowbias32" by Chris Wellons: alternating xor-shifts (fold high
 * bits down) and multiplications by odd constants (spread low bits up).
 * After two rounds, flipping any input bit flips about half of the output
 * bits ("avalanche"), which is what makes neighbouring columns look unrelated.
 */
uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* uint to float in [0,1). We keep the top 24 bits because a float's mantissa
 * holds exactly 24 bits, so the conversion is exact. */
float toF(uint x) { return float(x >> 8) * (1.0 / 16777216.0); }

/* Hash two values together. The golden-ratio constant 0x9E3779B9 keeps
 * (a,b) and (b,a) and small values from colliding. */
float hash2(uint a, uint b) { return toF(hashU(a ^ hashU(b + 0x9E3779B9u))); }
uint  hashU3(uint a, uint b, uint c) { return hashU(a ^ hashU(b ^ hashU(c + 0x9E3779B9u))); }

/* ======================================================================== */
/* DROPS: column speed & reset logic                                          */
/* ======================================================================== */
/*
 * Each column hosts two independent drops (k = 0, 1), so columns sometimes
 * have two streams at once. A drop's head moves down at `speed` rows/sec.
 *
 * RESET LOGIC WITHOUT STATE: the head position is wrapped with a modulo,
 *     pos  = rainTime * speed + randomOffset
 *     head = pos mod cycle
 * where cycle = (screen rows) + (max trail length) + (random gap).
 * So the head enters at the top (head = 0), falls past the bottom, keeps going
 * until its whole tail has left the screen, waits off-screen for the "gap",
 * then wraps to 0 and enters again. The random gap stops columns from
 * restarting in lock-step.
 *
 * iter = floor(pos / cycle) counts how many times the drop has wrapped. Using
 * it as a hash input gives every pass a new trail length and new identity,
 * so a column never repeats exactly. The change happens while the drop is
 * off-screen, so it's invisible.
 */
struct Drop {
    float head;   // row index of the head (0 = top row; fractional)
    float trail;  // trail length in rows
};

Drop getDrop(uint column, uint k, float rows, float speedMul)
{
    uint  id    = hashU(column * 2u + k);
    float speed = mix(SPEED_MIN, SPEED_MAX, hash2(id, 1u)) * speedMul;
    float gap   = hash2(id, 2u) * rows * 1.2;
    float cycle = rows + TRAIL_MAX + gap;
    float pos   = uRainTime * speed + hash2(id, 3u) * cycle;
    float iter  = floor(pos / cycle);

    Drop d;
    d.head  = pos - iter * cycle;                             // = pos mod cycle
    d.trail = mix(TRAIL_MIN, TRAIL_MAX, hash2(id, uint(iter) * 7u + 4u));
    return d;
}

/* ======================================================================== */
/* GLYPHS: procedural 5x7 dot-matrix characters                               */
/* ======================================================================== */
/*
 * The film used mirrored katakana and digits. We don't ship a font texture,
 * so we invent glyphs: a 5x7 grid where each dot is on or off according to a
 * hash of (glyph id, dot x, dot y). Pure random dots look like noise, so half
 * the glyphs are made left-right SYMMETRIC (mirror x around the centre
 * column), and symmetric shapes read as "characters" to our brains.
 *
 * uv: position inside the cell, [0,1]^2 with y up. Returns dot coverage 0..1.
 */
float glyph(vec2 uv, uint gid)
{
    const vec2 GRID = vec2(5.0, 7.0);
    const vec2 PAD  = vec2(0.14, 0.08);         // empty margin around each glyph
    vec2 p = (uv - PAD) / (1.0 - 2.0 * PAD);    // 0..1 inside the glyph box
    if (any(lessThan(p, vec2(0.0))) || any(greaterThanEqual(p, vec2(1.0)))) return 0.0;

    p *= GRID;
    ivec2 ip = ivec2(p);                        // which dot (0..4, 0..6)
    vec2  fp = fract(p);                        // position inside that dot

    int x = ip.x;
    if ((gid & 1u) != 0u) x = min(x, 4 - x);    // symmetric glyph: mirror columns

    // ~45% of dots lit. Rows 0 and 6 are lit a bit less, which looks less blocky.
    float threshold = (ip.y == 0 || ip.y == 6) ? 0.65 : 0.55;
    float on = step(threshold, toF(hashU3(gid, uint(x), uint(ip.y))));

    // Round each dot slightly (LED / CRT phosphor look) instead of a hard square.
    vec2  q   = abs(fp - 0.5);
    float dot = 1.0 - smoothstep(0.36, 0.5, max(q.x, q.y));
    return on * dot;
}

/* ======================================================================== */
/* ONE LAYER OF RAIN                                                          */
/* ======================================================================== */
/*
 *  frag     : pixel position (y up)
 *  rows     : how many character rows fit on screen (sets the cell size)
 *  seed     : decorrelates layers so they don't share column patterns
 *  speedMul : layer speed multiplier (far = slower: parallax)
 */
vec3 rainLayer(vec2 frag, float rows, uint seed, float speedMul)
{
    float cellH = uResolution.y / rows;
    float cellW = cellH * CELL_ASPECT;

    // Grid coordinates: x = column (left to right), y = row (top to bottom).
    vec2  g     = vec2(frag.x / cellW, (uResolution.y - frag.y) / cellH);
    vec2  cell  = floor(g);
    vec2  local = vec2(fract(g.x), 1.0 - fract(g.y)); // inside the cell, y up again
    int   colI  = int(cell.x);
    float row   = cell.y;

    /* GLYPH CHOICE: a cell's character belongs to the CELL, not to the drop.
     * That's how the film looks: the "screen" of characters mostly stays put and
     * the falling drops light them up as they pass. About 30% of cells mutate
     * now and then, at their own random rate, which gives the shimmer. */
    uint  cellId  = hashU3(uint(colI) ^ seed, uint(row), 0x51u);
    float mutRate = toF(cellId) < 0.3 ? mix(1.0, 9.0, hash2(cellId, 5u)) : 0.05;

    float lit   = 0.0;   // trail brightness of this cell (max over both drops)
    float head  = 0.0;   // 1 if this cell is a drop head
    float glow  = 0.0;   // soft light around heads (bloom substitute)
    float haze  = 0.0;   // soft light along trails

    /* We look at our own column AND both neighbours, because a head's glow
     * spills sideways into the next columns. Without this, the glow would be
     * cut off with a hard vertical edge at the column border. The glyph itself
     * only needs our own column (dc == 0). */
    for (int dc = -1; dc <= 1; dc++) {
        // int to uint keeps the bit pattern, so column -1 hashes fine.
        uint column = uint(colI + dc) ^ seed;
        for (uint k = 0u; k < 2u; k++) {
            Drop  d        = getDrop(column, k, rows, speedMul);
            float headCell = floor(d.head);  // characters jump cell by cell, like
                                             // a terminal, so snap the head to a row

            /* GLOW: Gaussian falloff by distance (in cell-height units) from
             * this pixel to the head cell's centre. exp(-r^2 * k) is a cheap,
             * smooth blob, which is what bloom looks like. */
            vec2 toHead = vec2((g.x - (cell.x + float(dc) + 0.5)) * CELL_ASPECT,
                               g.y - (headCell + 0.5));
            // Only drops whose head is near our pixel matter; skip the exp otherwise.
            if (abs(toHead.y) < 3.0)
                glow += exp(-dot(toHead, toHead) * 1.6);

            if (dc != 0) continue;

            float dist = headCell - row;     // 0 = head cell, >0 = rows behind it
            if (dist >= 0.0 && dist <= d.trail) {
                float fade = 1.0 - dist / d.trail;
                fade *= fade;                // quadratic: long dim tail, bright front
                lit  = max(lit, fade);
                if (dist < 0.5) head = 1.0;
                // Haze: a fuzzy vertical strip centred on the column.
                float dx = local.x - 0.5;
                haze = max(haze, fade * exp(-dx * dx * 10.0));
            }
        }
    }

    // The head character mutates every frame-ish (fast flicker); others at their own rate.
    float rate = head > 0.0 ? 14.0 : mutRate;
    uint  gid  = hashU(cellId ^ uint(floor(uTime * rate + toF(cellId) * 7.0)));
    float dots = glyph(local, gid);

    /* BEAT REACTION: heads flare on each kick, the haze breathes. The fall
     * speed boost is done on the CPU side via uRainTime (see main.c for why). */
    float beat = uBeat;
    vec3  c    = TRAIL_COLOR * dots * lit * (0.85 + 0.35 * beat);
    c         += HEAD_COLOR  * dots * head * (1.3 + 1.0 * beat);
    c         += TRAIL_COLOR * haze * (0.07 + 0.08 * beat);
    c         += mix(TRAIL_COLOR, HEAD_COLOR, 0.5) * glow * (0.22 + 0.25 * beat);
    return c;
}

/* ======================================================================== */
void main()
{
    vec2 frag = gl_FragCoord.xy;

    /* GLITCH: during the last quarter-beat of every 16-beat phrase (4 bars),
     * slice the screen into horizontal bands and shove some of them sideways.
     * Moving the sample position before drawing is the cheapest possible
     * "VHS tear". It changes 8 times per beat, so it jitters. */
    float phrase = mod(uBeatCount, 16.0);
    if (uBeatCount > 0.0 && phrase > 15.75) {
        uint band = uint(frag.y / (uResolution.y / 28.0));
        uint tick = uint(uBeatCount * 8.0);
        float h   = hash2(band, tick);
        if (h > 0.6) frag.x += (hash2(band, tick + 99u) - 0.5) * uResolution.x * 0.08;
    }

    // Far layer: small cells, slower, dim and slightly bluer (atmospheric depth).
    vec3 col = rainLayer(frag, 58.0, 0xA341316Cu, 0.55) * vec3(0.30, 0.38, 0.42);
    // Near layer: big cells, full speed. Layers are light, so they ADD.
    col     += rainLayer(frag, 34.0, 0xC8013EA4u, 1.0);

    /* ---- Post-processing ---- */
    vec2 uv = frag / uResolution;

    // Vignette: darken towards the corners to frame the image.
    vec2 v = uv - 0.5;
    col *= 1.0 - dot(v, v) * 1.1;

    // Scanlines: sin(pi*y) at pixel centres (y = n + 0.5) alternates +1/-1,
    // so every other line is 12% darker. A cheap CRT feel.
    col *= 0.94 + 0.06 * sin(gl_FragCoord.y * 3.14159265);

    // Flash on the first beat of each 4-bar phrase.
    if (mod(uBeatCount, 16.0) < 1.0) col += TRAIL_COLOR * 0.05 * uBeat;

    // Film grain: tiny per-pixel, per-frame noise. Also hides gradient banding.
    uint px = uint(gl_FragCoord.x) + uint(gl_FragCoord.y) * 4096u;
    col += (toF(hashU(px ^ hashU(uint(uTime * 60.0)))) - 0.5) * 0.025;

    /* Tone mapping: 1 - exp(-x) maps [0, infinity) smoothly into [0, 1), so
     * bright heads plus glow roll off softly to white-green instead of
     * clipping to a flat blob. */
    col = 1.0 - exp(-col * 1.25);

    fragColor = vec4(col, 1.0);
}
