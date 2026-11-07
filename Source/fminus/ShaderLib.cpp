// Source/fminus/ShaderLib.cpp: see ShaderLib.h. The library's texts are bgfx's shader language as shaderc's HLSL
// front end reads it on every renderer (Metal and SPIR-V through glslang's HLSL, Direct3D 11 through d3dcompiler): no
// vector made from one number (vec2_splat), no vector picked with ?:, texture2DLod (no derivatives, so a sample in a
// loop or a branch is fine), a fixed count for every loop, and no name HLSL keeps for itself (line, point, half).
#include "fminus/ShaderLib.h"

#include <cstdio>

namespace playground::shaderlib
{
    namespace
    {
        // funk/text.sh, its numbers as $NAME (filled from ShaderLib.h's constants).
        constexpr const char* kText = R"SH(// funk/text.sh: text and numbers in the panel's own font (JetBrains Mono: every character as wide), in px from
// the shader's top left corner with y down (v_px). Each gives the coverage (0..1) of its letters at p: mix a colour in
// by it. at: where the text stands (its left end with align 0, its middle 0.5, its right end 1), at the middle of its
// capitals' height; size: the font's size in px, as the panel's text has it (11: a label's).
//   text(p, at, size, align, "RPM")                 a string: up to 12 characters (a vec4), 48 (a mat4)
//   number(p, at, size, align, value, decimals)     a number: "1250", "-3.5" (decimals 0 to 6; 9999999 at most)
//   textWidth(size, "RPM")                          a string's width in px
#ifndef FUNK_TEXT_SH
#define FUNK_TEXT_SH

SAMPLER2D(s_funkFont, 0);

#define FUNK_FONT_ATLAS vec2($ATLAS_W.0, $ATLAS_H.0)
#define FUNK_FONT_CELL vec2($CELL_W.0, $CELL_H.0)
#define FUNK_FONT_PEN vec2($PEN_X.0, $BASELINE.0)
#define FUNK_FONT_COLUMNS $COLUMNS.0
#define FUNK_FONT_BASE $BASE
#define FUNK_FONT_SPREAD $SPREAD
#define FUNK_FONT_ADVANCE $ADVANCE
#define FUNK_FONT_CAP $CAP
#define FUNK_FONT_MINUS $MINUS.0

// The coverage at p of the glyph `code` whose pen (its baseline's left end) is at `pen`, scale px a texel.
float funkGlyph(vec2 p, vec2 pen, float scale, float code)
{
    vec2 local = (p - pen) / scale + FUNK_FONT_PEN;
    if (code < 32.5 || local.x < 0.0 || local.y < 0.0 || local.x > FUNK_FONT_CELL.x || local.y > FUNK_FONT_CELL.y)
        return 0.0;
    float cell = code - 32.0;
    float row = floor((cell + 0.5) / FUNK_FONT_COLUMNS);
    vec2 at = vec2(cell - FUNK_FONT_COLUMNS * row, row) * FUNK_FONT_CELL + local;
    float d = texture2DLod(s_funkFont, at / FUNK_FONT_ATLAS, 0.0).x;
    return clamp((d - 0.5) * 2.0 * FUNK_FONT_SPREAD * scale / u_px + 0.5, 0.0, 1.0);
}

// The code of a string's character i (0 for none), three a float, the first the lowest byte.
float funkCode(vec4 s, float i)
{
    float k = floor((i + 0.5) / 3.0);
    float c = k < 0.5 ? s.x : (k < 1.5 ? s.y : (k < 2.5 ? s.z : s.w));
    float b = i - 3.0 * k;
    float shifted = floor(c / (b < 0.5 ? 1.0 : (b < 1.5 ? 256.0 : 65536.0)));
    return shifted - 256.0 * floor(shifted / 256.0);
}

float funkCode(mat4 s, float i)
{
    float r = floor((i + 0.5) / 12.0);
    return funkCode(s[int(r)], i - 12.0 * r);
}

float funkLength(vec4 s)
{
    vec4 b = step(vec4_splat(0.5), s) + step(vec4_splat(255.5), s) + step(vec4_splat(65535.5), s);
    return b.x + b.y + b.z + b.w;
}

float funkLength(mat4 s)
{
    return funkLength(s[0]) + funkLength(s[1]) + funkLength(s[2]) + funkLength(s[3]);
}

float textWidth(float size, vec4 s)
{
    return funkLength(s) * FUNK_FONT_ADVANCE * size / FUNK_FONT_BASE;
}

float textWidth(float size, mat4 s)
{
    return funkLength(s) * FUNK_FONT_ADVANCE * size / FUNK_FONT_BASE;
}

float text(vec2 p, vec2 at, float size, float align, vec4 s)
{
    if (abs(p.y - at.y) > size)
        return 0.0;
    float scale = size / FUNK_FONT_BASE;
    float advance = FUNK_FONT_ADVANCE * scale;
    float n = funkLength(s);
    vec2 pen = vec2(at.x - align * n * advance, at.y + 0.5 * FUNK_FONT_CAP * scale);
    float i = floor((p.x - pen.x) / advance);
    float ink = 0.0;
    for (int k = -1; k <= 1; ++k)
    {
        float j = i + float(k);
        if (j >= 0.0 && j < n)
            ink = max(ink, funkGlyph(p, pen + vec2(j * advance, 0.0), scale, funkCode(s, j)));
    }
    return ink;
}

float text(vec2 p, vec2 at, float size, float align, mat4 s)
{
    if (abs(p.y - at.y) > size)
        return 0.0;
    float scale = size / FUNK_FONT_BASE;
    float advance = FUNK_FONT_ADVANCE * scale;
    float n = funkLength(s);
    vec2 pen = vec2(at.x - align * n * advance, at.y + 0.5 * FUNK_FONT_CAP * scale);
    float i = floor((p.x - pen.x) / advance);
    float ink = 0.0;
    for (int k = -1; k <= 1; ++k)
    {
        float j = i + float(k);
        if (j >= 0.0 && j < n)
            ink = max(ink, funkGlyph(p, pen + vec2(j * advance, 0.0), scale, funkCode(s, j)));
    }
    return ink;
}

// The code of the digit e places from the right (0: the ones) of the whole number r.
float funkDigit(int r, int e)
{
    int ten = 1;
    for (int k = 0; k < 7; ++k)
        if (k < e)
            ten *= 10;
    return float(48 + (r / ten) % 10);
}

float number(vec2 p, vec2 at, float size, float align, float value, float decimals)
{
    if (abs(p.y - at.y) > size)
        return 0.0;
    int d = int(clamp(floor(decimals + 0.5), 0.0, 6.0));
    float m = 1.0;
    for (int k = 0; k < 6; ++k)
        if (k < d)
            m *= 10.0;
    // Rounded by its fraction: from 2^23 up a float's x + 0.5 is a tie, rounded to even.
    float a = abs(value) * m;
    int r = int(min(floor(a) + step(0.5, fract(a)), 9999999.0));
    int digits = 1;
    int ten = 10;
    for (int k = 0; k < 7; ++k)
    {
        if (r >= ten)
            digits += 1;
        ten *= 10;
    }
    digits = max(digits, d + 1);
    float minus = value < 0.0 && r > 0 ? 1.0 : 0.0;
    float n = minus + float(digits) + (d > 0 ? 1.0 : 0.0);
    float scale = size / FUNK_FONT_BASE;
    float advance = FUNK_FONT_ADVANCE * scale;
    vec2 pen = vec2(at.x - align * n * advance, at.y + 0.5 * FUNK_FONT_CAP * scale);
    float i = floor((p.x - pen.x) / advance);
    float ink = 0.0;
    for (int k = -1; k <= 1; ++k)
    {
        float j = i + float(k);
        if (j >= 0.0 && j < n)
        {
            int q = int(n - 1.0 - j);
            float code = funkDigit(r, q > d && d > 0 ? q - 1 : q);
            if (d > 0 && q == d)
                code = 46.0;
            if (minus > 0.5 && j < 0.5)
                code = FUNK_FONT_MINUS;
            ink = max(ink, funkGlyph(p, pen + vec2(j * advance, 0.0), scale, code));
        }
    }
    return ink;
}

#endif
)SH";

        constexpr const char* kShapes = R"SH(// funk/shapes.sh: shapes for a panel's shader, in px from its top left corner with y down (v_px). Each gives the
// coverage (0..1) of its shape at p, its edge smoothed over one of the screen's pixels (u_px): mix a colour in by it,
// col = mix(col, ink, box(v_px, c, size, 6.0)). Angles are degrees clockwise from straight up, as a gauge reads them
// (a sweep from -135 to 135).
//   cover(d)                          a signed distance's (d < 0 inside, px)   glow(d, r): a halo r px wide outside it
//   sdBox(p, c, size, radius)  sdLine(p, a, b)   the distances box() and segment() use
//   box(p, c, size, radius)           a box, its middle c, its corners rounded
//   circle(p, c, r)  ring(p, c, r, w) a disc; a ring w px wide
//   segment(p, a, b, w)               a line from a to b, w px wide, its ends round
//   arc(p, c, r, w, from, to)         a ring's part from one angle to another
//   ticks(p, c, r0, r1, w, from, to, n)   n marks from radius r0 to r1, spread over that sweep
//   needle(p, c, angle, length, w)    a needle from its hub c, narrowing to its tip
//   dirOf(angle)  angleOf(p, c)       an angle's direction; p's angle around c
//   digit7(p, c, h, d)                a seven-segment digit, its middle c, h px tall (d 0 to 9; any other value none)
//   number7(p, at, h, value, n)       a whole number (to 9999999) in n seven-segment digits, its right end at at (the
//                                     digits' middle), leading zeros dark; 8888888.0 lights every segment: unlit ghosts
//   ramp(a, b, t)  ramp3(a, b, c, t)  two and three colours along t (0..1)
//   along(p, a, b)  around(p, c, r)   t for a straight gradient from a to b, a round one from c out to r
#ifndef FUNK_SHAPES_SH
#define FUNK_SHAPES_SH

float cover(float d)
{
    return clamp(0.5 - d / u_px, 0.0, 1.0);
}

float glow(float d, float r)
{
    return exp(-max(d, 0.0) / r);
}

float sdBox(vec2 p, vec2 c, vec2 size, float radius)
{
    vec2 q = abs(p - c) - 0.5 * size + vec2_splat(radius);
    return length(max(q, vec2_splat(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

float sdLine(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float t = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(pa - ba * t);
}

float box(vec2 p, vec2 c, vec2 size, float radius)
{
    return cover(sdBox(p, c, size, radius));
}

float circle(vec2 p, vec2 c, float r)
{
    return cover(length(p - c) - r);
}

float ring(vec2 p, vec2 c, float r, float w)
{
    return cover(abs(length(p - c) - r) - 0.5 * w);
}

float segment(vec2 p, vec2 a, vec2 b, float w)
{
    return cover(sdLine(p, a, b) - 0.5 * w);
}

vec2 dirOf(float angle)
{
    float a = radians(angle);
    return vec2(sin(a), -cos(a));
}

float angleOf(vec2 p, vec2 c)
{
    vec2 q = p - c;
    return degrees(atan2(q.x, -q.y));
}

float arc(vec2 p, vec2 c, float r, float w, float from, float to)
{
    float mid = 0.5 * (from + to);
    float span = 0.5 * abs(to - from);
    float delta = angleOf(p, c) - mid;
    delta -= 360.0 * floor((delta + 180.0) / 360.0);
    float d = length(p - c);
    float past = (abs(delta) - span) * 0.017453293 * d;
    return cover(max(abs(d - r) - 0.5 * w, past));
}

float ticks(vec2 p, vec2 c, float r0, float r1, float w, float from, float to, float n)
{
    float pitch = (to - from) / max(n - 1.0, 1.0);
    // p's angle from `from`, wrapped about the sweep's middle as arc() wraps it: a point just past either end is that
    // end's tick's, never the other end's (either way round).
    float mid = 0.5 * (from + to);
    float rel = angleOf(p, c) - mid;
    rel -= 360.0 * floor((rel + 180.0) / 360.0);
    rel += mid - from;
    float k = abs(pitch) > 1e-6 ? clamp(floor(rel / pitch + 0.5), 0.0, n - 1.0) : 0.0;
    vec2 dir = dirOf(from + k * pitch);
    return cover(sdLine(p, c + dir * r0, c + dir * r1) - 0.5 * w);
}

float needle(vec2 p, vec2 c, float angle, float len, float w)
{
    vec2 dir = dirOf(angle);
    vec2 a = c - dir * (0.15 * len);
    vec2 ba = dir * (1.15 * len);
    float t = clamp(dot(p - a, ba) / dot(ba, ba), 0.0, 1.0);
    return cover(length(p - a - ba * t) - mix(0.5 * w, 0.15 * w, t));
}

// A digit's lit segments, a to g as 1, 2, 4 ... 64 (none for a value that is no digit).
float funkSegments(float d)
{
    float m = 0.0;
    m += step(-0.5, d) * step(d, 0.5) * 63.0;
    m += step(0.5, d) * step(d, 1.5) * 6.0;
    m += step(1.5, d) * step(d, 2.5) * 91.0;
    m += step(2.5, d) * step(d, 3.5) * 79.0;
    m += step(3.5, d) * step(d, 4.5) * 102.0;
    m += step(4.5, d) * step(d, 5.5) * 109.0;
    m += step(5.5, d) * step(d, 6.5) * 125.0;
    m += step(6.5, d) * step(d, 7.5) * 7.0;
    m += step(7.5, d) * step(d, 8.5) * 127.0;
    m += step(8.5, d) * step(d, 9.5) * 111.0;
    return m;
}

float funkBit(float m, float bit)
{
    float shifted = floor(m / bit);
    return shifted - 2.0 * floor(shifted / 2.0);
}

float digit7(vec2 p, vec2 c, float h, float d)
{
    vec2 q = (p - c) / h;
    if (abs(q.y) > 0.6 || abs(q.x) > 0.4)
        return 0.0;
    float m = funkSegments(floor(d + 0.5));
    float w = 0.055;
    float x = 0.22;
    float y = 0.44;
    float lit = 0.0;
    lit = max(lit, funkBit(m, 1.0) * cover((sdLine(q, vec2(w - x, -y), vec2(x - w, -y)) - w) * h));
    lit = max(lit, funkBit(m, 2.0) * cover((sdLine(q, vec2(x, w - y), vec2(x, -w)) - w) * h));
    lit = max(lit, funkBit(m, 4.0) * cover((sdLine(q, vec2(x, w), vec2(x, y - w)) - w) * h));
    lit = max(lit, funkBit(m, 8.0) * cover((sdLine(q, vec2(w - x, y), vec2(x - w, y)) - w) * h));
    lit = max(lit, funkBit(m, 16.0) * cover((sdLine(q, vec2(-x, w), vec2(-x, y - w)) - w) * h));
    lit = max(lit, funkBit(m, 32.0) * cover((sdLine(q, vec2(-x, w - y), vec2(-x, -w)) - w) * h));
    lit = max(lit, funkBit(m, 64.0) * cover((sdLine(q, vec2(w - x, 0.0), vec2(x - w, 0.0)) - w) * h));
    return lit;
}

float number7(vec2 p, vec2 at, float h, float value, float n)
{
    float pitch = 0.62 * h;
    float k = floor((at.x - p.x) / pitch);
    if (k < 0.0 || k > n - 1.0)
        return 0.0;
    // Rounded by its fraction and taken apart in ints: from 2^23 up a float's x + 0.5 is a tie, rounded to even.
    float a = abs(value);
    int v = int(min(floor(a) + step(0.5, fract(a)), 9999999.0));
    int ten = 1;
    for (int i = 0; i < 7; ++i)
        if (float(i) < k)
            ten *= 10;
    float digit = float((v / ten) % 10);
    if (k > 0.5 && v < ten)
        digit = 10.0;
    return digit7(p, vec2(at.x - (k + 0.5) * pitch, at.y), h, digit);
}

vec3 ramp(vec3 a, vec3 b, float t)
{
    return mix(a, b, clamp(t, 0.0, 1.0));
}

vec3 ramp3(vec3 a, vec3 b, vec3 c, float t)
{
    return mix(mix(a, b, clamp(2.0 * t, 0.0, 1.0)), c, clamp(2.0 * t - 1.0, 0.0, 1.0));
}

float along(vec2 p, vec2 a, vec2 b)
{
    vec2 ba = b - a;
    return clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
}

float around(vec2 p, vec2 c, float r)
{
    return clamp(length(p - c) / r, 0.0, 1.0);
}

#endif
)SH";

        void fill(std::string& s, const std::string& name, const std::string& value)
        {
            for (std::size_t at = s.find(name); at != std::string::npos; at = s.find(name, at + value.size()))
                s.replace(at, name.size(), value);
        }

        std::string number(float v)
        {
            char out[32];
            std::snprintf(out, sizeof out, "%.6f", static_cast<double>(v));
            return out;
        }

        std::vector<LibraryFile> made()
        {
            std::string text = kText;
            // Longer names first: $CELL_W is not $CELL's.
            fill(text, "$ATLAS_W", std::to_string(kAtlasW));
            fill(text, "$ATLAS_H", std::to_string(kAtlasH));
            fill(text, "$CELL_W", std::to_string(kCellW));
            fill(text, "$CELL_H", std::to_string(kCellH));
            fill(text, "$PEN_X", std::to_string(kPenX));
            fill(text, "$BASELINE", std::to_string(kBaseline));
            fill(text, "$COLUMNS", std::to_string(kColumns));
            fill(text, "$BASE", number(kBasePx));
            fill(text, "$SPREAD", number(kSpread));
            fill(text, "$ADVANCE", number(kAdvance));
            fill(text, "$CAP", number(kCapHeight));
            fill(text, "$MINUS", std::to_string(glyphCode(0x2212)));
            return { { "funk/text.sh", text }, { "funk/shapes.sh", kShapes } };
        }
    }

    int glyphCode(uint32_t codePoint)
    {
        if (codePoint >= 32 && codePoint <= 126)
            return static_cast<int>(codePoint);
        for (std::size_t k = 0; k < kExtra.size(); ++k)
            if (kExtra[k] == codePoint)
                return kFirstExtra + static_cast<int>(k);
        return -1;
    }

    uint32_t codePointOf(int code)
    {
        if (code >= 32 && code <= 126)
            return static_cast<uint32_t>(code);
        if (code >= kFirstExtra && code < kCodes)
            return kExtra[static_cast<std::size_t>(code - kFirstExtra)];
        return 0;
    }

    const std::vector<LibraryFile>& library()
    {
        static const std::vector<LibraryFile> files = made();
        return files;
    }

    const LibraryFile* libraryFile(std::string_view name)
    {
        for (const LibraryFile& f : library())
            if (f.name == name)
                return &f;
        return nullptr;
    }
}
