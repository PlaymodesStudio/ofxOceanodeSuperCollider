//
//  OceanodePhysical.cpp — PhysicalBox's voice.
//
//  PhysicalVoice.ar(ext, freq, gate, hold, strk, ...): two outputs (the two
//  pickups). Inputs after ext follow physical::PHYSICAL_PARAMS (PhysicalModes.h),
//  the same order and meaning as the Phase 0 DynGen script
//  (synthdefs/WIP/PhysicalBox/physicalbox.eel2), which this ports line by line:
//
//    Strike -+ impulsive -+                                  +- pick1 -> out0
//    Scrape -+            +-> A: waveguide (string <> tube) -+
//    Ext    -+            |        couple / mutual           +- pick2 -> out1
//    Bow    -+ feedback  -+-> B: modal body (a model from a family, +
//    Blow   -+                   each mode one or a beating pair)
//              (bow and blow read the resonators' own motion at the contact)
//
//  Jawari: A's bridge end is a curved bridge: as the string swings into it the
//  vibrating length shortens with the swing, every cycle (a sitar's, a
//  tanpura's buzz and the brightness that sweeps as the note dies). The swing
//  is measured against its own recent peak, so the buzz lasts the whole note.
//  Sympathetic: NSYM strings (a taraf, a handpan's other fields: a tuning set
//  in semitones from the note, each slightly detuned) driven by the pickups,
//  ringing on into the pickups after the note.
//
//  ext, gate and strk are read per sample (audio rate) or per block; the other
//  inputs once per block, the ones used inside the sample loop ramped across it.
//  The gate is a level: a strike fires when the gate settles after its edge
//  (an Oceanode value held at control rate ramps over a block, from the last
//  note's level), at that level. Every feedback loop runs inside the sample loop, sample-accurate.
//

#include "SC_PlugIn.h"
#include "PhysicalModes.h"
#include <cstring>

static InterfaceTable* ft;

namespace {

using namespace physical;

// ── tuning: the gains of the Phase 0 script (set by rendering its test matrix) ──
constexpr double kstrikea = 0.1;    // strike into the waveguide, times its period
constexpr double kconta = 4;        // scrape / ext into the waveguide (noise-normalised)
constexpr double kcontb = 2;        // scrape / ext into the modes (noise-normalised)
constexpr double kfba = 1.0;        // bow into the waveguide junction
constexpr double kjet = 0.05;       // jet force: small, or the jet sets the pitch
constexpr double kreed = 1.0;       // reed force (exact STK reed reflection)
constexpr double kfbb = 1.0;        // bow into the modes
constexpr double kblowb = 1.0;      // blow into the modes
constexpr double kagc = 200;        // blow into the modes backs off as the body gets loud
constexpr double kvb = 1.0;         // modal motion seen by the bow / blow
constexpr double kcab = 0.25;       // A -> B coupling
constexpr double kcba = 0.25;       // B -> A coupling
constexpr double kcas = 12;         // cascade: low modes into the upper modes
constexpr double krat = 1.0;        // rattle into the modes
constexpr double kair = 0.25;       // the mouthpiece's breath noise into the pickups
constexpr double kjaw = 1.5;        // jawari: the bridge's buzz into the pickups at 1
constexpr double ksymIn = 0.6;      // the voice into the sympathetic strings (energy-normalised)
constexpr double ksymOut = 1.2;     // the sympathetic strings into the pickups
constexpr double ksymCap = 0.3;     // their soft ceiling (sat: at most ~0.3 x 1 per side)

constexpr int NA = 16384;           // waveguide segments
constexpr int NJ = 8192;            // jet delay
constexpr int NT = 4096;            // resonator tube (per pickup)
constexpr int NSY = 4096;           // each sympathetic string

inline double clip(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double sat(double x) { const double c = clip(x, -3, 3); return c * (27 + c * c) / (27 + 9 * c * c); }
inline double dz(double x, double th) { return x - clip(x, -th, th); }
inline double pfold(double p) { return 0.02 + 0.96 * std::min(p, 1 - p); }
inline double lppd(double b, double w) { return std::atan2(b * std::sin(w), 1 - b * std::cos(w)) / w; }
inline double appd(double a, double w) {
    return (std::atan2(-a * std::sin(w), 1 + a * std::cos(w)) - std::atan2(-std::sin(w), a + std::cos(w))) / w;
}
// d samples behind the write position wp (the slot written next)
inline double rdc(const float* b, int wp, double d) {
    double p = wp - d; if (p < 0) p += NA;
    const int i = int(p); const double x = p - i;
    const int i0 = (i - 1 + NA) % NA, i2 = (i + 1) % NA, i3 = (i + 2) % NA;
    const double y0 = b[i0], y1 = b[i], y2 = b[i2], y3 = b[i3];
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + y1;
}
inline double rdl(const float* b, int size, int wp, double d) {
    double p = wp - d; if (p < 0) p += size;
    const int i = int(p); const double x = p - i;
    const int i2 = i + 1 >= size ? i + 1 - size : i + 1;
    return b[i] + x * (b[i2] - b[i]);
}
inline float clean(double v) { return std::fabs(v) < 1e-15 ? 0.f : float(v); }

}  // namespace

struct PhysicalVoice : public Unit {
    float* mem;                 // dln | dlb | dlh (NA each) | dlj (NJ) | dlt (2 x NT) | dls (NSYM x NSY)
    float *dln, *dlb, *dlh, *dlj, *dlt, *dls;
    int wn, wb, wh, wj, wt, ws;
    // jawari: the bridge-side wave (lowpassed) and its recent peak
    double jx, jpk, jb;
    // sympathetic strings: delay, loop gain, input gain, loop lowpass and its state, pans
    double sl[NSYM], sg[NSYM], sgi[NSYM], sb[NSYM], slp[NSYM], spl[NSYM], spr[NSYM];
    bool sOn;
    // damper: on after the gate falls (not after Strike), smoothed per block
    int damped;
    double dmp;
    // resonator: per pickup c, the modes' outputs and the shared input history; the tube
    double rx1[2], rx2[2], ry1[2][RADMODES], ry2[2][RADMODES], rlp[2], tph;
    double rb0[RADMODES], ra1[RADMODES], ra2[RADMODES], tlen, tg, tnorm;
    int rtype;
    // Air: the mouthpiece heard (the jet and the breath's turbulence), DC-free and lowpassed
    double airdc, airlp;
    // which resonators run (one neither heard nor felt is skipped; cleared when it comes back)
    bool aOn, bOn;
    double lpa, ap[4][2];
    double nutrefl, brefl, ya, sc;
    double envf, enve, prevg;
    double st, stlen, samp, nextb, gap, hard;
    int nb, armed, armCount;
    double armPrev, prevStrk;
    double sclp, vdc, vrx;
    // the modes: k at k, its doublet partner at MAXM + k; act lists the running ones
    double my1[2 * MAXM], my2[2 * MAXM], mfr[2 * MAXM], mdr[MAXM];
    double ma1[2 * MAXM], ma2[2 * MAXM], mgi[2 * MAXM], mgx[2 * MAXM];
    double mpe[2 * MAXM], mp1[2 * MAXM], mp2[2 * MAXM], mpc[2 * MAXM], mcs[2 * MAXM], mlo[2 * MAXM];
    int act[2 * MAXM], nact;
    // the running modes packed in a row for the sample loop (so it vectorises):
    // their state, copied in and out each block, and their coefficients
    // (excitation gains pre-multiplied by where they are touched)
    double cy1[2 * MAXM], cy2[2 * MAXM], cfr[2 * MAXM], ca1[2 * MAXM], ca2[2 * MAXM];
    double cpe[2 * MAXM], cplo[2 * MAXM], cp1[2 * MAXM], cp2[2 * MAXM], cpc[2 * MAXM];
    double cei[2 * MAXM], cex[2 * MAXM], cec[2 * MAXM], ces[2 * MAXM];
    // the body's modes, recomputed when what they depend on changes
    Mode bm[MAXM];
    double bkey[9];
    int bcount;
    // per block
    double apc, pda, ga, bla, la0, fa0, fb0, lfac, rn, rbs, kia, kca, catk, crel;
    double bnorm, scgc, exa, exb, cab, cba, lref, ga2;
    int mcount, kcut;
    // ramped across the block: the inputs used per sample
    double last[P_COUNT];
    bool first;
};

static void PhysicalVoice_next(PhysicalVoice* unit, int inNumSamples);

static void clearState(PhysicalVoice* u) {
    std::memset(u->mem, 0, sizeof(float) * (3 * NA + NJ + 2 * NT + NSYM * NSY));
    u->jx = u->jpk = u->jb = 0;
    for (int j = 0; j < NSYM; ++j) u->slp[j] = 0;
    for (int c = 0; c < 2; ++c) {
        u->rx1[c] = u->rx2[c] = u->rlp[c] = 0;
        for (int m = 0; m < RADMODES; ++m) u->ry1[c][m] = u->ry2[c][m] = 0;
    }
    for (int k = 0; k < 2 * MAXM; ++k) u->my1[k] = u->my2[k] = u->mfr[k] = 0;
    u->lpa = 0; std::memset(u->ap, 0, sizeof(u->ap));
    u->nutrefl = u->brefl = u->ya = u->sc = u->sclp = u->enve = 0;
}

static void PhysicalVoice_Ctor(PhysicalVoice* unit) {
    unit->mem = (float*)RTAlloc(unit->mWorld, sizeof(float) * (3 * NA + NJ + 2 * NT + NSYM * NSY));
    if (!unit->mem) {
        Print("PhysicalVoice: could not allocate its delay lines\n");
        SETCALC(ft->fClearUnitOutputs);
        ClearUnitOutputs(unit, 1);
        return;
    }
    unit->dln = unit->mem; unit->dlb = unit->dln + NA; unit->dlh = unit->dlb + NA; unit->dlj = unit->dlh + NA;
    unit->dlt = unit->dlj + NJ;
    unit->dls = unit->dlt + 2 * NT;
    unit->wn = unit->wb = unit->wh = unit->wj = unit->wt = unit->ws = 0;
    unit->sOn = false;
    for (int j = 0; j < NSYM; ++j) unit->sl[j] = 100, unit->sg[j] = unit->sgi[j] = unit->sb[j] = 0, unit->spl[j] = unit->spr[j] = 0.5;
    unit->damped = 0; unit->dmp = 0; unit->tph = 0; unit->rtype = 0;
    unit->airdc = unit->airlp = 0;
    unit->aOn = unit->bOn = true;
    unit->tlen = 100; unit->tg = 0; unit->tnorm = 1;
    for (int m = 0; m < RADMODES; ++m) unit->rb0[m] = unit->ra1[m] = unit->ra2[m] = 0;
    clearState(unit);
    for (int k = 0; k < 2 * MAXM; ++k) {
        unit->ma1[k] = unit->ma2[k] = unit->mgi[k] = unit->mgx[k] = 0;
        unit->mpe[k] = unit->mp1[k] = unit->mp2[k] = unit->mpc[k] = unit->mcs[k] = unit->mlo[k] = 0;
    }
    for (int k = 0; k < MAXM; ++k) unit->mdr[k] = 0;
    unit->nact = 0;
    for (double& k : unit->bkey) k = -1e30;
    unit->bcount = 0;
    unit->envf = unit->prevg = 0;
    unit->st = 1e9; unit->stlen = 1; unit->samp = unit->nextb = unit->gap = unit->hard = 0;
    unit->nb = 0; unit->armed = 0; unit->armCount = 0; unit->armPrev = 0; unit->prevStrk = 0;
    unit->vdc = unit->vrx = 0;
    unit->apc = 0; unit->pda = 1; unit->ga = 0.99; unit->bla = 0; unit->la0 = 100; unit->fa0 = 220; unit->fb0 = 220;
    unit->lfac = 1; unit->rn = -1; unit->rbs = -1; unit->mcount = 1; unit->kcut = 2;
    unit->first = true;
    SETCALC(PhysicalVoice_next);
    PhysicalVoice_next(unit, 1);
    // the first call must not count as time passed
    unit->wn = unit->wb = unit->wh = unit->wj = unit->wt = unit->ws = 0;
    clearState(unit);
    unit->damped = 0; unit->dmp = 0; unit->tph = 0;
    unit->envf = unit->prevg = 0; unit->st = 1e9; unit->armed = 0; unit->armPrev = 0; unit->prevStrk = 0;
    unit->first = true;
}

static void PhysicalVoice_Dtor(PhysicalVoice* unit) {
    if (unit->mem) RTFree(unit->mWorld, unit->mem);
}

static void PhysicalVoice_next(PhysicalVoice* u, int n) {
    PhysicalVoice* unit = u;   // the SC macros name it unit
    const double sr = SAMPLERATE;
    RGET
    auto rnd2 = [&]() { return double(frand2(s1, s2, s3)); };
    auto rnd1 = [&]() { return double(frand(s1, s2, s3)); };
    // a SynthDef built before an input existed has fewer: those read their default
    static const double defaults[P_COUNT] = {0,
#define PHYSICAL_DEFAULT(name, def) double(def),
        PHYSICAL_PARAMS(PHYSICAL_DEFAULT)
#undef PHYSICAL_DEFAULT
    };
    const int nin = int(unit->mNumInputs);
    auto in = [&](int p) { return p < nin ? double(IN0(p)) : defaults[p]; };

    // ── block ───────────────────────────────────────────────────────────────
    double cur[P_COUNT];
    for (int p = 0; p < P_COUNT; ++p) cur[p] = in(p);
    if (u->first) { for (int p = 0; p < P_COUNT; ++p) u->last[p] = cur[p]; u->first = false; }
    auto P = [&](int p) { return cur[p]; };

    const double frz = 1 + std::pow(clip(P(P_freeze), 0, 1), 3) * 400;
    u->catk = std::exp(-1 / (std::max(P(P_atk), 0.001) * sr));
    u->crel = std::exp(-1 / (std::max(P(P_rel), 0.001) * sr));
    const double mat = P(P_material);
    // damper: the gate released (not held, not a Strike) adds damping to every mode
    u->dmp += ((u->damped && P(P_hold) < 0.5 ? 1.0 : 0.0) - u->dmp) * 0.25;
    const double sdamp = u->dmp * std::pow(clip(P(P_damper), 0, 1), 2) * 138;   // 1: rings stop within ~50 ms
    auto damped = [&](double t60) { return sdamp > 0 ? 6.91 / (6.91 / t60 + sdamp) : t60; };

    // resonator A: a waveguide split at the contact into a nut-side and a
    // bridge-side segment. bore: 0 string (-1, -1), 0.5 closed tube (+1, -1,
    // loop halved), 1 open tube (+1, +1).
    const double hol = clip(P(P_abore), 0, 1);
    u->rn = hol < 0.5 ? -std::cos(M_PI * 2 * hol) : 1;
    u->rbs = hol < 0.5 ? -1 : -std::cos(M_PI * (2 * hol - 1));
    u->lfac = hol < 0.5 ? lin(2 * hol, 1, 0.5) : lin(2 * hol - 1, 0.5, 1);
    u->fa0 = clip(P(P_freq) * std::pow(2, P(P_atune) / 12), 4, sr * 0.3);
    u->la0 = sr / u->fa0;
    const double lt0 = u->la0 * u->lfac;
    const double wa0 = 2 * M_PI * u->fa0 / sr;
    const double beta0 = clip(P(P_pos), 0.02, 0.98);
    const double abr = clip(P(P_abright) * mat3(mat, 0.5, 0.85, 1.0), 0, 1);
    u->bla = std::exp(-2 * M_PI * std::min(u->fa0 * (1 + 60 * abr * abr), sr * 0.45) / sr);
    u->apc = -0.9 * clip(P(P_astiff), 0, 1);
    u->pda = lppd(u->bla, wa0) + 4 * appd(u->apc, wa0);
    while ((lt0 - std::max(beta0 * lt0, 3.0) - u->pda < 3) && (u->apc < -0.002)) {
        u->apc *= 0.7;
        u->pda = lppd(u->bla, wa0) + 4 * appd(u->apc, wa0);
    }
    {
        const double tsec = damped(std::max(P(P_adecay) * mat3(mat, 0.4, 1.3, 2.2) * frz, 0.01));
        double ga = std::pow(0.001, lt0 / (sr * tsec));
        ga = std::min(ga * std::sqrt(1 - 2 * u->bla * std::cos(wa0) + u->bla * u->bla) / (1 - u->bla), 0.99999);
        u->ga = ga;
        u->ga2 = std::sqrt(1 - ga * ga);
    }
    u->kia = kstrikea * lt0;
    u->kca = kconta * u->ga2;

    // resonator B: the model's modes (cached), drift, tension and doublets
    u->fb0 = std::max(P(P_freq) * std::pow(2, P(P_btune) / 12), 1.0);
    {
        Body bd;
        bd.family = P(P_bfamily); bd.shape = P(P_bshape); bd.stretch = P(P_bstretch);
        bd.material = mat; bd.bdecay = P(P_bdecay); bd.freeze = P(P_freeze); bd.bright = P(P_bbright);
        bd.f0 = u->fb0;
        const double key[9] = {bd.family, bd.shape, bd.stretch, bd.material, bd.bdecay, bd.freeze, bd.bright, bd.f0, P(P_bmodes)};
        if (!std::equal(key, key + 9, u->bkey)) {
            std::copy(key, key + 9, u->bkey);
            u->bcount = std::min(int(clip(std::ceil(P(P_bmodes)), 1, MAXM)), modeCount(bd));
            for (int k = 0; k < u->bcount; ++k) u->bm[k] = mode(k, bd);
        }
    }
    const int mc = u->bcount;
    const double effectiveModes = clip(P(P_bmodes), 1, std::max(mc, 1));
    const double tens = glide(P(P_tension), u->enve);
    const double split = clip(P(P_split), 0, 100), beat = clip(P(P_beat), 0, 50);
    const bool pairs = split > 0 || beat > 0;
    const double gA = pairs ? 1 / (1 + kPairGain) : 1, gB = kPairGain / (1 + kPairGain);
    const double r0 = mc > 0 ? u->bm[0].ratio : 1;
    // one resonator: frequency (radians per sample), ring time, excitation gain
    auto setRes = [&](int j, double w, double t60, double g) {
        if (w < 2.9 && w > 0.0005) {
            const double r = std::min(std::pow(0.001, 1 / (t60 * sr)), 0.999995);
            u->ma1[j] = 2 * r * std::cos(w);
            u->ma2[j] = r * r;
            const double sw = std::sin(w);
            u->mgi[j] = sw * g;
            u->mgx[j] = sw * std::sqrt(2 * (1 - r)) * g;
            // decayed below anything audible: no denormals
            if (std::fabs(u->my1[j]) < 1e-25 && std::fabs(u->my2[j]) < 1e-25) u->my1[j] = u->my2[j] = 0;
            return true;
        }
        u->ma1[j] = u->ma2[j] = u->mgi[j] = u->mgx[j] = 0;
        u->my1[j] = u->my2[j] = 0;
        return false;
    };
    // the contact and pickups on the body (x shared with the waveguide; y: a plate's other side, a drum's angle)
    Spots spots;
    spots.cx = clip(P(P_pos), 0, 1); spots.cy = P(P_posy);
    spots.p1x = clip(P(P_pick1), 0, 1); spots.p1y = P(P_pick1y);
    spots.p2x = clip(P(P_pick2), 0, 1); spots.p2y = P(P_pick2y);
    int na = 0;
    for (int k = 0; k < mc; ++k) {
        const Mode& m = u->bm[k];
        u->mdr[k] = (u->mdr[k] + rnd2() * P(P_driftrate) * 0.05) * 0.995;
        const double rt = m.ratio * (1 + P(P_drift) * 0.02 * u->mdr[k]);
        const double w = 2 * M_PI * u->fb0 * rt * tens / sr;
        // the pair sits around the mode: split cents (spread per mode) and beat Hz (at the pitch, times the ratio)
        const double c = pairs ? std::pow(2, split * pairSpread(k) / 2400) : 1;
        const double bw = pairs ? M_PI * beat * (rt / r0) / sr : 0;
        bool on = m.on && setRes(k, w / c - bw, damped(m.t60), gA);
        bool onB = on && pairs && setRes(MAXM + k, w * c + bw, damped(m.t60 * kPairDecay), gB);
        if (!onB) { u->my1[MAXM + k] = u->my2[MAXM + k] = 0; u->ma1[MAXM + k] = u->ma2[MAXM + k] = 0; }
        if (on) {
            const Weights wt = weights(m, spots);
            // Contact and pickup both contribute one square-root presence:
            // each mode enters continuously without resetting existing state.
            const double presence = std::sqrt(clip(effectiveModes - k, 0, 1));
            u->mpe[k] = wt.e * presence;
            u->mp1[k] = wt.p1 * m.amp * presence;
            u->mp2[k] = wt.p2 * m.amp * presence;
            u->mpc[k] = wt.c * presence;
            u->act[na++] = k;
            if (onB) u->act[na++] = MAXM + k;
        } else {
            u->mpe[k] = u->mp1[k] = u->mp2[k] = u->mpc[k] = 0;
        }
    }
    for (int k = mc; k < MAXM; ++k) u->my1[k] = u->my2[k] = u->my1[MAXM + k] = u->my2[MAXM + k] = 0;
    // both pickups keep the lowest mode in phase: the pair never cancels in mono
    const double sg1 = u->mp1[0] < 0 ? -1 : 1, sg2 = u->mp2[0] < 0 ? -1 : 1;
    u->kcut = std::max(2, mc / 4);
    for (int k = 0; k < mc; ++k) {
        u->mp1[k] *= sg1; u->mp2[k] *= sg2;
        // the partner is heard and touched where its mode is
        u->mpe[MAXM + k] = u->mpe[k]; u->mp1[MAXM + k] = u->mp1[k];
        u->mp2[MAXM + k] = u->mp2[k]; u->mpc[MAXM + k] = u->mpc[k];
        // The low/high cascade split depends on count too. Interpolate
        // its two integer results so crossing a multiple of four cannot step.
        const int low = std::max(2, int(std::floor(effectiveModes)) / 4);
        const int high = std::max(2, int(std::ceil(effectiveModes)) / 4);
        const double fraction = effectiveModes - std::floor(effectiveModes);
        const double lo = (k < low ? 1.0 : 0.0) * (1-fraction) + (k < high ? 1.0 : 0.0) * fraction;
        u->mlo[k] = u->mlo[MAXM + k] = lo;
        u->mcs[k] = u->mcs[MAXM + k] = 1-lo;
    }
    u->nact = na;
    for (int i2 = 0; i2 < na; ++i2) {
        const int j = u->act[i2];
        u->cy1[i2] = u->my1[j]; u->cy2[i2] = u->my2[j];
        u->ca1[i2] = u->ma1[j]; u->ca2[i2] = u->ma2[j];
        u->cpe[i2] = u->mpe[j]; u->cplo[i2] = u->mpc[j] * u->mlo[j];
        u->cp1[i2] = u->mp1[j]; u->cp2[i2] = u->mp2[j]; u->cpc[i2] = u->mpc[j];
        u->cei[i2] = u->mpe[j] * u->mgi[j]; u->cex[i2] = u->mpe[j] * u->mgx[j];
        u->cec[i2] = u->mpc[j] * u->mgx[j]; u->ces[i2] = u->mpc[j] * u->mcs[j] * u->mgx[j];
    }
    u->mcount = std::max(mc, 1);
    u->bnorm = 1.5 / std::sqrt(effectiveModes);

    u->scgc = std::exp(-2 * M_PI * (200 + 14000 * std::pow(1 - clip(P(P_grit), 0, 1), 2)) / sr);
    u->exa = 1 - clip(P(P_excite), 0, 1);
    u->exb = clip(P(P_excite), 0, 1);
    u->cab = P(P_couple) * kcab;
    u->cba = P(P_couple) * P(P_mutual) * kcba;
    u->lref = lin(u->exb, u->la0, sr / u->fb0);

    // resonator after the pickups
    u->rtype = int(clip(std::lround(P(P_radiator)), 0, NRADIATORS - 1));
    const double rmix = clip(P(P_radmix), 0, 1);
    const double rtune = std::pow(2, clip(P(P_radtune), -36, 36) / 12);
    if (u->rtype == 1) {
        const double f = clip(P(P_freq) * rtune, sr / (2.0 * (NT - 4)), sr / 8);
        // the loop's lowpass delays it: shorten the tube by as much, or it plays flat
        u->tlen = std::max(sr / (2 * f) - lppd(0.35, 2 * M_PI * f / sr), 1.0);
        u->tg = std::pow(0.001, u->tlen / (sr * kTubeT60));
        u->tnorm = 1 - u->tg;
    } else if (u->rtype >= 2) {
        const RadMode* rm = radiatorModes(u->rtype);
        for (int m = 0; m < RADMODES; ++m) {
            const double f = radiatorBase(u->rtype) * rtune * rm[m].ratio;
            if (rm[m].amp <= 0 || f >= sr * 0.45 || f < 10) { u->rb0[m] = u->ra1[m] = u->ra2[m] = 0; continue; }
            const double w = 2 * M_PI * f / sr;
            const double q = M_PI * f * rm[m].t60 / 6.91;
            const double al = std::sin(w) / (2 * q), a0 = 1 + al;
            u->rb0[m] = 0.6 * rm[m].amp * al / a0;
            u->ra1[m] = -2 * std::cos(w) / a0;
            u->ra2[m] = (1 - al) / a0;
        }
    }
    // sympathetic strings: tuned from the note, octave-folded into the delay's range
    const double symp = clip(P(P_symp), 0, 1);
    const bool sympOn = symp > 0;
    if (sympOn) {
        if (!u->sOn) { std::memset(u->dls, 0, sizeof(float) * NSYM * NSY); for (int j = 0; j < NSYM; ++j) u->slp[j] = 0; }
        const double* iv = sympIntervals(int(clip(std::lround(P(P_sympset)), 0, NSYMSETS - 1)));
        const double shim = clip(P(P_sympshimmer), 0, 50) / 100;
        const double t60 = clip(P(P_sympdecay), 0.05, 30) * frz;
        const double sbr = clip(P(P_sympbright), 0, 1);
        for (int j = 0; j < NSYM; ++j) {
            double f = P(P_freq) * std::pow(2, (iv[j] + sympDetune(j) * shim) / 12);
            while (f > sr * 0.25) f *= 0.5;
            while (f < sr / (NSY - 8)) f *= 2;
            const double w = 2 * M_PI * f / sr;
            // Brightness: 0 a soft string, 1 open to the top (a cymbal's combs)
            const double b = std::exp(-2 * M_PI * std::min(f * (1 + 400 * sbr * sbr * sbr), sr * 0.45) / sr);
            const double L = std::max(sr / f - lppd(b, w), 2.0);
            double g = std::pow(0.001, L / (sr * t60));
            g = std::min(g * std::sqrt(1 - 2 * b * std::cos(w) + b * b) / (1 - b), 0.99995);
            u->sl[j] = L; u->sg[j] = g; u->sb[j] = b;
            u->sgi[j] = ksymIn * std::sqrt(1 - g * g);
            const double side = (j % 2 ? 1 : -1) * (0.15 + 0.7 * double(j) / (NSYM - 1));
            u->spl[j] = 0.5 - 0.5 * side; u->spr[j] = 0.5 + 0.5 * side;
        }
    }
    u->sOn = sympOn;
    const double jaw = clip(P(P_jawari), 0, 1);
    const double jrel = std::exp(-1 / (0.25 * sr)), jhp = 1 - std::exp(-2 * M_PI * 900 / sr);
    const double tinc = 2 * M_PI * clip(P(P_tremrate), 0, 20) / sr;
    const double tdep = clip(P(P_tremdepth), 0, 1);
    // one pickup through the resonator
    auto radiate = [&](int c, double x) {
        double y = 0;
        if (u->rtype == 1) {
            float* t = u->dlt + c * NT;
            u->rlp[c] = 0.65 * rdl(t, NT, u->wt, u->tlen) + 0.35 * u->rlp[c];
            const double yt = x - u->tg * u->rlp[c];
            t[u->wt] = clean(yt);
            y = yt * u->tnorm;
        } else {
            const RadMode* rm = radiatorModes(u->rtype);
            for (int m = 0; m < RADMODES; ++m) {
                if (rm[m].amp <= 0) continue;
                const double ym = u->rb0[m] * (x - u->rx2[c]) - u->ra1[m] * u->ry1[c][m] - u->ra2[m] * u->ry2[c][m];
                u->ry2[c][m] = u->ry1[c][m]; u->ry1[c][m] = ym;
                y += ym;
            }
            u->rx2[c] = u->rx1[c]; u->rx1[c] = x;
        }
        return y;
    };

    // ── samples ─────────────────────────────────────────────────────────────
    float* out0 = OUT(0);
    float* out1 = OUT(1);
    const bool extAr = INRATE(P_EXT_IN) == calc_FullRate;
    const bool gateAr = INRATE(P_gate) == calc_FullRate;
    const bool strkAr = INRATE(P_strk) == calc_FullRate;
    const double invN = 1.0 / n;
    // the inputs read inside the loop, ramped from the last block's values
    static const int ramped[] = {P_freq, P_tension, P_pos, P_rattle, P_rattleth, P_hardness, P_mass, P_bounce,
        P_strike, P_scrape, P_scrapespeed, P_rough, P_grit, P_chatter, P_ext, P_bow, P_bowspeed, P_bowpress,
        P_rosin, P_blow, P_breath, P_blowtype, P_jetratio, P_turb, P_cascade, P_pick1, P_pick2, P_listen, P_hold, P_air};
    double v[P_COUNT], dv[P_COUNT];
    for (int p : ramped) { v[p] = u->last[p]; dv[p] = (cur[p] - u->last[p]) * invN; }
    v[P_hold] = cur[P_hold]; dv[P_hold] = 0;
    const double fa0 = u->fa0, lfac = u->lfac, ga = u->ga, bla = u->bla, apc = u->apc, pda = u->pda;
    const double exa = u->exa, exb = u->exb, kia = u->kia, kca = u->kca;
    const double freqBase = std::max(P(P_freq), 1e-6);
    const double airc = 1 - std::exp(-2 * M_PI * 5000 / sr);
    // A resonator that cannot be heard (Listen exactly at the other one all block)
    // and does not touch anything (no coupling, its exciters off or Excite away
    // from it) is skipped: the output is the same, the random draws stay in order
    const bool lis1 = u->last[P_listen] >= 1 && cur[P_listen] >= 1, lis0 = u->last[P_listen] <= 0 && cur[P_listen] <= 0;
    const bool coupled = u->cab != 0 || u->cba != 0;
    const bool runA = !(exa == 0 && lis1 && !coupled);
    const bool runB = !(exb == 0 && lis0 && !coupled);
    if (runA && !u->aOn) {
        std::memset(u->mem, 0, sizeof(float) * 3 * NA);
        u->lpa = 0; std::memset(u->ap, 0, sizeof(u->ap));
        u->nutrefl = u->brefl = u->ya = 0;
    }
    if (runB && !u->bOn) {
        for (int k = 0; k < 2 * MAXM; ++k) u->my1[k] = u->my2[k] = u->mfr[k] = 0;
        for (int i2 = 0; i2 < u->nact; ++i2) u->cy1[i2] = u->cy2[i2] = 0;
    }
    if (!runB) u->sc = 0;
    u->aOn = runA; u->bOn = runB;
    const bool bowing = u->last[P_bow] != 0 || cur[P_bow] != 0;

    for (int i = 0; i < n; ++i) {
        for (int p : ramped) v[p] += dv[p];
        const double g = gateAr ? IN(P_gate)[i] : cur[P_gate];
        const double strk = strkAr ? IN(P_strk)[i] : cur[P_strk];
        const double ext = extAr ? IN(P_EXT_IN)[i] : cur[P_EXT_IN];

        const double lvl = g > 0.0001 ? g : (v[P_hold] > 0.5 ? 1 : 0);
        u->envf = lvl + (lvl > u->envf ? u->catk : u->crel) * (u->envf - lvl);

        // A: waves arriving at the contact (the pitch ramped across the block)
        const double fa = fa0 * (v[P_freq] / freqBase) * glide(v[P_tension], u->enve);
        const double lt = sr / fa * lfac;
        const double beta = clip(v[P_pos], 0.02, 0.98);
        const double dn = std::max(beta * lt, 3.0);
        double va = 0;
        if (runA) {
            u->nutrefl = u->rn * rdc(u->dln, u->wn, dn);
            double s = rdc(u->dlb, u->wb, std::max(lt - dn - pda, 3.0));
            u->lpa = (1 - bla) * s + bla * u->lpa;
            s = u->lpa;
            for (auto& a : u->ap) { a[1] = apc * s + a[0] - apc * a[1]; a[0] = s; s = a[1]; }
            // jawari: the curved bridge. The part of the wave swinging past the
            // bridge's curve (a share of its recent peak) folds back off it, after
            // the loop's filters, so the new harmonics travel a whole round trip:
            // every period, so the buzz stays harmonic, and it lasts the whole note
            if (jaw > 0) {
                u->jpk = std::max(std::fabs(s), u->jpk * jrel);
                if (u->jpk > 1e-7) {
                    // the contact: one-sided, sharp (squared past the bridge's reach)
                    const double c = std::max(0.0, s / u->jpk - 0.35);
                    const double kink = u->jpk * c * c * 4;
                    // the bridge radiates the contact's highs: the buzz, heard at the
                    // pickups (it follows the string, so it is harmonic and dies with
                    // the note; it does not circulate, so it cannot sustain it). The
                    // peak it is measured against lags the decay: the contact narrows
                    // as the note dies and the buzz sweeps
                    u->jb = kink - u->jx;
                    u->jx += (kink - u->jx) * jhp;
                }
            }
            s -= v[P_rattle] * dz(s, v[P_rattleth]) * (1 + 0.5 * rnd2());
            u->brefl = u->rbs * ga * s;
            va = u->nutrefl + u->brefl;
        } else rnd2();

        // B: free response
        double se = 0, slo = 0;
        const int na = runB ? u->nact : 0;
        {
            double* __restrict cfr = u->cfr;
            const double* __restrict cy1 = u->cy1; const double* __restrict cy2 = u->cy2;
            const double* __restrict ca1 = u->ca1; const double* __restrict ca2 = u->ca2;
            const double* __restrict cpe = u->cpe; const double* __restrict cplo = u->cplo;
            for (int i2 = 0; i2 < na; ++i2) {
                const double yf = ca1[i2] * cy1[i2] - ca2[i2] * cy2[i2];
                cfr[i2] = yf;
                se += yf * cpe[i2];
                slo += yf * cplo[i2];
            }
        }
        const double vr = exa * va + exb * kvb * se;

        // strike: a half-sine contact pulse of unit area, armed on the gate's
        // rising edge and fired once the gate settles (at its level)
        if (g > 0.0001 && u->prevg <= 0.0001) { u->armed = 1; u->armCount = 0; u->damped = 0; }
        if (g <= 0.0001 && u->prevg > 0.0001) u->damped = 1;
        u->prevg = g;
        bool fire = false;
        if (u->armed) {
            // ModGate ramps a held Oceanode level over a block, from the last note's
            if (std::fabs(g - u->armPrev) > 1e-7 && u->armCount < 128) ++u->armCount;
            else { fire = g > 0.0001; u->armed = 0; }
        }
        u->armPrev = g;
        if (strk > 0 && u->prevStrk <= 0) { fire = true; u->damped = 0; }
        u->prevStrk = strk;
        if (fire) {
            u->samp = g > 0.0001 ? g : 1;
            u->hard = clip(v[P_hardness] + 0.3 * (u->samp - 0.5), 0, 1);
            double len = (0.00025 + std::pow(1 - u->hard, 2) * 0.010) * (0.4 + 1.2 * v[P_mass]) * sr;
            u->stlen = std::max(std::min(len, 0.5 * u->lref), 2.0);
            u->st = 0; u->nb = 0;
            u->gap = u->stlen * (4 + 30 * v[P_mass]);
        }
        double fstrike = 0;
        if (u->st < u->stlen) {
            fstrike = std::sin(M_PI * u->st / u->stlen) * u->samp * (M_PI / (2 * u->stlen)) * (1 + u->hard * u->hard * 0.6 * rnd2());
            u->st += 1;
            if (u->st >= u->stlen) { u->samp *= v[P_bounce]; u->nextb = u->gap; u->gap *= 0.7; u->nb += 1; }
        } else if (u->nextb > 0 && u->samp > 0.02 && u->nb < 16) {
            u->nextb -= 1;
            if (u->nextb <= 0) u->st = 0;
        }

        // scrape: micro-impacts; chatter lets the object's motion trigger more
        const double sspd = v[P_scrapespeed] * u->envf;
        const double dens = sspd * (30 + 4000 * v[P_rough]);
        const double prob = dens / sr * (1 + v[P_chatter] * std::fabs(vr) * 20);
        const double imp = rnd1() < prob ? rnd2() * (0.3 + 0.7 * rnd1()) : 0;
        u->sclp = (1 - u->scgc) * imp + u->scgc * u->sclp;
        const double fscrape = v[P_scrape] * (u->sclp * (1 + 4 * v[P_grit]) + rnd2() * 0.02 * sspd);
        const double fimp = v[P_strike] * fstrike;
        const double fcont = fscrape + v[P_ext] * ext;

        // bow: stick-slip friction (STK bow table) against the contact motion
        const double vbow = v[P_bowspeed] * u->envf * (1 + v[P_rosin] * 0.5 * rnd2());
        const double dvb = vbow - vr;
        const double slope = std::max(5 - 4.5 * v[P_bowpress], 0.3);
        const double fbow = bowing ? v[P_bow] * dvb * std::min(std::pow(std::fabs(dvb * slope) + 0.75, -4.0), 1.0) : 0.0;

        // blow: jet (flute) <-> reed (clarinet) on the same breath
        const double pb = 1.5 * v[P_breath] * u->envf * (1 + v[P_turb] * 0.4 * rnd2());
        u->vdc = vr - u->vrx + 0.995 * u->vdc;
        u->vrx = vr;
        u->dlj[u->wj] = clean(pb - 0.5 * u->vdc);
        if (++u->wj >= NJ) u->wj = 0;
        const double jd = rdl(u->dlj, NJ, u->wj, clip(lin(exb, sr / fa, sr / u->fb0) * v[P_jetratio], 2, NJ - 4));
        const double fjet = clip(jd * (jd * jd - 1), -1, 1);
        // what the mouthpiece radiates: the breath's turbulence, swelling with the
        // jet's swing across the labium (so it fuses with the tone), lowpassed
        u->airdc += (jd - u->airdc) * 0.003;
        const double swing = clip(std::fabs(jd - u->airdc), 0, 1.5);
        u->airlp += (1.5 * v[P_breath] * u->envf * rnd2() * (0.35 + swing) - u->airlp) * airc;
        const double air = v[P_blow] * v[P_air] * kair * u->airlp;
        const double rr = clip(0.7 - 0.3 * (vr - pb), -1, 1);
        const double freed = (pb - vr) * (1 - rr);
        const double fblow = v[P_blow] * lin(clip(v[P_blowtype], 0, 1), fjet * kjet, freed * kreed);

        // A: junction
        if (runA) {
            const double fja = exa * (fimp * kia + fcont * kca + fbow * kfba + fblow) + u->cba * u->sc * u->ga2;
            u->dln[u->wn] = clean(sat(u->brefl + fja));
            u->dlb[u->wb] = clean(sat(u->nutrefl + fja));
            if (++u->wn >= NA) u->wn = 0;
            if (++u->wb >= NA) u->wb = 0;
            u->ya = u->brefl;
            u->dlh[u->wh] = clean(u->ya);
            if (++u->wh >= NA) u->wh = 0;
        }

        // B: update (cascade feeds forward only: it cannot run away)
        slo *= u->bnorm;
        const double fcas = kcas * sat(v[P_cascade] * 4 * slo * std::fabs(slo));
        const double frat = -krat * v[P_rattle] * dz(u->sc, v[P_rattleth]) * (1 + 0.5 * rnd2());
        const double fib = exb * (fimp + fbow * kfbb + fblow * kblowb / (1 + kagc * se * se));
        const double fxb = exb * fcont * kcontb;
        const double fcpl = u->cab * u->ya;
        double o1 = 0, o2 = 0, scn = 0;
        {
            const double fc = fcpl + frat;
            double* __restrict cy1 = u->cy1; double* __restrict cy2 = u->cy2;
            const double* __restrict cfr = u->cfr;
            const double* __restrict cei = u->cei; const double* __restrict cex = u->cex;
            const double* __restrict cec = u->cec; const double* __restrict ces = u->ces;
            const double* __restrict cp1 = u->cp1; const double* __restrict cp2 = u->cp2;
            const double* __restrict cpc = u->cpc;
            for (int i2 = 0; i2 < na; ++i2) {
                const double y = cfr[i2] + fib * cei[i2] + fxb * cex[i2] + fc * cec[i2] + fcas * ces[i2];
                cy2[i2] = cy1[i2];
                cy1[i2] = y;
                o1 += y * cp1[i2];
                o2 += y * cp2[i2];
                scn += y * cpc[i2];
            }
        }
        if (runB) u->sc = scn * u->bnorm;

        // pickups
        const double pa1 = runA ? u->ya - rdl(u->dlh, NA, u->wh, std::max(lt * pfold(v[P_pick1]), 1.0)) : 0.0;
        const double pa2 = runA ? u->ya - rdl(u->dlh, NA, u->wh, std::max(lt * pfold(v[P_pick2]), 1.0)) : 0.0;
        const double lb = clip(v[P_listen], 0, 1);
        const double buzz = runA && jaw > 0 ? jaw * kjaw * (1 - lb) * u->jb : 0.0;
        double x1 = lin(lb, pa1, o1 * u->bnorm) + air + buzz;
        double x2 = lin(lb, pa2, o2 * u->bnorm) + air + buzz;
        u->enve += (std::fabs(x1) - u->enve) * 0.001;
        if (sympOn) {
            const double xin = 0.5 * (x1 + x2);
            double yl = 0, yr = 0;
            for (int j = 0; j < NSYM; ++j) {
                float* b = u->dls + j * NSY;
                u->slp[j] = (1 - u->sb[j]) * rdl(b, NSY, u->ws, u->sl[j]) + u->sb[j] * u->slp[j];
                const double y = xin * u->sgi[j] + u->sg[j] * u->slp[j];
                b[u->ws] = clean(y);
                yl += y * u->spl[j]; yr += y * u->spr[j];
            }
            if (++u->ws >= NSY) u->ws = 0;
            // a soft ceiling: a sustained note (bowed, blown) keeps driving the strings at
            // resonance; they saturate gently instead of outgrowing the string that feeds them
            yl = ksymCap * sat(yl / ksymCap); yr = ksymCap * sat(yr / ksymCap);
            x1 += symp * ksymOut * yl; x2 += symp * ksymOut * yr;
        }

        // safety: anything non-finite or runaway clears the voice
        if (!std::isfinite(x1) || !std::isfinite(x2) || std::fabs(x1) > 1000 || std::fabs(x2) > 1000) {
            clearState(u);
            for (int i2 = 0; i2 < u->nact; ++i2) u->cy1[i2] = u->cy2[i2] = 0;
            x1 = x2 = 0;
        }
        if (u->rtype > 0) {
            // the fan in a vibraphone's tubes: only what the resonator radiates trembles
            const double am = 1 - tdep * (0.5 - 0.5 * std::cos(u->tph));
            u->tph += tinc; if (u->tph > 2 * M_PI) u->tph -= 2 * M_PI;
            const double r1 = radiate(0, x1), r2 = radiate(1, x2);
            if (u->rtype == 1 && ++u->wt >= NT) u->wt = 0;
            x1 = x1 * (1 - 0.3 * rmix) + rmix * am * r1 * 1.5;
            x2 = x2 * (1 - 0.3 * rmix) + rmix * am * r2 * 1.5;
            if (!std::isfinite(x1) || !std::isfinite(x2)) { clearState(u); x1 = x2 = 0; }
        }
        out0[i] = float(sat(x1));
        out1[i] = float(sat(x2));
    }
    for (int i2 = 0; i2 < u->nact; ++i2) { u->my1[u->act[i2]] = u->cy1[i2]; u->my2[u->act[i2]] = u->cy2[i2]; }
    for (int p : ramped) u->last[p] = cur[p];
    RPUT
}

PluginLoad(OceanodePhysical) {
    ft = inTable;
    DefineDtorUnit(PhysicalVoice);
}
