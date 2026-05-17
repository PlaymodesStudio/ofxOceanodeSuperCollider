//
//  noise.cpp
//  PinkTrombone
//
//  Simplex noise by Stefan Gustavson (public domain), adapted to sample_t.
//  Vendored into ofxSantiNodes.
//

#include "noise.hpp"
#include <math.h>
#include <stdlib.h>

// ── Gradient table (12 edges of a cube) ──────────────────────────────────────
static int grad3[12][3] = {
    { 1, 1, 0}, {-1, 1, 0}, { 1,-1, 0}, {-1,-1, 0},
    { 1, 0, 1}, {-1, 0, 1}, { 1, 0,-1}, {-1, 0,-1},
    { 0, 1, 1}, { 0,-1, 1}, { 0, 1,-1}, { 0,-1,-1}
};

static int perm[512];
static int permMod12[512];
static bool noiseSeeded = false;

static inline sample_t dot2(const int g[3], sample_t x, sample_t y) {
    return (sample_t)g[0] * x + (sample_t)g[1] * y;
}

// ── Seed ─────────────────────────────────────────────────────────────────────
void pt_noise_seed(unsigned int s) {
    int p[256];
    for (int i = 0; i < 256; i++) p[i] = i;
    // Fisher–Yates shuffle with the provided seed
    unsigned int state = s;
    for (int i = 255; i > 0; i--) {
        state = state * 1664525u + 1013904223u; // LCG
        int j = (int)(state >> 16) % (i + 1);
        int tmp = p[i]; p[i] = p[j]; p[j] = tmp;
    }
    for (int i = 0; i < 512; i++) {
        perm[i]      = p[i & 255];
        permMod12[i] = perm[i] % 12;
    }
    noiseSeeded = true;
}

// ── 2-D simplex noise ─────────────────────────────────────────────────────────
sample_t simplex2(sample_t xin, sample_t yin) {
    if (!noiseSeeded) pt_noise_seed(42u);

    const sample_t F2 = (sample_t)(0.5 * (sqrt(3.0) - 1.0));
    const sample_t G2 = (sample_t)((3.0 - sqrt(3.0)) / 6.0);

    // Skew input to simplex cell
    sample_t s  = (xin + yin) * F2;
    int      i  = (int)floor(xin + s);
    int      j  = (int)floor(yin + s);
    sample_t t  = (sample_t)(i + j) * G2;

    // Unskew cell origin back to (x,y)
    sample_t X0 = (sample_t)i - t;
    sample_t Y0 = (sample_t)j - t;
    sample_t x0 = xin - X0;
    sample_t y0 = yin - Y0;

    // Determine which simplex triangle we're in
    int i1 = (x0 > y0) ? 1 : 0;
    int j1 = (x0 > y0) ? 0 : 1;

    // Offsets for the other two corners
    sample_t x1 = x0 - (sample_t)i1 + G2;
    sample_t y1 = y0 - (sample_t)j1 + G2;
    sample_t x2 = x0 - 1.0f + 2.0f * G2;
    sample_t y2 = y0 - 1.0f + 2.0f * G2;

    // Hashed gradient indices
    int ii  = i & 255;
    int jj  = j & 255;
    int gi0 = permMod12[ii       + perm[jj      ]];
    int gi1 = permMod12[ii + i1  + perm[jj + j1 ]];
    int gi2 = permMod12[ii + 1   + perm[jj + 1  ]];

    // Contributions from each corner
    sample_t t0 = 0.5f - x0*x0 - y0*y0;
    sample_t n0 = (t0 < 0) ? 0 : (t0*t0*t0*t0 * dot2(grad3[gi0], x0, y0));

    sample_t t1 = 0.5f - x1*x1 - y1*y1;
    sample_t n1 = (t1 < 0) ? 0 : (t1*t1*t1*t1 * dot2(grad3[gi1], x1, y1));

    sample_t t2 = 0.5f - x2*x2 - y2*y2;
    sample_t n2 = (t2 < 0) ? 0 : (t2*t2*t2*t2 * dot2(grad3[gi2], x2, y2));

    // Scale to [-1, 1]
    return 70.0f * (n0 + n1 + n2);
}

// ── 1-D wrapper ───────────────────────────────────────────────────────────────
sample_t simplex1(sample_t xin) {
    return simplex2(xin, 0.0f);
}
