#pragma once
//
//  PhysicalModes.h — PhysicalBox's body models and parameter list, shared by the
//  PhysicalVoice UGen (OceanodePhysical.cpp) and the node's display
//  (src/nodes/scPhysicalBox.cpp), so what the window draws is what plays.
//
//  The body B is a model from a family (bfamily); Shape (bshape, 0..1) morphs
//  through the models of that family. A model holds its modes' frequency
//  ratios, strike amplitudes, relative decays and spatial shapes. Computed
//  models (harmonic, free bar, wine glass, plate, membrane) follow their
//  physics; measured ones come from the old Oceanode instruments (prayer bell,
//  Bells13, gamelan, gong ageng, handpan, steel drum, temple / wood block) and
//  the literature (tuned bars, church bell, tabla after Raman, timpani after
//  Rossing). No mode is invented: a model has as many modes as it has data.
//
//  Decay: a damping law per material, rate ~ 1 + (f / fc)^p (wood loses its
//  highs fast, glass stays clean, metal rings long in the lows and mids). A
//  measured model's decays were heard on its own material ("native"): the law
//  only corrects for the difference when another material is chosen.
//
#include <algorithm>
#include <cmath>

namespace physical {

constexpr int MAXM = 48;
constexpr int NMODELS = 18;
constexpr int NFAMILIES = 6;
constexpr double kTilt0 = 0.64;   // the brightness tilt at the default B Brightness (0.6)

// The UGen's inputs after in0 (external audio), in this order, with defaults.
#define PHYSICAL_PARAMS(X) \
    X(freq, 220) X(gate, 0) X(hold, 0) X(strk, 0) X(atk, 0.08) X(rel, 0.6) \
    X(strike, 1) X(hardness, 0.5) X(mass, 0.3) X(bounce, 0) \
    X(bow, 0) X(bowpress, 0.5) X(bowspeed, 0.5) X(rosin, 0.2) \
    X(blow, 0) X(breath, 0.75) X(blowtype, 0) X(jetratio, 0.48) X(turb, 0.2) \
    X(scrape, 0) X(scrapespeed, 0.5) X(rough, 0.5) X(grit, 0.3) X(chatter, 0) \
    X(ext, 0) X(pos, 0.3) X(pick1, 0.2) X(pick2, 0.58) X(posy, 0.37) X(pick1y, 0.23) X(pick2y, 0.71) \
    X(atune, 0) X(adecay, 3) X(abright, 0.7) X(abore, 0) X(astiff, 0) \
    X(btune, 0) X(bdecay, 3) X(bfamily, 1) X(bshape, 0) X(bstretch, 0) X(bmodes, 32) X(bbright, 0.6) \
    X(split, 0) X(beat, 0) \
    X(material, 0.5) X(excite, 0.5) X(listen, 0.5) X(couple, 0) X(mutual, 0) \
    X(rattle, 0) X(rattleth, 0.3) X(tension, 0) X(cascade, 0) \
    X(drift, 0) X(driftrate, 0.3) X(freeze, 0) \
    X(damper, 0) X(radiator, 0) X(radmix, 0.5) X(radtune, 0) X(tremrate, 0) X(tremdepth, 0.5)

enum Param {
    P_EXT_IN = 0,
#define PHYSICAL_ENUM(name, def) P_##name,
    PHYSICAL_PARAMS(PHYSICAL_ENUM)
#undef PHYSICAL_ENUM
    P_COUNT
};

// ── the models ───────────────────────────────────────────────────────────────
enum Model {
    M_HARMONIC,
    M_FREEBAR, M_XYLOPHONE, M_GAMELAN, M_MARIMBA, M_KALIMBA,
    M_WINEGLASS, M_BOWL, M_HANDBELL, M_CHURCHBELL, M_GONG,
    M_HANDPAN, M_STEELPAN,
    M_TABLA, M_TIMPANI, M_MEMBRANE,
    M_PLATE, M_WOODBLOCK
};
inline const char* familyName(int f) {
    static const char* n[NFAMILIES] = {"Harmonic", "Bars", "Shells & Bells", "Tonefields", "Membranes", "Plates & Blocks"};
    return n[std::clamp(f, 0, NFAMILIES - 1)];
}
inline const char* modelName(int m) {
    static const char* n[NMODELS] = {"Harmonic", "Free bar", "Xylophone", "Gamelan", "Marimba", "Kalimba",
        "Wine glass", "Singing bowl", "Hand bell", "Church bell", "Gong", "Handpan", "Steel pan",
        "Tabla", "Timpani", "Membrane", "Plate", "Wood block"};
    return n[std::clamp(m, 0, NMODELS - 1)];
}
inline int familyStart(int f) { static const int s[NFAMILIES + 1] = {0, 1, 6, 11, 13, 16, 18}; return s[std::clamp(f, 0, NFAMILIES)]; }
inline int familySize(int f) { return familyStart(f + 1) - familyStart(f); }

// J_m(x): Bessel's integral over a full period, where the trapezoid rule
// converges exponentially (no special-function library on macOS)
inline double besselJ(int m, double x) {
    constexpr int N = 128;
    double s = 0;
    for (int i = 0; i < N; ++i) {
        const double t = 2 * M_PI * i / N;
        s += std::cos(m * t - x * std::sin(t));
    }
    return s / N;
}

// How a mode is shaped across the object: 0 along a line (sin(pi q x + ph)),
// 1 a circular membrane (J_m(j_mn r) cos(m angle)), 2 a rectangular plate
// (sin(m pi x) sin(n pi y))
enum Kind { K_LINE, K_MEMBRANE, K_PLATE };
constexpr int NZ = 96;      // circular modes (m, n) known: j_mn < 26, m < 16
constexpr int NR = 65;      // their radial shapes, sampled from centre to rim

struct Tables {
    double ratio[NMODELS][MAXM];   // mode frequency / the played pitch
    double amp[NMODELS][MAXM];     // strike amplitude (at the default brightness)
    double dec[NMODELS][MAXM];     // relative decay, on the native material
    double q[NMODELS][MAXM];       // spatial index (shape along the object)
    double ph[NMODELS][MAXM];      // spatial phase
    int count[NMODELS];
    double native[NMODELS];        // material the decays were heard on; < 0: none (the law alone)
    int kind[NMODELS];             // Kind
    int mi[NMODELS][MAXM], ni[NMODELS][MAXM];   // membrane: mi = zero index; plate: (m, n)
    // circular modes: j_mn, sorted, with m and the radial shape normalised to a peak of 1
    double zj[NZ], zr[NZ][NR];
    int zm[NZ], zn[NZ], nz = 0;
    int zero(int m, int n) const { for (int i = 0; i < nz; ++i) if (zm[i] == m && zn[i] == n) return i; return 0; }

    void computedAmps(int m) { for (int k = 0; k < count[m]; ++k) { amp[m][k] = std::pow(ratio[m][k] / ratio[m][0], -kTilt0); dec[m][k] = 1; } }
    template <int N>
    void measured(int m, const double (&r)[N], const double (&a)[N], const double (&d)[N], double nat) {
        count[m] = N; native[m] = nat;
        double dmax = 0;
        for (int k = 0; k < N; ++k) dmax = std::max(dmax, d[k]);
        // B Decay is how long the longest-ringing mode lasts
        for (int k = 0; k < N; ++k) { ratio[m][k] = r[k]; amp[m][k] = a[k]; dec[m][k] = d[k] / dmax; }
    }
    void spatial(int m, double q0, double qs, double phs) {
        for (int k = 0; k < MAXM; ++k) { q[m][k] = q0 + qs * k; ph[m][k] = phs * k; }
    }
    // a measured model's spatial shapes from its ratios: the two partials of a
    // pair (near-equal ratios) move together, as the rotated halves of one mode do
    // (opposite signs would cancel them at the contact: a bow could not find them)
    void spatialByRatio(int m, double phs) {
        for (int k = 0; k < count[m]; ++k) {
            const double x = 1 + 2.5 * std::log2(ratio[m][k] / ratio[m][0]);
            q[m][k] = x; ph[m][k] = phs * (x - 1);
        }
    }

    Tables() {
        for (int m = 0; m < NMODELS; ++m) {
            count[m] = MAXM; native[m] = -1; kind[m] = K_LINE;
            for (int k = 0; k < MAXM; ++k) { ratio[m][k] = amp[m][k] = dec[m][k] = q[m][k] = ph[m][k] = 0; mi[m][k] = ni[m][k] = 0; }
        }
        // the zeros of J_0 .. J_15 below 26 (scan and bisect), sorted
        for (int m = 0; m < 16; ++m) {
            int n = 0;
            // the first zero lies above m; below it J_m is tiny and the sum only noise
            double xa = m + 0.5, fa = besselJ(m, xa);
            for (double xb = xa + 0.05; xb < 26 && nz < NZ; xb += 0.05) {
                const double fb = besselJ(m, xb);
                if ((fa > 0) != (fb > 0)) {
                    double lo = xa, hi = xb, flo = fa;
                    for (int it = 0; it < 40; ++it) {
                        const double mid = 0.5 * (lo + hi), fm = besselJ(m, mid);
                        if ((fm > 0) == (flo > 0)) { lo = mid; flo = fm; } else hi = mid;
                    }
                    zj[nz] = 0.5 * (lo + hi); zm[nz] = m; zn[nz] = ++n; ++nz;
                }
                xa = xb; fa = fb;
            }
        }
        for (int i = 1; i < nz; ++i)
            for (int j = i; j > 0 && zj[j] < zj[j - 1]; --j) { std::swap(zj[j], zj[j - 1]); std::swap(zm[j], zm[j - 1]); std::swap(zn[j], zn[j - 1]); }
        for (int i = 0; i < nz; ++i) {
            double peak = 0;
            for (int r = 0; r < NR; ++r) { zr[i][r] = besselJ(zm[i], zj[i] * r / (NR - 1.0)); peak = std::max(peak, std::fabs(zr[i][r])); }
            for (int r = 0; r < NR; ++r) zr[i][r] /= peak;
        }
        // Harmonic: string / tube
        for (int k = 0; k < MAXM; ++k) ratio[M_HARMONIC][k] = k + 1;
        computedAmps(M_HARMONIC); spatial(M_HARMONIC, 1, 1, 0);

        // Bars: the free-free beam, then bars tuned by undercutting
        double free[MAXM];
        for (int k = 0; k < MAXM; ++k) {
            const double beta = k == 0 ? 4.7300 : (k == 1 ? 7.8532 : (k + 1.5) * M_PI);
            free[k] = (beta / 4.7300) * (beta / 4.7300);
        }
        for (int k = 0; k < MAXM; ++k) {
            ratio[M_FREEBAR][k] = free[k];
            ratio[M_XYLOPHONE][k] = k == 0 ? 1 : free[k] * 3.0 / free[1];             // 1 : 3 (quint tuning)
            ratio[M_MARIMBA][k] = k == 0 ? 1 : (k == 1 ? 3.99 : (k == 2 ? 9.95 : free[k] * 9.95 / free[2]));   // 1 : 4 : 10
        }
        for (int m : {M_FREEBAR, M_XYLOPHONE, M_MARIMBA}) { computedAmps(m); spatial(m, 1.5, 1, 0); }
        {   // gamelan bar (the old Gamelan synth's ratios, decays and spectrum), bronze
            const double r[] = {1, 3.37, 6.75, 10.53, 14.96, 19.97, 25.61, 31.84, 38.69, 46.15, 54.23, 62.94};
            double a[12], d[12];
            for (int k = 0; k < 12; ++k) { a[k] = std::exp(-0.4 * k); d[k] = std::exp(-0.5 * k); }
            measured(M_GAMELAN, r, a, d, 1);
            spatial(M_GAMELAN, 1.5, 1, 0);
        }
        {   // kalimba tine: a clamped-free beam, the fundamental far above its overtones
            const double r[] = {1, 6.267, 17.55, 34.39, 56.84, 84.91};
            const double a[] = {1, 0.3, 0.1, 0.04, 0.02, 0.01};
            const double d[] = {1, 1, 1, 1, 1, 1};
            measured(M_KALIMBA, r, a, d, -1);
            spatial(M_KALIMBA, 0.5, 1, 0);
        }

        // Shells & bells
        for (int k = 0; k < MAXM; ++k) {   // wine glass: the bending modes of a cup, n = 2, 3, ...
            const double n = k + 2;
            ratio[M_WINEGLASS][k] = (n * (n * n - 1) / std::sqrt(n * n + 1)) / (6 / std::sqrt(5.0));
            q[M_WINEGLASS][k] = n; ph[M_WINEGLASS][k] = 0.25 * n;
        }
        computedAmps(M_WINEGLASS);
        {   // singing bowl: the old PrayerBell's measured partials (Hz / 2434) with their pairs
            const double r[] = {1, 1.0008, 2.234, 2.2525, 3.4676, 3.561, 3.577, 3.6204, 3.9394, 4.3425, 4.3679, 6.0366};
            const double a[] = {1, 0.42, 0.84, 0.84, 0.24, 0.24, 0.24, 0.13, 0.75, 0.24, 0.24, 0.13};
            const double d[] = {20, 20, 5, 5, 0.6, 0.5, 0.3, 0.25, 0.4, 0.5, 0.4, 0.6};
            measured(M_BOWL, r, a, d, 1);
        }
        {   // hand bell: the old Bells13 bank
            const double r[] = {1, 2, 2.803, 3.871, 5.074, 7.81, 10.948, 14.421};
            const double a[] = {1, 0.044, 0.891, 0.0891, 0.794, 0.1, 0.281, 0.079};
            const double d[] = {1, 0.205, 1, 0.196, 0.339, 0.047, 0.058, 0.047};
            measured(M_HANDBELL, r, a, d, 1);
        }
        {   // church bell (minor third): hum, prime, tierce, quint, nominal, ... relative to the prime
            const double r[] = {0.5, 1.0, 1.183, 1.506, 2.0, 2.514, 2.662, 3.011, 4.166,
                                4.944, 5.433, 6.796, 7.07, 8.215, 9.0, 9.81, 10.53, 11.9};
            const double a[] = {0.7, 0.6, 0.8, 0.35, 1.0, 0.5, 0.6, 0.55, 0.5,
                                0.4, 0.35, 0.3, 0.28, 0.25, 0.22, 0.2, 0.18, 0.15};
            const double d[] = {1, 0.6, 0.55, 0.3, 0.4, 0.25, 0.25, 0.2, 0.15,
                                0.12, 0.1, 0.09, 0.08, 0.07, 0.06, 0.05, 0.05, 0.04};
            measured(M_CHURCHBELL, r, a, d, 1);
        }
        {   // gong ageng: the old GongAgeng's ratios, decays and spectrum
            const double r[] = {1, 2.1, 3.79, 5.13, 6.75, 8.21, 10.05, 11.98, 13.65, 15.71, 17.89, 20.15, 22.67, 25.34, 28.19};
            double a[15], d[15];
            for (int k = 0; k < 15; ++k) { a[k] = std::exp(-0.21 * k); d[k] = std::exp(-0.3 * k); }
            measured(M_GONG, r, a, d, 1);
        }
        for (int m : {M_BOWL, M_HANDBELL, M_CHURCHBELL, M_GONG}) spatialByRatio(m, 0.37);

        // Tonefields: hammered steel note areas
        {   // handpan: the old Handpan's main and high modes
            const double r[] = {1, 2.0, 2.83, 3.91, 4.95, 6.12, 8.23, 9.87, 11.54, 13.21};
            const double a[] = {1, 0.8, 0.6, 0.5, 0.4, 0.3, 0.2, 0.15, 0.12, 0.1};
            const double d[] = {1, 0.74, 0.55, 0.41, 0.3, 0.22, 0.15, 0.12, 0.1, 0.08};
            measured(M_HANDPAN, r, a, d, 1);
        }
        {   // steel pan: the old SteelDrum's fundamental, octave and resonator bank in one
            const double r[] = {1, 2, 2.015, 2.98, 4.75, 6.21, 9, 9.15, 11.87};
            const double a[] = {1, 0.6, 0.45, 0.4, 0.2, 0.12, 0.14, 0.1, 0.03};
            const double d[] = {1, 0.86, 0.5, 0.72, 0.37, 0.55, 0.32, 0.21, 0.16};
            measured(M_STEELPAN, r, a, d, 1);
        }
        for (int m : {M_HANDPAN, M_STEELPAN}) spatialByRatio(m, 0.37);

        // Membranes
        {   // tabla: the loaded head's near-harmonic groups (Raman): (0,1); (1,1); (2,1)+(0,2); ...
            const double r[] = {1, 2, 2.99, 3.02, 3.98, 4.03, 4.97, 5.0, 5.04};
            const double a[] = {1, 0.8, 0.5, 0.45, 0.35, 0.3, 0.22, 0.2, 0.18};
            const double d[] = {0.6, 1, 0.8, 0.75, 0.6, 0.55, 0.45, 0.42, 0.4};
            measured(M_TABLA, r, a, d, -1);
            const int id[9][2] = {{0, 1}, {1, 1}, {2, 1}, {0, 2}, {3, 1}, {1, 2}, {4, 1}, {2, 2}, {0, 3}};
            for (int k = 0; k < 9; ++k) mi[M_TABLA][k] = zero(id[k][0], id[k][1]);
        }
        {   // timpani: the air-loaded head, (1,1) the pitch, (n,1) near 1 : 1.5 : 2 (Rossing)
            const double r[] = {0.82, 1, 1.5, 1.72, 1.98, 2.27, 2.44, 2.85, 2.89, 3.33};
            const double a[] = {0.5, 1, 0.8, 0.3, 0.6, 0.3, 0.45, 0.2, 0.3, 0.2};
            const double d[] = {0.12, 1, 0.85, 0.2, 0.7, 0.35, 0.6, 0.3, 0.5, 0.4};
            measured(M_TIMPANI, r, a, d, -1);
            const int id[10][2] = {{0, 1}, {1, 1}, {2, 1}, {0, 2}, {3, 1}, {1, 2}, {4, 1}, {2, 2}, {5, 1}, {6, 1}};
            for (int k = 0; k < 10; ++k) mi[M_TIMPANI][k] = zero(id[k][0], id[k][1]);
        }
        {   // ideal membrane: every circular mode, by frequency
            count[M_MEMBRANE] = std::min(MAXM, nz); native[M_MEMBRANE] = -1;
            for (int k = 0; k < count[M_MEMBRANE]; ++k) { ratio[M_MEMBRANE][k] = zj[k] / zj[0]; mi[M_MEMBRANE][k] = k; }
            computedAmps(M_MEMBRANE);
        }
        for (int m : {M_TABLA, M_TIMPANI, M_MEMBRANE}) kind[m] = K_MEMBRANE;

        // Plates & blocks
        {   // plate: lowest MAXM of m^2 + (n * 0.618)^2, m, n = 1..12 (first minimum wins)
            double cand[144], cm[144], cn[144];
            int c = 0;
            for (int m = 1; m <= 12; ++m)
                for (int n = 1; n <= 12; ++n) { cand[c] = m * m + n * n * 0.381966; cm[c] = m; cn[c] = n; ++c; }
            for (int k = 0; k < MAXM; ++k) {
                int best = 0;
                for (int j = 1; j < c; ++j) if (cand[j] < cand[best]) best = j;
                ratio[M_PLATE][k] = cand[best]; mi[M_PLATE][k] = int(cm[best]); ni[M_PLATE][k] = int(cn[best]);
                cand[best] = 1e6;
            }
            const double p0 = ratio[M_PLATE][0];
            for (int k = 0; k < MAXM; ++k) ratio[M_PLATE][k] /= p0;
            computedAmps(M_PLATE);
            kind[M_PLATE] = K_PLATE;
        }
        {   // wood / temple block: the old WoodBlock ratios, TempleBlock spectrum
            const double r[] = {1, 1.47, 2.09, 2.56, 3.39, 4.08, 4.87, 5.62, 6.15, 7.23, 8.17, 9.32};
            const double a[] = {1, 0.7, 0.45, 0.35, 0.25, 0.15, 0.1, 0.05, 0.04, 0.03, 0.02, 0.02};
            const double d[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
            measured(M_WOODBLOCK, r, a, d, -1);
            spatialByRatio(M_WOODBLOCK, 0.29);
        }
    }
};

inline const Tables& tables() { static const Tables t; return t; }

inline double lin(double x, double a, double b) { return a + x * (b - a); }
// wood (0) -> glass (0.5) -> metal (1)
inline double mat3(double m, double a, double b, double c) {
    return m < 0.5 ? lin(m * 2, a, b) : lin(m * 2 - 1, b, c);
}
// how long a mode at f Hz rings on a material, relative: 1 / (1 + (f / fc)^p)
inline double damping(double material, double f) {
    const double fc = std::exp(mat3(material, std::log(150.0), std::log(3000.0), std::log(2500.0)));
    const double p = mat3(material, 1.1, 2.0, 1.8);
    return 1 / (1 + std::pow(std::max(f, 1.0) / fc, p));
}

// The family and the two models Shape is between
struct Place { int family, a, b; double f; };
inline Place place(double family, double shape) {
    Place p;
    p.family = std::clamp(int(std::lround(family)), 0, NFAMILIES - 1);
    const int n = familySize(p.family);
    const double s = std::clamp(shape, 0.0, 1.0) * (n - 1);
    const int i0 = std::min(int(std::floor(s)), n - 1);
    p.a = familyStart(p.family) + i0;
    p.b = familyStart(p.family) + std::min(i0 + 1, n - 1);
    p.f = p.a == p.b ? 0 : s - i0;
    return p;
}

// What a body's modes depend on
struct Body {
    double family = 1, shape = 0, stretch = 0, material = 0.5, bdecay = 3, freeze = 0, bright = 0.6;
    double f0 = 220;   // the body's pitch (freq with B Tune)
};

// One mode of the body B as the voice computes it (no drift / tension / doublet).
// Where it is touched and heard: weights(), from the models it blends.
struct Mode {
    double ratio = 1, t60 = 0.002, amp = 0; bool on = false;
    int k = 0, ma = 0, mb = 0; bool aOn = false, bOn = false; double f = 0;
};

inline Mode modelMode(int m, int k, const Body& b) {
    const auto& T = tables();
    Mode o;
    o.k = k; o.ma = o.mb = m;
    if (k >= T.count[m]) return o;
    o.on = o.aOn = true;
    const double st = 1 + 0.25 * b.stretch;
    const double r0 = std::pow(T.ratio[m][0], st);
    o.ratio = std::pow(T.ratio[m][k], st);
    const double bexp = lin(std::clamp(b.bright, 0.0, 1.0), 1.6, 0);
    o.amp = T.amp[m][k] * std::pow(o.ratio / r0, -(bexp - kTilt0));
    const double mat = std::clamp(b.material, 0.0, 1.0);
    const double f0 = b.f0 * r0, fk = b.f0 * o.ratio;
    double rel = damping(mat, fk) / damping(mat, f0);
    if (T.native[m] >= 0) rel /= damping(T.native[m], fk) / damping(T.native[m], f0);
    const double reg = std::sqrt(damping(mat, f0) / damping(mat, 220));   // higher notes ring shorter
    const double frz = 1 + std::pow(std::clamp(b.freeze, 0.0, 1.0), 3) * 400;
    o.t60 = std::max(b.bdecay * mat3(mat, 0.3, 1.6, 3.0) * T.dec[m][k] * rel * reg * frz, 0.002);
    return o;
}

// Mode k of the body: Shape's two models blended (a mode only one of them has fades with it)
inline Mode mode(int k, const Body& b) {
    const Place p = place(b.family, b.shape);
    Mode A = modelMode(p.a, k, b);
    if (p.f <= 1e-6) return A;
    Mode B = modelMode(p.b, k, b);
    if (!A.on && !B.on) return A;
    if (!A.on) { B.amp *= p.f; return B; }
    if (!B.on) { A.amp *= 1 - p.f; return A; }
    Mode o;
    o.on = true; o.k = k; o.ma = p.a; o.mb = p.b; o.aOn = o.bOn = true; o.f = p.f;
    o.ratio = std::exp(lin(p.f, std::log(A.ratio), std::log(B.ratio)));
    o.t60 = std::exp(lin(p.f, std::log(A.t60), std::log(B.t60)));
    o.amp = lin(p.f, A.amp, B.amp);
    return o;
}
// Where on the object: x along it (a membrane: the radius, centre 0 .. rim 1),
// y across it (a membrane: the angle, in turns; a plate: its other side)
struct Spots { double cx = 0.3, cy = 0.37, p1x = 0.2, p1y = 0.23, p2x = 0.58, p2y = 0.71; };
struct Weights { double e = 0, p1 = 0, p2 = 0, c = 0; };   // contact, pickups, the coupling point

// mode k of model m at (x, y); a membrane's angle counts from the contact's (ref)
inline double shapeAt(int m, int k, double x, double y, double ref) {
    const auto& T = tables();
    switch (T.kind[m]) {
        case K_MEMBRANE: {
            const int z = T.mi[m][k];
            const double r = std::clamp(x, 0.0, 1.0) * (NR - 1);
            const int i = std::min(int(r), NR - 2);
            const double radial = T.zr[z][i] + (r - i) * (T.zr[z][i + 1] - T.zr[z][i]);
            return radial * std::cos(2 * M_PI * T.zm[z] * (y - ref));
        }
        case K_PLATE:
            return std::sin(M_PI * T.mi[m][k] * x) * std::sin(M_PI * T.ni[m][k] * y);
        default:
            return std::sin(M_PI * T.q[m][k] * x + T.ph[m][k]);
    }
}
inline Weights modelWeights(int m, int k, const Spots& s) {
    Weights w;
    w.e = shapeAt(m, k, s.cx, s.cy, s.cy);
    w.p1 = shapeAt(m, k, s.p1x, s.p1y, s.cy);
    w.p2 = shapeAt(m, k, s.p2x, s.p2y, s.cy);
    w.c = shapeAt(m, k, 0.137, 0.29, 0.29);   // where the waveguide, cascade and rattle meet the body
    return w;
}
inline Weights weights(const Mode& md, const Spots& s) {
    if (!md.aOn) return modelWeights(md.mb, md.k, s);
    const Weights a = modelWeights(md.ma, md.k, s);
    if (!md.bOn || md.f <= 1e-6) return a;
    const Weights b = modelWeights(md.mb, md.k, s);
    return {lin(md.f, a.e, b.e), lin(md.f, a.p1, b.p1), lin(md.f, a.p2, b.p2), lin(md.f, a.c, b.c)};
}
// a model shaped in two dimensions (a membrane or plate): Kind, else K_LINE
inline int bodyKind(double family, double shape) {
    const Place p = place(family, shape);
    const auto& T = tables();
    return T.kind[p.f < 0.5 ? p.a : p.b];
}

inline int modeCount(const Body& b) {
    const Place p = place(b.family, b.shape);
    const auto& T = tables();
    return p.f > 1e-6 ? std::max(T.count[p.a], T.count[p.b]) : T.count[p.a];
}

// Tension: + the pitch rises with the level and falls back as it decays (a
// drum head, the big opera gong), - it starts low and rises (the small gong)
inline double glide(double tension, double level) {
    return tension >= 0 ? 1 + 3 * tension * level : 1 / (1 + 3 * -tension * level);
}

// The resonator after the pickups: what turns a bare bar into an instrument.
// 1 Tube: a closed tube under the bar, tuned to the note (odd harmonics), its
// fan (Tremolo) like a vibraphone's; the others fixed bodies, Res Tune from
// their own pitch: 2 Gourd (Helmholtz + shell), 3 Box (a guitar-like air and
// top), 4 Shell (a drum's wooden shell), 5 Cone (a metal horn, Baschet).
constexpr int NRADIATORS = 6;
constexpr int RADMODES = 6;
inline const char* radiatorName(int r) {
    static const char* n[NRADIATORS] = {"Off", "Tube", "Gourd", "Box", "Shell", "Cone"};
    return n[std::clamp(r, 0, NRADIATORS - 1)];
}
struct RadMode { double ratio, amp, t60; };
inline double radiatorBase(int r) { static const double b[NRADIATORS] = {0, 0, 180, 110, 240, 350}; return b[std::clamp(r, 0, NRADIATORS - 1)]; }
inline const RadMode* radiatorModes(int r) {
    static const RadMode m[NRADIATORS][RADMODES] = {
        {}, {},
        {{1, 1, 0.25}, {2.7, 0.5, 0.12}, {4.1, 0.35, 0.08}, {6.3, 0.2, 0.05}, {0, 0, 0}, {0, 0, 0}},
        {{1, 1, 0.2}, {2.0, 0.8, 0.15}, {3.9, 0.5, 0.1}, {5.5, 0.35, 0.07}, {8.2, 0.2, 0.05}, {0, 0, 0}},
        {{1, 0.8, 0.08}, {1.6, 0.7, 0.07}, {2.4, 0.5, 0.06}, {3.3, 0.4, 0.05}, {4.4, 0.3, 0.04}, {0, 0, 0}},
        {{1, 0.6, 0.6}, {2.89, 0.5, 0.5}, {4.95, 0.45, 0.4}, {6.99, 0.4, 0.35}, {9.53, 0.3, 0.3}, {11.97, 0.25, 0.25}}};
    return m[std::clamp(r, 0, NRADIATORS - 1)];
}
constexpr double kTubeT60 = 0.35;

// Doublets: each mode a pair, split by cents (an imperfect round object: warble)
// and by Hz at the pitch (a twin tuned apart: gamelan ombak), the partner
// excited a little less and dying a little sooner
constexpr double kPairGain = 0.8;
constexpr double kPairDecay = 0.85;
inline double pairSpread(int k) {   // per-mode spread of the split, 0.5 .. 1.5, fixed
    const double x = std::sin(k * 12.9898 + 1.7) * 43758.5453;
    return 0.5 + (x - std::floor(x));
}

}  // namespace physical
