//
//  scEnvelopeCurve.h
//  ofxOceanodeSuperCollider
//
//  An ADSR display whose segments bend with Alt+drag, shared by SlicerBox's
//  amplitude envelope and SampleBox's envelopes (header-only).
//
//  Curves are SuperCollider's own curve values (Env's numeric curves, shape
//  5): 0 is linear, negative values move fast first, positive ones slow
//  first; Env.adsr's default is -4. The display draws the same formula the
//  synth runs, so what you see is what plays.
//
//  • Alt + drag up / down on a segment (attack, decay, release): bend it; up
//    always raises the segment's middle, whichever way it goes.
//  • Alt + double-click on a segment: back to -4.
//

#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace scEnvCurve {

constexpr float kDefault = -4.0f;
constexpr float kMin = -12.0f, kMax = 12.0f;

// Progress 0..1 of a segment at time t (0..1) with curve c (SC's Env shape 5)
inline float shape(float t, float c){
    if(std::abs(c) < 0.001f) return t;
    return (1.0f - std::exp(c * t)) / (1.0f - std::exp(c));
}

// An ADSR: times in seconds, sustain 0..1, curves {attack, decay, release}
struct ADSR { float a = 0.01f, d = 0.3f, s = 0.8f, r = 1.0f; float curve[3] = {kDefault, kDefault, kDefault}; };

// Draws the ADSR in a box at the cursor and edits it:
//  • drag a point: the attack peak (attack time), the decay end (decay time
//    and sustain level), the sustain end (sustain level), the release end
//    (release time). The time scale stays put while dragging.
//  • Alt + drag up / down on a segment: bend it (up always raises its
//    middle); Alt + double-click: back to -4.
// Returns true when something changed. invert: drawn upside down.
inline bool editADSR(const char* id, ImVec2 size, ADSR& e, ImU32 color, ImU32 background,
                     bool invert = false, float maxTime = 20.0f){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID kPoint = ImGui::GetID("##pt"), kSeg = ImGui::GetID("##seg"), kPps = ImGui::GetID("##pps");
    const ImGuiIO& io = ImGui::GetIO();

    e.a = std::max(0.001f, e.a); e.d = std::max(0.001f, e.d); e.r = std::max(0.001f, e.r);
    e.s = std::min(1.0f, std::max(0.0f, e.s));
    const float hold = std::max(0.05f, (e.a + e.d + e.r) * 0.35f);
    const float total = e.a + e.d + hold + e.r;
    const float pad = 6.0f, iw = size.x - 2 * pad, ih = size.y - 2 * pad;
    int point = st->GetInt(kPoint, -1), seg = st->GetInt(kSeg, -1);
    if(!active){ point = -1; seg = -1; }
    // pixels per second: frozen while a point is dragged, so it does not jump
    const float pps = point >= 0 ? st->GetFloat(kPps, iw / total) : iw / total;
    auto X = [&](float t){ return a.x + pad + t * pps; };
    auto Y = [&](float v){ return invert ? a.y + pad + ih * v : a.y + pad + ih * (1.0f - v); };
    auto level = [&](float y){ const float v = (y - a.y - pad) / ih; return std::min(1.0f, std::max(0.0f, invert ? v : 1.0f - v)); };

    struct Seg { float t0, t1, v0, v1; int curve; };
    const Seg segs[4] = {{0, e.a, 0, 1, 0}, {e.a, e.a + e.d, 1, e.s, 1},
                         {e.a + e.d, e.a + e.d + hold, e.s, e.s, -1}, {e.a + e.d + hold, total, e.s, 0, 2}};
    const ImVec2 pts[4] = {ImVec2(X(e.a), Y(1)), ImVec2(X(e.a + e.d), Y(e.s)),
                           ImVec2(X(e.a + e.d + hold), Y(e.s)), ImVec2(X(total), Y(0))};
    const ImVec2 m = io.MousePos;
    int hoverPoint = -1, hoverSeg = -1;
    if(hovered){
        for(int k = 0; k < 4; k++) if(std::hypot(m.x - pts[k].x, m.y - pts[k].y) < 8.0f){ hoverPoint = k; break; }
        if(io.KeyAlt)
            for(const auto& sgm : segs) if(sgm.curve >= 0 && m.x >= X(sgm.t0) && m.x <= X(sgm.t1)){ hoverSeg = sgm.curve; break; }
    }
    if(ImGui::IsItemActivated()){
        if(io.KeyAlt && hoverSeg >= 0) seg = hoverSeg;
        else if(hoverPoint >= 0){ point = hoverPoint; st->SetFloat(kPps, iw / total); }
    }
    st->SetInt(kPoint, point);
    st->SetInt(kSeg, seg);

    bool changed = false;
    if(point >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)){
        const float frozen = st->GetFloat(kPps, iw / total);
        auto seconds = [&](float fromT){ return std::min(maxTime, std::max(0.001f, (m.x - (a.x + pad + fromT * frozen)) / frozen)); };
        switch(point){
            case 0: e.a = seconds(0); break;
            case 1: e.d = seconds(e.a); e.s = level(m.y); break;
            case 2: e.s = level(m.y); break;
            case 3: e.r = seconds(e.a + e.d + hold); break;
        }
        changed = true;
    }
    if(seg >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && io.MouseDelta.y != 0.0f){
        float dir = seg == 0 ? -1.0f : 1.0f;
        if(invert) dir = -dir;
        e.curve[seg] = std::min(kMax, std::max(kMin, e.curve[seg] - io.MouseDelta.y * 0.08f * dir));
        changed = true;
    }
    if(hovered && io.KeyAlt && hoverSeg >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
        e.curve[hoverSeg] = kDefault;
        changed = true;
    }

    dl->AddRectFilled(a, b, background, 3.0f);
    dl->PushClipRect(a, b, true);
    for(const auto& sgm : segs){
        const bool hot = sgm.curve >= 0 && (sgm.curve == hoverSeg || sgm.curve == seg);
        constexpr int n = 24;
        ImVec2 line[n + 1];
        for(int k = 0; k <= n; k++){
            const float u = (float)k / n;
            const float p = sgm.curve >= 0 ? shape(u, e.curve[sgm.curve]) : u;
            line[k] = ImVec2(X(sgm.t0 + (sgm.t1 - sgm.t0) * u), Y(sgm.v0 + (sgm.v1 - sgm.v0) * p));
        }
        dl->AddPolyline(line, n + 1, hot ? IM_COL32(255, 255, 255, 240) : color, 0, hot ? 2.5f : 1.8f);
    }
    for(int k = 0; k < 4; k++){
        const bool hot = k == hoverPoint || k == point;
        dl->AddCircleFilled(pts[k], hot ? 5.0f : 3.5f, hot ? IM_COL32(255, 255, 255, 255) : color);
    }
    dl->PopClipRect();
    if(hovered || active){
        static const char* names[3] = {"Attack", "Decay", "Release"};
        if(point >= 0 || hoverPoint >= 0){
            const int k = point >= 0 ? point : hoverPoint;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if(k == 0) ImGui::SetTooltip("Attack %.3f s", e.a);
            if(k == 1) ImGui::SetTooltip("Decay %.3f s, Sustain %.2f", e.d, e.s);
            if(k == 2) ImGui::SetTooltip("Sustain %.2f", e.s);
            if(k == 3) ImGui::SetTooltip("Release %.3f s", e.r);
        }else if(seg >= 0 || hoverSeg >= 0){
            const int k = seg >= 0 ? seg : hoverSeg;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ImGui::SetTooltip("%s curve %.2f\nAlt+drag up / down to bend · Alt+double-click: default", names[k], e.curve[k]);
        }else if(hovered){
            ImGui::SetTooltip("Drag the points: times and sustain · Alt+drag a segment: its curve");
        }
    }
    return changed;
}

// The old call (curves only), kept for callers that edit times elsewhere
inline int drawADSR(const char* id, ImVec2 size, float A, float D, float S, float R,
                    float curves[3], ImU32 color, ImU32 background, bool invert = false){
    ADSR e{A, D, S, R, {curves[0], curves[1], curves[2]}};
    const float before[3] = {curves[0], curves[1], curves[2]};
    editADSR(id, size, e, color, background, invert);
    for(int k = 0; k < 3; k++) if(e.curve[k] != before[k]){ curves[k] = e.curve[k]; return k; }
    return -1;
}

}
