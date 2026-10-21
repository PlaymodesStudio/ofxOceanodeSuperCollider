#pragma once
//
//  PhysicalModes.h — PhysicalBox's tables and parameter list, shared by the
//  PhysicalVoice UGen (OceanodePhysical.cpp) and the node's display
//  (src/nodes/scPhysicalBox.cpp), so what the window draws is what plays.
//  The same tables as synthdefs/WIP/PhysicalBox/physicalbox.eel2 (Phase 0).
//
#include <algorithm>
#include <cmath>

namespace physical {

constexpr int MAXM = 48;
constexpr int NSHAPES = 6;   // 0 harmonic, 1 bar, 2 glass, 3 bell, 4 plate, 5 membrane

// The UGen's inputs after in0 (external audio), in this order, with defaults.
// Same order as the @param lines of the Phase 0 script.
#define PHYSICAL_PARAMS(X) \
    X(freq, 220) X(gate, 0) X(hold, 0) X(strk, 0) X(atk, 0.08) X(rel, 0.6) \
    X(strike, 1) X(hardness, 0.5) X(mass, 0.3) X(bounce, 0) \
    X(bow, 0) X(bowpress, 0.5) X(bowspeed, 0.5) X(rosin, 0.2) \
    X(blow, 0) X(breath, 0.75) X(blowtype, 0) X(jetratio, 0.48) X(turb, 0.2) \
    X(scrape, 0) X(scrapespeed, 0.5) X(rough, 0.5) X(grit, 0.3) X(chatter, 0) \
    X(ext, 0) X(pos, 0.3) X(pick1, 0.2) X(pick2, 0.58) \
    X(atune, 0) X(adecay, 3) X(abright, 0.7) X(abore, 0) X(astiff, 0) \
    X(btune, 0) X(bdecay, 3) X(bshape, 1) X(bstretch, 0) X(bmodes, 32) X(bbright, 0.6) \
    X(material, 0.5) X(excite, 0.5) X(listen, 0.5) X(couple, 0) X(mutual, 0) \
    X(rattle, 0) X(rattleth, 0.3) X(tension, 0) X(cascade, 0) \
    X(drift, 0) X(driftrate, 0.3) X(freeze, 0)

enum Param {
    P_EXT_IN = 0,
#define PHYSICAL_ENUM(name, def) P_##name,
    PHYSICAL_PARAMS(PHYSICAL_ENUM)
#undef PHYSICAL_ENUM
    P_COUNT
};

struct Tables {
    double ratio[NSHAPES][MAXM];   // mode frequency / fundamental
    double q[NSHAPES][MAXM];       // spatial index (shape along the object)
    double ph[NSHAPES][MAXM];      // spatial phase
    Tables() {
        for (int k = 0; k < MAXM; ++k) {
            ratio[0][k] = k + 1; q[0][k] = k + 1; ph[0][k] = 0;
            const double beta = k == 0 ? 4.7300 : (k == 1 ? 7.8532 : (k + 1.5) * M_PI);
            ratio[1][k] = (beta / 4.7300) * (beta / 4.7300); q[1][k] = k + 1.5; ph[1][k] = 0;
            const double n = k + 2;
            ratio[2][k] = (n * (n * n - 1) / std::sqrt(n * n + 1)) / (6 / std::sqrt(5.0));
            q[2][k] = n; ph[2][k] = 0.25 * n;
        }
        // Bell partials (minor-third church bell), relative to the prime.
        static const double bell[18] = {0.5, 1.0, 1.183, 1.506, 2.0, 2.514, 2.662, 3.011, 4.166,
                                        4.944, 5.433, 6.796, 7.07, 8.215, 9.0, 9.81, 10.53, 11.9};
        for (int k = 0; k < MAXM; ++k) {
            ratio[3][k] = k < 18 ? bell[k] : ratio[3][k - 1] * 1.09;
            q[3][k] = k + 1; ph[3][k] = 0.37 * k;
        }
        // Plate: lowest MAXM of m^2 + (n * 0.618)^2, m, n = 1..12 (first minimum wins).
        double cand[144], cm[144], cn[144];
        int c = 0;
        for (int m = 1; m <= 12; ++m)
            for (int n = 1; n <= 12; ++n) { cand[c] = m * m + n * n * 0.381966; cm[c] = m; cn[c] = n; ++c; }
        for (int k = 0; k < MAXM; ++k) {
            int best = 0;
            for (int j = 1; j < c; ++j) if (cand[j] < cand[best]) best = j;
            ratio[4][k] = cand[best]; q[4][k] = cm[best]; ph[4][k] = 0.9 * cn[best];
            cand[best] = 1e6;
        }
        const double p0 = ratio[4][0];
        for (int k = 0; k < MAXM; ++k) ratio[4][k] /= p0;
        // Membrane: zeros of the Bessel functions J_m, sorted.
        static const double mem[36] = {2.4048, 3.8317, 5.1356, 5.5201, 6.3802, 7.0156, 7.5883, 8.4172,
            8.6537, 8.7715, 9.7610, 9.9361, 10.1735, 11.0647, 11.0864, 11.6198, 11.7915, 12.2251,
            12.3386, 13.0152, 13.3237, 13.3543, 13.5893, 14.3725, 14.4755, 14.7960, 14.8213, 14.9309,
            15.5898, 15.7002, 16.0378, 16.2235, 16.4706, 16.6983, 17.0038, 17.2412};
        for (int k = 0; k < MAXM; ++k) ratio[5][k] = k < 36 ? mem[k] : ratio[5][k - 1] + 0.31;
        for (int k = 0; k < MAXM; ++k) { ratio[5][k] /= 2.4048; q[5][k] = 1 + 0.5 * k; ph[5][k] = 0.61 * k; }
    }
};

inline const Tables& tables() { static const Tables t; return t; }

inline double lin(double x, double a, double b) { return a + x * (b - a); }
// wood (0) -> glass (0.5) -> metal (1)
inline double mat3(double m, double a, double b, double c) {
    return m < 0.5 ? lin(m * 2, a, b) : lin(m * 2 - 1, b, c);
}

// One mode of the body B as the voice computes it (no drift / tension).
struct Mode { double ratio, q, ph, t60; };
inline Mode mode(int k, double shape, double stretch, double material, double bdecay, double freeze) {
    const auto& t = tables();
    const double s = std::clamp(shape, 0.0, double(NSHAPES - 1));
    const int s0 = int(std::floor(s));
    const int s1 = std::min(s0 + 1, NSHAPES - 1);
    const double f = s - s0;
    Mode m;
    m.ratio = std::exp(lin(f, std::log(t.ratio[s0][k]), std::log(t.ratio[s1][k])));
    m.ratio = std::pow(m.ratio, 1 + 0.25 * stretch);
    m.q = lin(f, t.q[s0][k], t.q[s1][k]);
    m.ph = lin(f, t.ph[s0][k], t.ph[s1][k]);
    const double frz = 1 + std::pow(std::clamp(freeze, 0.0, 1.0), 3) * 400;
    m.t60 = std::max(bdecay * mat3(material, 0.3, 1.6, 3.0) * std::pow(m.ratio, -mat3(material, 1.6, 0.6, 0.15)) * frz, 0.002);
    return m;
}

}  // namespace physical
