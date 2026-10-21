#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <functional>

// PhysicalBox's pictures, all vector (ImDrawList): the 18 body models drawn as
// the objects they are (tinted by the material: wood, glass, metal), and small
// line icons for the window's section bars.
namespace physIcons {

constexpr float kPi = 3.14159265358979f;

inline ImU32 rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return int(std::clamp(v, 0.0f, 255.0f)); };
    return IM_COL32(c(r), c(g), c(b), c(a));
}
inline ImU32 shade(ImU32 col, float k, float alpha = 1.0f) {
    const float r = (col & 255), g = ((col >> 8) & 255), b = ((col >> 16) & 255), a = ((col >> 24) & 255);
    return k >= 1 ? rgba(r + (255 - r) * (k - 1), g + (255 - g) * (k - 1), b + (255 - b) * (k - 1), a * alpha)
                  : rgba(r * k, g * k, b * k, a * alpha);
}

// the colours of a material: 0 wood, 0.5 glass, 1 metal
struct Colors { ImU32 body, hi, edge, dark, skin, wood, metal; };
inline Colors materialColors(float material, float alpha = 1.0f) {
    const float wood[4] = {176, 118, 66, 255}, glass[4] = {150, 212, 222, 200}, metal[4] = {196, 200, 214, 255};
    float c[4];
    const float t = std::clamp(material, 0.0f, 1.0f);
    for(int i = 0; i < 4; ++i) c[i] = t < 0.5f ? wood[i] + (glass[i] - wood[i]) * t * 2 : glass[i] + (metal[i] - glass[i]) * (t * 2 - 1);
    const ImU32 body = rgba(c[0], c[1], c[2], c[3] * alpha);
    return {body, shade(body, 1.45f), shade(body, 0.55f), shade(body, 0.35f),
            rgba(232, 218, 190, 255 * alpha), rgba(96, 64, 40, 255 * alpha), rgba(150, 156, 170, 255 * alpha)};
}

// A drawing in units: x -0.8..0.8, y -0.5..0.5 (down), centred in the box
struct Canvas {
    ImDrawList* dl; ImVec2 c; float U;
    Canvas(ImDrawList* d, ImVec2 a, ImVec2 b) : dl(d), c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
        U(std::min((b.x - a.x) / 1.65f, (b.y - a.y) / 1.05f)) {}
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * U, c.y + y * U); }
    float th(float t) const { return std::max(1.0f, t * U); }
    void rect(float x0, float y0, float x1, float y1, ImU32 col, float r = 0) const { dl->AddRectFilled(P(x0, y0), P(x1, y1), col, r * U); }
    void rectO(float x0, float y0, float x1, float y1, ImU32 col, float r = 0, float t = 0.012f) const { dl->AddRect(P(x0, y0), P(x1, y1), col, r * U, 0, th(t)); }
    void line(float x0, float y0, float x1, float y1, ImU32 col, float t = 0.012f) const { dl->AddLine(P(x0, y0), P(x1, y1), col, th(t)); }
    void ell(float x, float y, float rx, float ry, ImU32 col) const { dl->AddEllipseFilled(P(x, y), ImVec2(rx * U, ry * U), col); }
    void ellO(float x, float y, float rx, float ry, ImU32 col, float t = 0.012f) const { dl->AddEllipse(P(x, y), ImVec2(rx * U, ry * U), col, 0, 0, th(t)); }
    void circle(float x, float y, float r, ImU32 col) const { dl->AddCircleFilled(P(x, y), r * U, col); }
    void circleO(float x, float y, float r, ImU32 col, float t = 0.012f) const { dl->AddCircle(P(x, y), r * U, col, 0, th(t)); }
    // half an ellipse (lower: from 0 to pi, y down), filled
    void halfEll(float x, float y, float rx, float ry, ImU32 col, bool lower = true) const {
        dl->PathEllipticalArcTo(P(x, y), ImVec2(rx * U, ry * U), 0, lower ? 0.0f : kPi, lower ? kPi : 2 * kPi, 32);
        dl->PathFillConvex(col);
    }
    void arcO(float x, float y, float rx, float ry, float a0, float a1, ImU32 col, float t = 0.012f) const {
        dl->PathEllipticalArcTo(P(x, y), ImVec2(rx * U, ry * U), 0, a0, a1, 32);
        dl->PathStroke(col, 0, th(t));
    }
    void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, ImU32 col) const {
        dl->AddQuadFilled(P(x0, y0), P(x1, y1), P(x2, y2), P(x3, y3), col);
    }
    // a profile turned around the vertical axis, filled in slices (bells, glasses): half width w(t), t 0..1 from y0 to y1
    void profile(float y0, float y1, const std::function<float(float)>& w, ImU32 col, ImU32 edge) const {
        const int n = 24;
        // no anti-aliased fill between the slices (its fringes would show as seams)
        const auto flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for(int i = 0; i < n; ++i) {
            const float t0 = float(i) / n, t1 = float(i + 1) / n;
            const float a = w(t0), b = w(t1), ya = y0 + (y1 - y0) * t0, yb = y0 + (y1 - y0) * t1;
            quad(-a, ya, a, ya, b, yb + (i + 1 < n ? 0.002f : 0.0f), -b, yb + (i + 1 < n ? 0.002f : 0.0f), col);
        }
        dl->Flags = flags;
        for(int side = -1; side <= 1; side += 2) {
            for(int i = 0; i <= n; ++i) { const float t = float(i) / n; dl->PathLineTo(P(side * w(t), y0 + (y1 - y0) * t)); }
            dl->PathStroke(edge, 0, th(0.012f));
        }
    }
};

// One body model (physical::Model order) in the box a..b; alpha fades it (crossfades)
inline void drawModel(ImDrawList* dl, ImVec2 a, ImVec2 b, int model, float material, float alpha = 1.0f) {
    const Canvas k(dl, a, b);
    const Colors m = materialColors(material, alpha);
    const ImU32 skin = m.skin, skinEdge = shade(skin, 0.6f), wood = m.wood, steel = m.metal;
    const ImU32 shadow = rgba(0, 0, 0, 70 * alpha), cord = rgba(200, 200, 210, 150 * alpha);
    auto bar = [&](float x0, float y0, float x1, float y1) {
        k.rect(x0, y0, x1, y1, m.body, 0.025f);
        k.line(x0 + 0.03f, y0 + 0.012f, x1 - 0.03f, y0 + 0.012f, m.hi, 0.014f);
        k.rectO(x0, y0, x1, y1, m.edge, 0.025f);
    };
    switch(model) {
    case 0: {   // Harmonic: a string between two bridges, its first standing waves
        k.ell(0, 0.33f, 0.72f, 0.04f, shadow);
        k.rect(-0.74f, -0.2f, -0.67f, 0.28f, wood, 0.01f);
        k.rect(0.67f, -0.2f, 0.74f, 0.28f, wood, 0.01f);
        for(int h = 1; h <= 3; ++h) {
            for(int i = 0; i <= 40; ++i) {
                const float x = -0.67f + 1.34f * i / 40;
                dl->PathLineTo(k.P(x, 0.02f - 0.2f / h * std::sin(kPi * h * i / 40)));
            }
            dl->PathStroke(shade(m.hi, 1.0f, 1.0f / h), 0, k.th(0.014f));
        }
        k.line(-0.67f, 0.02f, 0.67f, 0.02f, steel, 0.02f);
        break;
    }
    case 1: {   // Free bar: hung by cords at its nodes (0.224 from the ends)
        k.ell(0, 0.36f, 0.62f, 0.04f, shadow);
        for(float s : {-1.0f, 1.0f}) { k.line(s * 0.36f, -0.02f, s * 0.22f, -0.46f, cord, 0.008f); k.circle(s * 0.36f, 0.02f, 0.012f, m.dark); }
        bar(-0.65f, -0.03f, 0.65f, 0.1f);
        break;
    }
    case 2: {   // Xylophone: a thick bar on felt rails, between its neighbours
        k.rect(-0.78f, 0.08f, 0.78f, 0.13f, rgba(120, 40, 40, 230 * alpha), 0.01f);
        bar(-0.78f, -0.05f, -0.6f, 0.08f); bar(0.6f, -0.05f, 0.78f, 0.08f);
        bar(-0.5f, -0.14f, 0.5f, 0.08f);
        for(float s : {-1.0f, 1.0f}) k.circle(s * 0.28f, -0.03f, 0.018f, m.dark);
        break;
    }
    case 3: {   // Gamelan (saron): a bronze bar pinned over a carved trough
        k.quad(-0.74f, 0.0f, 0.74f, 0.0f, 0.6f, 0.38f, -0.6f, 0.38f, wood);
        k.line(-0.74f, 0.0f, 0.74f, 0.0f, shade(wood, 1.5f), 0.016f);
        k.rect(-0.5f, 0.06f, 0.5f, 0.3f, shade(wood, 0.6f), 0.04f);
        bar(-0.62f, -0.14f, 0.62f, 0.0f);
        for(float s : {-1.0f, 1.0f}) { k.line(s * 0.46f, -0.2f, s * 0.46f, 0.0f, steel, 0.02f); k.circle(s * 0.46f, -0.2f, 0.022f, steel); }
        break;
    }
    case 4: {   // Marimba: a wide bar, its tuned tube under it
        k.rect(-0.12f, -0.16f, 0.12f, 0.46f, steel, 0.02f);
        k.line(-0.08f, -0.14f, -0.08f, 0.44f, shade(steel, 1.4f), 0.014f);
        k.ellO(0, -0.16f, 0.12f, 0.03f, shade(steel, 0.6f));
        bar(-0.62f, -0.34f, 0.62f, -0.2f);
        break;
    }
    case 5: {   // Kalimba: steel tines over a bridge on a wooden box
        k.rect(-0.55f, -0.36f, 0.55f, 0.44f, wood, 0.08f);
        k.rectO(-0.55f, -0.36f, 0.55f, 0.44f, shade(wood, 0.6f), 0.08f);
        k.circle(0, 0.25f, 0.08f, shade(wood, 0.35f));
        k.rect(-0.42f, 0.04f, 0.42f, 0.08f, shade(wood, 0.5f));
        for(int i = 0; i < 9; ++i) {
            const float x = -0.36f + 0.09f * i, len = 0.36f - 0.06f * std::abs(i - 4);
            k.line(x, 0.08f, x, 0.04f - len, m.body, 0.03f);
            k.line(x - 0.008f, 0.06f, x - 0.008f, 0.06f - len, m.hi, 0.008f);
        }
        break;
    }
    case 6: {   // Wine glass
        k.ell(0, 0.42f, 0.26f, 0.04f, shadow);
        k.halfEll(0, -0.24f, 0.28f, 0.3f, m.body);
        k.ell(0, -0.24f, 0.28f, 0.05f, shade(m.body, 0.75f));
        k.ellO(0, -0.24f, 0.28f, 0.05f, m.hi);
        k.arcO(0, -0.24f, 0.28f, 0.3f, 0, kPi, m.edge);
        k.arcO(0, -0.24f, 0.2f, 0.22f, 0.25f, 1.1f, m.hi, 0.016f);
        k.line(0, 0.06f, 0, 0.4f, m.body, 0.03f);
        k.ell(0, 0.4f, 0.22f, 0.045f, m.body);
        k.ellO(0, 0.4f, 0.22f, 0.045f, m.edge);
        break;
    }
    case 7: {   // Singing bowl, its striker leaning on it
        k.ell(0, 0.36f, 0.5f, 0.05f, shadow);
        k.halfEll(0, -0.08f, 0.62f, 0.42f, m.body);
        k.arcO(0, -0.08f, 0.62f, 0.42f, 0, kPi, m.edge);
        k.ell(0, -0.08f, 0.62f, 0.11f, m.dark);
        k.ellO(0, -0.08f, 0.62f, 0.11f, m.hi, 0.016f);
        k.arcO(0, -0.08f, 0.5f, 0.32f, 0.3f, 1.2f, m.hi, 0.014f);
        k.line(0.38f, -0.48f, 0.78f, 0.1f, wood, 0.045f);
        break;
    }
    case 8: {   // Hand bell
        k.rect(-0.045f, -0.5f, 0.045f, -0.24f, wood, 0.02f);
        k.halfEll(0, -0.22f, 0.16f, 0.06f, m.body, false);
        k.profile(-0.22f, 0.3f, [](float t){ return 0.16f + 0.24f * std::pow(t, 2.2f); }, m.body, m.edge);
        k.ell(0, 0.3f, 0.4f, 0.06f, m.dark);
        k.ellO(0, 0.3f, 0.4f, 0.06f, m.hi);
        k.line(-0.08f, -0.16f, -0.2f, 0.24f, m.hi, 0.014f);
        break;
    }
    case 9: {   // Church bell: yoke, crown, clapper
        k.rect(-0.55f, -0.5f, 0.55f, -0.42f, wood, 0.02f);
        k.circleO(0, -0.38f, 0.06f, m.edge, 0.025f);
        k.halfEll(0, -0.32f, 0.2f, 0.07f, m.body, false);
        k.profile(-0.32f, 0.3f, [](float t){ return 0.2f + 0.32f * std::pow(t, 2.6f); }, m.body, m.edge);
        k.ell(0, 0.3f, 0.52f, 0.07f, m.dark);
        k.line(0, 0.0f, 0.02f, 0.3f, shade(steel, 0.6f), 0.02f);
        k.circle(0.02f, 0.33f, 0.05f, shade(steel, 0.5f));
        k.ellO(0, 0.3f, 0.52f, 0.07f, m.hi);
        k.line(-0.1f, -0.24f, -0.3f, 0.24f, m.hi, 0.014f);
        break;
    }
    case 10: {  // Gong on its frame
        k.line(-0.72f, -0.46f, -0.72f, 0.48f, wood, 0.04f);
        k.line(0.72f, -0.46f, 0.72f, 0.48f, wood, 0.04f);
        k.line(-0.78f, -0.44f, 0.78f, -0.44f, wood, 0.05f);
        for(float s : {-1.0f, 1.0f}) k.line(s * 0.3f, -0.44f, s * 0.18f, -0.25f, cord, 0.008f);
        k.circle(0, 0.06f, 0.36f, m.body);
        for(float r : {0.3f, 0.22f}) k.circleO(0, 0.06f, r, shade(m.body, 0.8f), 0.008f);
        k.circle(0, 0.06f, 0.1f, m.hi);
        k.circleO(0, 0.06f, 0.36f, m.edge, 0.02f);
        k.arcO(0, 0.06f, 0.33f, 0.33f, 3.6f, 4.6f, m.hi, 0.014f);
        break;
    }
    case 11: {  // Handpan: two shells, the ding and the tone fields
        k.ell(0, 0.24f, 0.58f, 0.05f, shadow);
        k.halfEll(0, 0.06f, 0.64f, 0.14f, shade(m.body, 0.7f));
        k.halfEll(0, 0.06f, 0.64f, 0.34f, m.body, false);
        k.arcO(0, 0.06f, 0.64f, 0.34f, kPi, 2 * kPi, m.edge);
        k.line(-0.64f, 0.06f, 0.64f, 0.06f, m.dark, 0.016f);
        for(int i = 0; i < 7; ++i) {
            const float ang = kPi * (0.12f + 0.76f * i / 6);
            k.ellO(-std::cos(ang) * 0.42f, -0.06f - std::sin(ang) * 0.14f, 0.075f, 0.035f, m.hi, 0.01f);
        }
        k.ell(0, -0.2f, 0.1f, 0.045f, m.hi);
        break;
    }
    case 12: {  // Steel pan: the skirt, the bowl with its grooved notes
        k.rect(-0.62f, -0.14f, 0.62f, 0.36f, shade(m.body, 0.65f));
        k.ell(0, 0.36f, 0.62f, 0.08f, shade(m.body, 0.5f));
        k.line(-0.62f, -0.14f, -0.62f, 0.36f, m.edge, 0.014f);
        k.line(0.62f, -0.14f, 0.62f, 0.36f, m.edge, 0.014f);
        k.ell(0, -0.14f, 0.62f, 0.22f, m.body);
        k.ellO(0, -0.14f, 0.62f, 0.22f, m.hi, 0.018f);
        for(int i = 0; i < 8; ++i) {
            const float ang = 2 * kPi * i / 8;
            k.ellO(std::cos(ang) * 0.4f, -0.14f + std::sin(ang) * 0.13f, 0.1f, 0.045f, m.edge, 0.01f);
        }
        k.ellO(0, -0.14f, 0.12f, 0.05f, m.edge, 0.01f);
        break;
    }
    case 13: {  // Tabla: the shell, its laced straps, the black syahi
        k.ell(0, 0.42f, 0.3f, 0.05f, shadow);
        k.profile(-0.24f, 0.4f, [](float t){ return 0.24f + 0.05f * std::sin(kPi * t); }, m.body, m.edge);
        for(int i = 0; i < 7; ++i) { const float x = -0.21f + 0.07f * i; k.line(x, -0.22f, x * 1.1f, 0.38f, rgba(70, 50, 40, 200 * alpha), 0.01f); }
        k.ell(0, -0.24f, 0.24f, 0.07f, skin);
        k.ellO(0, -0.24f, 0.24f, 0.07f, skinEdge);
        k.ell(0, -0.24f, 0.09f, 0.028f, rgba(30, 28, 30, 255 * alpha));
        k.ellO(0, 0.4f, 0.26f, 0.06f, shade(wood, 0.8f), 0.02f);
        break;
    }
    case 14: {  // Timpani: the kettle on legs, the head, the tuning rods
        for(float s : {-1.0f, 1.0f}) k.line(s * 0.32f, 0.22f, s * 0.46f, 0.48f, steel, 0.025f);
        k.halfEll(0, -0.14f, 0.56f, 0.46f, m.body);
        k.arcO(0, -0.14f, 0.56f, 0.46f, 0, kPi, m.edge);
        k.arcO(0, -0.14f, 0.44f, 0.36f, 0.4f, 1.3f, m.hi, 0.016f);
        for(int i = 0; i < 6; ++i) { const float x = -0.5f + 0.2f * i; k.line(x, -0.18f, x * 0.96f, 0.0f, steel, 0.012f); }
        k.ell(0, -0.16f, 0.58f, 0.1f, skin);
        k.ellO(0, -0.16f, 0.58f, 0.1f, skinEdge, 0.016f);
        break;
    }
    case 15: {  // Membrane: a drum head face on, two of its nodal lines, the lugs
        k.circle(0, 0, 0.44f, m.body);
        k.circle(0, 0, 0.39f, skin);
        k.line(-0.39f, 0, 0.39f, 0, rgba(120, 100, 80, 140 * alpha), 0.01f);
        k.circleO(0, 0, 0.2f, rgba(120, 100, 80, 140 * alpha), 0.01f);
        for(int i = 0; i < 8; ++i) { const float ang = 2 * kPi * i / 8; k.circle(std::cos(ang) * 0.415f, std::sin(ang) * 0.415f, 0.02f, steel); }
        k.circleO(0, 0, 0.44f, m.edge, 0.016f);
        break;
    }
    case 16: {  // Plate: hung by two wires, its Chladni lines
        k.line(-0.45f, -0.5f, -0.5f, -0.06f, cord, 0.008f);
        k.line(0.5f, -0.5f, 0.42f, -0.24f, cord, 0.008f);
        k.quad(-0.66f, -0.02f, 0.44f, -0.28f, 0.72f, 0.1f, -0.36f, 0.38f, m.body);
        k.line(-0.51f, 0.18f, 0.58f, -0.09f, shade(m.body, 0.7f), 0.01f);
        k.line(-0.11f, -0.15f, 0.18f, 0.24f, shade(m.body, 0.7f), 0.01f);
        k.line(-0.66f, -0.02f, 0.44f, -0.28f, m.hi, 0.016f);
        k.line(0.72f, 0.1f, -0.36f, 0.38f, m.edge, 0.016f);
        k.line(-0.66f, -0.02f, -0.36f, 0.38f, m.edge, 0.016f);
        break;
    }
    default: {  // Wood block: faces and its slit
        k.ell(0.05f, 0.3f, 0.6f, 0.05f, shadow);
        k.quad(-0.5f, -0.16f, 0.42f, -0.16f, 0.6f, -0.3f, -0.32f, -0.3f, m.hi);
        k.quad(0.42f, -0.16f, 0.6f, -0.3f, 0.6f, 0.1f, 0.42f, 0.24f, m.edge);
        k.rect(-0.5f, -0.16f, 0.42f, 0.24f, m.body);
        k.rect(-0.42f, 0.02f, 0.34f, 0.08f, m.dark, 0.02f);
        k.rectO(-0.5f, -0.16f, 0.42f, 0.24f, m.edge);
        break;
    }
    }
}

// ── small line icons for the section bars ────────────────────────────────────
enum Icon { Mallet, Bow, Air, Rasp, Contact, Speaker, ResOff, ResTube, ResGourd, ResBox, ResShell, ResCone,
            String, Pipe, Couple, Rattle, Drift, Placement, Saturate, Equalizer, Echo, Reverb, Space, NumIcons };

// an icon in a square of side s centred at c
inline void drawIcon(ImDrawList* dl, ImVec2 c, float s, int icon, ImU32 col) {
    const float u = s * 0.5f, t = std::max(1.0f, s * 0.085f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };
    auto L = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, t); };
    auto poly = [&](std::function<ImVec2(float)> f, int n = 24) { for(int i = 0; i <= n; ++i) dl->PathLineTo(f(float(i) / n)); dl->PathStroke(col, 0, t); };
    switch(icon) {
    case Mallet:
        L(-0.8f, 0.8f, 0.25f, -0.25f);
        dl->AddCircleFilled(P(0.45f, -0.45f), u * 0.36f, col);
        break;
    case Bow:
        poly([&](float k){ return P(-0.9f + 1.8f * k, 0.5f - 1.3f * k - 0.22f * std::sin(kPi * k)); });
        L(-0.78f, 0.62f, 0.86f, -0.6f);
        break;
    case Air:
        for(int i = 0; i < 3; ++i) {
            const float y = -0.5f + 0.5f * i, len = i == 1 ? 1.7f : 1.3f;
            poly([&](float k){ return P(-0.85f + len * k, y + 0.12f * std::sin(k * 2 * kPi)); });
        }
        break;
    case Rasp:
        poly([&](float k){ const int n = 7; const float x = -0.9f + 1.8f * k; return P(x, 0.25f - 0.5f * std::fabs(std::fmod(k * n, 1.0f) * 2 - 1)); }, 28);
        L(-0.9f, 0.55f, 0.9f, 0.55f);
        break;
    case Contact:
        dl->AddCircle(c, u * 0.75f, col, 0, t);
        dl->AddCircleFilled(c, u * 0.22f, col);
        L(-1, 0, -0.55f, 0); L(0.55f, 0, 1, 0); L(0, -1, 0, -0.55f); L(0, 0.55f, 0, 1);
        break;
    case Speaker:
        dl->AddRectFilled(P(-0.9f, -0.3f), P(-0.45f, 0.3f), col);
        dl->AddQuadFilled(P(-0.45f, -0.3f), P(0.05f, -0.75f), P(0.05f, 0.75f), P(-0.45f, 0.3f), col);
        dl->PathArcTo(P(0.05f, 0), u * 0.45f, -0.9f, 0.9f); dl->PathStroke(col, 0, t);
        dl->PathArcTo(P(0.05f, 0), u * 0.8f, -0.9f, 0.9f); dl->PathStroke(col, 0, t);
        break;
    case ResOff:
        dl->AddCircle(c, u * 0.7f, col, 0, t); L(-0.5f, 0.5f, 0.5f, -0.5f);
        break;
    case ResTube:
        L(-0.8f, -0.75f, 0.8f, -0.75f);
        dl->AddRect(P(-0.3f, -0.55f), P(0.3f, 0.9f), col, u * 0.1f, 0, t);
        break;
    case ResGourd:
        dl->AddCircle(P(0, 0.25f), u * 0.62f, col, 0, t);
        dl->AddRect(P(-0.18f, -0.9f), P(0.18f, -0.35f), col, 0, 0, t);
        break;
    case ResBox:
        dl->AddRect(P(-0.85f, -0.6f), P(0.85f, 0.6f), col, u * 0.1f, 0, t);
        dl->AddCircle(c, u * 0.25f, col, 0, t);
        break;
    case ResShell:
        dl->PathArcTo(P(0, -0.3f), u * 0.8f, 0, kPi); dl->PathStroke(col, ImDrawFlags_Closed, t);
        break;
    case ResCone:
        dl->AddQuad(P(-0.2f, -0.8f), P(0.2f, -0.8f), P(0.8f, 0.8f), P(-0.8f, 0.8f), col, t);
        break;
    case String:
        L(-0.9f, -0.6f, -0.9f, 0.6f); L(0.9f, -0.6f, 0.9f, 0.6f);
        poly([&](float k){ return P(-0.9f + 1.8f * k, -0.45f * std::sin(kPi * k)); });
        L(-0.9f, 0, 0.9f, 0);
        break;
    case Pipe:
        L(-0.9f, -0.4f, 0.9f, -0.4f); L(-0.9f, 0.4f, 0.9f, 0.4f); L(-0.9f, -0.4f, -0.9f, 0.4f);
        poly([&](float k){ return P(-0.9f + 1.8f * k, 0.22f * std::cos(kPi * k * 1.5f)); });
        break;
    case Couple:
        dl->AddCircle(P(-0.35f, 0), u * 0.5f, col, 0, t);
        dl->AddCircle(P(0.35f, 0), u * 0.5f, col, 0, t);
        break;
    case Rattle:
        poly([&](float k){ const float x = -0.9f + 1.8f * k; return P(x, (int(k * 8) % 2 ? -0.5f : 0.5f) * (1 - std::fabs(x) * 0.5f)); }, 8);
        break;
    case Drift:
        poly([&](float k){ return P(-0.9f + 1.8f * k, 0.45f * std::sin(k * 2 * kPi) * (0.4f + 0.6f * k)); });
        dl->AddCircleFilled(P(0.9f, 0), u * 0.16f, col);
        break;
    case Placement:
        dl->AddCircle(c, u * 0.75f, col, 0, t);
        for(int i = 0; i < 4; ++i) { const float a = kPi * 0.5f * i - kPi * 0.25f; dl->AddRectFilled(P(std::cos(a) * 0.75f - 0.18f, std::sin(a) * 0.75f - 0.18f), P(std::cos(a) * 0.75f + 0.18f, std::sin(a) * 0.75f + 0.18f), col); }
        dl->AddCircleFilled(P(0.2f, -0.15f), u * 0.18f, col);
        break;
    case Saturate:
        poly([&](float k){ return P(-0.9f + 1.8f * k, -std::clamp(std::sin(k * 2 * kPi) * 1.6f, -0.6f, 0.6f)); });
        break;
    case Equalizer:
        poly([&](float k){ const float x = -0.9f + 1.8f * k; return P(x, 0.3f - 0.7f * std::exp(-std::pow((x - 0.15f) * 3.0f, 2.0f)) + 0.2f * x); });
        break;
    case Echo:
        for(int i = 0; i < 4; ++i) { const float x = -0.75f + 0.5f * i, h = 0.85f * std::pow(0.62f, float(i)); L(x, 0.7f, x, 0.7f - 1.4f * h); }
        break;
    case Reverb:
        for(int i = 0; i < 3; ++i) { dl->PathArcTo(P(-0.8f, 0.6f), u * (0.5f + 0.5f * i), -kPi * 0.5f, 0); dl->PathStroke(col, 0, t); }
        break;
    case Space:
        for(int i = 0; i < 5; ++i) {
            const float x = std::sin(i * 2.4f) * 0.7f, y = std::cos(i * 1.7f) * 0.6f, r = 0.12f + 0.08f * (i % 2);
            L(x - r, y, x + r, y); L(x, y - r, x, y + r);
        }
        break;
    }
}

}
