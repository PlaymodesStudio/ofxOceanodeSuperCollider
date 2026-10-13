//
//  PV_MaskFilter.cpp — a spectral filter drawn by an array of gains.
//
//  PV_MaskFilter(chain, transpose, lowFreq, highFreq, wrap, attack, release,
//                hold, mask...)
//
//  Every FFT frame, each bin is scaled by the mask read at the bin's place on a
//  log-frequency axis: the K mask values (the UGen's last K inputs) lie evenly
//  from lowFreq to highFreq, the whole mask moved up by `transpose` semitones.
//  wrap > 0: the mask repeats outside lowFreq..highFreq (it scrolls around),
//  else its first / last value holds below / above.
//  attack / release (seconds): each bin's level moves towards its target with
//  these time constants (0: at once); hold > 0 keeps every bin's level.
//
//  Works on complex or polar chains (no conversion: a gain on the magnitude).
//  Used by the SpectralFilterBox SynthDefs (synthdefs/Defaults/SpectralFilterBox).
//

#include "FFT_UGens.h"
#include <cmath>

InterfaceTable* ft;   // FFT_UGens.h declares it extern

enum { kChain, kTranspose, kLow, kHigh, kWrap, kAttack, kRelease, kHold, kMask };

struct PV_MaskFilter : PV_Unit {
    float* level;        // per bin: the smoothed magnitude (attack / release / hold)
    int levelBins;
    int blocks;          // control blocks since the last frame (frame period)
};

static void PV_MaskFilter_next(PV_MaskFilter* unit, int inNumSamples);

static void PV_MaskFilter_Ctor(PV_MaskFilter* unit) {
    unit->level = nullptr;
    unit->levelBins = 0;
    unit->blocks = 0;
    SETCALC(PV_MaskFilter_next);
    ZOUT0(0) = ZIN0(0);
}

static void PV_MaskFilter_Dtor(PV_MaskFilter* unit) {
    if (unit->level)
        RTFree(unit->mWorld, unit->level);
}

// The mask at x (0..1 = lowFreq..highFreq)
static inline float maskAt(PV_MaskFilter* unit, int K, float x, bool wrap) {
    if (K <= 0)
        return 1.f;
    if (K == 1)
        return IN0(kMask);
    float p;
    int i0, i1;
    if (wrap) {
        // K points around one closed cycle
        x -= std::floor(x);
        p = x * K;
        i0 = (int)p;
        if (i0 >= K)
            i0 = K - 1;
        i1 = i0 + 1 >= K ? 0 : i0 + 1;
    } else {
        x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
        p = x * (K - 1);
        i0 = (int)p;
        if (i0 > K - 2)
            i0 = K - 2;
        i1 = i0 + 1;
    }
    const float f = p - i0;
    const float a = IN0(kMask + i0), b = IN0(kMask + i1);
    return a + (b - a) * f;
}

static void PV_MaskFilter_next(PV_MaskFilter* unit, int inNumSamples) {
    unit->blocks++;
    PV_GET_BUF

    const int K = (int)unit->mNumInputs - kMask;
    const float sr = (float)unit->mWorld->mFullRate.mSampleRate;
    const float binHz = sr / (float)buf->samples;
    const float lo = sc_max(1.f, ZIN0(kLow));
    const float hi = sc_max(lo * 1.001f, ZIN0(kHigh));
    const float logLo = std::log2(lo);
    const float span = std::log2(hi) - logLo;
    const float shift = ZIN0(kTranspose) / 12.f;
    const bool wrap = ZIN0(kWrap) > 0.5f;
    const bool hold = ZIN0(kHold) > 0.5f;

    // Per-bin smoothing coefficients from the measured frame period
    const float frameDur = unit->blocks * (float)BUFLENGTH / (float)unit->mWorld->mFullRate.mSampleRate;
    unit->blocks = 0;
    const float attack = ZIN0(kAttack), release = ZIN0(kRelease);
    const float ca = attack > 0.f ? std::exp(-frameDur / attack) : 0.f;
    const float cr = release > 0.f ? std::exp(-frameDur / release) : 0.f;
    const bool smooth = hold || ca > 0.f || cr > 0.f;

    if (smooth && unit->levelBins != numbins) {
        if (unit->level)
            RTFree(unit->mWorld, unit->level);
        unit->level = (float*)RTAlloc(unit->mWorld, numbins * sizeof(float));
        unit->levelBins = unit->level ? numbins : 0;
        if (unit->level)
            for (int i = 0; i < numbins; ++i)
                unit->level[i] = 0.f;
    }
    const bool useLevel = smooth && unit->level;

    auto gainAt = [&](float hz) {
        const float x = (std::log2(sc_max(hz, 0.001f)) - logLo - shift) / span;
        const float g = maskAt(unit, K, x, wrap);
        return g < 0.f ? 0.f : g;
    };

    float* d = buf->data;
    // DC (as the first bin) and Nyquist: gain only
    d[0] *= gainAt(binHz);
    d[1] *= gainAt(sr * 0.5f);
    const bool polar = buf->coord == coord_Polar;
    for (int i = 0; i < numbins; ++i) {
        float* bin = d + 2 + 2 * i;
        const float g = gainAt((i + 1) * binHz);
        if (!useLevel) {
            if (polar)
                bin[0] *= g;
            else {
                bin[0] *= g;
                bin[1] *= g;
            }
            continue;
        }
        const float mag = polar ? std::fabs(bin[0]) : std::sqrt(bin[0] * bin[0] + bin[1] * bin[1]);
        const float target = mag * g;
        float lv = unit->level[i];
        if (!hold)
            lv = target + (lv - target) * (target > lv ? ca : cr);
        unit->level[i] = lv;
        const float s = mag > 1e-12f ? lv / mag : 0.f;
        if (polar)
            bin[0] *= s;
        else {
            bin[0] *= s;
            bin[1] *= s;
        }
    }
}

PluginLoad(OceanodeSpectral) {
    ft = inTable;
    DefineDtorUnit(PV_MaskFilter);
}
