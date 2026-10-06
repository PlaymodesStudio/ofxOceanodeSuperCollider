//
//  scGrainBoxPoly.cpp
//  ofxOceanodeSuperCollider
//

#include "scGrainBoxPoly.h"
#include "ofxSuperCollider.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <array>

// Static member definitions
const char* scGrainBoxPoly::LFO_TARGET_NAMES[scGrainBoxPoly::NUM_LFO] = {"Pos","Dur","Pitch","Amp","Pan","TrRt","Cut"};

namespace {
// Index 6 (Cut) drives the playhead's output filter (control rate, one value
// per output channel).
const char* kLfoSCShape[7]  = {"lfoShapePos","lfoShapeDur","lfoShapePitch","lfoShapeAmp","lfoShapePan","lfoShapeTrRt","lfoShapeCut"};
const char* kLfoSCSpeed[7]  = {"lfoSpeedPos","lfoSpeedDur","lfoSpeedPitch","lfoSpeedAmp","lfoSpeedPan","lfoSpeedTrRt","lfoSpeedCut"};
const char* kLfoSCPhase[7]  = {"lfoPhasePos","lfoPhaseDur","lfoPhasePitch","lfoPhaseAmp","lfoPhasePan","lfoPhaseTrRt","lfoPhaseCut"};
const char* kLfoSCQuant[7]  = {"lfoQuantPos","lfoQuantDur","lfoQuantPitch","lfoQuantAmp","lfoQuantPan","lfoQuantTrRt","lfoQuantCut"};
const char* kLfoSCStr[7]    = {"lfoStrPos",  "lfoStrDur",  "lfoStrPitch",  "lfoStrAmp",  "lfoStrPan",  "lfoStrTrRt",  "lfoStrCut"};
const char* kLfoSCPow[7]    = {"lfoPowPos",  "lfoPowDur",  "lfoPowPitch",  "lfoPowAmp",  "lfoPowPan",  "lfoPowTrRt",  "lfoPowCut"};
const char* kLfoSCUnique[7] = {"uniquelfoPos","uniquelfoDur","uniquelfoPitch","uniquelfoAmp","uniquelfoPan","uniquelfoTrRt","uniquelfoCut"};
#if OFXOCEANODESC_HAS_TRANSPORT
const char* kLfoSCAnchor[7] = {"lfoAnchorPos","lfoAnchorDur","lfoAnchorPitch","lfoAnchorAmp","lfoAnchorPan","lfoAnchorTrRt","lfoAnchorCut"};
#endif
// Strength ranges differ per target:
//   Pos/Dur/Amp: 0..1   Pan: 0..2   Pitch: 0..48 semitones   TrRt: 0..32 beat-divisions
//   Cut: 0..8 octaves above the cutoff
const float kLfoStrMax[7] = {1.0f, 1.0f, 48.0f, 1.0f, 2.0f, 16.0f, 8.0f};
const char* kFilterNames[6] = {"Off", "LPF2", "BPF", "Notch", "HPF2", "LP-HP"};
const char* kEqBandNames[5] = {"Low", "LoMid", "Mid", "HiMid", "High"};

// ── Colour palette (the only place colours are defined) ─────────────────────
// Dark neutral panels like the Oceanode timeline toolbar and the RhythmBox
// window, a calm light-grey waveform, and one restrained accent per playhead
// (tab, grain highlights, position cursors, in/out markers, envelope and LFO
// curves). Voices of a playhead differ only by a slight brightness change.
namespace gbpPal {
const ImU32 canvas      = IM_COL32( 11,  12,  14, 255);  // waveform / curve / LFO background
const ImU32 border      = IM_COL32( 46,  50,  58, 255);
const ImU32 grid        = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong  = IM_COL32(255, 255, 255,  34);
const ImU32 header      = IM_COL32( 30,  32,  38, 255);  // section headers
const ImU32 headerText  = IM_COL32(205, 205, 215, 255);
const ImU32 textDim     = IM_COL32(150, 150, 160, 255);
const ImU32 wave        = IM_COL32(214, 216, 222, 215);  // EQ curve / handles
// Waveforms are drawn in their playhead's accent, all at the same alpha:
const int   waveAlpha   = 150;   // inside the region / Global view
const int   waveAlphaDim = 45;   // outside the region
const ImU32 outside     = IM_COL32(  0,   0,   0,  90);  // dims out-of-region areas
const ImU32 handleLight = IM_COL32(235, 235, 240, 230);
const ImU32 splitter    = IM_COL32( 90,  90,  90, 150);  // as RhythmBox's splitter
const ImU32 splitterHot = IM_COL32(180, 180, 180, 200);
// One accent per playhead (muted, similar lightness; the first is the
// timeline's periwinkle).
const unsigned char accentRGB[8][3] = {
    {150, 165, 255},   // periwinkle
    {236, 178,  96},   // amber
    { 96, 200, 176},   // teal
    {228, 122, 146},   // rose
    {170, 208, 112},   // lime
    {186, 146, 232},   // lavender
    {108, 180, 232},   // sky
    {232, 146, 104},   // coral
};
} // namespace gbpPal

ImU32 gbpAccent(int playhead, int alpha, float brightness = 1.0f) {
    const unsigned char* c = gbpPal::accentRGB[((playhead % 8) + 8) % 8];
    auto ch = [brightness](unsigned char v) { return (int)std::max(0.0f, std::min(255.0f, v * brightness)); };
    return IM_COL32(ch(c[0]), ch(c[1]), ch(c[2]), std::max(0, std::min(255, alpha)));
}
// Slight per-voice variation inside a playhead's accent
float gbpVoiceBrightness(int voice) { return 1.0f - 0.07f * (float)(((voice % 4) + 4) % 4); }

// ── Slider with fine / snap drag ─────────────────────────────────────────────
// ImGui::SliderFloat, plus: Shift while dragging = FINE (the value moves 10x
// slower than the mouse, from where it was, instead of jumping to the mouse);
// semitones: Ctrl (Cmd on macOS) while dragging snaps to whole semitones.
// Logarithmic sliders are fine-dragged in log space. The modifiers are listed
// in a (delayed) tooltip; a caller's own tooltip replaces it.
// Multislider for a vector parameter (one bar per value, e.g. per playhead):
// drawn instead of a single slider whenever the vector has more than one
// value (typically while a vector is connected). Click / drag paints the
// value under the mouse (sweeps fill every bar crossed); hovering shows
// index and value. Same height and colours as a slider frame.
// Trigger interval in Beats mode: note values (T = triplet, D = dotted), in beats
struct GbpNote { float beats; const char* name; };
const GbpNote kGbpNotes[] = {
    {1.0f / 16.0f, "64th"},  {1.0f / 12.0f, "32ndT"}, {1.0f / 8.0f, "32nd"},  {1.0f / 6.0f, "16thT"},
    {3.0f / 16.0f, "32ndD"}, {1.0f / 4.0f,  "16th"},  {1.0f / 3.0f, "8thT"},  {3.0f / 8.0f, "16thD"},
    {1.0f / 2.0f,  "8th"},   {2.0f / 3.0f,  "4thT"},  {3.0f / 4.0f, "8thD"},  {1.0f,        "4th"},
    {4.0f / 3.0f,  "halfT"}, {1.5f,         "4thD"},  {2.0f,        "half"},  {8.0f / 3.0f, "1 barT"},
    {3.0f,         "halfD"}, {4.0f,         "1 bar"}, {6.0f,        "1 barD"}, {8.0f,       "2 bars"},
    {16.0f,        "4 bars"}
};
constexpr int kGbpNoteCount = (int)(sizeof(kGbpNotes) / sizeof(kGbpNotes[0]));
int gbpNearestNote(float beats) {
    int best = 0; float bd = 1e9f;
    const float lb = std::log(std::max(1e-4f, beats));
    for(int i = 0; i < kGbpNoteCount; i++) {
        const float d = std::abs(std::log(kGbpNotes[i].beats) - lb);
        if(d < bd) { bd = d; best = i; }
    }
    return best;
}

bool gbpMultiSlider(const char* id, std::vector<float>& v, float mn, float mx, const char* fmt = "%.3f",
                    bool logScale = false) {
    logScale = logScale && mn > 0.0f && mx > mn;
    // value <-> 0..1 of the height (log: equal ratios get equal heights)
    auto toT = [&](float x) {
        if(logScale) return std::log(std::max(mn, x) / mn) / std::log(mx / mn);
        return (x - mn) / ((mx - mn) != 0.0f ? (mx - mn) : 1.0f);
    };
    auto fromT = [&](float t) {
        if(logScale) return mn * std::exp(t * std::log(mx / mn));
        return mn + t * ((mx - mn) != 0.0f ? (mx - mn) : 1.0f);
    };
    if(v.empty()) return false;
    const float w = ImGui::CalcItemWidth();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(std::max(w, 8.0f), std::max(1.0f, h)));
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                      ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                      st.FrameRounding);
    const int   n    = (int)v.size();
    const float bw   = w / (float)n;
    const float span = (mx - mn) != 0.0f ? (mx - mn) : 1.0f;
    // bars grow from 0 when the range crosses it, else from the bottom
    const float zeroT = (!logScale && mn < 0.0f && mx > 0.0f) ? (0.0f - mn) / span : 0.0f;
    const ImU32 barCol = ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
    for(int i = 0; i < n; i++) {
        const float t  = std::max(0.0f, std::min(1.0f, toT(v[i])));
        const float x0 = p0.x + i * bw + (bw > 4.0f ? 1.0f : 0.0f);
        const float x1 = p0.x + (i + 1) * bw - (bw > 4.0f ? 1.0f : 0.0f);
        const float yA = p0.y + h - zeroT * h;
        const float yB = p0.y + h - t * h;
        dl->AddRectFilled(ImVec2(x0, std::min(yA, yB)), ImVec2(std::max(x0 + 1.0f, x1), std::max(yA, yB) + (yA == yB ? 1.0f : 0.0f)), barCol);
    }
    bool changed = false;
    static ImGuiID dragId = 0;
    static int     lastIdx = -1;
    static float   lastVal = 0.0f;
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const int   idx = std::max(0, std::min(n - 1, (int)((mp.x - p0.x) / bw)));
    const float val = fromT(std::max(0.0f, std::min(1.0f, 1.0f - (mp.y - p0.y) / h)));
    if(active) {
        const ImGuiID me = ImGui::GetItemID();
        if(ImGui::IsItemActivated() || dragId != me) { dragId = me; lastIdx = idx; lastVal = val; }
        // fill every bar between the previous and the current mouse position
        const int a = std::min(lastIdx, idx), b = std::max(lastIdx, idx);
        for(int i = a; i <= b; i++) {
            const float f  = (b == a) ? 1.0f : (float)(i - lastIdx) / (float)(idx - lastIdx);
            const float nv = lastVal + (val - lastVal) * f;
            if(v[i] != nv) { v[i] = nv; changed = true; }
        }
        lastIdx = idx; lastVal = val;
    }
    if(hovered || active) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, v[idx]);
        ImGui::SetTooltip("%d / %d:  %s", idx + 1, n, buf);
    }
    return changed;
}

bool gbpSliderFloat(const char* id, float* v, float mn, float mx, const char* fmt = "%.3f",
                   ImGuiSliderFlags flags = 0, bool semitones = false) {
    // After the click (which jumps to the mouse as usual, unless Shift is
    // held), the drag is RELATIVE: the value moves by the mouse delta (1x, or
    // 0.1x with Shift) from an unsnapped value, so pressing or releasing a
    // modifier never makes it jump; a change of the snap state re-anchors the
    // unsnapped value at the value shown.
    static ImGuiID dragId = 0;
    static float   rawValue = 0.0f;
    static bool    wasSnapping = false;
    const float before = *v;
    bool changed = ImGui::SliderFloat(id, v, mn, mx, fmt, flags);
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID  itemId = ImGui::GetItemID();
    const float lo = std::min(mn, mx), hi = std::max(mn, mx);
    const bool logScale = (flags & ImGuiSliderFlags_Logarithmic) && lo > 0.0f;
    const bool snapping = semitones && (io.KeyCtrl || io.KeySuper);
    if(ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {   // not while typing (Ctrl+click)
        if(ImGui::IsItemActivated() || dragId != itemId) {
            dragId = itemId;
            if(io.KeyShift) { *v = before; changed = false; }   // fine from the start: no click jump
            rawValue = *v;
            wasSnapping = snapping;
        } else {
            if(snapping != wasSnapping) { rawValue = before; wasSnapping = snapping; }
            const float w     = std::max(1.0f, ImGui::GetItemRectSize().x);
            const float speed = io.KeyShift ? 0.1f : 1.0f;
            const float dx    = io.MouseDelta.x;
            if(logScale) {
                const float lr = std::log(std::max(lo, rawValue)) + dx * (std::log(hi) - std::log(lo)) / w * speed;
                rawValue = std::exp(lr);
            } else {
                rawValue += dx * (hi - lo) / w * speed;
            }
            rawValue = std::max(lo, std::min(hi, rawValue));
            *v = snapping ? std::max(lo, std::min(hi, std::round(rawValue))) : rawValue;
            changed = (*v != before);
        }
    } else if(dragId == itemId && !ImGui::IsItemActive()) {
        dragId = 0;
    }
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip(semitones ? "Shift+drag: fine (10x slower)\n"
                                      "Ctrl/Cmd+drag: whole semitones (press it after starting the\n"
                                      "drag: Ctrl/Cmd+click types a value)"
                                    : "Shift+drag: fine (10x slower)");
    return changed;
}

// ── Minimal OSC encoder (ofxOsc has no public serializer) ───────────────────
// Encodes "/b_setn bufnum 0 n v0..v{n-1}" as a raw OSC message, used as the
// completion message of /b_alloc: scsynth runs it right after the buffer is
// installed, so the envelope data can never land before the buffer exists.
void gbpOscPad(std::string& o) { while(o.size() % 4) o.push_back('\0'); }
void gbpOscString(std::string& o, const std::string& str) { o += str; o.push_back('\0'); gbpOscPad(o); }
void gbpOscU32(std::string& o, uint32_t u) {
    o.push_back((char)((u >> 24) & 0xFF));
    o.push_back((char)((u >> 16) & 0xFF));
    o.push_back((char)((u >>  8) & 0xFF));
    o.push_back((char)( u        & 0xFF));
}
ofBuffer gbpEncodeBSetn(int bufnum, const std::vector<float>& data) {
    std::string o;
    gbpOscString(o, "/b_setn");
    gbpOscString(o, ",iii" + std::string(data.size(), 'f'));
    gbpOscU32(o, (uint32_t)(int32_t)bufnum);
    gbpOscU32(o, 0);
    gbpOscU32(o, (uint32_t)data.size());
    for(float f : data) { uint32_t u; std::memcpy(&u, &f, 4); gbpOscU32(o, u); }
    ofBuffer b;
    b.set(o.data(), o.size());
    return b;
}

// False for a bus object whose index the server no longer maps to it (server
// rebooted, allocators reset): it must not be freed.
bool gbpBusIsLive(ofxSCServer* srv, ofxSCBus* b) {
    if(!srv || !b || b->index < 0 || b->index >= (int)srv->audioBusses.size()) return false;
    return srv->audioBusses[b->index] == b;
}
} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ════════════════════════════════════════════════════════════════════════════

scGrainBoxPoly::scGrainBoxPoly(vector<serverManager*> servers)
    : scNode("GrainBoxPoly"), allServers(servers)
{
    color = ofColor(255, 140, 60);

    for(auto* sm : allServers) {
        if(sm && sm->getServer()) {
            browser.setPreviewServer(sm->getServer());
            break;
        }
    }

    mainSample.peaks.resize(WAVEFORM_BINS, 0.0f);

    std::string dir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(dir))
        dir = ofToDataPath("", true);
    browser.refresh(dir);
    // Double-click / Enter on a file: load it into the selected playhead's
    // sample (click and the arrow keys only preview; files can also be dragged
    // onto the waveform).
    browser.onActivate = [this](const std::string& path) {
        if(playheads.empty()) return;
        Playhead& ph = *playheads[std::max(0, std::min((int)playheads.size() - 1, activePlayhead))];
        if(globalTab) loadSample(path);   // Global tab: the shared sample
        else          loadSampleForPlayhead(ph, path);
        browser.stopPreview();
    };
}

scGrainBoxPoly::~scGrainBoxPoly() {
    try {
        nodeListeners.unsubscribeAll();
        browser.stopPreview();
        for(auto& [srv, d] : drySynths) if(d) { d->free(); delete d; }
        drySynths.clear();

        for(auto& ph : playheads) {
            if(!ph) continue;
            ph->listeners.unsubscribeAll();
            ph->grainListeners.clear();
            for(auto& [srv, s] : ph->synths) {
                if(s) { s->free(); delete s; }
            }
            ph->synths.clear();
            for(auto& [srv, b] : ph->envBufs) {
                if(b) { if(bufferIsLive(srv, b)) b->free(); delete b; }
            }
            ph->envBufs.clear();
            for(auto& [srv, bufs] : ph->own.bufs)
                for(auto* b : bufs) { if(b) { if(bufferIsLive(srv, b)) b->free(); delete b; } }
            ph->own.bufs.clear();
            for(auto& [srv, l] : ph->liveSC) {
                if(l.rec) { l.rec->free(); delete l.rec; }
                if(l.buf) { if(bufferIsLive(srv, l.buf)) l.buf->free(); delete l.buf; }
                if(l.phaseBus) { if(gbpBusIsLive(srv, l.phaseBus)) l.phaseBus->free(); delete l.phaseBus; }
                if(l.fbBus) { if(gbpBusIsLive(srv, l.fbBus)) l.fbBus->free(); delete l.fbBus; }
            }
            ph->liveSC.clear();
        }

        for(auto& [srv, bufs] : mainSample.bufs) {
            for(auto* b : bufs) { if(b) { if(bufferIsLive(srv, b)) b->free(); delete b; } }
        }
        mainSample.bufs.clear();

        for(auto& [srv, st] : fx) {
            for(auto* fs : st.synths) if(fs) { fs->free(); delete fs; }
            for(auto* b : st.buses) if(b) { if(gbpBusIsLive(srv, b)) b->free(); delete b; }
        }
        fx.clear();

        for(auto& [srv, b] : privateBuses) {
            if(b) { b->free(); delete b; }
        }
        privateBuses.clear();
    } catch(const std::exception& e) {
        ofLogError("scGrainBoxPoly") << "Destructor: " << e.what();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::setup() {
    // Playhead 1: its parameters keep their original names (presets, connections).
    playheads.push_back(std::make_unique<Playhead>());
    Playhead& p0 = ph0();
    p0.index = 0;
    p0.sfx   = "";
    createPlayheadParameters(p0);

    // Minimal node GUI: everything else is published on demand from the window.
    addSeparator("GrainBox");
    addParameter(showWindow.set("Show", false));
    addParameter(numChannelsP.set("N Chan", 1, 1, MAX_CHANNELS));
    addParameter(dryWetP.set("Dry/Wet", 1.0f, 0.0f, 1.0f));   // 1 = only the engines (as before)

    addSeparator("Trigger");
    addParameter(p0.trigger);
    addParameter(p0.autoTrig);

    addSeparator("Grain");
    addParameter(p0.amp);
    addParameter(p0.pitch);
    addParameter(p0.duration);
    addParameter(p0.position);
    addParameter(p0.panAz);      // GrainBoxPoly: per-playhead placement (a vector = one value per playhead)

    addSeparator("Levels");
    addParameter(p0.levels);
    addOutputParameter(sampleMsP.set("SampleMs", 0.0f, 0.0f, 3600000.0f));

    // Global tab (published on demand, like the other window controls).
    // Defaults leave the sound unchanged.
    transposeP.set ("Transpose", 0.0f, -48.0f, 48.0f);
    volumeP.set    ("Volume",    1.0f,   0.0f,  1.0f);
    speedP.set     ("Speed",     1.0f, 0.125f,  8.0f);
    // (distinct names: playhead 1 owns "Position" / "Duration" / "Chance")
    gPosP.set      ("Global Position", 0.0f, -1.0f, 1.0f);
    gDurP.set      ("Global Duration", 0.0f, -1.0f, 1.0f);
    gChanceP.set   ("Global Chance",   0.0f, -1.0f, 1.0f);
    timeUnitsP.set ("Time Units", 0, 0, 2);   // 0 Relative, 1 ms, 2 Beats
    snapshotP.set  ("Snapshot",   0, 0, 16);  // 1..16 recalls that slot (0 = none)
    morphP.set      ("Morph",        0.0f, 0.0f, 1.0f);   // A -> B
    morphAP.set     ("Morph A",      0, 0, 16);           // 0 = none
    morphBP.set     ("Morph B",      0, 0, 16);
    morphTimeP.set  ("Morph Time",   4.0f, 0.0f, 64.0f);  // beats (or s)
    morphBeatsP.set ("Morph Beats",  true);
    morphCurveP.set ("Morph Curve",  1, 0, 3);            // Lin, Ease, Exp, Log
    morphSwitchP.set("Morph Switch", 1, 0, 2);            // switches jump at start / middle / end
    morphGlideP.set ("Morph Glide",  false);              // every recall glides
    fxOnP[FX_EQ].set     ("EQ On",     false);
    fxOnP[FX_ECHO].set   ("Echo On",   false);
    fxOnP[FX_REVERB].set ("Reverb On", false);
    for(int b = 0; b < 5; b++) eqGainP[b].set(std::string("EQ ") + kEqBandNames[b], 0.0f, -24.0f, 24.0f);
    {
        const float eqFreqDef[5] = {80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
        for(int b = 0; b < 5; b++) {
            const bool shelf = scEQEditor::isShelf(b);
            eqFreqP[b].set (std::string("EQ ") + kEqBandNames[b] + " Freq", eqFreqDef[b], 20.0f, 20000.0f);
            eqShapeP[b].set(std::string("EQ ") + kEqBandNames[b] + (shelf ? " Slope" : " Q"), 1.0f,
                            0.1f, shelf ? 4.0f : 20.0f);
        }
    }
    eqMixP.set      ("EQ Mix",       1.0f,   0.0f,     1.0f);
    echoDelayP.set  ("Echo Delay",   0.2f,   0.01f,    2.0f);
    echoBeatsP.set  ("Echo Beats",   false);
    echoBeatValP.set("Echo Delay Beats", 0.75f, 1.0f / 16.0f, 4.0f);   // 8thD
    echoFeedP.set   ("Echo Feed",    0.5f,   0.0f,     1.0f);
    echoCutoffP.set ("Echo Cutoff", 60.0f,  12.0f,   120.0f);   // MIDI note, as the Echo SynthDef
    echoMixP.set    ("Echo Mix",     0.35f,  0.0f,     1.0f);
    echoFilterP.set ("Echo Filter",  0, 0, 3);
    echoResonanceP.set("Echo Resonance", 0.2f, 0.0f,   1.0f);
    revSizeP.set    ("Rev Size",    30.0f,   0.0f,    60.0f);
    revDecayP.set   ("Rev Decay",    4.0f,   0.05f,   60.0f);
    revPredelayP.set("Rev Predelay",20.0f,   0.0f,  1000.0f);   // ms
    revLowpassP.set ("Rev Lowpass", 10000.0f, 20.0f, 20000.0f);
    revMixP.set     ("Rev Mix",      0.33f,  0.0f,     1.0f);
    revPositionP.set("Rev Position", 0.8f,   0.0f,     1.0f);    // SpaceMaster \position (early -> late)
    revSpreadP.set  ("Rev Spread",   0.15f, -1.0f,     1.0f);    // SpaceMaster \spread

#if OFXOCEANODESC_HAS_TRANSPORT
    // In Sync To Transport mode the auto-trigger grid, the LFOs and TrigDur
    // follow the global transport (all playheads); BPM input and SyncGate are
    // ignored by the synths.
    addInspectorParameter(syncToTransportP.set("Sync To Transport", false));
    addInspectorParameter(beatOffsetP.set("Beat Offset", 0.0f, -64.0f, 64.0f));
    stepClock.setPatternLength(32.0);   // 32-beat grid block -> anchorPos period 2048 beats
#endif

    // Output port
    scNode::addInput("In");     // live input (recorded by Live playheads)
    scNode::addOutput("Out");
    requireFxSynthdefs();

    // ── Pre-allocate env buffers for all servers. The data travels as the
    //    /b_alloc completion message (see ensureEnvBuffer), so it is written
    //    right after the (asynchronous) allocation, never before it. ──────
    computeEnvData(p0);
    allocEnvBuffers(p0);

    setupPlayheadListeners(p0);

    // ── Global listeners ──────────────────────────────────────────────────

    // numChannels: recreate synths with new SynthDef variant (each playhead's
    // effective channel count / offset depends on it)
    nodeListeners.push(numChannelsP.newListener([this](int& n) {
        if(n < 1 || n > MAX_CHANNELS) return;
        if(n == oldNumChannels) return;
        oldNumChannels = n;
        for(auto* sm : allServers) {
            if(!sm || !sm->getServer()) continue;
            replaceSynths(sm->getServer());
            if(drySynths.count(sm->getServer())) createDry(sm->getServer());   // GrainBoxPolyDry_N
            // FX SynthDefs are per channel count too
            if(fx.count(sm->getServer())) rebuildFx(sm->getServer());
        }
    }));

    // Global mixing controls → every synth
    nodeListeners.push(transposeP.newListener([this](float&) { sendMixParams(); }));
    nodeListeners.push(volumeP.newListener([this](float&)    { sendMixParams(); }));
    nodeListeners.push(dryWetP.newListener([this](float&) {
        sendMixParams();
        for(auto& [srv, d] : drySynths) if(d) d->set("dry", dryGain());
    }));
    nodeListeners.push(speedP.newListener([this](float&)     { sendMixParams(); }));
    // Time units: convert the stored values (no jump), or, while a preset is
    // setting them (already in its unit), just adopt the unit
    nodeListeners.push(snapshotP.newListener([this](int& v) {
        if(snapRecalling || v < 1 || v > 16) return;
        if(morphGlideP.get()) startGlide(v - 1);
        else                  recallSnapshot(v - 1);
    }));
    nodeListeners.push(morphP.newListener([this](float&)  { applyAB(); }));
    nodeListeners.push(morphAP.newListener([this](int&)   { applyAB(); }));
    nodeListeners.push(morphBP.newListener([this](int&)   { applyAB(); }));
    nodeListeners.push(timeUnitsP.newListener([this](int& m) {
        const int to = std::max(0, std::min(2, m));
        if(to == timeUnitsCur) { updateTimeRanges(); return; }
        if(loadingPreset) { timeUnitsCur = to; updateTimeRanges(); timeDirty = true; return; }
        convertTimeUnits(timeUnitsCur, to);
    }));
    for(auto* gp : {&gPosP, &gDurP, &gChanceP})
        nodeListeners.push(gp->newListener([this](float&) {
            for(auto& ph : playheads) sendOffsetParams(*ph);
        }));

    // FX chain: an On toggle re-plans the chain; the other controls go to
    // their stage's synth
    for(int st = 0; st < FX_COUNT; st++)
        nodeListeners.push(fxOnP[st].newListener([this](bool&) { rebuildFxAll(); }));
    auto fxParamListener = [this](ofParameter<float>& p, int stage) {
        nodeListeners.push(p.newListener([this, stage](float&) {
            for(auto& [srv, st] : fx)
                if(st.synths[stage]) sendFxParams(stage, st.synths[stage]);
        }));
    };
    for(int b = 0; b < 5; b++) {
        fxParamListener(eqGainP[b],  FX_EQ);
        fxParamListener(eqFreqP[b],  FX_EQ);
        fxParamListener(eqShapeP[b], FX_EQ);
        for(auto* ep : {&eqGainP[b], &eqFreqP[b], &eqShapeP[b]})
            nodeListeners.push(ep->newListener([this](float&) { eqCurveDirty = true; }));
    }
    fxParamListener(eqMixP,       FX_EQ);
    fxParamListener(echoDelayP,   FX_ECHO);
    nodeListeners.push(echoBeatsP.newListener([this](bool&) {
        for(auto& [srv, st] : fx) if(st.synths[FX_ECHO]) sendFxParams(FX_ECHO, st.synths[FX_ECHO]);
    }));
    fxParamListener(echoBeatValP, FX_ECHO);
    fxParamListener(echoFeedP,    FX_ECHO);
    fxParamListener(echoCutoffP,  FX_ECHO);
    fxParamListener(echoMixP,     FX_ECHO);
    fxParamListener(echoResonanceP, FX_ECHO);
    nodeListeners.push(echoFilterP.newListener([this](int&) {
        for(auto& [srv, st] : fx)
            if(st.synths[FX_ECHO]) sendFxParams(FX_ECHO, st.synths[FX_ECHO]);
    }));
    fxParamListener(revSizeP,     FX_REVERB);
    fxParamListener(revDecayP,    FX_REVERB);
    fxParamListener(revPredelayP, FX_REVERB);
    fxParamListener(revLowpassP,  FX_REVERB);
    fxParamListener(revMixP,      FX_REVERB);
    fxParamListener(revPositionP, FX_REVERB);
    fxParamListener(revSpreadP,   FX_REVERB);

#if OFXOCEANODESC_HAS_TRANSPORT
    nodeListeners.push(syncToTransportP.newListener([this](bool& on) {
        handleSyncChanged(on);
    }));
    nodeListeners.push(beatOffsetP.newListener([this](float& /*v*/) {
        if(!syncToTransportP.get()) return;
        // The position jumps: re-anchor every playhead hard.
        for(auto& ph : playheads) ph->needHard = true;
        follower.requestAnchor();
    }));
#endif

    initializePublishableEditorParameters();
    loadSnapshotLibrary();   // the shared snapshots (every project / preset)
}

void scGrainBoxPoly::createPlayheadParameters(Playhead& ph) {
    const std::string x = ph.sfx;
    ph.trigger.set        ("Trigger"      + x, {0},      {0},      {1});
    ph.autoTrig.set       ("AutoTrig"     + x, false);
    // Interval: in the engine's own unit (default Beats; ranges: setIntervalRange)
    ph.intervalUnit.set   ("IntervalUnit" + x, 2, 0, 3);   // Rel, ms, Beats, Pitch
    ph.intervalUnitCur = 2;
    ph.autoTrigBeatDiv.set("Interval"     + x, {defaultInterval(2)}, {1.0f / 16.0f}, {16.0f});
    ph.monoTrig.set       ("MonoTrig"     + x, false);
    ph.trigPhase.set      ("TrigPhase"    + x, {0.0f},   {0.0f},   {1.0f});
    ph.maxGrains.set      ("MaxGrains"    + x, 0, 0, 512);
    ph.chance.set         ("Chance"       + x, {1.0f},   {0.0f},   {1.0f});
    ph.uniqueTrig.set     ("UniqueTrig"   + x, false);
    ph.syncGate.set       ("SyncGate"     + x, 0, 0, 1);

    ph.amp.set            ("Amp"          + x, {1.0f},   {0.0f},   {1.0f});
    ph.pitch.set          ("Pitch"        + x, {0.0f},   {-48.0f}, {48.0f});
    ph.dynamicDur.set     ("DynamicDur"   + x, false);
    ph.trigDur.set        ("TrigDur"      + x, false);
    ph.duration.set       ("Duration"     + x, {0.1f},   {0.0f},   {1.0f});
    ph.position.set       ("Position"     + x, {0.0f},   {0.0f},   {1.0f});
    ph.panAz.set          ("PanAz"        + x, {0.0f},   {0.0f},   {2.0f});
    ph.reverse.set        ("Reverse"      + x, {0.0f},   {0.0f},   {1.0f});
    ph.scaleType.set      ("Scale"        + x, 0, 0, 11);
    ph.scaleRoot.set      ("Root"         + x, 0, 0, 11);
    ph.scaleMask.set      ("ScaleMask"    + x, 4095, 0, 4095);   // Custom: bit k = root + k allowed

    ph.uniqueJit.set      ("UniqueJit"    + x, false);
    ph.posJit.set         ("PosJit"       + x, {0.0f},   {0.0f},   {1.0f});
    ph.pitchJit.set       ("PitchJit"     + x, {0.0f},   {0.0f},   {12.0f});
    ph.durJit.set         ("DurJit"       + x, {0.0f},   {0.0f},   {1.0f});
    ph.ampJit.set         ("AmpJit"       + x, {0.0f},   {0.0f},   {1.0f});
    ph.panAzJit.set       ("PanAzJit"     + x, {0.0f},   {0.0f},   {2.0f});

    ph.inPoint.set        ("InPoint"      + x, 0.0f, 0.0f, 1.0f);
    ph.outPoint.set       ("OutPoint"     + x, 1.0f, 0.0f, 1.0f);
    ph.envAttack.set      ("EnvAttack"    + x, 0.1f, 0.0f, 1.0f);
    ph.envRelease.set     ("EnvRelease"   + x, 0.1f, 0.0f, 1.0f);
    ph.envTension.set     ("EnvTension"   + x, 0.0f, -1.0f, 1.0f);
    ph.envShape.set       ("EnvShape"     + x, 0, 0, 6);
    ph.liveLength.set     ("Live Length"  + x, 4.0f, 1.0f, 30.0f);
    ph.liveLenBeats.set   ("LiveLenBeats" + x, false);
    ph.grainless.set      ("Grainless"    + x, false);
    ph.delayJump.set      ("DelayJump"    + x, false);
    ph.overlap.set        ("Overlap"      + x, 1.0f, 0.25f, 8.0f);
    ph.delayGlide.set     ("DelayGlide"   + x, 50.0f, 0.0f, 2000.0f);
    ph.freeze.set         ("Freeze"       + x, false);
    ph.feedback.set       ("Feedback"     + x, 0.0f, 0.0f, 1.1f);
    ph.fbTone.set         ("FbTone"       + x, 0.5f, 0.0f, 1.0f);

    ph.levels.set         ("Levels"       + x, {1.0f},   {0.0f},   {1.0f});
    // 0 = follow N Chan (from the offset to the last channel)
    ph.channels.set       ("Channels"     + x, 0, 0, MAX_CHANNELS);
    ph.chanOffset.set     ("ChanOffset"   + x, 0, 0, MAX_CHANNELS - 1);
    ph.numPlayheads.set   ("Playheads"    + x, 1, 1, MAX_VOICES);
    ph.mute.set           ("Mute"         + x, false);
    ph.solo.set           ("Solo"         + x, false);
    ph.filterType.set     ("Filter"       + x, 0, 0, 5);   // 5 LP-HP (DJ)
    ph.cutoff.set         ("Cutoff"       + x, 1000.0f, 20.0f, 20000.0f);
    ph.filterQ.set        ("FilterQ"      + x, 0.707f, 0.5f, 20.0f);
    ph.latchAmp.set       ("LatchAmp"     + x, false);
    ph.latchCut.set       ("LatchCut"     + x, false);

    for(int t = 0; t < NUM_LFO; t++) {
        const std::string n = LFO_TARGET_NAMES[t];
        ph.uniqueLfo[t].set      ("UniqueLfo" + n + x, false);
        ph.lfo[t].shape.set      ("LFOShape"  + n + x, {0.0f}, {0.0f},   {5.0f});
        ph.lfo[t].speed.set      ("LFOSpeed"  + n + x, {1.0f}, {1.0f / 128.0f}, {128.0f});
        ph.lfo[t].speedRel.set   ("LFOSpdRel" + n + x, false);
        ph.lfo[t].phase.set      ("LFOPhase"  + n + x, {0.0f}, {0.0f},   {1.0f});
        ph.lfo[t].quant.set      ("LFOQuant"  + n + x, {0.0f}, {0.0f},   {32.0f});
        ph.lfo[t].strength.set   ("LFOStr"    + n + x, {0.0f}, {0.0f},   {kLfoStrMax[t]});
        ph.lfo[t].pow.set        ("LFOPow"    + n + x, {1.0f}, {0.1f},   {10.0f});
    }
    ph.envData.assign(ENV_BUFFER_SIZE, 0.0f);
}

// Exactly n values (the playhead's voice count): a shorter vector repeats, a
// longer one is cut (a longer /n_setn would spill into the next controls).
std::vector<float> scGrainBoxPoly::expandF(const vector<float>& v, int n) const {
    n = std::max(1, n);
    if((int)v.size() == n || v.empty()) return v;
    vector<float> out(n);
    for(int i = 0; i < n; i++) out[i] = v[i % (int)v.size()];
    return out;
}

std::vector<int> scGrainBoxPoly::expandI(const vector<int>& v, int n) const {
    n = std::max(1, n);
    if((int)v.size() == n || v.empty()) return v;
    vector<int> out(n);
    for(int i = 0; i < n; i++) out[i] = v[i % (int)v.size()];
    return out;
}

// Global offset added to every voice (no clipping here: the SynthDef clips
// pos / dur after adding jitter and LFO, and chance is a threshold)
std::vector<float> scGrainBoxPoly::withOffset(const vector<float>& v, int n, float off) const {
    std::vector<float> out = expandF(v, n);
    for(auto& x : out) x += off;
    return out;
}

void scGrainBoxPoly::sendOffsetParams(Playhead& ph) {
    const int nv = phVoices(ph);
    const auto pos = posSend(ph, nv);
    const auto dur = durSend(ph, nv);
    const auto chc = withOffset(ph.chance.get(), nv, gChanceP.get());
    const auto div = divSend(ph, nv);
    sendLfoSpeeds(ph);   // Smp LFO speeds follow the region
    const bool rel = (timeUnitsCur == 0);
    for(auto& [srv, s] : ph.synths) {
        if(!s) continue;
        s->set("autotrigbeatdiv", div);
        s->set("position", pos);
        s->set("duration", dur);
        s->set("chance",   chc);
        if(!rel) {
            // absolute units: every one of these depends on the region / length
            s->set("inpoint",  inRelOf(ph));
            s->set("outpoint", outRelOf(ph));
            s->set("posjit",   posAmtSend(ph, ph.posJit.get(), nv));
            s->set("durjit",   durAmtSend(ph, ph.durJit.get(), nv));
            s->set(kLfoSCStr[0], lfoStrSend(ph, 0, 1));   // engine LFOs: one value
            s->set(kLfoSCStr[1], lfoStrSend(ph, 1, 1));
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Time units
// ════════════════════════════════════════════════════════════════════════════
//
// Relative (0): the values are sent exactly as stored (today's behaviour).
// ms (1) / Beats (2), converted when sent (at the effective bpm):
//   file:  In / Out = time from the file start;  Position = time from In;
//          Duration = grain length;  jitter / LFO amounts = time amounts.
//   Live:  Position = delay behind the write head (D; the SynthDef maps raw
//          position r to D = 5 ms + r * (Len - 10 ms));  In / Out = the
//          delay range (same mapping);  Duration = grain length.
// = the SynthDef's safety gap (5 ms: a grain never reads where the recorder
// is writing; it was 30 ms, which took a visible bite off short buffers)
namespace { constexpr float kLiveGap = 0.005f; }

// Grainless (a delay line): Position 0 = now, no grain safety gap
float scGrainBoxPoly::liveGap(const Playhead& ph) const {
    return (ph.liveInput && ph.grainless.get()) ? 0.0f : kLiveGap;
}

namespace { inline float ph0LiveConv(float v, bool toBeats, float bpm) {
    return toBeats ? v * bpm / 60.0f : v * 60.0f / bpm;
} }

float scGrainBoxPoly::liveLenSec(const Playhead& ph) const {
    const float v = ph.liveLength.get();
    return ph.liveLenBeats.get() ? v * 60.0f / std::max(1.0f, effectiveBpm()) : v;
}

double scGrainBoxPoly::unitToSec(double u) const {
    switch(timeUnitsCur) {
        case 1:  return u * 0.001;
        case 2:  return u * 60.0 / std::max(1.0, (double)effectiveBpm());
        default: return u;
    }
}

double scGrainBoxPoly::secToUnit(double sec) const {
    switch(timeUnitsCur) {
        case 1:  return sec * 1000.0;
        case 2:  return sec * std::max(1.0, (double)effectiveBpm()) / 60.0;
        default: return sec;
    }
}

float scGrainBoxPoly::lengthSec(const Playhead& ph) const {
    if(ph.liveInput) return std::max(0.1f, liveLenSec(ph));
    const float d = sampleOf(ph).durationSecs;
    return d > 0.0f ? d : 1.0f;
}

float scGrainBoxPoly::inRelOf(const Playhead& ph) const {
    if(timeUnitsCur == 0) return ph.inPoint.get();
    const float L = lengthSec(ph), sec = (float)unitToSec(ph.inPoint.get());
    const float r = ph.liveInput ? (sec - liveGap(ph)) / std::max(1e-6f, L - 2.0f * liveGap(ph)) : sec / L;
    return std::max(0.0f, std::min(1.0f, r));
}

float scGrainBoxPoly::outRelOf(const Playhead& ph) const {
    if(timeUnitsCur == 0) return ph.outPoint.get();
    const float L = lengthSec(ph), sec = (float)unitToSec(ph.outPoint.get());
    const float r = ph.liveInput ? (sec - liveGap(ph)) / std::max(1e-6f, L - 2.0f * liveGap(ph)) : sec / L;
    return std::max(0.0f, std::min(1.0f, r));
}

float scGrainBoxPoly::posScaleSec(const Playhead& ph) const {
    const float L = lengthSec(ph), span = std::max(1e-6f, outRelOf(ph) - inRelOf(ph));
    return std::max(1e-6f, span * (ph.liveInput ? std::max(1e-6f, L - 2.0f * liveGap(ph)) : L));
}

float scGrainBoxPoly::posOffsetSec(const Playhead& ph) const {
    if(!ph.liveInput) return 0.0f;
    return liveGap(ph) + inRelOf(ph) * std::max(1e-6f, lengthSec(ph) - 2.0f * liveGap(ph));
}

float scGrainBoxPoly::durScaleSec(const Playhead& ph) const {
    return std::max(1e-6f, std::max(1e-6f, outRelOf(ph) - inRelOf(ph)) * lengthSec(ph));
}

std::vector<float> scGrainBoxPoly::posSend(const Playhead& ph, int n) const {
    if(timeUnitsCur == 0) return withOffset(ph.position.get(), n, gPosP.get());
    std::vector<float> out = expandF(ph.position.get(), n);
    const float sc = posScaleSec(ph), off = posOffsetSec(ph);
    const float g  = (float)unitToSec(gPosP.get()) / sc;
    for(auto& x : out) x = ((float)unitToSec(x) - off) / sc + g;
    return out;
}

std::vector<float> scGrainBoxPoly::durSend(const Playhead& ph, int n) const {
    if(timeUnitsCur == 0) return withOffset(ph.duration.get(), n, gDurP.get());
    std::vector<float> out = expandF(ph.duration.get(), n);
    const float sc = durScaleSec(ph), g = (float)unitToSec(gDurP.get());
    for(auto& x : out) x = ((float)unitToSec(x) + g) / sc;
    return out;
}

std::vector<float> scGrainBoxPoly::posAmtSend(const Playhead& ph, const vector<float>& v, int n) const {
    std::vector<float> out = expandF(v, n);
    if(timeUnitsCur == 0) return out;
    const float sc = posScaleSec(ph);
    for(auto& x : out) x = (float)unitToSec(x) / sc;
    return out;
}

std::vector<float> scGrainBoxPoly::durAmtSend(const Playhead& ph, const vector<float>& v, int n) const {
    std::vector<float> out = expandF(v, n);
    if(timeUnitsCur == 0) return out;
    const float sc = durScaleSec(ph);
    for(auto& x : out) x = (float)unitToSec(x) / sc;
    return out;
}

std::vector<float> scGrainBoxPoly::lfoStrSend(const Playhead& ph, int t, int n) const {
    if(t == 0) return posAmtSend(ph, ph.lfo[0].strength.get(), n);   // Pos
    if(t == 1) return durAmtSend(ph, ph.lfo[1].strength.get(), n);   // Dur
    return expandF(ph.lfo[t].strength.get(), n);
}

// Trigger interval, in the engine's own unit (not the global Time Units):
// 0 Relative = fraction of the region, 1 ms, 2 Beats, 3 Pitch (MIDI note:
// the trigger rate is that note's frequency, the interval its period)
double scGrainBoxPoly::intervalUnitToSec(const Playhead& ph, int unit, double v) const {
    const double beat = 60.0 / std::max(1.0, (double)effectiveBpm());
    if(unit == 3) return 1.0 / (440.0 * std::pow(2.0, (v - 69.0) / 12.0));
    return unit == 1 ? v * 0.001 : unit == 2 ? v * beat : v * durScaleSec(ph);
}

double scGrainBoxPoly::intervalSecToUnit(const Playhead& ph, int unit, double sec) const {
    const double beat = 60.0 / std::max(1.0, (double)effectiveBpm());
    if(unit == 3) return 69.0 + 12.0 * std::log2(1.0 / (std::max(1e-6, sec) * 440.0));
    return unit == 1 ? sec * 1000.0 : unit == 2 ? sec / beat : sec / std::max(1e-6, (double)durScaleSec(ph));
}

float scGrainBoxPoly::intervalSec(const Playhead& ph, float v) const {
    return (float)intervalUnitToSec(ph, ph.intervalUnitCur, v);
}

// 1/1000 .. all of the region, 10 ms .. 16 s, 64th .. 4 bars
void scGrainBoxPoly::setIntervalRange(Playhead& ph) {
    const int u = ph.intervalUnitCur;
    // Pitch: MIDI 0 (8 Hz) .. 96 (C7, 2093 Hz)
    ph.autoTrigBeatDiv.setMin(vector<float>{u == 1 ? 10.0f    : u == 2 ? 1.0f / 16.0f : u == 3 ? 0.0f  : 0.001f});
    ph.autoTrigBeatDiv.setMax(vector<float>{u == 1 ? 16000.0f : u == 2 ? 16.0f        : u == 3 ? 96.0f : 1.0f});
}

// The SynthDef's clock counts grains per beat: beat length / interval
// (Beats: 1 / interval, so the tempo never needs a re-send)
std::vector<float> scGrainBoxPoly::divSend(const Playhead& ph, int n) const {
    std::vector<float> out = expandF(ph.autoTrigBeatDiv.get(), n);
    const double beat = 60.0 / std::max(1.0, (double)effectiveBpm());
    for(auto& x : out)
        // up to 4096 grains per beat: audio-rate triggers (Pitch unit)
        x = (float)std::max(1.0 / 64.0, std::min(4096.0, beat / std::max(1e-5, (double)intervalSec(ph, x))));
    return out;
}

void scGrainBoxPoly::sendDiv(Playhead& ph) {
    const auto div = divSend(ph, phVoices(ph));
    for(auto& [srv, s] : ph.synths) if(s) s->set("autotrigbeatdiv", div);
    sendLfoSpeeds(ph);   // Smp LFO speeds depend on the same tempo / region
}

// Bar mode: Speed = cycles per bar. Smp mode: Speed = cycles per region
// length (x1 = one cycle while the region plays at its own speed, 1/8 = an
// eighth as fast), converted to cycles per bar at the current tempo.
double scGrainBoxPoly::lfoSpeedBar(const Playhead& ph, int t) const {
    const auto& sp = ph.lfo[t].speed.get();
    const double v = sp.empty() ? 1.0 : (double)sp[0];
    if(!ph.lfo[t].speedRel.get()) return v;
    const double barSec = 240.0 / std::max(1.0, (double)effectiveBpm());
    // Pos: the span the LFO really sweeps = its Length (Str, of the region;
    // Live: of the delay range, without the safety gaps), so x1 moves the read
    // point exactly as fast as time passes whatever the Length (Live: it holds
    // still against the recording, then jumps back by Length -> clean stutters
    // of any size)
    double span = (double)durScaleSec(ph);
    if(t == 0) {
        const auto st = lfoStrSend(ph, 0, 1);   // relative to the region
        const double len = st.empty() ? 1.0 : std::max(1e-3, (double)std::abs(st[0]));
        span = (double)posScaleSec(ph) * len;
    }
    return v * barSec / std::max(1e-3, span);
}

void scGrainBoxPoly::sendLfoSpeeds(Playhead& ph) {
    for(auto& [srv, s] : ph.synths) {
        if(!s) continue;
        const bool filtered = synthIsFiltered(s);
        for(int t = 0; t < NUM_LFO; t++)
            if(t != LFO_CUT || filtered) s->set(kLfoSCSpeed[t], vector<float>{(float)lfoSpeedBar(ph, t)});
    }
}

float scGrainBoxPoly::defaultInterval(int unit) const {
    return unit == 1 ? 500.0f : unit == 2 ? 1.0f : unit == 3 ? 36.0f : 0.25f;   // Pitch: C2 (65 Hz)
}

const char* scGrainBoxPoly::timeFmt() const {
    return timeUnitsCur == 1 ? "%.0f ms" : timeUnitsCur == 2 ? "%.2f b" : "%.3f";
}

// Waveform x (0..1 of the view's full width) of a relative position: files as
// is; Live shows age (0 = now), raw r sits at D = gap + r * (Len - 2 gap)
float scGrainBoxPoly::relToDisplay(const Playhead& ph, float rel) const {
    if(!ph.liveInput) return rel;
    const float L = lengthSec(ph);
    return (liveGap(ph) + rel * std::max(1e-6f, L - 2.0f * liveGap(ph))) / L;
}

float scGrainBoxPoly::displayToRel(const Playhead& ph, float x) const {
    if(!ph.liveInput) return x;
    const float L = lengthSec(ph);
    return std::max(0.0f, std::min(1.0f, (x * L - liveGap(ph)) / std::max(1e-6f, L - 2.0f * liveGap(ph))));
}

// Parameter ranges of the current unit: 0 .. the playhead's length (file or
// Live Len), rounded UP to 1 / 2 / 5 x 10^k so they stay stable when the
// length changes a little; the global offsets: +- the largest of them.
// Values are never clamped by a range change (connections are not clipped).
void scGrainBoxPoly::updateTimeRanges() {
    auto nice = [](double x) {
        if(x <= 0.0) return 1.0;
        const double p = std::pow(10.0, std::floor(std::log10(x)));
        for(double m : {1.0, 2.0, 5.0, 10.0}) if(m * p >= x * (1.0 - 1e-9)) return m * p;
        return 10.0 * p;
    };
    float gMax = 1.0f;
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        float hi = 1.0f;
        if(timeUnitsCur != 0) hi = (float)nice(secToUnit(lengthSec(ph)));
        gMax = std::max(gMax, hi);
        ph.inPoint.setMin(0.0f);  ph.inPoint.setMax(hi);
        ph.outPoint.setMin(0.0f); ph.outPoint.setMax(hi);
        for(auto* v : {&ph.position, &ph.duration, &ph.posJit, &ph.durJit,
                       &ph.lfo[0].strength, &ph.lfo[1].strength}) {
            v->setMin(vector<float>{0.0f});
            v->setMax(vector<float>{hi});
        }
        ph.intervalUnitCur = std::max(0, std::min(3, ph.intervalUnit.get()));
        setIntervalRange(ph);
    }
    gPosP.setMin(-gMax); gPosP.setMax(gMax);
    gDurP.setMin(-gMax); gDurP.setMax(gMax);
}

// Switch unit without a jump: every affected value -> relative (old unit) ->
// new unit. Relative in/out and the scales do not depend on the unit.
void scGrainBoxPoly::convertTimeUnits(int from, int to) {
    if(from == to) return;
    const double bpm = std::max(1.0, (double)effectiveBpm());
    auto secOf  = [bpm](int m, double v) { return m == 1 ? v * 0.001 : m == 2 ? v * 60.0 / bpm : v; };
    auto unitOf = [bpm](int m, double sec) { return m == 1 ? sec * 1000.0 : m == 2 ? sec * bpm / 60.0 : sec; };

    struct Conv {
        Playhead* ph; float inR, outR, L, ps, po, ds;
    };
    std::vector<Conv> cv;
    timeUnitsCur = from;
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        cv.push_back({&ph, inRelOf(ph), outRelOf(ph), lengthSec(ph),
                      posScaleSec(ph), posOffsetSec(ph), durScaleSec(ph)});
    }
    const Conv c0 = cv.empty() ? Conv{nullptr, 0, 1, 1, 1, 0, 1} : cv[0];
    const float gPosRel = from == 0 ? gPosP.get() : (float)(secOf(from, gPosP.get()) / c0.ps);
    const float gDurRel = from == 0 ? gDurP.get() : (float)(secOf(from, gDurP.get()) / c0.ds);

    struct NewVals {
        float in, out;
        vector<float> pos, dur, pj, dj, l0, l1;
    };
    std::vector<NewVals> nv;
    for(auto& c : cv) {
        Playhead& ph = *c.ph;
        auto regionUnit = [&](float rel) -> float {
            if(to == 0) return rel;
            const double sec = ph.liveInput ? liveGap(ph) + rel * std::max(1e-6f, c.L - 2.0f * liveGap(ph)) : rel * c.L;
            return (float)unitOf(to, sec);
        };
        auto convPos = [&](vector<float> v) {
            for(auto& x : v) {
                const double raw = from == 0 ? x : (secOf(from, x) - c.po) / c.ps;
                x = to == 0 ? (float)raw : (float)unitOf(to, c.po + raw * c.ps);
            }
            return v;
        };
        auto convAmt = [&](vector<float> v, float scale) {
            for(auto& x : v) {
                const double raw = from == 0 ? x : secOf(from, x) / scale;
                x = to == 0 ? (float)raw : (float)unitOf(to, raw * scale);
            }
            return v;
        };
        nv.push_back({regionUnit(c.inR), regionUnit(c.outR),
                      convPos(ph.position.get()), convAmt(ph.duration.get(), c.ds),
                      convAmt(ph.posJit.get(), c.ps), convAmt(ph.durJit.get(), c.ds),
                      convAmt(ph.lfo[0].strength.get(), c.ps), convAmt(ph.lfo[1].strength.get(), c.ds)});
    }

    timeUnitsCur = to;
    updateTimeRanges();
    for(size_t k = 0; k < cv.size(); k++) {
        Playhead& ph = *cv[k].ph;
        ph.inPoint.set(nv[k].in);
        ph.outPoint.set(nv[k].out);
        ph.position.set(nv[k].pos);
        ph.duration.set(nv[k].dur);
        ph.posJit.set(nv[k].pj);
        ph.durJit.set(nv[k].dj);
        ph.lfo[0].strength.set(nv[k].l0);
        ph.lfo[1].strength.set(nv[k].l1);
    }
    gPosP.set(to == 0 ? gPosRel : (float)unitOf(to, gPosRel * c0.ps));
    gDurP.set(to == 0 ? gDurRel : (float)unitOf(to, gDurRel * c0.ds));
    for(auto& ph : playheads) sendOffsetParams(*ph);   // one consistent state
    lastTimeBpm = effectiveBpm();
}

// ── Per-playhead output channels ─────────────────────────────────────────────
// Offset: first output channel (0..N-1). Channels: 0 follows N Chan (all
// channels from the offset on), otherwise clamped so offset + channels <= N.
int scGrainBoxPoly::phOffset(const Playhead& ph) const {
    const int N = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    return std::max(0, std::min(N - 1, ph.chanOffset.get()));
}

int scGrainBoxPoly::phChannels(const Playhead& ph) const {
    const int N   = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    const int off = phOffset(ph);
    int c = ph.channels.get();
    if(c <= 0) c = N - off;
    return std::max(1, std::min(N - off, c));
}

// ════════════════════════════════════════════════════════════════════════════
// Engine = control synth + one voice synth per playhead + output synth
// ════════════════════════════════════════════════════════════════════════════

namespace {
bool gbpCtlBusIsLive(ofxSCServer* srv, ofxSCBus* b) {
    if(!srv || !b || b->index < 0 || b->index >= (int)srv->controlBusses.size()) return false;
    return srv->controlBusses[b->index] == b;
}
} // namespace

int scGrainBoxPoly::phVoices(const Playhead& ph) const {
    return std::max(1, std::min(MAX_VOICES, ph.numPlayheads.get()));
}

// The control synth handles every slot of its capacity, so it is sized to the
// playhead count (one playhead costs as little as before)
int scGrainBoxPoly::ctlCap(const Playhead& ph) const {
    const int n = phVoices(ph);
    for(int c : {8, 32, 128}) if(n <= c) return c;
    return MAX_VOICES;
}

int scGrainBoxPoly::ctlBusSize(const Playhead& ph) const {
    return 80 + 15 * ctlCap(ph);
}

// Playheads crossed a capacity (8 / 32 / 128 / 500): a control synth of the
// new size on a new bus, replacing the old one in place; the voices and the
// output synth read the new bus. (The engine LFOs and free clock restart.)
void scGrainBoxPoly::replaceCtl(Playhead& ph, ofxSCServer* srv) {
    if(!ph.synths.count(srv) || !ph.synths[srv]) return;
    ofxSCSynth* old = ph.synths[srv];
    if(old->getName() == defNameFor(ph)) return;
    const int oldID = old->nodeID;
    delete old;                              // no /n_free: the replace frees it
    ensureEngineBuses(ph, srv);              // re-sized ctl bus
    auto* s = new ofxSCSynth(defNameFor(ph), srv);
    ph.synths[srv] = s;
    configureSynth(ph, s, srv);              // every control, incl. ctl / clk
    s->set("active", getActive() ? 1.0f : 0.0f);
    s->createAndRun(4, oldID, getActive() && !ph.isPaused);
    const int bus = ph.ebus[srv].ctl->index;
    for(auto* v : ph.voices[srv]) if(v) v->set("ctl", bus);
    if(ph.outSynths.count(srv) && ph.outSynths[srv]) ph.outSynths[srv]->set("ctl", bus);
#if OFXOCEANODESC_HAS_TRANSPORT
    ph.needHard = true;                      // re-anchor the new clock
    if(syncToTransportP.get()) follower.requestAnchor();
#endif
}

// GrainBoxPolyV_K_N: K = channels of the engine's sample, N = its speakers
std::string scGrainBoxPoly::voiceDefFor(const Playhead& ph) const {
    const int k = std::max(1, std::min(MAX_SAMPLE_CHANNELS, sampleOf(ph).numChannels));
    bool sync = false;
#if OFXOCEANODESC_HAS_TRANSPORT
    sync = syncToTransportP.get();
#endif
    // Live, Grainless: a delay line per playhead
    if(ph.liveInput && ph.grainless.get())
        return std::string(ph.delayJump.get() ? "GrainBoxPolyDJ_" : "GrainBoxPolyD_") + ofToString(phChannels(ph));
    // transport grid / Mono trigger: their own variants, so the others don't pay for them
    return std::string(sync ? "GrainBoxPolyVS" : "GrainBoxPolyV") + (ph.monoTrig.get() ? "M" : "") + "_"
           + ofToString(k) + "_" + ofToString(phChannels(ph));
}

std::string scGrainBoxPoly::outDefFor(const Playhead& ph) const {
    return "GrainBoxPolyOut_" + ofToString(phChannels(ph));
}

// Buses are allocated client-side only (no server message), so they are kept
// across graph recomputes; stale ones (server reboot) are replaced.
void scGrainBoxPoly::ensureEngineBuses(Playhead& ph, ofxSCServer* srv) {
    if(!srv) return;
    auto& b = ph.ebus[srv];
    if(b.ctl && !gbpCtlBusIsLive(srv, b.ctl)) { delete b.ctl; b.ctl = nullptr; }
    if(b.ctl && b.ctl->channels != ctlBusSize(ph)) { b.ctl->free(); delete b.ctl; b.ctl = nullptr; }
    if(b.clk && !gbpBusIsLive(srv, b.clk))    { delete b.clk; b.clk = nullptr; }
    if(b.mix && !gbpBusIsLive(srv, b.mix))    { delete b.mix; b.mix = nullptr; }
    if(!b.ctl) {
        b.ctl = new ofxSCBus(RATE_CONTROL, ctlBusSize(ph), srv);
        if(b.ctl->index + b.ctl->channels > 16384)
            ofLogWarning("scGrainBoxPoly") << "engine " << (ph.index + 1) << " uses control buses up to "
                << (b.ctl->index + b.ctl->channels) << ": raise the server's Control Busses above that "
                << "(Supercollider server settings) if this engine is silent";
    }
    if(!b.clk) b.clk = new ofxSCBus(RATE_AUDIO, 7, srv);
    if(!b.mix) b.mix = new ofxSCBus(RATE_AUDIO, MAX_CHANNELS, srv);
}

void scGrainBoxPoly::freeEngineBuses(Playhead& ph, ofxSCServer* srv) {
    auto it = ph.ebus.find(srv);
    if(it == ph.ebus.end()) return;
    auto& b = it->second;
    if(b.ctl) { if(gbpCtlBusIsLive(srv, b.ctl)) b.ctl->free(); delete b.ctl; }
    if(b.clk) { if(gbpBusIsLive(srv, b.clk))    b.clk->free(); delete b.clk; }
    if(b.mix) { if(gbpBusIsLive(srv, b.mix))    b.mix->free(); delete b.mix; }
    ph.ebus.erase(it);
}

// One playhead's voice synth (not running yet), with its /grainTrig listener
ofxSCSynth* scGrainBoxPoly::newVoiceSynth(Playhead& ph, ofxSCServer* srv, int i) {
    ensureEngineBuses(ph, srv);
    auto& b = ph.ebus[srv];
    auto* v = new ofxSCSynth(voiceDefFor(ph), srv);
    v->set("out", b.mix->index);
    v->set("ctl", b.ctl->index);
    v->set("clk", b.clk->index);
    v->set("idx", i);
    Playhead* p = &ph;
    // /grainTrig [nodeID, playhead, pos (0..1), dur (s), amp (0..1)]
    auto& ls = ph.voiceListeners[srv];
    if((int)ls.size() <= i) ls.resize(i + 1);
    ls[i] = std::make_unique<ofEventListener>(v->newFeedbackMessage.newListener(
        [this, p, v](ofxOscMessage& msg) {
            if(msg.getAddress() != "/grainTrig" || msg.getNumArgs() < 5) return;
            if(msg.getArgAsInt(0) != v->nodeID) return;
            GrainHighlight gh;
            gh.channel  = msg.getArgAsInt(1);                 // playhead index
            gh.position = msg.getArgAsFloat(2);
            gh.duration = msg.getArgAsFloat(3);
            gh.amp      = std::max(0.0f, msg.getArgAsFloat(4));
            gh.playhead = p->index;
            gh.maxLife  = std::max(gh.duration, 0.033f);
            gh.lifeTime = gh.maxLife;
            grainHighlights.push_back(gh);
        }));
    return v;
}

// Voices + output synth objects (args queued; created by runEngineChain)
void scGrainBoxPoly::buildEngineObjects(Playhead& ph, ofxSCServer* srv) {
    freeEngineVoices(ph, srv);
    ensureEngineBuses(ph, srv);
    auto& b  = ph.ebus[srv];
    auto& vs = ph.voices[srv];
    for(int i = 0; i < phVoices(ph); i++) vs.push_back(newVoiceSynth(ph, srv, i));
    auto* o = new ofxSCSynth(outDefFor(ph), srv);
    o->set("in",  b.mix->index);
    o->set("ctl", b.ctl->index);
    o->set("out", outBusFor(ph, srv));
    o->set("fbbus", fbBusFor(ph, srv));
    ph.outSynths[srv] = o;
}

// Voices right after the control synth, the output synth after the voices
void scGrainBoxPoly::runEngineChain(Playhead& ph, ofxSCServer* srv, bool run) {
    if(!ph.synths.count(srv) || !ph.synths[srv]) return;
    ofxSCSynth* prev = ph.synths[srv];
    for(auto* v : ph.voices[srv]) if(v) { v->createAndRun(3, prev->nodeID, run); prev = v; }
    auto o = ph.outSynths.find(srv);
    if(o != ph.outSynths.end() && o->second) o->second->createAndRun(3, prev->nodeID, run);
    if(ph.index == 0) createDry(srv);   // Dry/Wet's dry path, after engine 1
}

void scGrainBoxPoly::freeEngineVoices(Playhead& ph, ofxSCServer* srv) {
    if(ph.index == 0) freeDry(srv);
    ph.voiceListeners.erase(srv);
    auto it = ph.voices.find(srv);
    if(it != ph.voices.end()) {
        for(auto* v : it->second) if(v) { v->free(); delete v; }
        ph.voices.erase(it);
    }
    auto o = ph.outSynths.find(srv);
    if(o != ph.outSynths.end()) {
        if(o->second) { o->second->free(); delete o->second; }
        ph.outSynths.erase(o);
    }
}

// Playheads changed while the engine runs: new voices before the output
// synth, extra ones freed (the highest indices go first)
void scGrainBoxPoly::syncVoiceCount(Playhead& ph, ofxSCServer* srv) {
    if(!ph.synths.count(srv) || !ph.synths[srv]) return;
    auto& vs = ph.voices[srv];
    auto& ls = ph.voiceListeners[srv];
    const int want = phVoices(ph);
    while((int)vs.size() > want) {
        const int i = (int)vs.size() - 1;
        if(i < (int)ls.size()) ls[i].reset();
        if(vs[i]) { vs[i]->free(); delete vs[i]; }
        vs.pop_back();
    }
    const bool run = getActive() && !ph.isPaused;
    while((int)vs.size() < want) {
        const int i = (int)vs.size();
        auto* v = newVoiceSynth(ph, srv, i);
        auto o = ph.outSynths.find(srv);
        if(o != ph.outSynths.end() && o->second) v->createAndRun(2, o->second->nodeID, run);   // addBefore out
        else v->createAndRun(3, (vs.empty() ? ph.synths[srv] : vs.back())->nodeID, run);
        vs.push_back(v);
    }
}

// The engine's latest synth in SC's order: output, else last voice, else control
ofxSCSynth* scGrainBoxPoly::engineLast(Playhead& ph, ofxSCServer* srv) {
    auto o = ph.outSynths.find(srv);
    if(o != ph.outSynths.end() && o->second) return o->second;
    auto v = ph.voices.find(srv);
    if(v != ph.voices.end() && !v->second.empty() && v->second.back()) return v->second.back();
    auto c = ph.synths.find(srv);
    return (c != ph.synths.end()) ? c->second : nullptr;
}

void scGrainBoxPoly::forEngineSynth(Playhead& ph, const std::function<void(ofxSCSynth*)>& f) {
    for(auto& [srv, s] : ph.synths) if(s) f(s);
    for(auto& [srv, vs] : ph.voices) for(auto* v : vs) if(v) f(v);
    for(auto& [srv, o] : ph.outSynths) if(o) f(o);
}

// The node's output bus, or the FX chain's input bus when a stage is planned.
int scGrainBoxPoly::outBusFor(const Playhead& ph, ofxSCServer* srv) const {
    auto f = fx.find(srv);
    if(f != fx.end() && !f->second.plan.empty() && !f->second.buses.empty() && f->second.buses[0])
        return f->second.buses[0]->index + phOffset(ph);
    auto it = outputBuses.find(srv);
    return (it != outputBuses.end() ? it->second : 0) + phOffset(ph);
}

scGrainBoxPoly::SampleData& scGrainBoxPoly::sampleOf(Playhead& ph) {
    if(ph.liveInput) return ph.live;
    return (ph.index > 0 && ph.ownSample) ? ph.own : mainSample;
}

const scGrainBoxPoly::SampleData& scGrainBoxPoly::sampleOf(const Playhead& ph) const {
    if(ph.liveInput) return ph.live;
    return (ph.index > 0 && ph.ownSample) ? ph.own : mainSample;
}

// False for a buffer object whose index the server no longer maps to it: the
// server rebooted (allocators reset) and the index may belong to someone else,
// so it must neither be used nor /b_free'd.
bool scGrainBoxPoly::bufferIsLive(ofxSCServer* srv, ofxSCBuffer* b) {
    if(!srv || !b || b->index < 0 || b->index >= (int)srv->buffers.size()) return false;
    return srv->buffers[b->index] == b;
}

// Per-voice trigger counters: +1 on each rising edge of the Trigger input.
// SC fires on any increase, so a 1-then-0 pair landing in the same control
// block (which a plain 0/1 value would lose) still fires. Old SynthDefs, which
// fire on a rising value, behave the same with counters.
std::vector<int> scGrainBoxPoly::triggerCountsFor(Playhead& ph) {
    const int n = phVoices(ph);
    const auto& v = ph.trigger.get();
    const int sz = n;   // exactly one value per voice (the control is n wide)
    if((int)ph.triggerCounts.size() < sz) ph.triggerCounts.resize(sz, 0);
    if((int)ph.lastTrigger.size()   < sz) ph.lastTrigger.resize(sz, 0);
    for(int i = 0; i < sz; i++) {
        const int val = v.empty() ? 0 : v[i % (int)v.size()];
        if(val > 0 && ph.lastTrigger[i] <= 0) ph.triggerCounts[i]++;
        ph.lastTrigger[i] = val;
    }
    return std::vector<int>(ph.triggerCounts.begin(), ph.triggerCounts.begin() + sz);
}

void scGrainBoxPoly::setupPlayheadListeners(Playhead& ph) {
    Playhead* p = &ph;

    // Envelope params → recompute and upload
    auto envListener = [this, p](float&) {
        computeEnvData(*p);
        p->envNeedsUpdate = true;
    };
    ph.listeners.push(ph.envAttack.newListener(envListener));
    ph.listeners.push(ph.envRelease.newListener(envListener));
    ph.listeners.push(ph.envTension.newListener(envListener));

    // Scalar synth params
    // filteredOnly: filter / Cut-LFO controls (every GrainBox_K_N has them now)
    auto boolListener = [this, p](ofParameter<bool>& param, const std::string& name, bool filteredOnly = false) {
        p->listeners.push(param.newListener([p, name, filteredOnly](bool& v) {
            for(auto& [srv, s] : p->synths)
                if(s && (!filteredOnly || synthIsFiltered(s))) s->set(name, (int)v);
        }));
    };
    boolListener(ph.autoTrig,   "autotrig");
    boolListener(ph.dynamicDur, "dynamicdur");
    boolListener(ph.trigDur,    "trigdur");
    boolListener(ph.uniqueTrig, "uniquetrig");
    boolListener(ph.uniqueJit,  "uniquejit");
    for(int t = 0; t < NUM_LFO; t++) boolListener(ph.uniqueLfo[t], kLfoSCUnique[t], t == LFO_CUT);
    boolListener(ph.latchAmp,   "latchamp");

    // Mute / solo: every playhead's gain can change (solo)
    ph.listeners.push(ph.mute.newListener([this](bool&) { sendMixParams(); }));
    ph.listeners.push(ph.solo.newListener([this](bool&) { sendMixParams(); }));

    // Filter: an output filter inside the same synth, crossfaded on/off in
    // SC over 20 ms (no synth replacement, so no cut grains / clicks)
    ph.listeners.push(ph.filterType.newListener([this, p](int&) {
        for(auto& [srv, s] : p->synths) if(s && synthIsFiltered(s)) setFilterArgs(*p, s);
    }));
    ph.listeners.push(ph.cutoff.newListener([p](float& v) {
        for(auto& [srv, s] : p->synths) if(s && synthIsFiltered(s)) s->set("cutoff", v);
    }));
    ph.listeners.push(ph.filterQ.newListener([p](float& v) {
        for(auto& [srv, s] : p->synths) if(s && synthIsFiltered(s)) s->set("filterq", v);
    }));

    // In / Out: relative (as always) or, in ms / Beats, converted with every
    // other value that depends on the region (sendOffsetParams)
    auto regionListener = [this, p](float& v, const char* name) {
        if(timeUnitsCur == 0) { for(auto& [srv, s] : p->synths) if(s) s->set(name, v); sendDiv(*p); }
        else                  sendOffsetParams(*p);
        if(p->index == 0)
            sampleMsP.set(std::max(0.0f, (outRelOf(*p) - inRelOf(*p)) * mainSample.durationSecs * 1000.0f));
    };
    ph.listeners.push(ph.inPoint.newListener([regionListener](float& v)  { regionListener(v, "inpoint"); }));
    ph.listeners.push(ph.outPoint.newListener([regionListener](float& v) { regionListener(v, "outpoint"); }));

    // Output channels / offset → another SynthDef variant or output bus
    ph.listeners.push(ph.channels.newListener([this, p](int&) { refreshPlayheadSynths(*p); }));
    ph.listeners.push(ph.chanOffset.newListener([this, p](int&) { refreshPlayheadSynths(*p); }));
    // Playheads: start / free voice synths, then re-send every vector at the
    // new size (playhead i reads index i)
    ph.listeners.push(ph.numPlayheads.newListener([this, p](int&) {
        for(auto* sm : allServers)
            if(sm && sm->getServer()) {
                replaceCtl(*p, sm->getServer());       // only when the capacity changes
                syncVoiceCount(*p, sm->getServer());
            }
        sendAllParams(*p);
        if(p->trigger.get().size() > 1) p->trigger.set(vector<int>(phVoices(*p), 0));
    }));

    // Vector params — expand to the playhead's voice count so a scalar input reaches every voice
    auto vfListener = [this, p](const std::string& name, ofParameter<vector<float>>& param, bool filteredOnly = false) {
        p->listeners.push(param.newListener([this, p, name, filteredOnly](vector<float>& v) {
            auto ev = expandF(v, name == "levels" ? phChannels(*p) : name.rfind("lfo", 0) == 0 ? 1 : phVoices(*p));
            for(auto& [srv, s] : p->synths)
                if(s && (!filteredOnly || synthIsFiltered(s))) s->set(name, ev);
        }));
    };
    vfListener("amp",              ph.amp);
    vfListener("pitch",            ph.pitch);
    // position / duration / chance: + the global offsets (sendOffsetParams)
    ph.listeners.push(ph.duration.newListener([this, p](vector<float>&) { sendOffsetParams(*p); }));
    ph.listeners.push(ph.position.newListener([this, p](vector<float>&) { sendOffsetParams(*p); }));
    ph.listeners.push(ph.chance.newListener([this, p](vector<float>&)   { sendOffsetParams(*p); }));
    vfListener("panaz",            ph.panAz);
    vfListener("reverse",          ph.reverse);
    // Scale / root / custom mask → scaleon, scaleroot, scalemask
    auto scaleListener = [this, p](int&) { for(auto& [srv, s] : p->synths) setScaleArgs(*p, s); };
    ph.listeners.push(ph.scaleType.newListener(scaleListener));
    ph.listeners.push(ph.scaleRoot.newListener(scaleListener));
    ph.listeners.push(ph.scaleMask.newListener(scaleListener));
    // Envelope shape → recompute + upload (like attack / release / tension)
    ph.listeners.push(ph.envShape.newListener([this, p](int&) {
        computeEnvData(*p);
        p->envNeedsUpdate = true;
    }));
    // Live input
    // (debounced: a drag would otherwise re-allocate every frame; see update)
    ph.listeners.push(ph.liveLength.newListener([p](float&) { p->liveResizeAt = ofGetElapsedTimef() + 0.3f; }));
    ph.listeners.push(ph.freeze.newListener([p](bool& v) {
        for(auto& [srv, l] : p->liveSC) if(l.rec) l.rec->set("freeze", v ? 1.0f : 0.0f);
    }));
    ph.listeners.push(ph.feedback.newListener([p](float& v) {
        for(auto& [srv, l] : p->liveSC) if(l.rec) l.rec->set("fbamt", std::max(0.0f, v));
    }));
    ph.listeners.push(ph.fbTone.newListener([p](float& v) {
        for(auto& [srv, l] : p->liveSC) if(l.rec) l.rec->set("fbtone", v);
    }));
    ph.listeners.push(ph.autoTrigBeatDiv.newListener([this, p](vector<float>&) { sendDiv(*p); }));
    vfListener("trigphase",        ph.trigPhase);
    // Interval unit: the values are converted (same time, new unit); a
    // preset's values already arrive in its unit
    ph.listeners.push(ph.intervalUnit.newListener([this, p](int& m) {
        const int to = std::max(0, std::min(3, m)), from = p->intervalUnitCur;
        if(to == from) return;
        p->intervalUnitCur = to;
        setIntervalRange(*p);
        if(loadingPreset) return;
        vector<float> v = p->autoTrigBeatDiv.get();
        for(auto& x : v) {
            x = (float)intervalSecToUnit(*p, to, intervalUnitToSec(*p, from, x));
            if(to == 3) x = std::max(0.0f, std::min(96.0f, x));   // a pitch outside 0..96 is no use
        }
        p->autoTrigBeatDiv.set(v);   // -> sendDiv
    }));
    ph.listeners.push(ph.monoTrig.newListener([this, p](bool& on) {
        for(auto& [srv, s] : p->synths) if(s) s->set("monotrig", on ? 1.0f : 0.0f);
        refreshPlayheadSynths(*p);   // GrainBoxPolyV(S)M_K_N
    }));
    // Grainless: Glide / Jump variant, glide or crossfade time
    ph.listeners.push(ph.delayJump.newListener([this, p](bool&) { refreshPlayheadSynths(*p); }));
    ph.listeners.push(ph.overlap.newListener([p](float& v) {
        for(auto& [srv, s] : p->synths) if(s) s->set("overlap", std::max(0.01f, v));
    }));
    ph.listeners.push(ph.delayGlide.newListener([p](float& v) {
        for(auto& [srv, s] : p->synths) if(s) s->set("delayglide", std::max(0.0f, v) * 0.001f);
    }));
    // Grainless (Live): delay-line voices; Position loses the grain gap
    ph.listeners.push(ph.grainless.newListener([this, p](bool&) {
        refreshPlayheadSynths(*p);
        timeDirty = true;
        sendOffsetParams(*p);
    }));
    // Live Length unit: same length, other unit (seconds 1..30 / beats 1..64)
    ph.listeners.push(ph.liveLenBeats.newListener([this, p](bool& b) {
        const float bpm = std::max(1.0f, effectiveBpm());
        const float v = ph0LiveConv(p->liveLength.get(), b, bpm);
        p->liveLength.setMin(1.0f);
        p->liveLength.setMax(b ? 64.0f : 30.0f);
        if(!loadingPreset) p->liveLength.set(std::max(1.0f, std::min(b ? 64.0f : 30.0f, v)));
        p->liveResizeAt = ofGetElapsedTimef() + 0.3f;
    }));
    ph.listeners.push(ph.maxGrains.newListener([p](int& v) {
        for(auto& [srv, s] : p->synths) if(s) s->set("maxoverlap", (float)std::max(0, v));
    }));
    // posjit / durjit: time amounts in ms / Beats
    ph.listeners.push(ph.posJit.newListener([this, p](vector<float>& v) {
        const auto ev = posAmtSend(*p, v, phVoices(*p));
        for(auto& [srv, s] : p->synths) if(s) s->set("posjit", ev);
    }));
    vfListener("pitchjit",         ph.pitchJit);
    ph.listeners.push(ph.durJit.newListener([this, p](vector<float>& v) {
        const auto ev = durAmtSend(*p, v, phVoices(*p));
        for(auto& [srv, s] : p->synths) if(s) s->set("durjit", ev);
    }));
    vfListener("ampjit",           ph.ampJit);
    vfListener("panazjit",         ph.panAzJit);
    vfListener("levels",           ph.levels);
    for(int t = 0; t < NUM_LFO; t++) {
        const bool fo = (t == LFO_CUT);
        vfListener(kLfoSCShape[t], ph.lfo[t].shape,    fo);
        ph.listeners.push(ph.lfo[t].speed.newListener([this, p](vector<float>&) { sendLfoSpeeds(*p); }));
        ph.listeners.push(ph.lfo[t].speedRel.newListener([this, p](bool&) {
            sendLfoSpeeds(*p);
#if OFXOCEANODESC_HAS_TRANSPORT
            if(syncToTransportP.get()) follower.requestAnchor();
#endif
        }));
        vfListener(kLfoSCPhase[t], ph.lfo[t].phase,    fo);
        vfListener(kLfoSCQuant[t], ph.lfo[t].quant,    fo);
        if(t == 0 || t == 1) {
            // Pos / Dur strength: time amounts in ms / Beats
            ph.listeners.push(ph.lfo[t].strength.newListener([this, p, t](vector<float>&) {
                const auto ev = lfoStrSend(*p, t, 1);
                for(auto& [srv, s] : p->synths) if(s) s->set(kLfoSCStr[t], ev);
                if(t == 0 && p->lfo[0].speedRel.get()) {   // Smp: the speed follows the Length
                    sendLfoSpeeds(*p);
#if OFXOCEANODESC_HAS_TRANSPORT
                    if(syncToTransportP.get()) follower.requestAnchor();
#endif
                }
            }));
        } else {
            vfListener(kLfoSCStr[t],   ph.lfo[t].strength, fo);
        }
        vfListener(kLfoSCPow[t],   ph.lfo[t].pow,      fo);
#if OFXOCEANODESC_HAS_TRANSPORT
        // Sync mode: the LFO phase is a function of the transport beat and the
        // speed, so a new speed re-locks it at once (anchor on the next frame).
        ph.listeners.push(ph.lfo[t].speed.newListener([this](vector<float>&) {
            if(syncToTransportP.get()) follower.requestAnchor();
        }));
#endif
    }

    // Manual trigger → per-voice counters (see triggerCountsFor)
    ph.listeners.push(ph.trigger.newListener([this, p](vector<int>&) {
        auto counts = triggerCountsFor(*p);
        for(auto& [srv, s] : p->synths) if(s) s->set("trigger", counts);
    }));

    // SyncGate pulse → counter (rising edge in SC resets the free-running
    // LFO / beat phasors). Ignored by the synths in Sync To Transport mode.
    ph.listeners.push(ph.syncGate.newListener([this, p](int& v) {
        if(v == 1) {
            p->syncGateCount++;
            for(auto& [srv, s] : p->synths) if(s) s->set("syncgate", p->syncGateCount);
            p->syncGateReset = true;
        }
    }));
}

void scGrainBoxPoly::allocEnvBuffers(Playhead& ph) {
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        ensureEnvBuffer(ph, sm->getServer());
    }
}

// Silent-playhead fix: /b_alloc is asynchronous in scsynth (the buffer is
// installed, zeroed, later) while /b_setn is synchronous, so a /b_setn sent
// shortly after the /b_alloc could be applied to a buffer that does not exist
// yet and be lost: the envelope stayed all zeros and GrainBuf's grains were
// silent (a newly added playhead; any env buffer allocated inside the
// b_latency window). Now the data is the /b_alloc completion message, run by
// scsynth right after the allocation. Later edits of an existing buffer still
// use a plain /b_setn (uploadEnvBuffers); one extra upload shortly after the
// allocation covers edits made while it was pending.
void scGrainBoxPoly::ensureEnvBuffer(Playhead& ph, ofxSCServer* srv) {
    if(!srv) return;
    auto it = ph.envBufs.find(srv);
    if(it != ph.envBufs.end() && it->second) {
        if(bufferIsLive(srv, it->second)) return;
        delete it->second;          // stale after a server reboot: not ours to free
        it->second = nullptr;
    }
    if((int)ph.envData.size() != ENV_BUFFER_SIZE) computeEnvData(ph);
    auto* b = new ofxSCBuffer(ENV_BUFFER_SIZE, 1, srv);
    ofxOscMessage m;
    m.setAddress("/b_alloc");
    m.addIntArg(b->index);
    m.addIntArg(ENV_BUFFER_SIZE);
    m.addIntArg(1);
    m.addBlobArg(gbpEncodeBSetn(b->index, ph.envData));
    srv->sendMsg(m);
    ph.envBufs[srv] = b;
    ph.envResendAt = ofGetElapsedTimef() + 0.5f;
}

void scGrainBoxPoly::resendParametersForNRT() {
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        if((int)ph.envData.size() != ENV_BUFFER_SIZE) computeEnvData(ph);
        for(auto& [srv, b] : ph.envBufs) {
            if(!srv || !b || !srv->isNRTCapturing()) continue;
            // Same buffer number, allocated again with the data as the
            // completion message: lands right after the empty /b_alloc that
            // replayBuffersForNRT() put at time zero.
            ofxOscMessage m;
            m.setAddress("/b_alloc");
            m.addIntArg(b->index);
            m.addIntArg(ENV_BUFFER_SIZE);
            m.addIntArg(1);
            m.addBlobArg(gbpEncodeBSetn(b->index, ph.envData));
            srv->sendMsg(m);
        }
        sendAllParams(ph);
    }
    // FX chain synths are rebuilt with the graph (captured); push their values
    sendFxParamsAll();
    // Live recorders (their buffers are recreated empty at score time zero)
    for(auto& ph : playheads)
        for(auto& [srv, l] : ph->liveSC)
            if(l.rec && srv->isNRTCapturing()) setRecArgs(*ph, l.rec, srv);
}

void scGrainBoxPoly::freePlayheadSC(Playhead& ph) {
    ph.grainListeners.clear();
    {
        std::vector<ofxSCServer*> esrvs;
        for(auto& [srv, v] : ph.voices) esrvs.push_back(srv);
        for(auto& [srv, o] : ph.outSynths) esrvs.push_back(srv);
        for(auto& [srv, b] : ph.ebus) esrvs.push_back(srv);
        for(auto* srv : esrvs) { freeEngineVoices(ph, srv); freeEngineBuses(ph, srv); }
    }
    for(auto& [srv, s] : ph.synths) {
        if(s) { s->free(); delete s; }
    }
    ph.synths.clear();
    for(auto& [srv, b] : ph.envBufs) {
        if(b) { if(bufferIsLive(srv, b)) b->free(); delete b; }
    }
    ph.envBufs.clear();
    std::vector<ofxSCServer*> srvs;
    for(auto& [srv, bufs] : ph.own.bufs) srvs.push_back(srv);
    for(auto* srv : srvs) releaseSampleBuffers(ph.own, srv);
    ph.own.bufs.clear();
    std::vector<ofxSCServer*> lsrvs;
    for(auto& [srv, l] : ph.liveSC) lsrvs.push_back(srv);
    for(auto* srv : lsrvs) freeLiveResources(ph, srv);
}

bool scGrainBoxPoly::addPlayhead() {
    if((int)playheads.size() >= MAX_PLAYHEADS) return false;
    playheads.push_back(std::make_unique<Playhead>());
    Playhead& ph = *playheads.back();
    ph.index = (int)playheads.size() - 1;
    ph.sfx   = " " + ofToString(ph.index + 1);
    createPlayheadParameters(ph);
    // Same region as playhead 1 by default (it is the same sample).
    ph.inPoint.set(ph0().inPoint.get());
    ph.outPoint.set(ph0().outPoint.get());
    if(timeUnitsCur != 0) {
        // the defaults are relative: Duration 0.1 of the region, in this unit
        ph.duration.set(vector<float>{(float)secToUnit(0.1f * durScaleSec(ph))});
        timeDirty = true;   // ranges of the new parameters
    }
    // New playheads share playhead 1's sample and follow N Chan (offset 0).
    computeEnvData(ph);
    allocEnvBuffers(ph);   // data goes with the allocation (see ensureEnvBuffer)
    setupPlayheadListeners(ph);
    initializePublishableEditorParameters();

    // Graph already running: give it a synth on every server playhead 1 plays on.
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        auto* srv = sm->getServer();
        if(ph0().synths.count(srv) && ph0().synths[srv])
            createPlayheadSynth(ph, srv);
    }
    return true;
}

bool scGrainBoxPoly::removeLastPlayhead() {
    if(playheads.size() <= 1) return false;
    Playhead& ph = *playheads.back();

    // Unpublish its parameters (removes them from the node, with their connections).
    std::vector<std::string> keys;
    for(const auto& action : publishableEditorParameters)
        if(action.playhead == ph.index && isEditorParameterPublished(action.key)) keys.push_back(action.key);
    for(const auto& key : keys) unpublishEditorParameterFromNode(key);

    ph.listeners.unsubscribeAll();
    freePlayheadSC(ph);
    const int idx = ph.index;
    grainHighlights.erase(std::remove_if(grainHighlights.begin(), grainHighlights.end(),
                                         [idx](const GrainHighlight& g) { return g.playhead == idx; }),
                          grainHighlights.end());
    playheads.pop_back();
    activePlayhead = std::min(activePlayhead, (int)playheads.size() - 1);
    initializePublishableEditorParameters();
    sendMixParams();   // a removed solo no longer silences the others
    return true;
}

void scGrainBoxPoly::setNumPlayheads(int n) {
    n = std::max(1, std::min(MAX_PLAYHEADS, n));
    while((int)playheads.size() < n) { if(!addPlayhead()) break; }
    while((int)playheads.size() > n) { if(!removeLastPlayhead()) break; }
}

// ════════════════════════════════════════════════════════════════════════════
// scNode lifecycle
// ════════════════════════════════════════════════════════════════════════════

// configureSynth: queue every init arg of one playhead's synth (all sets made
// before /s_new become its init args).
void scGrainBoxPoly::configureSynth(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv) {
    if(!s || !srv) return;
    Playhead* p = &ph;

    // Engine buses: the control block the voices read, and the clock
    // (/grainTrig replies come from the voice synths: newVoiceSynth)
    ensureEngineBuses(ph, srv);
    s->set("ctl", ph.ebus[srv].ctl->index);
    s->set("clk", ph.ebus[srv].clk->index);

    // Per-channel sample buffers (bufnum0..bufnum{K-1}) of the playhead's sample
    setBufnums(ph, s, srv);

    // Custom envelope buffer (per playhead)
    if(ph.envBufs.count(srv) && ph.envBufs[srv])
        s->set("envbuf", ph.envBufs[srv]->index);

    // Scalar params
    s->set("dynamicdur",   (int)ph.dynamicDur.get());
    s->set("trigdur",      (int)ph.trigDur.get());
    s->set("autotrig",     (int)ph.autoTrig.get());
    s->set("uniquetrig",   (int)ph.uniqueTrig.get());
    s->set("uniquejit",    (int)ph.uniqueJit.get());
    const bool filtered = synthIsFiltered(s);
    for(int t = 0; t < NUM_LFO; t++)
        if(t != LFO_CUT || filtered) s->set(kLfoSCUnique[t], (int)ph.uniqueLfo[t].get());
    s->set("latchamp",     (int)ph.latchAmp.get());
    setMixArgs(ph, s);
    if(filtered) setFilterArgs(ph, s);
    s->set("bpm",          effectiveBpm());
    // Current counter values: a new synth must not see a change at start.
    s->set("syncgate",     ph.syncGateCount);
    s->set("trigger",      triggerCountsFor(ph));   // phChannels(ph) values
    s->set("inpoint",      inRelOf(ph));
    s->set("outpoint",     outRelOf(ph));

    // Vector params — expanded to the engine's playhead count so a scalar covers every playhead
    const int nv = phVoices(ph);
    s->set("amp",              expandF(ph.amp.get(), nv));
    s->set("pitch",            expandF(ph.pitch.get(), nv));
    s->set("duration",         durSend(ph, nv));
    s->set("position",         posSend(ph, nv));
    s->set("panaz",            expandF(ph.panAz.get(), nv));
    s->set("reverse",          expandF(ph.reverse.get(), nv));
    setScaleArgs(ph, s);
    setLiveArgs(ph, s);
    s->set("vis",              visOn ? 1.0f : 0.0f);
    s->set("autotrigbeatdiv",  divSend(ph, nv));
    s->set("trigphase",        expandF(ph.trigPhase.get(), nv));
    s->set("maxoverlap",       (float)std::max(0, ph.maxGrains.get()));
    s->set("monotrig",         ph.monoTrig.get() ? 1.0f : 0.0f);
    s->set("delayglide",       ph.delayGlide.get() * 0.001f);
    s->set("overlap",          ph.overlap.get());
    // /grainLoad [nodeID, -1, grains sounding, thinning] from the control synth
    // /lfoState [nodeID, -1, 7 phases, 7 values 0..1] (window open only)
    ph.grainListeners[srv] = s->newFeedbackMessage.newListener([p, s](ofxOscMessage& msg) {
        if(msg.getNumArgs() < 4 || msg.getArgAsInt(0) != s->nodeID) return;
        if(msg.getAddress() == "/grainLoad") {
            p->grainLoad = msg.getArgAsFloat(2);
            p->grainThin = msg.getArgAsFloat(3);
        } else if(msg.getAddress() == "/lfoState" && msg.getNumArgs() >= 2 + 2 * NUM_LFO) {
            const float now = ofGetElapsedTimef();
            for(int t = 0; t < NUM_LFO; t++) {
                p->lfoPhaseNow[t] = msg.getArgAsFloat(2 + t);
                p->lfoValNow[t]   = msg.getArgAsFloat(2 + NUM_LFO + t);
                auto& hst = p->lfoHist[t];
                hst.emplace_back(now, p->lfoValNow[t]);
                while(!hst.empty() && now - hst.front().first > 32.0f) hst.pop_front();
                while(hst.size() > 2048) hst.pop_front();
            }
            p->lfoStateAt = now;
        }
    });
    s->set("chance",           withOffset(ph.chance.get(), nv, gChanceP.get()));
    s->set("posjit",           posAmtSend(ph, ph.posJit.get(), nv));
    s->set("pitchjit",         expandF(ph.pitchJit.get(), nv));
    s->set("durjit",           durAmtSend(ph, ph.durJit.get(), nv));
    s->set("ampjit",           expandF(ph.ampJit.get(), nv));
    s->set("panazjit",         expandF(ph.panAzJit.get(), nv));
    s->set("levels",           expandF(ph.levels.get(), phChannels(ph)));   // per speaker

    for(int t = 0; t < NUM_LFO; t++) {
        if(t == LFO_CUT && !filtered) continue;
        // one LFO per engine: single values
        s->set(kLfoSCShape[t], expandF(ph.lfo[t].shape.get(), 1));
        s->set(kLfoSCSpeed[t], vector<float>{(float)lfoSpeedBar(ph, t)});
        s->set(kLfoSCPhase[t], expandF(ph.lfo[t].phase.get(), 1));
        s->set(kLfoSCQuant[t], expandF(ph.lfo[t].quant.get(), 1));
        s->set(kLfoSCStr[t],   lfoStrSend(ph, t, 1));
        s->set(kLfoSCPow[t],   expandF(ph.lfo[t].pow.get(), 1));
    }

#if OFXOCEANODESC_HAS_TRANSPORT
    setSyncHoldArgs(ph, s);
#endif
}

// buildSynth: create the ofxSCSynth objects and queue all init args.
// serverManager calls buildSynth → setOutputBus → createSynth, all within
// the same setBLatency bundle.  By creating the synth objects here (before
// setOutputBus fires), the "out" bus value gets stored as an init arg
// (created=false) and is baked into the /s_new message via createAndRun.
void scGrainBoxPoly::buildSynth(ofxSCServer* srv) {
    if(!srv) return;
    reloadSampleBuffersForServer(mainSample, srv);
    for(auto& phPtr : playheads)
        if(phPtr->index > 0 && phPtr->ownSample) reloadSampleBuffersForServer(phPtr->own, srv);
    bool hasSampleBufs = mainSample.bufs.count(srv) && !mainSample.bufs[srv].empty();
    ofLogNotice("scGrainBoxPoly") << "buildSynth: sampleBufs=" << hasSampleBufs
        << " samplePath=" << (mainSample.path.empty() ? "<empty>" : mainSample.path)
        << " numSampleCh=" << mainSample.numChannels
        << " playheads=" << playheads.size();

    // If we have a sample path but no buffers loaded yet for this server, load them now
    if(!mainSample.path.empty() && (!mainSample.bufs.count(srv) || mainSample.bufs[srv].empty())) {
        ofLogWarning("scGrainBoxPoly") << "buildSynth: lazy-loading sample INSIDE b_latency window — "
            << "buffer and synth scheduled at same T+200ms (may cause silence)";
        for(int ch = 0; ch < mainSample.numChannels; ch++) {
            auto* buf = new ofxSCBuffer(0, 0, srv);
            // readChannel records path + channel on the buffer, so an NRT capture
            // can replay the load at score time zero (replayBuffersForNRT).
            buf->readChannel(mainSample.path, {ch});
            mainSample.bufs[srv].push_back(buf);
            ofLogNotice("scGrainBoxPoly") << "buildSynth: lazy-load ch=" << ch << " bufIdx=" << buf->index;
        }
    }

    // FX chain: plan (and private buses) before setOutputBus routes the
    // playheads; the FX synths are created in createSynth.
    freeFx(srv);
    planFx(srv);

    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        // Free any stale synth
        if(ph.synths.count(srv) && ph.synths[srv]) {
            ph.synths[srv]->free();
            delete ph.synths[srv];
            ph.synths[srv] = nullptr;
        }
        freeEngineVoices(ph, srv);
        // Ensure env buffer exists (pre-allocated at creation; late servers and
        // rebooted ones get it here — safe inside b_latency, the data is the
        // /b_alloc completion message)
        ensureEnvBuffer(ph, srv);
        if(ph.liveInput) ensureLiveResources(ph, srv);   // bufnum0 + phasebus
        ph.synths[srv] = new ofxSCSynth(defNameFor(ph), srv);
        configureSynth(ph, ph.synths[srv], srv);
        buildEngineObjects(ph, srv);      // voices + output synth (created in createSynth)
    }
}

// The synth of the highest playhead below `belowIndex` that has one on srv:
// the earliest of them in SC's node order (extra playheads sit before
// playhead 1, each before the previous one).
ofxSCSynth* scGrainBoxPoly::earliestSynth(ofxSCServer* srv, int belowIndex) {
    for(int i = std::min(belowIndex, (int)playheads.size()) - 1; i >= 0; i--) {
        auto& m = playheads[i]->synths;
        if(m.count(srv) && m[srv]) return m[srv];
    }
    return nullptr;
}

// createSynth: send /s_new.  All args (including "out") were already queued
// during buildSynth / setOutputBus while created=false.
void scGrainBoxPoly::createSynth(ofxSCServer* srv) {
    if(!srv) return;
    // If buildSynth wasn't called (edge case), fall back to building now
    if(!ph0().synths.count(srv) || !ph0().synths[srv])
        buildSynth(srv);
    if(!ph0().synths[srv]) return;
    ofLogNotice("scGrainBoxPoly") << "createSynth: synthdefName=" << defNameFor(ph0())
        << " sampleBufs=" << (mainSample.bufs.count(srv) ? (int)mainSample.bufs[srv].size() : 0)
        << " outBus=" << (outputBuses.count(srv) ? outputBuses[srv] : -1)
        << " numCh=" << numChannelsP.get()
        << " playheads=" << playheads.size();
    ph0().synths[srv]->createAndRun(0, 1, true);
    runEngineChain(ph0(), srv, true);
    for(int i = 1; i < (int)playheads.size(); i++) {
        auto& m = playheads[i]->synths;
        if(!m.count(srv) || !m[srv]) continue;
        ofxSCSynth* before = earliestSynth(srv, i);
        if(before) m[srv]->createAndRun(2, before->nodeID, true);   // addBefore
        else       m[srv]->createAndRun(0, 1, true);
        runEngineChain(*playheads[i], srv, true);
    }
    // FX stages right after playhead 1's synth (same bundle)
    createFxSynths(srv);
    // Live recorders right before the earliest grain synth (after the input
    // source, which the graph puts before this node's earliest synth)
    for(auto& ph : playheads) {
        ph->isPaused = false;          // created running; updatePausing re-pauses
        ph->pauseAt  = -1.0f;
        if(ph->liveInput) createLiveRecorder(*ph, srv);
    }
    ofLogNotice("scGrainBoxPoly") << "createSynth AFTER: nodeID=" << ph0().synths[srv]->nodeID;
    // Refresh the envelope data (the buffers already hold it: it is sent with
    // every allocation; this only covers edits made while one was pending).
    for(auto& ph : playheads) ph->envNeedsUpdate = true;
}

// A playhead added while the graph runs: its own synth, right before the
// earliest synth of this node, writing to the same output bus.
void scGrainBoxPoly::createPlayheadSynth(Playhead& ph, ofxSCServer* srv) {
    if(!srv) return;
    if(ph.synths.count(srv) && ph.synths[srv]) return;
    ofxSCSynth* before = earliestSynth(srv, ph.index);
    if(!before) return;
    ensureEnvBuffer(ph, srv);
    ph.synths[srv] = new ofxSCSynth(defNameFor(ph), srv);
    configureSynth(ph, ph.synths[srv], srv);
    buildEngineObjects(ph, srv);
    ph.synths[srv]->set("active", getActive() ? 1.0f : 0.0f);
    ph.synths[srv]->createAndRun(2, before->nodeID, getActive());
    runEngineChain(ph, srv, getActive());
}

// SynthDef variant (channel count, sample channels) or output offset changed:
// bring every playhead's synth up to date.
void scGrainBoxPoly::replaceSynths(ofxSCServer* srv) {
    for(auto& phPtr : playheads) refreshPlayheadSynth(*phPtr, srv);
}

void scGrainBoxPoly::refreshPlayheadSynths(Playhead& ph) {
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        refreshPlayheadSynth(ph, sm->getServer());
    }
}

// Same variant: only the sample buffers and the output bus are (re)sent.
// Another variant (voices, sample channels): the synth is replaced in place
// (action 4 keeps its position in the graph).
void scGrainBoxPoly::refreshPlayheadSynth(Playhead& ph, ofxSCServer* srv) {
    if(!ph.synths.count(srv) || !ph.synths[srv]) return;
    replaceCtl(ph, srv);                     // capacity changed (no-op otherwise)
    setBufnums(ph, ph.synths[srv], srv);
    const bool run = getActive() && !ph.isPaused;
    // Voices: another variant (sample channels K, speakers N) -> replaced in
    // place (action 4 keeps the position in the graph)
    const std::string vdef = voiceDefFor(ph);
    auto& vs = ph.voices[srv];
    for(int i = 0; i < (int)vs.size(); i++) {
        if(!vs[i] || vs[i]->getName() == vdef) continue;
        const int oldID = vs[i]->nodeID;
        auto& ls = ph.voiceListeners[srv];
        if(i < (int)ls.size()) ls[i].reset();
        delete vs[i];                          // no /n_free: the replace frees it
        vs[i] = newVoiceSynth(ph, srv, i);
        vs[i]->createAndRun(4, oldID, run);
    }
    // Output synth: another speaker count -> replaced; always the output bus
    auto o = ph.outSynths.find(srv);
    if(o != ph.outSynths.end() && o->second) {
        if(o->second->getName() != outDefFor(ph)) {
            const int oldID = o->second->nodeID;
            delete o->second;
            auto& b = ph.ebus[srv];
            auto* n = new ofxSCSynth(outDefFor(ph), srv);
            n->set("in",  b.mix->index);
            n->set("ctl", b.ctl->index);
            n->set("out", outBusFor(ph, srv));
            n->set("fbbus", fbBusFor(ph, srv));
            n->createAndRun(4, oldID, run);
            o->second = n;
        } else {
            o->second->set("out", outBusFor(ph, srv));
        }
    }
}

void scGrainBoxPoly::setBufnums(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv) {
    if(!s) return;
    const SampleData& sd = sampleOf(ph);
    auto it = sd.bufs.find(srv);
    if(it == sd.bufs.end()) return;
    for(int ch = 0; ch < (int)it->second.size(); ch++)
        if(it->second[ch]) s->set("bufnum" + ofToString(ch), it->second[ch]->index);
}

void scGrainBoxPoly::free(ofxSCServer* srv) {
    if(!srv) return;
    ofLogNotice("scGrainBoxPoly") << "free: called. synth="
        << (ph0().synths.count(srv) && ph0().synths[srv] ? ph0().synths[srv]->nodeID : -1)
        << " sampleBufs=" << (mainSample.bufs.count(srv) ? (int)mainSample.bufs[srv].size() : 0);
    for(auto& ph : playheads) {
        if(ph->synths.count(srv) && ph->synths[srv]) {
            ph->synths[srv]->free();
            delete ph->synths[srv];
        }
        ph->synths.erase(srv);
        ph->grainListeners.erase(srv);
        freeEngineVoices(*ph, srv);       // engine buses kept (allocated client-side only)
        freeLiveRecorder(*ph, srv);      // buffer + bus kept (see Live input)
    }
    freeFx(srv);
    // Sample buffers (mainSample / own samples) intentionally NOT freed here —
    // disk-loaded buffers must persist
    // across recomputeGraph cycles so buildSynth can reference them without
    // re-issuing /b_allocReadChannel inside the b_latency window (which would
    // schedule both buffer load and /s_new at the same T+200ms, reading from an
    // unloaded buffer → silence).  Freed in loadSampleInto() on sample change,
    // setOwnSample(false), when a playhead is removed and in the destructor.
    //
    // envBufs likewise NOT freed here (cheap; avoids re-allocating in every
    // recompute). The allocation itself is now safe anywhere: its data is the
    // /b_alloc completion message (see ensureEnvBuffer), whereas a separate
    // /b_setn could arrive before the buffer exists → env reads zeros →
    // silent grains.  Env buffer is cheap per-server
    // state; keep it alive across recomputes.  Freed only in destructor (or
    // when its playhead is removed).
    if(privateBuses.count(srv) && privateBuses[srv]) {
        privateBuses[srv]->free();
        delete privateBuses[srv];
        privateBuses.erase(srv);
    }
    outputBuses.erase(srv);
}

void scGrainBoxPoly::setOutputBus(ofxSCServer* srv, int /*idx*/, int bus) {
    if(!srv) return;
    outputBuses[srv] = bus;
    ofLogNotice("scGrainBoxPoly") << "setOutputBus: bus=" << bus;
    if(drySynths.count(srv) && drySynths[srv]) drySynths[srv]->set("out", bus);
    // synth objects exist (created in buildSynth, not yet /s_new'd) — set "out"
    // so it becomes an init arg baked into the /s_new call in createSynth.
    // All playheads write (Out.ar adds) to the same bus, from their offset on
    // (or to the FX chain's input bus; its last stage writes to `bus`).
    for(auto& ph : playheads)
        if(ph->outSynths.count(srv) && ph->outSynths[srv])
            ph->outSynths[srv]->set("out", outBusFor(*ph, srv));
    auto f = fx.find(srv);
    if(f != fx.end() && !f->second.plan.empty()) {
        const int last = f->second.plan.back();
        if(f->second.synths[last]) f->second.synths[last]->set("out", bus);
    }
}

int scGrainBoxPoly::getOutputBusIndex(ofxSCServer* srv, int idx) {
    int result = outputBuses.count(srv) ? outputBuses.at(srv) : -1;
    ofLogNotice("scGrainBoxPoly") << "getOutputBusIndex: idx=" << idx << " return=" << result;
    return result;
}

void scGrainBoxPoly::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    if(!ph0().synths.count(srv) || !ph0().synths[srv]) {
        ofLogWarning("scGrainBoxPoly") << "moveSynthBefore: no synth for server, skipping";
        return;
    }
    ofLogNotice("scGrainBoxPoly") << "moveSynthBefore: myNodeID=" << ph0().synths[srv]->nodeID
        << " beforeNodeID=" << nodeID
        << " outBus=" << (outputBuses.count(srv) ? outputBuses[srv] : -1);
    // Resend all params then move — mirrors scFM7Drone/scRhythmBox pattern.
    // Do NOT rebuild the synth here; just refresh state and reposition.
    // Order: the FX stages (last one before nodeID), playhead 1 before the
    // first stage, each extra playhead before the previous.
    int target = nodeID;
    if(fx.count(srv)) {
        FxState& st = fx[srv];
        for(int i = (int)st.plan.size() - 1; i >= 0; i--) {
            ofxSCSynth* s = st.synths[st.plan[i]];
            if(!s) continue;
            sendFxParams(st.plan[i], s);
            s->set("in",  st.buses[i]->index);
            s->set("out", fxOutBus(srv, i));
            s->moveBefore(target);
            target = s->nodeID;
        }
    }
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        sendAllParams(ph);
        if(!ph.synths.count(srv) || !ph.synths[srv]) continue;
        // the engine's chain: control -> voices -> output (moved last to first)
        if(ph.outSynths.count(srv) && ph.outSynths[srv]) {
            if(outputBuses.count(srv)) ph.outSynths[srv]->set("out", outBusFor(ph, srv));
            ph.outSynths[srv]->moveBefore(target);
            target = ph.outSynths[srv]->nodeID;
        }
        auto& vs = ph.voices[srv];
        for(auto it = vs.rbegin(); it != vs.rend(); ++it)
            if(*it) { (*it)->moveBefore(target); target = (*it)->nodeID; }
        ph.synths[srv]->moveBefore(target);
        target = ph.synths[srv]->nodeID;
    }
    // Live recorders before every grain synth, keeping their order (the
    // front of recOrder ends up earliest)
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        if(ph.liveInput && ph.liveSC.count(srv) && ph.liveSC[srv].rec) setRecArgs(ph, ph.liveSC[srv].rec, srv);
    }
    if(recOrder.count(srv)) {
        auto& order = recOrder[srv];
        for(auto it = order.rbegin(); it != order.rend(); ++it) {
            (*it)->moveBefore(target);
            target = (*it)->nodeID;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Live input
// ════════════════════════════════════════════════════════════════════════════
//
// A Live playhead reads a mono rolling buffer (liveLength s) that its
// GrainBoxRec synth records from the node's "In" port (first channel), and
// whose write position is on a 1-channel audio bus. Buffer and bus are kept
// across graph recomputes (like the envelope buffers: re-allocating inside
// the b_latency window would drop the recording); the recorder synth follows
// the graph. NRT: the buffer has no file, so the capture recreates it empty
// at time zero (replayBuffersForNRT) and the recorder is part of the score.

void scGrainBoxPoly::setInputBus(ofxSCServer* srv, scNode* /*node*/, int bus) {
    if(!srv) return;
    inputBuses[srv] = bus;
    if(drySynths.count(srv) && drySynths[srv]) {
        drySynths[srv]->set("in", bus);
        drySynths[srv]->set("dry", dryGain());
    }
    for(auto& ph : playheads) {
        auto it = ph->liveSC.find(srv);
        if(it != ph->liveSC.end() && it->second.rec) it->second.rec->set("in", bus);
    }
}

void scGrainBoxPoly::resetInputBusses(ofxSCServer* srv, int targetBus) {
    setInputBus(srv, nullptr, targetBus);
}

void scGrainBoxPoly::ensureLiveResources(Playhead& ph, ofxSCServer* srv) {
    if(!srv) return;
    auto& l = ph.liveSC[srv];
    if(l.buf && !bufferIsLive(srv, l.buf)) { delete l.buf; l.buf = nullptr; }        // server rebooted
    if(l.phaseBus && !gbpBusIsLive(srv, l.phaseBus)) { delete l.phaseBus; l.phaseBus = nullptr; }
    int srI = serverManager::getSampleRateForServer(srv);
    if(srI < 8000) srI = 48000;            // not reported yet (server booting)
    const float sr = (float)srI;
    const int frames = std::max(64, (int)std::lround(liveLenSec(ph) * sr));
    if(!l.buf) {
        l.buf = new ofxSCBuffer(frames, 1, srv);
        l.buf->alloc();                     // zeroed: reads silence until recorded
    }
    if(!l.phaseBus) l.phaseBus = new ofxSCBus(RATE_AUDIO, 1, srv);
    if(l.fbBus && !gbpBusIsLive(srv, l.fbBus)) { delete l.fbBus; l.fbBus = nullptr; }   // server rebooted
    if(!l.fbBus) l.fbBus = new ofxSCBus(RATE_AUDIO, 1, srv);
    ph.live.bufs[srv] = { l.buf };
    ph.live.numChannels  = 1;
    ph.live.durationSecs = (float)l.buf->frames / sr;
    if((int)ph.live.peaks.size() != WAVEFORM_BINS) ph.live.peaks.assign(WAVEFORM_BINS, 0.0f);
    if((int)ph.liveRing.size()   != WAVEFORM_BINS) ph.liveRing.assign(WAVEFORM_BINS, 0.0f);
}

void scGrainBoxPoly::setRecArgs(Playhead& ph, ofxSCSynth* rec, ofxSCServer* srv) {
    if(!rec) return;
    auto& l = ph.liveSC[srv];
    rec->set("in",       inputBuses.count(srv) ? inputBuses[srv] : 0);
    if(l.buf)      rec->set("bufnum",   l.buf->index);
    if(l.phaseBus) rec->set("phasebus", l.phaseBus->index);
    rec->set("freeze",   ph.freeze.get() ? 1.0f : 0.0f);
    rec->set("vis",      visOn ? 1.0f : 0.0f);
    rec->set("fbbus",    l.fbBus ? l.fbBus->index : -1);
    rec->set("fbamt",    std::max(0.0f, ph.feedback.get()));
    rec->set("fbtone",   ph.fbTone.get());
}

int scGrainBoxPoly::fbBusFor(Playhead& ph, ofxSCServer* srv) {
    if(!ph.liveInput) return -1;
    auto it = ph.liveSC.find(srv);
    return (it != ph.liveSC.end() && it->second.fbBus) ? it->second.fbBus->index : -1;
}

void scGrainBoxPoly::setOutFb(Playhead& ph, ofxSCServer* srv) {
    auto o = ph.outSynths.find(srv);
    if(o != ph.outSynths.end() && o->second) o->second->set("fbbus", fbBusFor(ph, srv));
}

void scGrainBoxPoly::setLiveArgs(Playhead& ph, ofxSCSynth* s) {
    if(!s) return;
    ofxSCServer* srv = nullptr;
    for(auto& [sv, sy] : ph.synths) if(sy == s) srv = sv;
    int bus = 0;
    if(srv && ph.liveInput && ph.liveSC.count(srv) && ph.liveSC[srv].phaseBus) bus = ph.liveSC[srv].phaseBus->index;
    s->set("live",     ph.liveInput ? 1.0f : 0.0f);
    s->set("phasebus", bus);
}

// Recorder right before the earliest grain synth of this node
void scGrainBoxPoly::createLiveRecorder(Playhead& ph, ofxSCServer* srv) {
    ensureLiveResources(ph, srv);
    auto& l = ph.liveSC[srv];
    if(l.rec) return;
    // before every other synth of this node: the earliest recorder, or the
    // earliest grain synth
    ofxSCSynth* before = earliestRecorder(srv);
    if(!before) before = earliestSynth(srv, (int)playheads.size());
    if(!before) return;
    l.rec = new ofxSCSynth("GrainBoxPolyRec", srv);
    setRecArgs(ph, l.rec, srv);
    Playhead* p = &ph;
    // /grainRec [nodeID, replyID, max, min, position 0..1] every 8 ms: rolling
    // min / max display (a few bins written per message)
    l.recListener = l.rec->newFeedbackMessage.newListener([this, p, srv](ofxOscMessage& msg) {
        if(msg.getAddress() != "/grainRec" || msg.getNumArgs() < 5) return;
        if(!p->liveSC.count(srv) || !p->liveSC[srv].rec || msg.getArgAsInt(0) != p->liveSC[srv].rec->nodeID) return;
        const int bins = WAVEFORM_BINS;
        if((int)p->liveRing.size()    != bins) p->liveRing.assign(bins, 0.0f);
        if((int)p->liveRingMin.size() != bins) p->liveRingMin.assign(bins, 0.0f);
        const float hi  = std::max(-1.0f, std::min(1.0f, msg.getArgAsFloat(2)));
        const float lo  = std::max(-1.0f, std::min(1.0f, msg.getArgAsFloat(3)));
        const float pos = std::max(0.0f, std::min(0.999999f, msg.getArgAsFloat(4)));
        // fill the bins written since the previous report
        int from = (int)(p->liveHead * bins), to = (int)(pos * bins);
        int count = (to - from + bins) % bins;
        if(count > bins / 4) count = 1;     // a jump (freeze, re-alloc): mark one bin
        for(int k = 1; k <= std::max(1, count); k++) {
            const int b = (from + k) % bins;
            p->liveRing[b] = hi;
            p->liveRingMin[b] = lo;
        }
        p->liveHead = pos;
    });
    l.rec->createAndRun(2, before->nodeID, getActive());   // addBefore
    recOrder[srv].push_front(l.rec);
}

void scGrainBoxPoly::freeLiveRecorder(Playhead& ph, ofxSCServer* srv) {
    auto it = ph.liveSC.find(srv);
    if(it == ph.liveSC.end()) return;
    if(it->second.rec) {
        auto& order = recOrder[srv];
        order.erase(std::remove(order.begin(), order.end(), it->second.rec), order.end());
        it->second.rec->free(); delete it->second.rec; it->second.rec = nullptr;
    }
}

void scGrainBoxPoly::freeLiveResources(Playhead& ph, ofxSCServer* srv) {
    freeLiveRecorder(ph, srv);
    auto it = ph.liveSC.find(srv);
    if(it == ph.liveSC.end()) return;
    if(it->second.buf) { if(bufferIsLive(srv, it->second.buf)) it->second.buf->free(); delete it->second.buf; }
    if(it->second.phaseBus) { if(gbpBusIsLive(srv, it->second.phaseBus)) it->second.phaseBus->free(); delete it->second.phaseBus; }
    if(it->second.fbBus) { if(gbpBusIsLive(srv, it->second.fbBus)) it->second.fbBus->free(); delete it->second.fbBus; }
    ph.liveSC.erase(it);
    setOutFb(ph, srv);   // the output synth stops writing it
    ph.live.bufs.erase(srv);
}

// Live on / off: resources first (the grain synth then reads the new buffer),
// then the synth (K = 1 variant, live args), then the recorder.
void scGrainBoxPoly::setLiveInput(Playhead& ph, bool on) {
    if(ph.liveInput == on) return;
    timeDirty = true;
    ph.liveInput = on;
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        ofxSCServer* srv = sm->getServer();
        const bool running = ph.synths.count(srv) && ph.synths[srv];
        if(on) {
            ensureLiveResources(ph, srv);
            if(running) {
                refreshPlayheadSynth(ph, srv);
                setLiveArgs(ph, ph.synths[srv]);
                createLiveRecorder(ph, srv);
            }
            setOutFb(ph, srv);
        } else {
            if(running) {
                refreshPlayheadSynth(ph, srv);
                setLiveArgs(ph, ph.synths[srv]);
            }
            freeLiveResources(ph, srv);
        }
    }
}

// New length: new buffer + recorder (the old recording is dropped)
void scGrainBoxPoly::resizeLiveBuffers(Playhead& ph) {
    timeDirty = true;
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        ofxSCServer* srv = sm->getServer();
        const bool running = ph.synths.count(srv) && ph.synths[srv];
        freeLiveResources(ph, srv);
        ensureLiveResources(ph, srv);
        if(running) {
            setBufnums(ph, ph.synths[srv], srv);
            setLiveArgs(ph, ph.synths[srv]);
            createLiveRecorder(ph, srv);
        }
    }
    std::fill(ph.liveRing.begin(), ph.liveRing.end(), 0.0f);
    std::fill(ph.liveRingMin.begin(), ph.liveRingMin.end(), 0.0f);
    ph.liveHead = 0.0f;
}

// Display: the ring (buffer order) re-read in age order, 0 = the write head
void scGrainBoxPoly::updateLivePeaks(Playhead& ph) {
    const int bins = WAVEFORM_BINS;
    if((int)ph.liveRing.size()    != bins) ph.liveRing.assign(bins, 0.0f);
    if((int)ph.liveRingMin.size() != bins) ph.liveRingMin.assign(bins, 0.0f);
    if((int)ph.live.peaks.size()  != bins) ph.live.peaks.assign(bins, 0.0f);
    if((int)ph.live.lows.size()   != bins) ph.live.lows.assign(bins, 0.0f);
    const int head = (int)(ph.liveHead * bins);
    for(int b = 0; b < bins; b++) {
        const int r = ((head - b) % bins + bins) % bins;
        ph.live.peaks[b] = ph.liveRing[r];
        ph.live.lows[b]  = ph.liveRingMin[r];
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Scale
// ════════════════════════════════════════════════════════════════════════════

// 12-bit masks relative to the root (bit k: root + k semitones allowed)
int scGrainBoxPoly::scaleMaskFor(const Playhead& ph) const {
    auto bits = [](std::initializer_list<int> steps) { int m = 0; for(int k : steps) m |= 1 << k; return m; };
    switch(ph.scaleType.get()) {
        case 1:  return 4095;                                  // Chromatic
        case 2:  return bits({0, 2, 4, 5, 7, 9, 11});          // Major
        case 3:  return bits({0, 2, 3, 5, 7, 8, 10});          // Minor
        case 4:  return bits({0, 2, 3, 5, 7, 8, 11});          // Harmonic minor
        case 5:  return bits({0, 2, 3, 5, 7, 9, 10});          // Dorian
        case 6:  return bits({0, 2, 4, 7, 9});                 // Pentatonic major
        case 7:  return bits({0, 3, 5, 7, 10});                // Pentatonic minor
        case 8:  return bits({0, 2, 4, 6, 8, 10});             // Whole tone
        case 9:  return bits({0});                             // Octaves
        case 10: return bits({0, 7});                          // Fifths + octaves
        case 11: return ph.scaleMask.get() & 4095;             // Custom
        default: return 4095;
    }
}

// The synth snaps with ONE lookup per voice: x = (pitch - root) wrapped to
// 0..12, bin j = floor(2x) (half-semitone bins). Within a bin round(x) is
// constant (n0 = (j + 1) / 2, .5 rounding up), and the nearest allowed note
// is the first allowed of n0, n0 - 1, n0 + 1, ... n0 +- 6 (ties go down):
// table[j] = that note, relative to the octave start (may be -6..18).
void scGrainBoxPoly::setScaleArgs(Playhead& ph, ofxSCSynth* s) {
    if(!s) return;
    const int mask = scaleMaskFor(ph);
    static const int offs[13] = {0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6};
    std::vector<float> table(24);
    for(int j = 0; j < 24; j++) {
        const int n0 = (j + 1) / 2;
        int note = n0;
        for(int off : offs) {
            const int cand = n0 + off;
            if((mask >> (((cand % 12) + 12) % 12)) & 1) { note = cand; break; }
        }
        table[j] = (float)note;
    }
    // an empty custom mask would allow nothing: leave the pitch alone then
    const bool on = ph.scaleType.get() > 0 && mask != 0;
    s->set("scaleon",    on ? 1.0f : 0.0f);
    s->set("scaleroot",  (float)std::max(0, std::min(11, ph.scaleRoot.get())));
    s->set("scaletable", table);
}

// ════════════════════════════════════════════════════════════════════════════
// CPU: pausing muted playheads, /grainTrig only while the window is shown
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::sendVis() {
    const float v = visOn ? 1.0f : 0.0f;
    for(auto& ph : playheads) {
        for(auto& [srv, s] : ph->synths) if(s) s->set("vis", v);
        for(auto& [srv, l] : ph->liveSC) if(l.rec) l.rec->set("vis", v);
    }
}

// A muted (or solo-excluded) playhead's synth is paused (n_run 0) once its
// 20 ms fade (phgain lag) is over, ~70 ms after the mute; unmuting resumes it
// at once. Every n_run is a plain timed message, so an NRT capture records
// the same pauses. Never touches a deactivated node (deactivate pauses all).
void scGrainBoxPoly::updatePausing() {
    if(!getActive()) return;
    const float now = ofGetElapsedTimef();
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        const bool silent = phGainFor(ph) <= 0.0f;
        if(silent) {
            if(ph.isPaused) continue;
            if(ph.pauseAt < 0.0f) { ph.pauseAt = now + 0.07f; continue; }
            if(now < ph.pauseAt) continue;
            forEngineSynth(ph, [](ofxSCSynth* s) { s->run(false); });
            ph.isPaused = true;
        } else {
            ph.pauseAt = -1.0f;
            if(!ph.isPaused) continue;
            forEngineSynth(ph, [](ofxSCSynth* s) { s->run(true); });
            ph.isPaused = false;
        }
    }
}

int scGrainBoxPoly::getLastSynthID(ofxSCServer* srv) {
    // The earliest of this node's synths in SC's order (see moveSynthBefore):
    // a live recorder if there is one, else the highest playhead's synth.
    if(ofxSCSynth* r = earliestRecorder(srv)) return r->nodeID;
    ofxSCSynth* s = earliestSynth(srv, (int)playheads.size());
    return s ? s->nodeID : -1;
}

// Recorders are chained before the earliest grain synth, each before the
// previous one: the earliest is the one of the highest live playhead.
ofxSCSynth* scGrainBoxPoly::earliestRecorder(ofxSCServer* srv) {
    auto it = recOrder.find(srv);
    return (it != recOrder.end() && !it->second.empty()) ? it->second.front() : nullptr;
}

void scGrainBoxPoly::activate() {
    for(auto& ph : playheads) {
        for(auto& [srv, s] : ph->synths) if(s) s->set("active", 1.0f);
        forEngineSynth(*ph, [](ofxSCSynth* s) { s->run(true); });
        for(auto& [srv, l] : ph->liveSC) if(l.rec) l.rec->run(true);
        ph->isPaused = false;   // updatePausing re-pauses muted playheads
        ph->pauseAt  = -1.0f;
    }
    for(auto& [srv, st] : fx)
        for(int stage : st.plan) if(st.synths[stage]) st.synths[stage]->run(true);
    for(auto& [srv, d] : drySynths) if(d) d->run(true);
}

void scGrainBoxPoly::deactivate() {
    for(auto& ph : playheads) {
        for(auto& [srv, s] : ph->synths) if(s) s->set("active", 0.0f);
        forEngineSynth(*ph, [](ofxSCSynth* s) { s->run(false); });
        for(auto& [srv, l] : ph->liveSC) if(l.rec) l.rec->run(false);
    }
    for(auto& [srv, st] : fx)
        for(int stage : st.plan) if(st.synths[stage]) st.synths[stage]->run(false);
    for(auto& [srv, d] : drySynths) if(d) d->run(false);
}

// ════════════════════════════════════════════════════════════════════════════
// Global mixing (transpose / volume / speed / mute / solo) and filter args
// ════════════════════════════════════════════════════════════════════════════

// Every GrainBox_K_N synth has the output filter (\filteron 0 = bypassed), so
// every synth takes the filter and Cut-LFO controls.
bool scGrainBoxPoly::synthIsFiltered(ofxSCSynth* s) {
    return s != nullptr;
}

// 0 when muted, or when some other playhead is soloed and this one is not
float scGrainBoxPoly::phGainFor(const Playhead& ph) const {
    bool anySolo = false;
    for(const auto& p : playheads) anySolo = anySolo || p->solo.get();
    if(anySolo) return ph.solo.get() ? 1.0f : 0.0f;
    return ph.mute.get() ? 0.0f : 1.0f;
}

void scGrainBoxPoly::setMixArgs(Playhead& ph, ofxSCSynth* s) {
    if(!s) return;
    s->set("transpose", transposeP.get());
    s->set("mastervol", volumeP.get() * wetGain());
    s->set("speed",     speedP.get());
    s->set("phgain",    phGainFor(ph));
}

void scGrainBoxPoly::setFilterArgs(Playhead& ph, ofxSCSynth* s) {
    if(!s) return;
    // Filter parameter: 0 Off, 1 LPF2, 2 BPF, 3 Notch, 4 HPF2. Off keeps the
    // last type so the 20 ms fade-out still filters with it.
    s->set("filteron",   ph.filterType.get() > 0 ? 1 : 0);
    if(ph.filterType.get() > 0)
        s->set("filtertype", std::max(0, std::min(4, ph.filterType.get() - 1)));
    s->set("cutoff",     ph.cutoff.get());
    s->set("filterq",    ph.filterQ.get());
}

float scGrainBoxPoly::dryGain() const {
    return std::cos(std::max(0.0f, std::min(1.0f, dryWetP.get())) * (float)HALF_PI);
}
float scGrainBoxPoly::wetGain() const {
    return std::sin(std::max(0.0f, std::min(1.0f, dryWetP.get())) * (float)HALF_PI);
}

// Dry: the node's input -> its output, right after engine 1's chain (so after
// whatever feeds the input). Not paused with engine 1 (mute / solo).
void scGrainBoxPoly::createDry(ofxSCServer* srv) {
    freeDry(srv);
    if(!srv) return;
    ofxSCSynth* after = engineLast(ph0(), srv);
    if(!after) return;
    const int N = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    auto* d = new ofxSCSynth("GrainBoxPolyDry_" + ofToString(N), srv);
    auto in = inputBuses.find(srv);
    d->set("in",  in != inputBuses.end() ? in->second : 0);
    d->set("dry", in != inputBuses.end() ? dryGain() : 0.0f);
    auto out = outputBuses.find(srv);
    d->set("out", out != outputBuses.end() ? out->second : 0);
    d->createAndRun(3, after->nodeID, getActive());
    drySynths[srv] = d;
}

void scGrainBoxPoly::freeDry(ofxSCServer* srv) {
    auto it = drySynths.find(srv);
    if(it == drySynths.end()) return;
    if(it->second) { it->second->free(); delete it->second; }
    drySynths.erase(it);
}

void scGrainBoxPoly::sendMixParams() {
    for(auto& ph : playheads)
        for(auto& [srv, s] : ph->synths) setMixArgs(*ph, s);
}

// ════════════════════════════════════════════════════════════════════════════
// Global FX chain: EQ (graphiceqN) → Echo (EchoN) → Reverb (SpaceMasterN)
// ════════════════════════════════════════════════════════════════════════════
//
// The defs are the user's: graphiceq.scd (SPECIAL_SYNTHDEFS/KR, like
// GrainBox's own) and echo.scd / spaceMaster.scd (~synthCreator: def "Echo" ++
// n / "SpaceMaster" ++ n, controls "in" / "out" and the OceanodeParameterLag
// vectors). With "Load Synthdefs On Preset" on, serverManager loads only the
// Defaults folder plus the defs of the preset's own "SC ...*" nodes, so these
// may be missing: the node then sends /d_loadDir for their folder (found under
// Supercollider/Synthdefs) and uses a stage only ~1.5 s later (/d_loadDir is
// asynchronous and its /done is not forwarded to nodes). Until then the chain
// simply skips that stage.

namespace {
// Folder that holds "<name>1.scsyndef" under Supercollider/Synthdefs ("" if none).
std::string gbpFindSynthdefFolder(const std::string& name) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(name);
    if(it != cache.end()) return it->second;
    std::string found;
    try {
        const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(root)) {
            const std::string file = name + "1.scsyndef";
            for(auto& e : std::filesystem::recursive_directory_iterator(
                    root, std::filesystem::directory_options::skip_permission_denied)) {
                if(e.is_regular_file() && e.path().filename().string() == file) {
                    found = e.path().parent_path().string();
                    break;
                }
            }
        }
    } catch(const std::exception& e) {
        ofLogWarning("scGrainBoxPoly") << "FX SynthDef search: " << e.what();
    }
    if(found.empty()) ofLogWarning("scGrainBoxPoly") << "FX: no folder with " << name << "1.scsyndef under " << SYNTHDEF_DIRECTORY;
    cache[name] = found;
    return found;
}
const char* kFxDefBase[3] = {"graphiceq", "Echo", "SpaceMaster"};

} // namespace

std::string scGrainBoxPoly::fxDefName(int stage) const {
    const int n = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    return std::string(kFxDefBase[std::max(0, std::min(2, stage))]) + ofToString(n);
}

// Loaded: serverManager loads the folders registered by requireFxSynthdefs()
// with every other SynthDef (boot, before its initialization /sync; NRT
// capture start, at score time 0) and, when registered on a running server,
// right away followed by a /sync; a stage waits only until scsynth answers it
// (areRequiredSynthdefsLoading) — no timer.
bool scGrainBoxPoly::fxStageReady(int /*stage*/, ofxSCServer* srv) const {
    if(!srv) return false;
    if(srv->isNRTCapturing()) return true;   // the score has the loads at time 0
    for(auto* sm : allServers)
        if(sm && sm->getServer() == srv) return !sm->areRequiredSynthdefsLoading();
    return true;
}

// Echo / SpaceMaster (~synthCreator defs, outside Defaults) and graphiceq:
// ask every serverManager to load their folders (idempotent).
void scGrainBoxPoly::requireFxSynthdefs() {
    // GrainBoxPoly's own folder (CompiledSynthdefs/GrainBoxPoly), wherever it
    // was copied under Supercollider/Synthdefs
    {
        const std::string own = gbpFindSynthdefFolder("GrainBoxPolyOut_");
        if(!own.empty()) for(auto* sm : allServers) if(sm) sm->requireSynthdefFolder(own);
    }
    for(int st = 0; st < FX_COUNT; st++) {
        const std::string folder = gbpFindSynthdefFolder(kFxDefBase[st]);
        if(folder.empty()) continue;
        for(auto* sm : allServers) if(sm) sm->requireSynthdefFolder(folder);
    }
}

std::vector<int> scGrainBoxPoly::computeFxPlan(ofxSCServer* srv) const {
    std::vector<int> plan;
    for(int st = 0; st < FX_COUNT; st++)
        if(fxOnP[st].get() && fxStageReady(st, srv)) plan.push_back(st);
    return plan;
}

// Plan + private buses (MAX_CHANNELS wide, so N Chan changes need no new bus)
void scGrainBoxPoly::planFx(ofxSCServer* srv) {
    FxState& st = fx[srv];
    st.plan = computeFxPlan(srv);
    fxPending = false;
    for(int s = 0; s < FX_COUNT; s++) if(fxOnP[s].get() && !fxStageReady(s, srv)) fxPending = true;
    // stale buses (server reboot): not ours any more
    for(auto*& b : st.buses) if(b && !gbpBusIsLive(srv, b)) { delete b; b = nullptr; }
    st.buses.erase(std::remove(st.buses.begin(), st.buses.end(), nullptr), st.buses.end());
    while(st.buses.size() < st.plan.size())
        st.buses.push_back(new ofxSCBus(RATE_AUDIO, MAX_CHANNELS, srv));
}

int scGrainBoxPoly::fxOutBus(ofxSCServer* srv, int planPos) const {
    auto f = fx.find(srv);
    if(f != fx.end() && planPos + 1 < (int)f->second.plan.size() && planPos + 1 < (int)f->second.buses.size())
        return f->second.buses[planPos + 1]->index;
    auto it = outputBuses.find(srv);
    return it != outputBuses.end() ? it->second : 0;
}

float scGrainBoxPoly::echoDelaySec() const {
    if(!echoBeatsP.get()) return echoDelayP.get();
    return std::max(0.01f, std::min(2.0f, echoBeatValP.get() * 60.0f / std::max(1.0f, effectiveBpm())));
}

void scGrainBoxPoly::sendFxParams(int stage, ofxSCSynth* s) {
    if(!s) return;
    const int n = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    auto vec = [n](float v) { return std::vector<float>(n, v); };
    switch(stage) {
        case FX_EQ:
            // graphiceqN: bNgain dB, bNfreq Hz, shelves bNslope, peaks bNrq = 1/Q
            for(int b = 0; b < 5; b++) {
                const std::string k = "b" + ofToString(b + 1);
                s->set(k + "gain", vec(eqGainP[b].get()));
                s->set(k + "freq", vec(eqFreqP[b].get()));
                // shelves: SC's argument is rs = 1/S (the editor's "slope" is S),
                // kept where the filter stays defined (no NaN at high gain)
                if(scEQEditor::isShelf(b))
                    s->set(k + "slope", vec(1.0f / scEQEditor::safeShelfSlope(eqShapeP[b].get(), eqGainP[b].get())));
                else                       s->set(k + "rq",    vec(1.0f / std::max(0.01f, eqShapeP[b].get())));
            }
            s->set("mix", eqMixP.get());
            break;
        case FX_ECHO:
            s->set("delay",  vec(echoDelaySec()));
            s->set("feed",   vec(echoFeedP.get()));
            s->set("cutoff", vec(echoCutoffP.get()));
            s->set("mix",    vec(echoMixP.get()));
            // OceanodeParameterDropdown.ar(\filtertype, ...): a plain "filtertype"
            // control of n values (the item index as a float)
            s->set("filtertype", vec((float)std::max(0, std::min(3, echoFilterP.get()))));
            s->set("resonance",  vec(echoResonanceP.get()));
            break;
        case FX_REVERB:
            s->set("size",     vec(revSizeP.get()));
            s->set("decay",    vec(revDecayP.get()));
            s->set("predelay", vec(revPredelayP.get()));
            s->set("lowpass",  vec(revLowpassP.get()));
            s->set("mix",      vec(revMixP.get()));
            s->set("position", vec(revPositionP.get()));
            s->set("spread",   vec(revSpreadP.get()));
            break;
        default: break;
    }
}

void scGrainBoxPoly::sendFxParamsAll() {
    for(auto& [srv, st] : fx)
        for(int stage : st.plan) if(st.synths[stage]) sendFxParams(stage, st.synths[stage]);
}

// Stage i reads bus i and writes bus i+1 (the last one: the output bus); each
// is added right after the previous one, the first right after playhead 1.
void scGrainBoxPoly::createFxSynths(ofxSCServer* srv) {
    if(!ph0().synths.count(srv) || !ph0().synths[srv]) return;
    FxState& st = fx[srv];
    ofxSCSynth* prev = engineLast(ph0(), srv);   // after engine 1's output synth
    if(!prev) return;
    for(int i = 0; i < (int)st.plan.size() && i < (int)st.buses.size(); i++) {
        const int stage = st.plan[i];
        if(st.synths[stage]) { st.synths[stage]->free(); delete st.synths[stage]; }
        auto* s = new ofxSCSynth(fxDefName(stage), srv);
        s->set("in",  st.buses[i]->index);
        s->set("out", fxOutBus(srv, i));
        sendFxParams(stage, s);
        s->createAndRun(3, prev->nodeID, getActive());   // addAfter
        st.synths[stage] = s;
        prev = s;
    }
}

void scGrainBoxPoly::freeFxSynths(ofxSCServer* srv) {
    auto f = fx.find(srv);
    if(f == fx.end()) return;
    for(auto*& s : f->second.synths) if(s) { s->free(); delete s; s = nullptr; }
}

void scGrainBoxPoly::freeFx(ofxSCServer* srv) {
    freeFxSynths(srv);
    auto f = fx.find(srv);
    if(f == fx.end()) return;
    for(auto* b : f->second.buses) if(b) { if(gbpBusIsLive(srv, b)) b->free(); delete b; }
    fx.erase(f);
}

// Runtime change (On toggles, N Chan, defs now loaded): re-route the playheads
// to the new chain input first, then replace the FX synths.
void scGrainBoxPoly::rebuildFx(ofxSCServer* srv) {
    if(!srv) return;
    const bool running = ph0().synths.count(srv) && ph0().synths[srv];
    if(!running) return;                 // planned in buildSynth when the graph is built
    planFx(srv);                         // reuses the existing buses
    for(auto& ph : playheads)
        if(ph->outSynths.count(srv) && ph->outSynths[srv]) ph->outSynths[srv]->set("out", outBusFor(*ph, srv));
    freeFxSynths(srv);
    createFxSynths(srv);
    if(fx.count(srv) && fx[srv].plan.empty()) {
        // no stage: release the private buses (playheads write to the output again)
        for(auto* b : fx[srv].buses) if(b) { if(gbpBusIsLive(srv, b)) b->free(); delete b; }
        fx[srv].buses.clear();
    }
}

void scGrainBoxPoly::rebuildFxAll() {
    for(auto* sm : allServers)
        if(sm && sm->getServer()) rebuildFx(sm->getServer());
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::update(ofEventArgs& /*args*/) {
    float dt = (float)ofGetLastFrameTime();

    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        // ── Reset syncGate to 0 one frame after it was pulsed to 1 ──────────
        if(ph.syncGateReset) {
            ph.syncGateReset = false;
            ph.syncGate.set(0);
        }
        // ── Same for the window's Trig button ────────────────────────────────
        if(ph.triggerReset) {
            ph.triggerReset = false;
            ph.trigger.set(vector<int>(phVoices(ph), 0));
        }
        // ── Upload envelope buffer if dirty ──────────────────────────────────
        // (once more shortly after an allocation: an edit sent while the
        // /b_alloc was still pending would have been lost)
        if(ph.envResendAt > 0.0f && ofGetElapsedTimef() >= ph.envResendAt) {
            ph.envResendAt = -1.0f;
            ph.envNeedsUpdate = true;
        }
        if(ph.envNeedsUpdate) {
            uploadEnvBuffers(ph);
            ph.envNeedsUpdate = false;
        }
    }

#if OFXOCEANODESC_HAS_TRANSPORT
    if(syncToTransportP.get()) updateTransportSync();
#endif

    // ── CPU: /grainTrig (and live peak) replies only while the window is
    //    shown (drawn last frame, not collapsed); pause muted playheads ───
    {
        const bool vis = windowDrawn;
        windowDrawn = false;
        if(vis != visOn) {
            visOn = vis;
            sendVis();
            if(!visOn) grainHighlights.clear();
        }
    }
    updatePausing();
    // Snapshot bank dialogs (outside ImGui, as scDynGenNode)
    if(snapLoadDialog) {
        snapLoadDialog = false;
        ofFileDialogResult r = ofSystemLoadDialog("Snapshots from a bank or an Oceanode preset file",
                                                  false, snapBankDir());
        if(r.bSuccess) loadSnapshotBank(r.filePath);
    }
    // Shared library changed by another GrainBox node / app: take it over
    if(ofGetElapsedTimef() >= snapLibCheckAt) {
        snapLibCheckAt = ofGetElapsedTimef() + 1.0f;
        try {
            if(snapSetTime() != snapLibTime) loadSnapshotLibrary();
        } catch(...) {}
    }
    // Morph glide (from the state at its start to the target slot)
    if(morphRun) {
        const float u = (ofGetElapsedTimef() - morphT0) / std::max(0.001f, morphDur);
        morphProgress = std::max(0.0f, std::min(1.0f, u));
        applyMorph(morphFrom, morphTo, morphCurve(morphProgress));
        if(u >= 1.0f) {
            morphRun   = false;
            activeSnap = morphTarget;
            snapRecalling = true;
            snapshotP.set(morphTarget + 1);
            snapRecalling = false;
        }
    }
    // A per-playhead vector whose connection was removed: back to one value
    // (its first), so no hidden per-playhead values remain
    for(auto& phPtr : playheads)
        for(auto* vp : perPlayheadVectors(*phPtr)) {
            const std::string nm = vp->getName();
            const bool con = hasInputConnection(nm);
            auto it = vecWasConnected.find(nm);
            if(it != vecWasConnected.end() && it->second && !con && vp->get().size() > 1)
                vp->set(vector<float>{vp->get()[0]});
            vecWasConnected[nm] = con;
        }
    // Time units: Beats follow the tempo; lengths changed -> ranges + re-send
    if(std::abs(effectiveBpm() - lastTimeBpm) > 1e-3f) {
        lastTimeBpm = effectiveBpm();
        if(timeUnitsCur == 2) timeDirty = true;
        if(echoBeatsP.get()) sendFxParamsAll();   // Echo delay in beats follows the tempo
        for(auto& ph : playheads) {
            sendDiv(*ph);   // Relative / ms interval -> grains per beat
            if(ph->liveInput && ph->liveLenBeats.get())   // Live Length in beats: new buffer
                ph->liveResizeAt = ofGetElapsedTimef() + 0.3f;
        }
    }
    if(timeDirty && !loadingPreset) {
        timeDirty = false;
        updateTimeRanges();
        if(timeUnitsCur != 0) for(auto& ph : playheads) sendOffsetParams(*ph);
        else                  for(auto& ph : playheads) sendDiv(*ph);   // region length
        sampleMsP.set(std::max(0.0f, (outRelOf(ph0()) - inRelOf(ph0())) * mainSample.durationSecs * 1000.0f));
    }
    for(auto& ph : playheads)
        if(ph->liveResizeAt > 0.0f && ofGetElapsedTimef() >= ph->liveResizeAt) {
            ph->liveResizeAt = -1.0f;
            if(ph->liveInput) resizeLiveBuffers(*ph);
        }
    if(visOn)
        for(auto& ph : playheads) if(ph->liveInput) updateLivePeaks(*ph);

    // FX stage waiting for its SynthDefs (/d_loadDir + /sync): add it once loaded
    if(fxPending) {
        bool changed = false;
        for(auto& [srv, st] : fx)
            if(computeFxPlan(srv) != st.plan) changed = true;
        if(changed) rebuildFxAll();
    }

    // ── Update grain highlight lifetimes (none arrive while hidden) ─────────
    for(auto it = grainHighlights.begin(); it != grainHighlights.end();) {
        it->lifeTime -= dt;
        if(it->lifeTime <= 0.0f)
            it = grainHighlights.erase(it);
        else
            ++it;
    }

    // Grain highlights are spawned by the /grainTrig OSC callback in configureSynth
    // (SendReply.ar fires at the exact trigger sample in SC — no C++ approximation needed)

    // Limit highlight buffer size to avoid accumulation at very high trigger rates
    while(grainHighlights.size() > 2048)
        grainHighlights.pop_front();
}

// ════════════════════════════════════════════════════════════════════════════
// Draw
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::draw(ofEventArgs& /*args*/) {
    if(showWindow.get())
        drawGrainBoxWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Envelope buffer
// ════════════════════════════════════════════════════════════════════════════

// Values are in 0..1 by construction (clamped), read by GrainBuf with linear
// interpolation across the grain: the envelope never adds gain.
void scGrainBoxPoly::computeEnvData(Playhead& ph) {
    if((int)ph.envData.size() != ENV_BUFFER_SIZE)
        ph.envData.resize(ENV_BUFFER_SIZE, 0.0f);

    float rawAtk = ph.envAttack.get();   // 0..1
    float rawRel = ph.envRelease.get();  // 0..1
    float tension = ph.envTension.get(); // -1..1

    // Normalise so attack + release never exceed 1.0
    float total = rawAtk + rawRel;
    float atk = (total > 1.0f) ? rawAtk / total : rawAtk;
    float rel = (total > 1.0f) ? rawRel / total : rawRel;

    // Preset shapes (Custom = the attack / release / tension editor below)
    const int shape = ph.envShape.get();
    if(shape > 0) {
        const int N = ENV_BUFFER_SIZE;
        const float pi = 3.14159265358979f;
        const float gSigma = 0.15f, gEdge = std::exp(-0.5f * (0.5f / gSigma) * (0.5f / gSigma));
        const float pk = 6.0f, pEdge = std::exp(-pk);
        for(int i = 0; i < N; i++) {
            const float t = (float)i / (float)(N - 1);
            float v = 1.0f;
            switch(shape) {
                case 1: v = 0.5f - 0.5f * std::cos(2.0f * pi * t); break;                        // Hann
                case 2: {                                                                         // Gaussian (0 at the ends)
                    const float z = (t - 0.5f) / gSigma;
                    v = (std::exp(-0.5f * z * z) - gEdge) / (1.0f - gEdge);
                } break;
                case 3: case 4: {                                                                 // Percussive / reversed
                    const float u = (shape == 3) ? t : 1.0f - t;
                    const float a = 0.02f;                                                        // 2 % linear attack
                    v = (u < a) ? u / a : (std::exp(-pk * (u - a) / (1.0f - a)) - pEdge) / (1.0f - pEdge);
                } break;
                case 5: {                                                                         // Trapezoid, 10 % fades
                    const float f = 0.1f;
                    v = std::min(1.0f, std::min(t / f, (1.0f - t) / f));
                } break;
                case 6: {                                                                         // Rectangular, 3-point fades
                    const int f = 3;
                    v = std::min(1.0f, (float)std::min(i, N - 1 - i) / (float)f);
                } break;
                default: break;
            }
            ph.envData[i] = std::max(0.0f, std::min(1.0f, v));
        }
        return;
    }

    // Tension → exponent: <0 = fast start/concave, 0 = linear, >0 = slow start/convex
    float exponent = std::pow(4.0f, tension);

    auto applyTension = [&](float seg) -> float {
        return std::pow(std::max(0.0f, seg), exponent);
    };

    for(int i = 0; i < ENV_BUFFER_SIZE; i++) {
        float t = (float)i / (float)(ENV_BUFFER_SIZE - 1);
        float val;
        if(atk > 0.0f && t < atk) {
            val = applyTension(t / atk);
        } else if(rel > 0.0f && t > (1.0f - rel)) {
            val = applyTension((1.0f - t) / rel);
        } else {
            val = 1.0f;
        }
        ph.envData[i] = std::max(0.0f, std::min(1.0f, val));
    }
}

void scGrainBoxPoly::uploadEnvBuffers(Playhead& ph) {
    for(auto& [srv, buf] : ph.envBufs) {
        if(!buf || !srv || !bufferIsLive(srv, buf)) continue;
        ofLogVerbose("scGrainBoxPoly") << "uploadEnvBuffers: playhead=" << (ph.index + 1)
            << " bufIdx=" << buf->index << " samples=" << (int)ph.envData.size();
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(buf->index);
        m.addIntArg(0);
        m.addIntArg((int)ph.envData.size());
        for(float v : ph.envData) m.addFloatArg(v);
        srv->sendMsg(m);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Sample loading
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::loadSample(const std::string& path) {
    loadSampleInto(mainSample, path);
}

// The browser loads into the selected playhead: its own sample in "Own"
// mode, otherwise the shared one (playhead 1's).
void scGrainBoxPoly::loadSampleForPlayhead(Playhead& ph, const std::string& path) {
    if(ph.index > 0 && ph.ownSample) loadSampleInto(ph.own, path);
    else                             loadSample(path);
}

void scGrainBoxPoly::loadSampleInto(SampleData& sd, const std::string& path) {
    if(path.empty()) return;
    timeDirty = true;        // absolute time units depend on the length
    sd.path = path;

    loadWaveformData(sd, path);   // sets numChannels and durationSecs from WAV header
    if(&sd == &mainSample)
        sampleMsP.set(std::max(0.0f, (ph0().outPoint.get() - ph0().inPoint.get()) * mainSample.durationSecs * 1000.0f));

    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        auto* srv = sm->getServer();

        // Free old per-channel buffers
        releaseSampleBuffers(sd, srv);

        // Allocate one mono buffer per sample channel
        for(int ch = 0; ch < sd.numChannels; ch++) {
            auto* buf = new ofxSCBuffer(0, 0, srv);
            // readChannel records path + channel on the buffer, so an NRT capture
            // can replay the load at score time zero (replayBuffersForNRT).
            buf->readChannel(path, {ch});
            sd.bufs[srv].push_back(buf);
        }

        // Every playhead playing this sample: new bufnums, or a new synth
        // when the sample-channel count (SynthDef variant) changed.
        for(auto& ph : playheads)
            if(&sampleOf(*ph) == &sd) refreshPlayheadSynth(*ph, srv);
    }
}

void scGrainBoxPoly::releaseSampleBuffers(SampleData& sd, ofxSCServer* srv) {
    auto it = sd.bufs.find(srv);
    if(it == sd.bufs.end()) return;
    for(auto* b : it->second) {
        if(b) { if(bufferIsLive(srv, b)) b->free(); delete b; }
    }
    it->second.clear();
}

// Shared <-> Own. Own starts from its last own sample, or a copy (same file,
// own buffers) of the shared one; back to Shared frees the own buffers (the
// path is kept for the next switch).
void scGrainBoxPoly::setOwnSample(Playhead& ph, bool own) {
    timeDirty = true;
    if(ph.index == 0 || ph.ownSample == own) return;
    ph.ownSample = own;
    if(own) {
        const std::string path = ph.own.path.empty() ? mainSample.path : ph.own.path;
        if(!path.empty()) loadSampleInto(ph.own, path);   // refreshes ph's synths
        else              refreshPlayheadSynths(ph);
    } else {
        refreshPlayheadSynths(ph);                         // back on the shared buffers first
        for(auto* sm : allServers)
            if(sm && sm->getServer()) releaseSampleBuffers(ph.own, sm->getServer());
    }
}

void scGrainBoxPoly::reloadSampleBuffersForServer(SampleData& sd, ofxSCServer* srv) {
    if(!srv) return;

    // Graph rebuilds (including NRT arming) only replace synths and audio
    // buses. Disk-backed sample buffers deliberately survive those rebuilds:
    // reloading them here would schedule /b_free, /b_allocReadChannel and the
    // new /s_new at the same b_latency timestamp. The read is asynchronous,
    // so the new GrainBox synth can start against an unavailable buffer and
    // remain silent. Only recreate buffers after a server reboot (where the
    // allocator no longer owns these objects), or when the cached set is
    // genuinely incomplete.
    auto existing = sd.bufs.find(srv);
    if(existing != sd.bufs.end() &&
       (int)existing->second.size() == sd.numChannels &&
       std::all_of(existing->second.begin(), existing->second.end(),
                   [srv](ofxSCBuffer* buffer){ return bufferIsLive(srv, buffer); })){
        return;
    }

    releaseSampleBuffers(sd, srv);

    if(sd.path.empty()) return;

    if(!std::filesystem::exists(sd.path)) {
        ofLogWarning("scGrainBoxPoly") << "Cannot reload sample after server rebuild: " << sd.path;
        return;
    }

    for(int ch = 0; ch < sd.numChannels; ch++) {
        auto* buf = new ofxSCBuffer(0, 0, srv);
        // readChannel records path + channel on the buffer, so an NRT capture
        // can replay the load at score time zero (replayBuffersForNRT).
        buf->readChannel(sd.path, {ch});
        sd.bufs[srv].push_back(buf);
        ofLogNotice("scGrainBoxPoly") << "Reloaded sample channel " << ch
                                  << " for rebuilt server: " << sd.path
                                  << " bufnum=" << buf->index;
    }
}


void scGrainBoxPoly::loadWaveformData(SampleData& sd, const std::string& path) {
    if(path.empty()) return;
    sd.peaks.assign(WAVEFORM_BINS, 0.0f);
    sd.durationSecs = 0.0f;

    std::ifstream f(path, std::ios::binary);
    if(!f.is_open()) return;

    // Read raw PCM bytes; this is a simplified peak extraction.
    // Assumes 16-bit signed PCM. For real use the WAV header must be skipped.
    // We seek past the first 44 bytes (standard WAV header).
    f.seekg(0, std::ios::end);
    auto fileSize = f.tellg();
    f.seekg(0, std::ios::beg);
    if(fileSize < 46) return;

    // Read WAV header to find audio data offset and bit depth
    char riff[4]; f.read(riff, 4);
    if(std::string(riff, 4) != "RIFF") return;
    f.seekg(22, std::ios::beg);
    int16_t numCh; f.read((char*)&numCh, 2);
    // Only GrainBox_1_N and GrainBox_2_N exist: files with more channels use
    // their first two (averaged in the SynthDef, like any stereo file).
    sd.numChannels = std::max(1, std::min((int)numCh, MAX_SAMPLE_CHANNELS));
    int32_t sampleRate = 0;
    f.seekg(24, std::ios::beg);
    f.read((char*)&sampleRate, 4);
    f.seekg(34, std::ios::beg);
    int16_t bitDepth; f.read((char*)&bitDepth, 2);

    // Find "data" chunk
    f.seekg(12, std::ios::beg);
    int dataOffset = -1;
    int dataSize   = 0;
    while(f.tellg() < (std::streampos)((std::streamoff)fileSize - 8)) {
        char id[4]; f.read(id, 4);
        int32_t sz; f.read((char*)&sz, 4);
        if(std::string(id, 4) == "data") {
            dataOffset = (int)f.tellg();
            dataSize   = sz;
            break;
        }
        f.seekg(sz, std::ios::cur);
    }
    if(dataOffset < 0 || dataSize <= 0) return;

    f.seekg(dataOffset, std::ios::beg);
    int bytesPerSample = (bitDepth == 16) ? 2 : (bitDepth == 24 ? 3 : 4);
    int totalFrames    = dataSize / (bytesPerSample * std::max(1, (int)numCh));
    if(totalFrames <= 0) return;

    sd.durationSecs = (sampleRate > 0) ? (float)totalFrames / (float)sampleRate : 0.0f;

    int framesPerBin = std::max(1, totalFrames / WAVEFORM_BINS);
    std::vector<char> buf(framesPerBin * bytesPerSample * std::max(1, (int)numCh));

    for(int bin = 0; bin < WAVEFORM_BINS; bin++) {
        int framePos = bin * framesPerBin;
        if(framePos >= totalFrames) { sd.peaks[bin] = 0.0f; continue; }
        f.seekg(dataOffset + framePos * bytesPerSample * std::max(1, (int)numCh), std::ios::beg);
        int frames = std::min(framesPerBin, totalFrames - framePos);
        f.read(buf.data(), (std::streamsize)(frames * bytesPerSample * std::max(1, (int)numCh)));
        float peak = 0.0f;
        for(int s = 0; s < frames; s++) {
            float sample = 0.0f;
            if(bitDepth == 16) {
                int16_t v = *(int16_t*)(buf.data() + s * bytesPerSample * std::max(1, (int)numCh));
                sample = std::abs(v / 32768.0f);
            } else if(bitDepth == 24) {
                int32_t v = 0;
                memcpy(&v, buf.data() + s * 3 * std::max(1, (int)numCh), 3);
                if(v & 0x800000) v |= 0xFF000000;
                sample = std::abs(v / 8388608.0f);
            } else {
                float v; memcpy(&v, buf.data() + s * bytesPerSample * std::max(1, (int)numCh), 4);
                sample = std::abs(v);
            }
            peak = std::max(peak, sample);
        }
        sd.peaks[bin] = std::min(1.0f, peak);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Preview
// ════════════════════════════════════════════════════════════════════════════

// (preview and folder listing: scSampleBrowser, shared with scRhythmBox)

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

// GrainBox_K_N: K = channels of the playhead's sample, N = its voice count
std::string scGrainBoxPoly::defNameFor(const Playhead& ph) const {
    const int k = std::max(1, std::min(MAX_SAMPLE_CHANNELS, sampleOf(ph).numChannels));
    // One family: the filter is an output stage inside every GrainBox_K_N
    (void)k;
    return "GrainBoxPolyCtl_" + ofToString(ctlCap(ph));   // sized to the playhead count
}

float scGrainBoxPoly::effectiveBpm() const {
#if OFXOCEANODESC_HAS_TRANSPORT
    if(syncToTransportP.get()) return syncBpm;
#endif
    return currentBpm;
}

void scGrainBoxPoly::sendAllParams(Playhead& ph) {
    const int nv = phVoices(ph);
    for(auto& [srv, s] : ph.synths) {
        if(!s) continue;
        setBufnums(ph, s, srv);

        s->set("dynamicdur",    (int)ph.dynamicDur.get());
        s->set("trigdur",       (int)ph.trigDur.get());
        s->set("autotrig",      (int)ph.autoTrig.get());
        s->set("uniquetrig",    (int)ph.uniqueTrig.get());
        s->set("uniquejit",     (int)ph.uniqueJit.get());
        s->set("bpm",           effectiveBpm());
        s->set("inpoint",       inRelOf(ph));
        s->set("outpoint",      outRelOf(ph));

        s->set("amp",              expandF(ph.amp.get(), nv));
        s->set("pitch",            expandF(ph.pitch.get(), nv));
        s->set("duration",         durSend(ph, nv));
        s->set("position",         posSend(ph, nv));
        s->set("panaz",            expandF(ph.panAz.get(), nv));
        s->set("reverse",          expandF(ph.reverse.get(), nv));
        setScaleArgs(ph, s);
        setLiveArgs(ph, s);
        s->set("vis",              visOn ? 1.0f : 0.0f);
        s->set("autotrigbeatdiv",  divSend(ph, nv));
        s->set("trigphase",        expandF(ph.trigPhase.get(), nv));
        s->set("maxoverlap",       (float)std::max(0, ph.maxGrains.get()));
        s->set("monotrig",         ph.monoTrig.get() ? 1.0f : 0.0f);
        s->set("delayglide",       ph.delayGlide.get() * 0.001f);
        s->set("overlap",          ph.overlap.get());
        s->set("chance",           withOffset(ph.chance.get(), nv, gChanceP.get()));
        s->set("posjit",           posAmtSend(ph, ph.posJit.get(), nv));
        s->set("pitchjit",         expandF(ph.pitchJit.get(), nv));
        s->set("durjit",           durAmtSend(ph, ph.durJit.get(), nv));
        s->set("ampjit",           expandF(ph.ampJit.get(), nv));
        s->set("panazjit",         expandF(ph.panAzJit.get(), nv));
        s->set("levels",           expandF(ph.levels.get(), phChannels(ph)));   // per speaker

        const bool filtered = synthIsFiltered(s);
        s->set("latchamp",      (int)ph.latchAmp.get());
        setMixArgs(ph, s);
        if(filtered) setFilterArgs(ph, s);
        for(int t = 0; t < NUM_LFO; t++) {
            if(t == LFO_CUT && !filtered) continue;
            s->set(kLfoSCShape[t],   expandF(ph.lfo[t].shape.get(), 1));
            s->set(kLfoSCSpeed[t],   vector<float>{(float)lfoSpeedBar(ph, t)});
            s->set(kLfoSCPhase[t],   expandF(ph.lfo[t].phase.get(), 1));
            s->set(kLfoSCQuant[t],   expandF(ph.lfo[t].quant.get(), 1));
            s->set(kLfoSCStr[t],     lfoStrSend(ph, t, 1));
            s->set(kLfoSCPow[t],     expandF(ph.lfo[t].pow.get(), 1));
            s->set(kLfoSCUnique[t],  (int)ph.uniqueLfo[t].get());
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BPM
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::setBpm(float bpm) {
    currentBpm = bpm;
    // In Sync To Transport mode the tempo arrives with the anchors instead.
    if(isSyncing()) return;
    for(auto& ph : playheads)
        for(auto& [srv, s] : ph->synths) if(s) s->set("bpm", bpm);
}

// Validate and reshape vector parameters to match current channel count
void scGrainBoxPoly::validateAndReshapeVectorParams() {
    int nch = numChannelsP.get();
    auto reshape = [nch](ofParameter<vector<float>>& p) {
        auto v = p.get();
        if((int)v.size() != nch) {
            if(v.empty()) v.assign(nch, 0.0f);
            else if((int)v.size() > nch) v.resize(nch);
            else v.insert(v.end(), nch - v.size(), v[0]);
            p.set(v);
        }
    };
    auto reshapeI = [nch](ofParameter<vector<int>>& p) {
        auto v = p.get();
        if((int)v.size() != nch) {
            if(v.empty()) v.assign(nch, 0);
            else if((int)v.size() > nch) v.resize(nch);
            else v.insert(v.end(), nch - v.size(), v[0]);
            p.set(v);
        }
    };
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        reshapeI(ph.trigger);
        reshape(ph.chance);
        reshape(ph.amp);
        reshape(ph.pitch);
        reshape(ph.duration);
        reshape(ph.position);
        reshape(ph.panAz);
        reshape(ph.posJit);
        reshape(ph.pitchJit);
        reshape(ph.durJit);
        reshape(ph.ampJit);
        reshape(ph.panAzJit);
        reshape(ph.levels);
        reshape(ph.autoTrigBeatDiv);
        reshape(ph.trigPhase);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Publish to Node (pattern of polyphonicArpeggiatorGUI / chordSequence)
// ════════════════════════════════════════════════════════════════════════════

namespace {
// Tolerant JSON readers for values saved by presetSave or, in old presets, by
// the framework (vector<float>/<int> as number or array; scalars through
// ofSerialize, i.e. as strings).
bool gbpJsonToFloatVector(const ofJson& v, vector<float>& out) {
    if(v.is_array()) {
        out.clear();
        for(const auto& e : v) out.push_back(e.is_number() ? e.get<float>() : (e.is_boolean() ? (e.get<bool>() ? 1.0f : 0.0f) : 0.0f));
        return !out.empty();
    }
    if(v.is_number()) { out = {v.get<float>()}; return true; }
    return false;
}
bool gbpJsonToIntVector(const ofJson& v, vector<int>& out) {
    vector<float> f;
    if(!gbpJsonToFloatVector(v, f)) return false;
    out.clear();
    for(float x : f) out.push_back((int)std::lround(x));
    return true;
}
template<typename T>
void gbpLoadScalar(const ofJson& v, ofParameter<T>& p) {
    try {
        if(v.is_string())                         p.fromString(v.get<std::string>());
        else if(v.is_boolean())                   p.set((T)(v.get<bool>() ? 1 : 0));
        else if(v.is_number())                    p.set((T)v.get<double>());
    } catch(...) {}
}

// ── Morph helpers: a value between two stored ones (only set when it changes)
bool gbpJsonToDouble(const ofJson& v, double& out) {
    try {
        if(v.is_number())  { out = v.get<double>(); return true; }
        if(v.is_boolean()) { out = v.get<bool>() ? 1.0 : 0.0; return true; }
        if(v.is_string()) {
            const std::string s = v.get<std::string>();
            if(s == "true")  { out = 1.0; return true; }
            if(s == "false") { out = 0.0; return true; }
            out = std::stod(s); return true;
        }
    } catch(...) {}
    return false;
}
template<typename T>
void gbpMorphScalar(ofParameter<T>& p, const ofJson& a, const ofJson& b, float t, bool sw, bool step, bool logScale) {
    double va, vb;
    if(!gbpJsonToDouble(a, va) || !gbpJsonToDouble(b, vb)) return;
    if constexpr(std::is_floating_point<T>::value) {
        double nv;
        if(step)                                   nv = sw ? vb : va;
        else if(logScale && va > 0.0 && vb > 0.0)  nv = std::exp(std::log(va) + (std::log(vb) - std::log(va)) * t);
        else                                       nv = va + (vb - va) * t;
        if(std::abs((double)p.get() - nv) > 1e-7) p.set((T)nv);
    } else {
        const T nv = (T)std::lround(sw ? vb : va);
        if(p.get() != nv) p.set(nv);
    }
}
void gbpMorphVector(ofParameter<vector<float>>& p, const ofJson& a, const ofJson& b, float t, bool sw, bool step, bool logScale) {
    vector<float> fa, fb;
    if(!gbpJsonToFloatVector(a, fa) || !gbpJsonToFloatVector(b, fb)) return;
    const size_t n = std::max(fa.size(), fb.size());
    vector<float> out(n);
    for(size_t i = 0; i < n; i++) {
        const double x = fa[i % fa.size()], y = fb[i % fb.size()];
        if(step)                               out[i] = (float)(sw ? y : x);
        else if(logScale && x > 0.0 && y > 0.0) out[i] = (float)std::exp(std::log(x) + (std::log(y) - std::log(x)) * t);
        else                                   out[i] = (float)(x + (y - x) * t);
    }
    const auto& cur = p.get();
    bool same = cur.size() == out.size();
    for(size_t i = 0; same && i < n; i++) same = std::abs(cur[i] - out[i]) < 1e-7f;
    if(!same) p.set(out);
}
// Glide in log: frequencies, speeds, trigger intervals (not a pitch)
bool gbpMorphLogKey(const std::string& k) {
    return k.find("Freq") != std::string::npos || k.find("Cutoff") != std::string::npos
        || k.find("LFOSpeed") == 0 || k == "Speed" || k.find("Interval") == 0;
}

// ── Typed value entry (right-click menu) ─────────────────────────────────────
// Numbers separated by commas, semicolons or spaces.
std::vector<double> gbpParseNumbers(const std::string& text) {
    std::vector<double> out;
    const char* c = text.c_str();
    while(*c) {
        while(*c && (*c == ',' || *c == ';' || std::isspace((unsigned char)*c))) c++;
        if(!*c) break;
        char* e = nullptr;
        double v = std::strtod(c, &e);
        if(e == c) { c++; continue; }   // skip anything that is not a number
        out.push_back(v);
        c = e;
    }
    return out;
}
std::string gbpNumText(double v) {
    char b[48]; std::snprintf(b, sizeof(b), "%.6g", v); return b;
}
template<typename T>
std::string gbpScalarText(ofParameter<T>& p) {
    if constexpr(std::is_same<T, bool>::value) return p.get() ? "1" : "0";
    else return gbpNumText((double)p.get());
}
template<typename T>
void gbpApplyScalarText(ofParameter<T>& p, const std::string& text) {
    auto v = gbpParseNumbers(text);
    if(v.empty()) return;
    double x = v[0];
    if constexpr(std::is_same<T, bool>::value) p.set(x != 0.0);
    else {
        x = std::max((double)p.getMin(), std::min((double)p.getMax(), x));
        if constexpr(std::is_integral<T>::value) p.set((T)std::lround(x));
        else p.set((T)x);
    }
}
template<typename T>
std::string gbpVectorText(ofParameter<vector<T>>& p) {
    std::string s;
    for(const auto& x : p.get()) { if(!s.empty()) s += ", "; s += gbpNumText((double)x); }
    return s;
}
// One value: all n voices; several: as typed. Clamped to the (first) min/max.
template<typename T>
void gbpApplyVectorText(ofParameter<vector<T>>& p, const std::string& text, int n) {
    auto v = gbpParseNumbers(text);
    if(v.empty()) return;
    const double mn = p.getMin().empty() ? -1e30 : (double)p.getMin()[0];
    const double mx = p.getMax().empty() ?  1e30 : (double)p.getMax()[0];
    vector<T> out;
    for(double x : v) {
        x = std::max(mn, std::min(mx, x));
        if constexpr(std::is_integral<T>::value) out.push_back((T)std::lround(x));
        else out.push_back((T)x);
    }
    (void)n;   // a single value stays single: it applies to every playhead
    p.set(out);
}
} // namespace

void scGrainBoxPoly::initializePublishableEditorParameters() {
    publishableEditorParameters.clear();

    auto base = [this](const std::string& key, int playhead, bool legacy, auto& parameter) {
        EditorPublishAction action;
        action.key = key;
        action.playhead = playhead;
        action.legacyNodeParameter = legacy;
        action.publish = [this, key, &parameter]() { return publishEditorParameterToNode(key, parameter); };
        action.unpublish = [this, key]() { return unpublishEditorParameterFromNode(key); };
        action.isPublished = [this, key]() { return isEditorParameterPublished(key); };
        action.isAvailableInNode = [this, &parameter]() { return getParameterGroup().contains(parameter.getEscapedName()); };
        return action;
    };
    auto regVF = [this, base](Playhead& ph, ofParameter<vector<float>>& p, bool legacy) {
        auto a = base(p.getEscapedName(), ph.index, legacy && ph.index == 0, p);
        const std::string key = a.key;
        a.saveValue = [&p, key](ofJson& j) {
            const auto& v = p.get();
            if(v.size() == 1) j[key] = v[0]; else j[key] = v;
        };
        a.loadValue = [&p](const ofJson& v) {
            vector<float> f;
            if(gbpJsonToFloatVector(v, f)) p.set(f);
        };
        Playhead* pph = &ph;
        a.isVector  = true;
        a.valueText = [&p]() { return gbpVectorText(p); };
        a.applyText = [this, &p, pph](const std::string& t) { gbpApplyVectorText(p, t, phVoices(*pph)); };
        const bool isInterval = &p == &ph.autoTrigBeatDiv;
        const bool logK = gbpMorphLogKey(key);
        a.morph = [&p, pph, logK, isInterval](const ofJson& x, const ofJson& y, float t, bool sw, bool step) {
            // a Pitch interval glides in semitones
            gbpMorphVector(p, x, y, t, sw, step, logK && !(isInterval && pph->intervalUnitCur == 3));
        };
        if(&p == &ph.position || &p == &ph.duration || &p == &ph.posJit || &p == &ph.durJit
           || &p == &ph.lfo[0].strength || &p == &ph.lfo[1].strength) a.unitClass = 1;
        if(isInterval) { a.unitClass = 2; a.unitKey = ph.intervalUnit.getEscapedName(); }
        publishableEditorParameters.push_back(std::move(a));
    };
    auto regVI = [this, base](Playhead& ph, ofParameter<vector<int>>& p, bool legacy) {
        auto a = base(p.getEscapedName(), ph.index, legacy && ph.index == 0, p);
        const std::string key = a.key;
        a.saveValue = [&p, key](ofJson& j) {
            const auto& v = p.get();
            if(v.size() == 1) j[key] = v[0]; else j[key] = v;
        };
        a.loadValue = [&p](const ofJson& v) {
            vector<int> f;
            if(gbpJsonToIntVector(v, f)) p.set(f);
        };
        Playhead* pph = &ph;
        a.isVector  = true;
        a.valueText = [&p]() { return gbpVectorText(p); };
        a.applyText = [this, &p, pph](const std::string& t) { gbpApplyVectorText(p, t, phVoices(*pph)); };
        publishableEditorParameters.push_back(std::move(a));
    };
    auto regS = [this, base](Playhead& ph, auto& p, bool legacy) {
        auto a = base(p.getEscapedName(), ph.index, legacy && ph.index == 0, p);
        const std::string key = a.key;
        // Same format as the framework's ofSerialize (a string)
        a.saveValue = [&p, key](ofJson& j) { j[key] = p.toString(); };
        a.loadValue = [&p](const ofJson& v) { gbpLoadScalar(v, p); };
        a.valueText = [&p]() { return gbpScalarText(p); };
        a.applyText = [&p](const std::string& t) { gbpApplyScalarText(p, t); };
        {
            const bool logK = gbpMorphLogKey(key);
            a.morph = [&p, logK](const ofJson& x, const ofJson& y, float t, bool sw, bool step) {
                gbpMorphScalar(p, x, y, t, sw, step, logK);
            };
            if((const void*)&p == (const void*)&ph.inPoint || (const void*)&p == (const void*)&ph.outPoint)
                a.unitClass = 1;
        }
        publishableEditorParameters.push_back(std::move(a));
    };

    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        // Controls panel — trigger
        regVI(ph, ph.trigger,          true);
        regS (ph, ph.autoTrig,         true);
        regVF(ph, ph.autoTrigBeatDiv,  true);
        regVF(ph, ph.trigPhase,        false);
        regS (ph, ph.maxGrains,        false);
        regS (ph, ph.intervalUnit,     false);
        regS (ph, ph.monoTrig,         false);
        regS (ph, ph.grainless,        false);
        regS (ph, ph.delayJump,        false);
        regS (ph, ph.delayGlide,       false);
        regS (ph, ph.overlap,          false);
        regS (ph, ph.liveLenBeats,     false);
        regVF(ph, ph.chance,           true);
        regS (ph, ph.uniqueTrig,       true);
        regS (ph, ph.syncGate,         true);
        // Grain
        regVF(ph, ph.amp,              true);
        regVF(ph, ph.pitch,            true);
        regS (ph, ph.dynamicDur,       true);
        regS (ph, ph.trigDur,          true);
        regVF(ph, ph.duration,         true);
        regVF(ph, ph.position,         true);
        regVF(ph, ph.panAz,            true);
        // Jitter
        regS (ph, ph.uniqueJit,        true);
        regVF(ph, ph.posJit,           true);
        regVF(ph, ph.pitchJit,         true);
        regVF(ph, ph.durJit,           true);
        regVF(ph, ph.ampJit,           true);
        regVF(ph, ph.panAzJit,         true);
        // Region / envelope / levels
        regS (ph, ph.inPoint,          true);
        regS (ph, ph.outPoint,         true);
        regS (ph, ph.envAttack,        true);
        regS (ph, ph.envRelease,       true);
        regS (ph, ph.envTension,       true);
        regVF(ph, ph.levels,           true);
        // Output channels (new: never node parameters before)
        regS (ph, ph.channels,         false);
        regS (ph, ph.numPlayheads,     false);
        regS (ph, ph.chanOffset,       false);
        // Mixing / filter / latch (new)
        regS (ph, ph.mute,             false);
        regS (ph, ph.solo,             false);
        regS (ph, ph.filterType,       false);
        regS (ph, ph.cutoff,           false);
        regS (ph, ph.filterQ,          false);
        regS (ph, ph.latchAmp,         false);
        // Reverse, scale, envelope shape, live input (new)
        regVF(ph, ph.reverse,          false);
        regS (ph, ph.scaleType,        false);
        regS (ph, ph.scaleRoot,        false);
        regS (ph, ph.scaleMask,        false);
        regS (ph, ph.envShape,         false);
        regS (ph, ph.liveLength,       false);
        regS (ph, ph.freeze,           false);
        regS (ph, ph.feedback,         false);
        regS (ph, ph.fbTone,           false);
        // LFO rows (the per-target Unique toggles were node parameters; the
        // LFO settings themselves were inspector parameters)
        for(int t = 0; t < NUM_LFO; t++) {
            regS (ph, ph.uniqueLfo[t],     t != LFO_CUT);
            regVF(ph, ph.lfo[t].shape,     false);
            regVF(ph, ph.lfo[t].speed,     false);
            regVF(ph, ph.lfo[t].phase,     false);
            regVF(ph, ph.lfo[t].quant,     false);
            regVF(ph, ph.lfo[t].strength,  false);
            regVF(ph, ph.lfo[t].pow,       false);
            regS (ph, ph.lfo[t].speedRel,  false);
        }
    }

    // Global tab (node-wide; playhead -1: never removed with a playhead)
    auto regG = [this, base](auto& p) {
        auto a = base(p.getEscapedName(), -1, false, p);
        const std::string key = a.key;
        a.saveValue = [&p, key](ofJson& j) { j[key] = p.toString(); };
        a.loadValue = [&p](const ofJson& v) { gbpLoadScalar(v, p); };
        a.valueText = [&p]() { return gbpScalarText(p); };
        a.applyText = [&p](const std::string& t) { gbpApplyScalarText(p, t); };
        const bool logK = gbpMorphLogKey(key);
        a.morph = [&p, logK](const ofJson& x, const ofJson& y, float t, bool sw, bool step) {
            gbpMorphScalar(p, x, y, t, sw, step, logK);
        };
        if((const void*)&p == (const void*)&gPosP || (const void*)&p == (const void*)&gDurP) a.unitClass = 1;
        publishableEditorParameters.push_back(std::move(a));
    };
    regG(transposeP);
    regG(volumeP);
    regG(speedP);
    regG(gPosP);
    regG(gDurP);
    regG(gChanceP);
    regG(timeUnitsP);
    regG(snapshotP);
    regG(morphP); regG(morphAP); regG(morphBP); regG(morphTimeP); regG(morphBeatsP);
    regG(morphCurveP); regG(morphSwitchP); regG(morphGlideP);
    for(int st = 0; st < FX_COUNT; st++) regG(fxOnP[st]);
    for(int b = 0; b < 5; b++) { regG(eqGainP[b]); regG(eqFreqP[b]); regG(eqShapeP[b]); }
    regG(eqMixP);
    regG(echoDelayP);  regG(echoBeatsP); regG(echoBeatValP); regG(echoFeedP);  regG(echoCutoffP); regG(echoMixP);
    regG(echoFilterP); regG(echoResonanceP);
    regG(revSizeP);    regG(revDecayP);  regG(revPredelayP); regG(revLowpassP); regG(revMixP);
    regG(revPositionP); regG(revSpreadP);
}

const scGrainBoxPoly::EditorPublishAction* scGrainBoxPoly::findPublishableEditorParameter(const std::string& key) const {
    auto it = std::find_if(publishableEditorParameters.begin(), publishableEditorParameters.end(),
                           [&](const EditorPublishAction& action) { return action.key == key; });
    return it == publishableEditorParameters.end() ? nullptr : &(*it);
}

bool scGrainBoxPoly::isEditorParameterPublished(const std::string& key) const {
    return publishedEditorParameterHandles.find(key) != publishedEditorParameterHandles.end();
}

bool scGrainBoxPoly::hasInputConnection(const std::string& name) {
    auto& g = getParameterGroup();
    if(!g.contains(name)) return false;
    try { return static_cast<ofxOceanodeAbstractParameter&>(g.get(name)).hasInConnection(); }
    catch(...) { return false; }
}

std::vector<ofParameter<vector<float>>*> scGrainBoxPoly::perPlayheadVectors(Playhead& ph) {
    return {&ph.amp, &ph.pitch, &ph.duration, &ph.position, &ph.panAz, &ph.reverse, &ph.chance,
            &ph.autoTrigBeatDiv, &ph.trigPhase, &ph.posJit, &ph.pitchJit, &ph.durJit,
            &ph.ampJit, &ph.panAzJit};
}

bool scGrainBoxPoly::parameterHasConnection(const std::string& key) const {
    auto it = publishedEditorParameterHandles.find(key);
    if(it == publishedEditorParameterHandles.end() || !it->second) return false;
    return it->second->hasInConnection() || it->second->hasOutConnections();
}

bool scGrainBoxPoly::publishEditorParameterToNode(const std::string& key) {
    const EditorPublishAction* action = findPublishableEditorParameter(key);
    if(action == nullptr) return false;
    bool changed = action->publish();
    if(changed) parameterGroupChanged.notify(this);
    return changed;
}

bool scGrainBoxPoly::unpublishEditorParameterFromNode(const std::string& key) {
    auto it = publishedEditorParameterHandles.find(key);
    if(it == publishedEditorParameterHandles.end()) return false;

    const std::string parameterName = it->second->getEscapedName();
    removeParameter(parameterName);
    publishedEditorParameterHandles.erase(it);
    publishedEditorParameterKeys.erase(std::remove(publishedEditorParameterKeys.begin(),
                                                   publishedEditorParameterKeys.end(),
                                                   key),
                                       publishedEditorParameterKeys.end());
    if(publishedEditorParameterHandles.empty() && publishedEditorSeparatorAdded) {
        removeSeparator("Published");
        publishedEditorSeparatorAdded = false;
    }
    parameterGroupChanged.notify(this);
    return true;
}

// Like the arpeggiator's, but incremental: parameters already published and
// still wanted are left alone (removing and re-adding them would drop the
// connections the container restored just before presetRecall), and a
// published parameter that has a connection is never removed.
void scGrainBoxPoly::syncPublishedEditorParameters(const std::vector<std::string>& keys) {
    std::vector<std::string> wanted;
    for(const std::string& key : keys) {
        if(findPublishableEditorParameter(key) == nullptr) continue;
        if(std::find(wanted.begin(), wanted.end(), key) == wanted.end()) wanted.push_back(key);
    }
    bool changed = false;
    const std::vector<std::string> current = publishedEditorParameterKeys;
    for(const std::string& key : current) {
        if(std::find(wanted.begin(), wanted.end(), key) != wanted.end()) continue;
        if(parameterHasConnection(key)) continue;
        changed = unpublishEditorParameterFromNode(key) || changed;
    }
    for(const std::string& key : wanted) {
        if(isEditorParameterPublished(key)) continue;
        const EditorPublishAction* action = findPublishableEditorParameter(key);
        if(action != nullptr) changed = action->publish() || changed;
    }
    if(changed) parameterGroupChanged.notify(this);
}

void scGrainBoxPoly::drawPublishedLabelUnderline(const std::string& key,
                                             const char* label,
                                             float frameWidth,
                                             bool checkbox) const {
    if(!isEditorParameterPublished(key) || label == nullptr || label[0] == '\0' || label[0] == '#') return;

    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const ImGuiStyle &style = ImGui::GetStyle();
    const float labelStartX = checkbox
        ? itemMin.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x
        : itemMin.x + std::max(0.0f, frameWidth) + style.ItemInnerSpacing.x;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const float labelEndX = std::min(labelStartX + textSize.x, itemMax.x);
    if(labelEndX <= labelStartX) return;

    const float lineY = itemMax.y - 2.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(labelStartX, lineY),
                                        ImVec2(labelEndX, lineY),
                                        ImGui::GetColorU32(ImGuiCol_Text),
                                        1.0f);
}

void scGrainBoxPoly::drawPublishedCurrentItemUnderline(const std::string& key) const {
    if(!isEditorParameterPublished(key)) return;

    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    if(itemMax.x <= itemMin.x) return;

    const float lineY = itemMax.y - 2.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(itemMin.x, lineY),
                                        ImVec2(itemMax.x, lineY),
                                        ImGui::GetColorU32(ImGuiCol_Text),
                                        1.0f);
}

void scGrainBoxPoly::drawNodePublishContextMenu(const std::string& key,
                                            const char* label,
                                            float frameWidth,
                                            bool checkbox) {
    drawPublishedLabelUnderline(key, label, frameWidth, checkbox);
    if(!ImGui::BeginPopupContextItem(("##PublishToNode_" + key).c_str())) return;

    drawNodePublishMenuItems(key);

    ImGui::EndPopup();
}

void scGrainBoxPoly::drawNodePublishMenuItems(const std::string& key, bool focusValue, const char* title) {
    const EditorPublishAction* action = findPublishableEditorParameter(key);
    ImGui::PushID(key.c_str());   // several parameters can share one menu (EQ bands)
    // Typed value: applied on Enter (closes the menu) or when the field loses focus
    if(action != nullptr && action->applyText && action->valueText) {
        static std::map<std::string, std::array<char, 512>> bufs;   // one per parameter
        auto& buf = bufs[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buf.data(), buf.size(), "%s", action->valueText().c_str());
        ImGui::TextDisabled("%s", title ? title
                                        : (action->isVector ? "Value (one for all voices, or a,b,c...)" : "Value"));
        ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
        if(appearing && focusValue) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##gbpTypedValue", buf.data(), buf.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
            action->applyText(std::string(buf.data()));
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(action == nullptr) {
        ImGui::TextDisabled("Not publishable");
    } else if(action->isPublished()) {
        ImGui::TextDisabled("Published in node");
        if(ImGui::MenuItem("Unpublish from Node")) {
            unpublishEditorParameterFromNode(key);
        }
    } else if(action->isAvailableInNode != nullptr && action->isAvailableInNode()) {
        ImGui::TextDisabled("Already in node");
    } else {
        if(ImGui::MenuItem("Publish to Node")) {
            publishEditorParameterToNode(key);
        }
    }
    ImGui::PopID();
}

// ════════════════════════════════════════════════════════════════════════════
// Preset
// ════════════════════════════════════════════════════════════════════════════
//
// Old presets/projects stored every GrainBox parameter as a node parameter,
// by name, and may connect to any of them. Now most are published on demand:
//   - values: every publishable parameter that is NOT currently in the node is
//     written by presetSave under its usual (escaped) name, and read back in
//     presetRecallAfterSettingParameters from that same name — so old presets'
//     values load whether or not the parameter is published.
//   - connections: the container restores connections right after
//     loadBeforeConnections. For a preset saved before publishing existed (no
//     "publishedEditorParameters" key), loadBeforeConnections publishes every
//     former node parameter present in the JSON, so any connection finds its
//     target; presetHasLoaded (after the connections) then unpublishes the
//     ones that ended up without a connection, leaving the minimal node.
//   - new presets: "publishedEditorParameters" lists the published keys; they
//     are published in loadBeforeConnections (before connections) and again
//     (incrementally) in presetRecallBeforeSettingParameters.

void scGrainBoxPoly::presetSave(ofJson& j) {
    Playhead& p0 = ph0();
    j["samplePath"]  = mainSample.path;
    j["browseDir"]   = browser.getDir();
    // Playheads 2+: sample mode and own sample ("sampleOwn 2", "samplePath 2"...)
    for(auto& ph : playheads) {
        if(ph->index == 0) continue;
        j["sampleOwn"  + ph->sfx] = ph->ownSample;
        j["samplePath" + ph->sfx] = ph->own.path;
    }
    // Live input, any playhead ("sampleLive", "sampleLive 2"...)
    for(auto& ph : playheads) j["sampleLive" + ph->sfx] = ph->liveInput;
    j["inpoint"]     = p0.inPoint.get();
    j["outpoint"]    = p0.outPoint.get();
    j["envAttack"]   = p0.envAttack.get();
    j["envRelease"]  = p0.envRelease.get();
    j["envTension"]  = p0.envTension.get();
    j["waveZoom"]    = waveZoom;
    j["waveH"]       = waveH / std::max(0.01f, ofxOceanodeShared::getZoomLevel());
    j["waveScroll"]  = waveScroll;
    j["browserW"]    = browserW;
    j["numPlayheads"] = (int)playheads.size();
    j["activePlayhead"] = activePlayhead;
    j["globalTab"]      = globalTab;
    j["publishedEditorParameters"] = publishedEditorParameterKeys;
    j["activeSnapshot"] = activeSnap;
    j["gbpSnapshots"]   = ofJson::array();
    for(int k = 0; k < 16; k++) j["gbpSnapshots"].push_back(snapUsed[k] ? snapSlots[k] : ofJson(nullptr));

    // Values of the parameters that are not in the node right now (the node
    // saves the others itself), under their usual names.
    for(const auto& action : publishableEditorParameters) {
        if(action.isAvailableInNode && action.isAvailableInNode()) continue;
        if(action.saveValue) action.saveValue(j);
    }

    // Playhead 1, legacy keys: audio vector params — framework auto-serialization
    // was unreliable for vector<float>; the first element is saved explicitly
    // (broadcast on restore).
    auto get0 = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.0f : p.get()[0];
    };
    j["amp"]         = get0(p0.amp);
    j["pitch"]       = get0(p0.pitch);
    j["duration"]    = get0(p0.duration);
    j["position"]    = get0(p0.position);
    j["panaz"]       = get0(p0.panAz);
    j["chance"]      = get0(p0.chance);
    j["interval"]    = get0(p0.autoTrigBeatDiv);   // current Time Units
    j["trigphase"]   = get0(p0.trigPhase);
    j["posjit"]      = get0(p0.posJit);
    j["pitchjit"]    = get0(p0.pitchJit);
    j["durjit"]      = get0(p0.durJit);
    j["ampjit"]      = get0(p0.ampJit);
    j["panazjit"]    = get0(p0.panAzJit);
    j["levels"]      = get0(p0.levels);

    for(int t = 0; t < NUM_LFO; t++) {
        std::string n = LFO_TARGET_NAMES[t];
        j["lfoShape" + n] = get0(p0.lfo[t].shape);
        j["lfoSpeed" + n] = get0(p0.lfo[t].speed);
        j["lfoPhase" + n] = get0(p0.lfo[t].phase);
        j["lfoQuant" + n] = get0(p0.lfo[t].quant);
        j["lfoStr"   + n] = get0(p0.lfo[t].strength);
    }
}

void scGrainBoxPoly::loadBeforeConnections(ofJson& j) {
    loadingPreset = true;    // cleared in presetRecallAfterSettingParameters / presetHasLoaded
#if OFXOCEANODESC_HAS_TRANSPORT
    deserializeParameter(j, syncToTransportP);
#endif
    if(j.contains("browseDir") && j["browseDir"].is_string()) {
        std::string d = j["browseDir"].get<std::string>();
        if(std::filesystem::exists(d)) browser.refresh(d);
    }
    // Playheads first: their published parameters must exist before connections.
    int n = 1;
    if(j.contains("numPlayheads") && j["numPlayheads"].is_number()) n = j["numPlayheads"].get<int>();
    setNumPlayheads(n);

    legacyAutoPublishedKeys.clear();
    if(j.contains("publishedEditorParameters") && j["publishedEditorParameters"].is_array()) {
        std::vector<std::string> keys;
        try { keys = j["publishedEditorParameters"].get<std::vector<std::string>>(); } catch(...) {}
        syncPublishedEditorParameters(keys);
    } else {
        // Saved before Publish to Node existed: every one of these was a node
        // parameter and may be the target of a saved connection.
        bool changed = false;
        for(const auto& action : publishableEditorParameters) {
            if(!action.legacyNodeParameter || !j.contains(action.key)) continue;
            if(action.isPublished() || (action.isAvailableInNode && action.isAvailableInNode())) continue;
            if(action.publish()) {
                legacyAutoPublishedKeys.push_back(action.key);
                changed = true;
            }
        }
        if(changed) parameterGroupChanged.notify(this);
    }
}

void scGrainBoxPoly::presetRecallBeforeSettingParameters(ofJson& j) {
    loadingPreset = true;    // values arrive in the preset's time unit (see timeUnitsP)
    int n = 1;
    if(j.contains("numPlayheads") && j["numPlayheads"].is_number()) n = j["numPlayheads"].get<int>();
    setNumPlayheads(n);
    if(j.contains("publishedEditorParameters") && j["publishedEditorParameters"].is_array()) {
        std::vector<std::string> keys;
        try { keys = j["publishedEditorParameters"].get<std::vector<std::string>>(); } catch(...) {}
        syncPublishedEditorParameters(keys);
    }
    // Old preset: leave the published set as loadBeforeConnections made it.
}

void scGrainBoxPoly::presetHasLoaded() {
    loadingPreset = false;
    timeDirty = true;
    // Old project: the connections are restored now; hide what nobody uses.
    for(const auto& key : legacyAutoPublishedKeys)
        if(!parameterHasConnection(key)) unpublishEditorParameterFromNode(key);
    legacyAutoPublishedKeys.clear();
}

void scGrainBoxPoly::presetRecallAfterSettingParameters(ofJson& j) {
    Playhead& p0 = ph0();

    // Features newer than the preset: back to their defaults (= today's sound),
    // so recalling an old preset on a node that had them on behaves like a
    // fresh node.
    {
        auto rs = [this, &j](auto& p, auto def) {
            const std::string k = p.getEscapedName();
            if(!j.contains(k) && !getParameterGroup().contains(k)) p.set(def);
        };
        auto rv = [this, &j](ofParameter<vector<float>>& p, float def) {
            const std::string k = p.getEscapedName();
            if(!j.contains(k) && !getParameterGroup().contains(k)) p.set(vector<float>{def});
        };
        rs(transposeP, 0.0f); rs(volumeP, 1.0f); rs(speedP, 1.0f);
        rs(timeUnitsP, 0);   // before the values: old presets are Relative
        rs(morphP, 0.0f); rs(morphAP, 0); rs(morphBP, 0); rs(morphTimeP, 4.0f); rs(morphBeatsP, true);
        rs(morphCurveP, 1); rs(morphSwitchP, 1); rs(morphGlideP, false);
        rs(gPosP, 0.0f); rs(gDurP, 0.0f); rs(gChanceP, 0.0f);
        for(int st = 0; st < FX_COUNT; st++) rs(fxOnP[st], false);
        for(int b = 0; b < 5; b++) rs(eqGainP[b], 0.0f);
        {   // band frequencies / shapes (not in presets before the EQ editor)
            const float eqFreqDef[5] = {80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
            for(int b = 0; b < 5; b++) { rs(eqFreqP[b], eqFreqDef[b]); rs(eqShapeP[b], 1.0f); }
        }
        rs(eqMixP, 1.0f);
        rs(echoDelayP, 0.2f); rs(echoBeatsP, false); rs(echoBeatValP, 0.75f); rs(echoFeedP, 0.5f); rs(echoCutoffP, 60.0f); rs(echoMixP, 0.35f);
        rs(revSizeP, 30.0f); rs(revDecayP, 4.0f); rs(revPredelayP, 20.0f);
        rs(revLowpassP, 10000.0f); rs(revMixP, 0.33f);
        rs(revPositionP, 0.8f); rs(revSpreadP, 0.15f);
        rs(echoFilterP, 0); rs(echoResonanceP, 0.2f);
        for(auto& ph : playheads) {
            rs(ph->mute, false); rs(ph->solo, false);
            rs(ph->filterType, 0); rs(ph->cutoff, 1000.0f); rs(ph->filterQ, 0.707f);
            rs(ph->latchAmp, false); rs(ph->latchCut, false);
            rs(ph->scaleType, 0); rs(ph->scaleRoot, 0); rs(ph->scaleMask, 4095);
            rs(ph->envShape, 0); rs(ph->liveLength, 4.0f); rs(ph->freeze, false);
            rs(ph->feedback, 0.0f); rs(ph->fbTone, 0.5f);
            rv(ph->reverse, 0.0f);
            rs(ph->channels, 0); rs(ph->chanOffset, 0); rs(ph->numPlayheads, 1);
            rs(ph->intervalUnit, 2); rs(ph->monoTrig, false);
            rs(ph->grainless, false); rs(ph->liveLenBeats, false);
            rs(ph->delayJump, false); rs(ph->delayGlide, 50.0f); rs(ph->overlap, 1.0f);
            rv(ph->autoTrigBeatDiv, defaultInterval(ph->intervalUnit.get()));
            rv(ph->trigPhase, 0.0f); rs(ph->maxGrains, 0);
            rs(ph->uniqueLfo[LFO_CUT], false);
            rv(ph->lfo[LFO_CUT].shape, 0.0f); rv(ph->lfo[LFO_CUT].speed, 1.0f);
            rv(ph->lfo[LFO_CUT].phase, 0.0f); rv(ph->lfo[LFO_CUT].quant, 0.0f);
            rv(ph->lfo[LFO_CUT].strength, 0.0f);
            for(int t = 0; t < NUM_LFO; t++) rv(ph->lfo[t].pow, 1.0f);   // newer than old presets
            for(int t = 0; t < NUM_LFO; t++) rs(ph->lfo[t].speedRel, false);
        }
    }

    // Parameters not in the node: values by their usual names (old presets
    // wrote all of them there as node parameters).
    for(const auto& action : publishableEditorParameters) {
        if(action.isAvailableInNode && action.isAvailableInNode()) continue;
        if(!action.loadValue || !j.contains(action.key)) continue;
        action.loadValue(j[action.key]);
    }

    if(j.contains("samplePath") && !j["samplePath"].get<std::string>().empty())
        loadSample(j["samplePath"].get<std::string>());
    if(j.contains("browseDir") && j["browseDir"].is_string()) {
        std::string d = j["browseDir"].get<std::string>();
        if(std::filesystem::exists(d)) browser.refresh(d);
    }
    // Live input (any playhead; "sampleLive", "sampleLive 2"...; old presets: off)
    for(auto& ph : playheads) {
        const std::string liveKey = "sampleLive" + ph->sfx;
        setLiveInput(*ph, j.contains(liveKey) && j[liveKey].is_boolean() && j[liveKey].get<bool>());
    }
    // Playheads 2+: own sample or shared (old presets: shared)
    for(auto& ph : playheads) {
        if(ph->index == 0) continue;
        const std::string ownKey = "sampleOwn" + ph->sfx, pathKey = "samplePath" + ph->sfx;
        bool own = false;
        if(j.contains(ownKey) && j[ownKey].is_boolean()) own = j[ownKey].get<bool>();
        std::string path;
        if(j.contains(pathKey) && j[pathKey].is_string()) path = j[pathKey].get<std::string>();
        if(own) {
            if(ph->ownSample) {
                if(!path.empty() && path != ph->own.path) loadSampleInto(ph->own, path);
            } else {
                ph->own.path = path;       // setOwnSample loads it (or the shared file if empty)
                setOwnSample(*ph, true);
            }
        } else {
            setOwnSample(*ph, false);
            ph->own.path = path;
        }
    }
    if(j.contains("inpoint"))    p0.inPoint.set(j["inpoint"].get<float>());
    if(j.contains("outpoint"))   p0.outPoint.set(j["outpoint"].get<float>());
    if(j.contains("envAttack"))  p0.envAttack.set(j["envAttack"].get<float>());
    if(j.contains("envRelease")) p0.envRelease.set(j["envRelease"].get<float>());
    if(j.contains("envTension")) p0.envTension.set(j["envTension"].get<float>());
    if(j.contains("waveZoom"))   waveZoom   = j["waveZoom"].get<float>();
    if(j.contains("waveH") && j["waveH"].is_number())
        waveH = std::max(40.0f, j["waveH"].get<float>()) * ofxOceanodeShared::getZoomLevel();
    if(j.contains("waveScroll")) waveScroll = j["waveScroll"].get<float>();
    if(j.contains("browserW"))   browserW   = j["browserW"].get<float>();
    if(j.contains("activePlayhead") && j["activePlayhead"].is_number())
        activePlayhead = std::max(0, std::min((int)playheads.size() - 1, j["activePlayhead"].get<int>()));
    globalTab = j.contains("globalTab") && j["globalTab"].is_boolean() && j["globalTab"].get<bool>();
    tabsInitialized = false;   // select the restored tab on the next draw
    for(auto& ph : playheads) {
        computeEnvData(*ph);
        ph->envNeedsUpdate = true;
    }

    // Playhead 1, legacy keys: restored as uniform vectors of the current
    // channel count (as before).
    int nch = numChannelsP.get();
    auto setVf = [nch](ofParameter<vector<float>>& p, const ofJson& root,
                       const std::string& key) {
        if(!root.contains(key)) return;
        float v = root[key].get<float>();
        (void)nch;
        p.set(vector<float>{v});   // one value: applies to every playhead
    };

    setVf(p0.amp,             j, "amp");
    setVf(p0.pitch,           j, "pitch");
    setVf(p0.duration,        j, "duration");
    setVf(p0.position,        j, "position");
    setVf(p0.panAz,           j, "panaz");
    setVf(p0.chance,          j, "chance");
    setVf(p0.autoTrigBeatDiv, j, "interval");
    setVf(p0.trigPhase,       j, "trigphase");
    setVf(p0.posJit,          j, "posjit");
    setVf(p0.pitchJit,        j, "pitchjit");
    setVf(p0.durJit,          j, "durjit");
    setVf(p0.ampJit,          j, "ampjit");
    setVf(p0.panAzJit,        j, "panazjit");
    setVf(p0.levels,          j, "levels");

    for(int t = 0; t < NUM_LFO; t++) {
        std::string n = LFO_TARGET_NAMES[t];
        setVf(p0.lfo[t].shape,    j, "lfoShape" + n);
        setVf(p0.lfo[t].speed,    j, "lfoSpeed" + n);
        setVf(p0.lfo[t].phase,    j, "lfoPhase" + n);
        setVf(p0.lfo[t].quant,    j, "lfoQuant" + n);
        setVf(p0.lfo[t].strength, j, "lfoStr"   + n);
    }
    // Snapshots: they live in the shared library, not in the preset. A preset
    // saved with snapshots before that fills the library only while it is empty.
    {
        bool any = false;
        for(bool u : snapUsed) any = any || u;
        if(!any && j.contains("gbpSnapshots") && j["gbpSnapshots"].is_array()) {
            const auto& a = j["gbpSnapshots"];
            for(int k = 0; k < std::min(16, (int)a.size()); k++)
                if(a[k].is_object()) { snapSlots[k] = a[k]; snapUsed[k] = true; any = true; }
            if(any) saveSnapshotLibrary();
        }
    }
    loadingPreset = false;
    timeDirty = true;        // ranges + one consistent send (update)
}

// ════════════════════════════════════════════════════════════════════════════
// Sync To Transport
// ════════════════════════════════════════════════════════════════════════════
#if OFXOCEANODESC_HAS_TRANSPORT

// A synth created while syncing holds (run=0) until the next anchor, which
// positions it at the current transport beat (hard for its playhead).
void scGrainBoxPoly::setSyncHoldArgs(Playhead& ph, ofxSCSynth* s) {
    if(!s || !syncToTransportP.get()) return;
    s->set("sync",       1.0f);
    s->set("run",        0.0f);
    s->set("anchorId",   anchorIdCounter);
    s->set("anchorHard", ph.hardCounter);
    s->set("anchorPos",  0.0f);
    s->set("anchorFire", 0.0f);
    ph.needHard = true;
    follower.requestAnchor();
}

void scGrainBoxPoly::handleSyncChanged(bool on) {
    // playhead synths: GrainBoxPolyV_ (free) <-> GrainBoxPolyVS_ (transport grid)
    for(auto& ph : playheads) refreshPlayheadSynths(*ph);
    if(on) {
        // Start from scratch: the first poll anchors every playhead (hard) at
        // the current transport position. "sync"=1 goes out with that anchor
        // (same timetag), so the synths switch clocks at the anchor instant.
        follower.reset();
        stepClock.reset();
        for(auto& ph : playheads) ph->needHard = true;
        syncBpm = getFrameTransportState().current.bpm;
    } else {
        // Back to the free clocks. They kept running in the background, so
        // they simply resume from wherever they are, at the node's BPM.
        // Timetagged no earlier than the last anchor sent (anchors go out
        // ahead of time), so a late anchor cannot override sync=0.
        scTransportSync::Anchor off;
        off.steadyTimeUs = std::max(getFrameTransportState().current.steadyTimeUs, lastAnchorUs);
        scTransportSync::AnchorScope scope(off);
        for(auto& ph : playheads)
            for(auto& [srv, s] : ph->synths)
                if(s) {
                    s->set("sync", 0.0f);
                    s->set("run",  0.0f);
                    s->set("bpm",  currentBpm);
                }
    }
}

void scGrainBoxPoly::updateTransportSync() {
    const auto frame = getFrameTransportState();
    for(const auto& a : follower.poll(frame))
        sendAnchor(a);
}

void scGrainBoxPoly::sendAnchor(const scTransportSync::Anchor& a) {
    const double beat = a.beat + (double)beatOffsetP.get();
    syncBpm = a.bpm;
    lastAnchorUs = std::max(lastAnchorUs, a.steadyTimeUs);
    anchorIdCounter += 1.0;

    // One clock (beats) for every playhead; anchorPos wraps at 2048 beats, a
    // multiple of the 32-beat grid block the SynthDef works in.
    const scTransportSync::StepAnchor sa = stepClock.make(beat, a.discontinuity, a.playing);

    bool anyPending = false;
    for(auto& phPtr : playheads) {
        Playhead& ph = *phPtr;
        // A synth not confirmed by /n_go yet would store these sets and flush
        // them later WITHOUT the timetag: skip it, anchor it (hard) next frame.
        bool pending = false;
        for(auto& [srv, s] : ph.synths) if(s && !s->isCreated()) pending = true;

        const bool hard = sa.hard || ph.needHard;
        if(hard) ph.hardCounter += 1.0;
        const bool fire = hard && sa.fire;
        ph.needHard = pending;
        anyPending = anyPending || pending;

        // LFO phases at this beat, exact in double: phase = beat * speed / 4
        // (bar-rate LFOs, 4 beats per bar), before the lfoPhase offset.
        const int n = 1;                // one LFO per engine: one anchor phase
        std::vector<float> lfoAnchor[NUM_LFO];
        for(int t = 0; t < NUM_LFO; t++) {
            lfoAnchor[t].resize(n, 0.0f);
            for(int i = 0; i < n; i++) {
                const double speed = lfoSpeedBar(ph, t);   // Smp: from the region length
                lfoAnchor[t][i] = (float)scTransportSync::positiveMod(beat * speed / 4.0, 1.0);
            }
        }

        scTransportSync::AnchorScope scope(a);
        for(auto& [srv, s] : ph.synths) {
            if(!s || !s->isCreated()) continue;
            s->set("sync",       1.0f);
            s->set("bpm",        a.bpm);
            s->set("run",        a.playing ? 1.0f : 0.0f);
            s->set("anchorPos",  sa.pos);
            s->set("anchorFire", fire ? 1.0f : 0.0f);
            s->set("anchorHard", ph.hardCounter);
            for(int t = 0; t < NUM_LFO; t++)
                if(t != LFO_CUT || synthIsFiltered(s)) s->set(kLfoSCAnchor[t], lfoAnchor[t]);
            s->set("anchorId",   anchorIdCounter);   // last: starts the new anchor
        }
    }
    if(anyPending) follower.requestAnchor();
}

#endif // OFXOCEANODESC_HAS_TRANSPORT


// ════════════════════════════════════════════════════════════════════════════
// GUI — main window
// ════════════════════════════════════════════════════════════════════════════

void scGrainBoxPoly::drawGrainBoxWindow() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 6.0f * zoom;   // general margin / gap

    ImGui::SetNextWindowSize(ImVec2(900 * zoom, 506 * zoom), ImGuiCond_FirstUseEver);
    std::string winTitle = "GrainBox " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    if(!ImGui::Begin(winTitle.c_str(), &open,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if(!open) showWindow = false;
        ImGui::End();
        return;
    }
    if(!open) showWindow = false;
    windowDrawn = true;    // shown and not collapsed: grain replies wanted (see update)

    activePlayhead = std::max(0, std::min((int)playheads.size() - 1, activePlayhead));

    float totalW  = ImGui::GetContentRegionAvail().x;
    float totalH  = ImGui::GetContentRegionAvail().y;
    if(totalW < 40.0f || totalH < 20.0f) {   // squeezed to nothing: draw nothing (ImGui
        ImGui::End();                        // asserts on zero-sized items)
        return;
    }
    float bw      = browserW * zoom;
    float dragW   = 5.0f * zoom;
    float rw      = totalW - bw - dragW;

    // Init waveH on first open
    if(waveH <= 0.0f) waveH = totalH * 0.52f;

    // ── Left: file browser (shared component, as in RhythmBox) ────────────
    ImGui::BeginChild("##gbpBrowser", ImVec2(bw, totalH), false);
    drawBrowser(bw, totalH);
    ImGui::EndChild();

    // ── Browser resize handle (RhythmBox's splitter) ──────────────────────
    ImGui::SameLine(0.0f, 0.0f);
    if(scSampleBrowser::drawSplitter("##gbpBrowserResize", browserW, dragW, totalH,
                                     80.0f, totalW / zoom * 0.5f, 1.0f / zoom)) {
        bw = browserW * zoom;
        rw = totalW - bw - dragW;
    }
    ImGui::SameLine(0.0f, 0.0f);

    // ── Right panel (single scrollable column) ────────────────────────────
    ImGui::BeginChild("##gbpRight", ImVec2(rw, totalH), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    float rightW = rw - ImGui::GetStyle().ScrollbarSize - m;

    // ── Waveform (resizable height); drop a browser file on it to load it
    //    into the selected playhead's sample ──────────────────────────────
    {
        // Snapshots: a square 4 x 4 matrix left of the waveform, always shown
        const float snapSide = std::max(40.0f * zoom, std::min(waveH, rightW * 0.3f));
        ImGui::BeginChild("##gbpSnapCell", ImVec2(snapSide, waveH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawSnapshotMatrix(snapSide);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, m);
        const float waveW = std::max(1.0f, rightW - snapSide - m);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gbpWave", ImVec2(waveW, std::max(1.0f, waveH)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if(globalTab) drawGlobalWaveform(dl, pos, waveW, waveH);
        else          drawWaveformPanel(dl, pos, waveW, waveH);
        std::string dropped;
        if(browser.acceptDropOnLastItem(dropped)) {
            if(globalTab) loadSample(dropped);       // Global: the shared sample
            else          loadSampleForPlayhead(*playheads[activePlayhead], dropped);
        }
    }

    // ── Resize handle, right under the waveform: a grip bar; drag up / down
    //    (the height is kept with the patch) ─────────────────────────────
    {
        const float handleH = 8.0f * zoom;
        const ImVec2 hpos   = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gbpWaveResize", ImVec2(std::max(1.0f, rightW), std::max(1.0f, handleH)));
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();
        const ImU32 hcol = (hov || act) ? gbpPal::splitterHot : gbpPal::splitter;
        ImDrawList* hdl = ImGui::GetWindowDrawList();
        const float cy = hpos.y + handleH * 0.5f;
        hdl->AddLine(ImVec2(hpos.x, cy), ImVec2(hpos.x + rightW, cy), hcol, (hov || act ? 2.0f : 1.0f) * zoom);
        // grip: three dots in the middle
        const float cx = hpos.x + rightW * 0.5f;
        for(int k = -1; k <= 1; k++)
            hdl->AddCircleFilled(ImVec2(cx + k * 7.0f * zoom, cy), 2.0f * zoom, hcol);
        if(hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(hov && !act) ImGui::SetTooltip("Drag to resize the waveform\n(double-click: default height)");
        if(act && ImGui::IsMouseDragging(0, 0.0f))
            waveH = std::max(40.0f * zoom, std::min(totalH * 0.9f, waveH + ImGui::GetIO().MouseDelta.y));
        if(hov && ImGui::IsMouseDoubleClicked(0)) waveH = totalH * 0.52f;
    }

    // ── Zoom + scroll controls strip ──────────────────────────────────────
    {
        float vis = 1.0f / waveZoom;
        if(ImGui::Button("-##wz")) {
            waveZoom   = std::max(1.0f, waveZoom / 1.5f);
            vis        = 1.0f / waveZoom;
            waveScroll = std::min(waveScroll, 1.0f - vis);
        }
        ImGui::SameLine(0, m * 0.5f);
        ImGui::Text("x%.1f", waveZoom);
        ImGui::SameLine(0, m * 0.5f);
        if(ImGui::Button("+##wz")) {
            float pivot = waveScroll + vis * 0.5f;
            waveZoom    = std::min(64.0f, waveZoom * 1.5f);
            vis         = 1.0f / waveZoom;
            waveScroll  = std::max(0.0f, std::min(1.0f - vis, pivot - vis * 0.5f));
        }
        ImGui::SameLine(0, m * 0.5f);
        if(ImGui::Button("fit##wz")) { waveZoom = 1.0f; waveScroll = 0.0f; vis = 1.0f; }
        ImGui::SameLine(0, m);
        float scrollMax = std::max(0.0f, 1.0f - vis);
        ImGui::SetNextItemWidth(rightW - ImGui::GetCursorPos().x - m);
        ImGui::SliderFloat("##wscr", &waveScroll, 0.0f, scrollMax);
    }

    // ── Playhead tabs: every control below belongs to the selected playhead
    drawPlayheadTabs();
    if(globalTab) {
        drawGlobalPanel(rightW);
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    Playhead& ph = *playheads[std::max(0, std::min((int)playheads.size() - 1, activePlayhead))];
    ImGui::PushID(ph.index);

    // ── Sample source + output channels of this playhead ──────────────────
    // (a tinted band in the engine's colour with an accent rule below: the
    //  engine-wide settings read as one block, above the per-grain controls)
    {
        const float padX = 6.0f * zoom, padY = 5.0f * zoom;
        ImDrawList* hdl = ImGui::GetWindowDrawList();
        hdl->ChannelsSplit(2);
        hdl->ChannelsSetCurrent(1);
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(b0.x + padX, b0.y + padY));
        ImGui::BeginGroup();
        drawPlayheadHeader(ph, rightW - 2.0f * padX);
        ImGui::EndGroup();
        const float yEnd = ImGui::GetItemRectMax().y + padY;
        hdl->ChannelsSetCurrent(0);
        hdl->AddRectFilled(b0, ImVec2(b0.x + rightW, yEnd), gbpAccent(ph.index, 38, 0.55f), 4.0f * zoom);
        hdl->AddLine(ImVec2(b0.x, yEnd), ImVec2(b0.x + rightW, yEnd), gbpAccent(ph.index, 200), 2.0f * zoom);
        hdl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(b0.x, yEnd + 6.0f * zoom));
        ImGui::Dummy(ImVec2(rightW, 0.0f));
    }

    // ── Bottom row: 4 equal cells separated by margins ────────────────────
    //   cell 0 = envelope  |  cells 1-3 = controls (3 columns)
    float cellW   = (rightW - 3.0f * m) / 4.0f;
    // as tall as the tallest controls column (measured last frame), at least
    // what the envelope needs
    // Grainless (Live delay taps): no envelope, the controls take the row
    const bool glRow = ph.liveInput && ph.grainless.get();
    float bottomH = ctrlContentH > 0.0f ? std::max(glRow ? 40.0f * zoom : 195.0f * zoom, ctrlContentH + 2.0f * zoom)
                                        : 345.0f * zoom;

    if(!glRow) {
        // Cell 0 — envelope (wrapped in child so SameLine works correctly)
        ImGui::BeginChild("##gbpEnvCell", ImVec2(cellW, bottomH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawEnvelopePanel(ph, cellW, bottomH);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, m);
    }

    // Cells 1-3 — grain controls (3 columns)
    drawControlsPanel(ph, glRow ? rightW : rightW - cellW - m, bottomH);

    // ── Modulation row ────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // LFO row: the controls + a preview 3x as tall as it used to be
    // (it was the room left in a 220 px row)
    float modH = 220.0f * zoom;
    if(lfoCtrlBottom > 0.0f) {
        const float oldPreview = std::max(16.0f * zoom, 220.0f * zoom - lfoCtrlBottom - 4.0f * zoom);
        modH = lfoCtrlBottom + 3.0f * oldPreview + 4.0f * zoom;
    }
    lfoCtrlBottom = 0.0f;   // measured again while drawing
    drawModRow(ph, rightW, modH);

    ImGui::PopID();
    ImGui::EndChild();
    ImGui::End();
}

// ── Playhead tabs ────────────────────────────────────────────────────────────
// One tab per playhead, "+" adds one (a full default set, same region as
// playhead 1). Only the last playhead can be closed (never playhead 1): the
// others keep their names, values and connections.

void scGrainBoxPoly::drawPlayheadTabs() {
    bool addRequested = false, removeRequested = false;
    // First draw: reopen the tab saved in the preset (ImGui would pick Global)
    if(!tabsInitialized) {
        tabsInitialized = true;
        if(globalTab) pendingGlobalSelect = true;
        else          pendingTabSelect = activePlayhead;
    }
    if(ImGui::BeginTabBar("##gbpPlayheads", ImGuiTabBarFlags_FittingPolicyScroll)) {
        const int numPh = (int)playheads.size();
        bool globalSel = false;
        {
            // Global: every playhead overlaid, global controls, mute/solo, FX
            ImGuiTabItemFlags gf = pendingGlobalSelect ? ImGuiTabItemFlags_SetSelected : 0;
            if(ImGui::BeginTabItem("Global##gbglobal", nullptr, gf)) {
                globalSel = true;
                ImGui::EndTabItem();
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("All engines overlaid; transpose, volume, speed, mute / solo, FX");
            pendingGlobalSelect = false;
        }
        for(int i = 0; i < numPh; i++) {
            // The playhead's accent: text, and a light wash on the tab itself
            ImGui::PushStyleColor(ImGuiCol_Text,             gbpAccent(i, 255));
            ImGui::PushStyleColor(ImGuiCol_TabActive,        gbpAccent(i, 70, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,       gbpAccent(i, 90, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, gbpAccent(i, 50, 0.55f));
            std::string label = "Engine " + ofToString(i + 1) + "##gbph" + ofToString(i);
            bool keepOpen = true;
            const bool closable = (i > 0 && i == numPh - 1);
            ImGuiTabItemFlags flags = (pendingTabSelect == i) ? ImGuiTabItemFlags_SetSelected : 0;
            if(ImGui::BeginTabItem(label.c_str(), closable ? &keepOpen : nullptr, flags)) {
                activePlayhead = i;
                ImGui::EndTabItem();
            }
            ImGui::PopStyleColor(4);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip(i == 0 ? "Engine 1: the original parameters"
                                         : "Parameters named \"... %d\" (publish them from their right-click menu)", i + 1);
            if(closable && !keepOpen) removeRequested = true;
        }
        pendingTabSelect = -1;
        globalTab = globalSel;
        if((int)playheads.size() < MAX_PLAYHEADS) {
            if(ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                addRequested = true;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a engine");
        }
        ImGui::EndTabBar();
    }
    if(addRequested && addPlayhead()) {
        pendingTabSelect = (int)playheads.size() - 1;
        activePlayhead   = pendingTabSelect;
        globalTab        = false;
    }
    if(removeRequested) removeLastPlayhead();
}

// ── File browser ─────────────────────────────────────────────────────────────

void scGrainBoxPoly::drawBrowser(float w, float h) {
    // Shared with scRhythmBox: click / Up / Down preview a file; double-click,
    // Enter, or drag onto the waveform loads it (browser.onActivate).
    browser.draw(w, h);
}

// ── Waveform panel ────────────────────────────────────────────────────────────
// Shows the selected playhead's sample: its region (in/out markers, dimmed
// outside), and the grains and position cursors of every playhead playing
// that sample, each in its playhead's accent colour.

void scGrainBoxPoly::drawWaveformPanel(ImDrawList* dl, ImVec2 pos, float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    Playhead& ph = *playheads[std::max(0, std::min((int)playheads.size() - 1, activePlayhead))];
    const SampleData& sd = sampleOf(ph);   // the selected playhead's sample
    const ImVec2 end(pos.x + w, pos.y + h);

    // Background
    dl->AddRectFilled(pos, end, gbpPal::canvas, 3.0f * zoom);

    bool active  = ImGui::IsItemActive();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // ── Setup variables ───────────────────────────────────────────────────
    float visStart = waveScroll;
    float visEnd   = waveScroll + (1.0f / waveZoom);
    float visW     = std::max(0.0001f, visEnd - visStart);
    float midY     = pos.y + h * 0.5f;

    // In / Out as positions of the view (relative; Live: age)
    float inP  = relToDisplay(ph, inRelOf(ph));
    float outP = relToDisplay(ph, outRelOf(ph));
    float inNorm  = (inP  - visStart) / visW;
    float outNorm = (outP - visStart) / visW;
    float inX     = pos.x + inNorm  * w;
    float outX    = pos.x + outNorm * w;

    // Selected region: a subtle wash of the playhead's accent
    {
        float rx0 = std::max(pos.x, inX), rx1 = std::min(end.x, outX);
        if(rx1 > rx0) dl->AddRectFilled(ImVec2(rx0, pos.y), ImVec2(rx1, end.y), gbpAccent(ph.index, 14));
    }
    dl->AddLine(ImVec2(pos.x, midY), ImVec2(end.x, midY), gbpPal::grid);

    // ── Grain highlights (every playhead playing this sample), behind the
    //    waveform, in the playhead's accent ─────────────────────────────────
    const float bufDur = (sd.durationSecs > 0.0f) ? sd.durationSecs : 1.0f;
    for(const auto& gh : grainHighlights) {
        if(gh.playhead < 0 || gh.playhead >= (int)playheads.size()) continue;
        if(&sampleOf(*playheads[gh.playhead]) != &sd) continue;
        if(playheads[gh.playhead]->liveInput && playheads[gh.playhead]->grainless.get()) continue;   // tap line (below)
        // gh.position is SC's usedPos (0..1 of the buffer; Live: of the delay range)
        float normPos = (relToDisplay(*playheads[gh.playhead], gh.position) - visStart) / visW;
        if(normPos < -0.1f || normPos > 1.1f) continue;

        // grain duration (seconds) → fraction of buffer → fraction of visible window → pixels
        float grainWidthPx = (gh.duration / bufDur / visW) * w;
        grainWidthPx = std::max(2.0f * zoom, std::min(w, grainWidthPx));

        float x     = pos.x + normPos * w;
        float x2    = std::min(end.x, x + grainWidthPx);
        if(x2 <= pos.x || x >= end.x) continue;
        float fade  = gh.lifeTime / gh.maxLife;
        float alpha = fade * (0.3f + 0.7f * gh.amp);   // quiet grains stay visible
        dl->AddRectFilled(ImVec2(std::max(pos.x, x), pos.y), ImVec2(x2, end.y),
                          gbpAccent(gh.playhead, (int)(alpha * 110), gbpVoiceBrightness(gh.channel)));
    }

    // ── Waveform: light neutral grey inside the region, faint outside ─────
    if(!sd.peaks.empty()) {
        int bins = (int)sd.peaks.size();
        for(int i = 0; i < (int)w; i++) {
            float norm    = visStart + ((float)i / w) * visW;
            int   binIdx  = std::max(0, std::min((int)(norm * bins), bins - 1));
            float peak    = sd.peaks[binIdx];
            // Live: signed max / min (a real waveform shape); files: +-|peak|
            const bool  signedMM = !sd.lows.empty();
            float low     = signedMM ? sd.lows[std::min(binIdx, (int)sd.lows.size() - 1)] : -peak;
            if(!signedMM && peak <= 0.0f) continue;
            if(signedMM && peak <= low) { peak += 0.002f; low -= 0.002f; }   // silence: a hairline
            float px      = pos.x + (float)i;
            float py      = midY - peak * (h * 0.45f);
            float py2     = midY - low  * (h * 0.45f);
            const bool inside = norm >= inP && norm <= outP;
            // the selected playhead's accent (playhead 1: its blue, like its grains)
            dl->AddLine(ImVec2(px, py), ImVec2(px, py2),
                        gbpAccent(ph.index, inside ? gbpPal::waveAlpha : gbpPal::waveAlphaDim));
        }
    }

    // Out-of-region areas dimmed
    if(inNorm > 0.0f)
        dl->AddRectFilled(pos, ImVec2(std::min(end.x, inX), end.y), gbpPal::outside);
    if(outNorm < 1.0f)
        dl->AddRectFilled(ImVec2(std::max(pos.x, outX), pos.y), end, gbpPal::outside);

    // ── Grainless delay taps: a pink line at each tap's current place (over
    //    the waveform; reported 20 x / s, so it follows the Pos LFO / glide)
    for(const auto& gh : grainHighlights) {
        if(gh.playhead < 0 || gh.playhead >= (int)playheads.size()) continue;
        Playhead& gp = *playheads[gh.playhead];
        if(!(gp.liveInput && gp.grainless.get()) || &sampleOf(gp) != &sd) continue;
        const float nx = (relToDisplay(gp, gh.position) - visStart) / visW;
        if(nx < 0.0f || nx > 1.0f) continue;
        const float x = pos.x + nx * w;
        const float a = std::max(0.0f, std::min(1.0f, gh.lifeTime / gh.maxLife));
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), IM_COL32(255, 90, 190, (int)(70 + 185 * a)), 2.0f * zoom);
    }

    // ── Per-voice position cursors of every playhead on this sample
    //    (selected one on top) ──────────────────────────────────────────────
    auto drawCursors = [&](Playhead& p, bool selected) {
        const int   nv    = phVoices(p);
        const auto  positions = posSend(p, nv);        // as sent (any time unit, + global offset)
        const float pIn   = inRelOf(p);
        const float pSpan = std::max(0.001f, outRelOf(p) - pIn);
        for(int i = 0; i < nv; i++) {
            float pp    = positions.empty() ? 0.0f : positions[i % (int)positions.size()];
            pp          = std::max(0.0f, std::min(1.0f, pp));   // as the SynthDef clips it
            float absP  = relToDisplay(p, pIn + pp * pSpan);
            float normP = (absP - visStart) / visW;
            if(normP < 0.0f || normP > 1.0f) continue;
            float x = pos.x + normP * w;
            dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y),
                        gbpAccent(p.index, selected ? 200 : 90, gbpVoiceBrightness(i)),
                        1.0f * zoom);
        }
    };
    for(auto& p : playheads)
        if(p.get() != &ph && &sampleOf(*p) == &sd) drawCursors(*p, false);
    drawCursors(ph, true);

    // ── InPoint / OutPoint markers (selected playhead): accent lines with
    //    small flags at the top (in: flag to the right, out: to the left) ───
    const ImU32 markCol = gbpAccent(ph.index, 220);
    const float flagW = 7.0f * zoom, flagH = 9.0f * zoom;
    if(inNorm >= 0.0f && inNorm <= 1.0f) {
        dl->AddLine(ImVec2(inX, pos.y), ImVec2(inX, end.y), markCol, 1.5f * zoom);
        dl->AddTriangleFilled(ImVec2(inX, pos.y), ImVec2(inX + flagW, pos.y), ImVec2(inX, pos.y + flagH), markCol);
    }
    if(outNorm >= 0.0f && outNorm <= 1.0f) {
        dl->AddLine(ImVec2(outX, pos.y), ImVec2(outX, end.y), markCol, 1.5f * zoom);
        dl->AddTriangleFilled(ImVec2(outX, pos.y), ImVec2(outX - flagW, pos.y), ImVec2(outX, pos.y + flagH), markCol);
    }

    // Hit-test for handle dragging
    constexpr float HANDLE_R = 12.0f;
    bool nearIn  = (std::abs(mouse.x - inX)  < HANDLE_R * zoom && mouse.y > pos.y && mouse.y < end.y);
    bool nearOut = (std::abs(mouse.x - outX) < HANDLE_R * zoom && mouse.y > pos.y && mouse.y < end.y);
    if((nearIn || nearOut) && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if(active) {
        if(wavDragMode == WavDrag::None) {
            if(ImGui::IsMouseClicked(0)) {
                if(nearIn)  wavDragMode = WavDrag::InPoint;
                else if(nearOut) wavDragMode = WavDrag::OutPoint;
            }
        }
        if(ImGui::IsMouseDragging(0)) {
            // Relative drag (Shift: 10x slower): pressing / releasing Shift
            // never makes the handle jump
            // (in view coordinates, then back to the parameter's time unit)
            const float cur   = wavDragMode == WavDrag::InPoint ? inP : outP;
            const float speed = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
            float absVal = cur + ImGui::GetIO().MouseDelta.x / std::max(1.0f, w) * visW * speed;
            absVal = std::max(0.0f, std::min(1.0f, absVal));
            float rel = displayToRel(ph, absVal);
            auto toUnit = [&](float r) -> float {
                if(timeUnitsCur == 0) return r;
                const float L = lengthSec(ph);
                return (float)secToUnit(ph.liveInput ? liveGap(ph) + r * std::max(1e-6f, L - 2.0f * liveGap(ph)) : r * L);
            };
            if(wavDragMode == WavDrag::InPoint)
                ph.inPoint.set(toUnit(std::min(rel, outRelOf(ph) - 0.001f)));
            else if(wavDragMode == WavDrag::OutPoint)
                ph.outPoint.set(toUnit(std::max(rel, inRelOf(ph) + 0.001f)));
        }
    }
    if(!ImGui::IsMouseDown(0))
        wavDragMode = WavDrag::None;

    // Label: file name, and whose sample it is
    {
        std::string label = sd.path.empty() ? std::string("no file loaded")
                                            : std::filesystem::path(sd.path).filename().string();
        if(ph.liveInput)
            label = "Live input   (left: now, right: " + ofToString(sd.durationSecs, 1) + " s ago)"
                  + (ph.freeze.get() ? "   FROZEN" : "");
        else if(ph.index > 0)
            label += ph.ownSample ? "   (own sample of Engine " + ofToString(ph.index + 1) + ")"
                                  : "   (shared: Engine 1)";
        // Live: the write head is age 0, the left edge of the unzoomed view
        if(ph.liveInput) {
            const float hx = pos.x + (0.0f - visStart) / visW * w;
            if(hx >= pos.x && hx <= end.x)
                dl->AddLine(ImVec2(hx, pos.y), ImVec2(hx, end.y), gbpPal::handleLight, 2.0f * zoom);
        }
        dl->AddText(ImVec2(pos.x + 6 * zoom, pos.y + 4 * zoom), gbpPal::textDim, label.c_str());
    }

    // Drop target feedback while a browser file is dragged over the waveform
    const bool dropHover = browser.isDraggingFile() &&
                           ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    dl->AddRect(pos, end, dropHover ? gbpAccent(ph.index, 230) : gbpPal::border, 3.0f * zoom, 0,
                dropHover ? 2.0f * zoom : 1.0f);
    if(dropHover) {
        std::string msg = "Drop to load into " +
            std::string((ph.index > 0 && ph.ownSample) ? "Engine " + ofToString(ph.index + 1)
                                                       : std::string("the shared sample"));
        ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, midY - ts.y * 0.5f), gbpPal::handleLight, msg.c_str());
    }
}

// ── Playhead header row ───────────────────────────────────────────────────────
// Sample source (playheads 2+: Shared = playhead 1's sample, Own = loaded from
// the browser while this tab is selected) and output channels: Channels (0 =
// follow N Chan) and Offset (first output channel), both publishable.

void scGrainBoxPoly::drawPlayheadHeader(Playhead& ph, float /*w*/) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    const float gap = 14.0f * zoom;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Source");
    ImGui::SameLine();
    if(ph.index == 0) {
        // Playhead 1: its sample (shared by the others in Shared mode) or Live
        int mode = ph.liveInput ? 1 : 0;
        const char* items[] = {"Sample (shared)", "Live input"};
        ImGui::SetNextItemWidth(135.0f * zoom);
        if(ImGui::Combo("##gbpSampleMode", &mode, items, 2)) setLiveInput(ph, mode == 1);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Sample: the loaded sample (Shared engines play it too).\n"
                              "Live input: a rolling recording of the node's In port;\n"
                              "Position = delay behind the input: Relative 0..1 = 5 ms .. Len - 5 ms (Grainless: 0 .. Len),\n"
                              "ms / Beats (Time Units, Global tab) = the delay itself.");
    } else {
        int mode = ph.liveInput ? 2 : (ph.ownSample ? 1 : 0);
        const char* items[] = {"Shared (Engine 1)", "Own", "Live input"};
        ImGui::SetNextItemWidth(150.0f * zoom);
        if(ImGui::Combo("##gbpSampleMode", &mode, items, 3)) {
            if(mode == 2) setLiveInput(ph, true);
            else { setLiveInput(ph, false); setOwnSample(ph, mode == 1); }
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Shared: plays Engine 1's sample.\nOwn: its own sample; with this tab selected,\n"
                              "loading from the browser loads into this engine.\n"
                              "Live input: a rolling recording of the node's In port (first channel);\n"
                              "Position = delay behind the input: Relative 0..1 = 5 ms .. Len - 5 ms (Grainless: 0 .. Len),\n"
                              "ms / Beats (Time Units, Global tab) = the delay itself (e.g. 500 ms = a\n"
                              "grain delay of 500 ms at pitch 0).");
    }
    if(ph.liveInput) {
        // Live: buffer length + freeze
        ImGui::SameLine(0, 8.0f * zoom);
        const std::string lk = ph.liveLength.getEscapedName();
        ImGui::TextUnformatted("Len");
        drawPublishedCurrentItemUnderline(lk);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f * zoom);
        float len = ph.liveLength.get();
        const bool lb = ph.liveLenBeats.get();
        if(gbpSliderFloat("##gbpLiveLen", &len, 1.0f, lb ? 64.0f : 30.0f, lb ? "%.0f b" : "%.1f s"))
            ph.liveLength.set(lb ? std::round(len) : len);
        drawNodePublishContextMenu(lk);
        ImGui::SameLine(0, 2.0f * zoom);
        if(ImGui::SmallButton(lb ? "b##gbpLenU" : "s##gbpLenU")) ph.liveLenBeats.set(!lb);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Live Length in seconds (s) or beats (b, follows the tempo).\n"
                              "A new length starts an empty buffer.");
        drawNodePublishContextMenu(ph.liveLenBeats.getEscapedName());
        ImGui::SameLine(0, 6.0f * zoom);
        bool gl = ph.grainless.get();
        if(ImGui::Checkbox("Grainless##gbpGl", &gl)) ph.grainless.set(gl);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("A plain delay instead of grains: every playhead is a delay tap.\n"
                              "Position = delay (0 = now, 1 = the whole Live Length, within In / Out;\n"
                              "ms / Beats Time Units: the delay itself). Amp, Pan and their LFOs apply;\n"
                              "a moving Position glides like tape. Pitch, Duration and triggers do nothing.");
        drawNodePublishContextMenu(ph.grainless.getEscapedName(), "Grainless##gbpGl", 0.0f, true);
        if(gl) {
            // how a new delay is reached: Glide (tape slide) / Jump (crossfade)
            ImGui::SameLine(0, 4.0f * zoom);
            ImGui::SetNextItemWidth(62.0f * zoom);
            int dm = ph.delayJump.get() ? 1 : 0;
            const char* dms[] = {"Glide", "Jump"};
            if(ImGui::Combo("##gbpDlyMode", &dm, dms, 2)) ph.delayJump.set(dm == 1);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("How a tap reaches a new delay:\n"
                                  "Glide: slides there like tape (bends the pitch while moving;\n"
                                  "  0 ms = an instant jump, which clicks).\n"
                                  "Jump: a second read head starts at the new delay and the two\n"
                                  "  crossfade: no pitch bend, no click. For stepped / sequenced\n"
                                  "  positions; a continuous LFO moves in fade-length steps.");
            drawNodePublishContextMenu(ph.delayJump.getEscapedName());
            ImGui::SameLine(0, 2.0f * zoom);
            ImGui::SetNextItemWidth(70.0f * zoom);
            float gt = ph.delayGlide.get();
            if(gbpSliderFloat("##gbpDlyGlide", &gt, 0.0f, 2000.0f, ph.delayJump.get() ? "fade %.0f ms" : "%.0f ms",
                              ImGuiSliderFlags_Logarithmic)) ph.delayGlide.set(gt);
            drawPublishedCurrentItemUnderline(ph.delayGlide.getEscapedName());
            drawNodePublishContextMenu(ph.delayGlide.getEscapedName());
        }
        ImGui::SameLine(0, 6.0f * zoom);
        bool fz = ph.freeze.get();
        if(ImGui::Checkbox("Freeze##gbpFrz", &fz)) ph.freeze.set(fz);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Stop recording: the buffer holds its content");
        drawNodePublishContextMenu(ph.freeze.getEscapedName(), "Freeze##gbpFrz", 0.0f, true);
        // Feedback: the engine's sound (before the node FX) back into its buffer
        ImGui::SameLine(0, 6.0f * zoom);
        ImGui::TextUnformatted("Fb");
        drawPublishedCurrentItemUnderline(ph.feedback.getEscapedName());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * zoom);
        {
            float fbv = ph.feedback.get();
            if(gbpSliderFloat("##gbpFb", &fbv, 0.0f, 1.1f, "%.2f")) ph.feedback.set(fbv);
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
                ImGui::SetTooltip("Feedback: this engine's sound (before the node FX) is recorded again\n"
                                  "with the input, so it comes back around: repeats (Grainless), pitch\n"
                                  "spirals (Pitch), smearing stutters. Soft-saturated: above 1 it builds\n"
                                  "and thickens instead of exploding. Freeze stops it (nothing recorded).");
            drawNodePublishContextMenu(ph.feedback.getEscapedName());
        }
        ImGui::SameLine(0, 2.0f * zoom);
        ImGui::SetNextItemWidth(56.0f * zoom);
        {
            float tv = ph.fbTone.get();
            const char* tl = tv < 0.45f ? "dark" : tv > 0.55f ? "thin" : "tone";
            if(gbpSliderFloat("##gbpFbT", &tv, 0.0f, 1.0f, tl)) ph.fbTone.set(tv);
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
                ImGui::SetTooltip("Feedback tone: left = darker every pass (lowpass), middle = untouched,\n"
                                  "right = thinner every pass (highpass)");
            drawPublishedCurrentItemUnderline(ph.fbTone.getEscapedName());
            drawNodePublishContextMenu(ph.fbTone.getEscapedName());
        }
    }

    // Playheads (simultaneous grain streams of this engine)
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted("Playheads");
    drawPublishedCurrentItemUnderline(ph.numPlayheads.getEscapedName());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f * zoom);
    {
        int np = ph.numPlayheads.get();
        if(ImGui::SliderInt("##gbpPhNum", &np, 1, MAX_VOICES)) ph.numPlayheads.set(np);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Simultaneous playheads of this engine (1..%d), each with its own grain stream.\n"
                              "Playhead i reads value i of every per-playhead parameter (Position, Pan,\n"
                              "Pitch, Duration, Amp, Chance, jitters...): connect a vector of that size\n"
                              "(e.g. an oscillator + indexer) to move them independently; a single value\n"
                              "applies to all. The engine's LFOs are added to every playhead.", MAX_VOICES);
        drawNodePublishContextMenu(ph.numPlayheads.getEscapedName());
    }

    // Channels / Offset
    const int N   = std::max(1, std::min(MAX_CHANNELS, numChannelsP.get()));
    const int off = phOffset(ph);
    const int nv  = phChannels(ph);
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted("Channels");
    drawPublishedCurrentItemUnderline(ph.channels.getEscapedName());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f * zoom);
    int c = ph.channels.get();
    if(ImGui::SliderInt("##gbpPhCh", &c, 0, MAX_CHANNELS, c == 0 ? "Node" : "%d")) ph.channels.set(c);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Speakers of this engine (Node = follow N Chan).\nIts playheads are panned over them with Pan.");
    drawNodePublishContextMenu(ph.channels.getEscapedName());
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted("Offset");
    drawPublishedCurrentItemUnderline(ph.chanOffset.getEscapedName());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f * zoom);
    int o = ph.chanOffset.get();
    if(ImGui::SliderInt("##gbpPhOff", &o, 0, MAX_CHANNELS - 1)) ph.chanOffset.set(o);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("First output channel of this engine");
    drawNodePublishContextMenu(ph.chanOffset.getEscapedName());
    ImGui::SameLine(0, gap);
    ImGui::PushStyleColor(ImGuiCol_Text, gbpPal::textDim);
    if(nv == 1) ImGui::Text("out %d of %d", off + 1, N);
    else        ImGui::Text("outs %d-%d of %d", off + 1, off + nv, N);
    ImGui::PopStyleColor();

    // Mute / Solo: the same parameters as the Global tab's M / S row
    ImGui::SameLine(0, gap);
    auto toggleBtn = [&](const char* lbl, ofParameter<bool>& b, ImU32 onCol, const char* tip) {
        const bool on = b.get();
        if(on) ImGui::PushStyleColor(ImGuiCol_Button, onCol);
        if(ImGui::Button(lbl, ImVec2(ImGui::GetFrameHeight() * 1.2f, 0))) b.set(!on);
        if(on) ImGui::PopStyleColor();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        drawPublishedCurrentItemUnderline(b.getEscapedName());
        drawNodePublishContextMenu(b.getEscapedName());
    };
    toggleBtn("M##phMute", ph.mute, gbpAccent(ph.index, 150, 0.6f), "Mute this engine");
    ImGui::SameLine(0, 2.0f * zoom);
    toggleBtn("S##phSolo", ph.solo, gbpAccent(ph.index, 200, 0.9f), "Solo: only soloed engines sound");
    ImGui::Spacing();
}

// ── Global tab: waveform overlay ─────────────────────────────────────────────
// The shared sample in neutral grey; a playhead with its own sample adds its
// waveform lightly tinted in its accent. Every playhead's region (band + in/out
// lines, numbered), grains and position cursors in its accent. View only.

void scGrainBoxPoly::drawGlobalWaveform(ImDrawList* dl, ImVec2 pos, float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    const ImVec2 end(pos.x + w, pos.y + h);
    const float visStart = waveScroll;
    const float visW     = std::max(0.0001f, 1.0f / waveZoom);
    const float midY     = pos.y + h * 0.5f;
    auto xOf = [&](float norm) { return pos.x + (norm - visStart) / visW * w; };

    dl->AddRectFilled(pos, end, gbpPal::canvas, 3.0f * zoom);
    dl->AddLine(ImVec2(pos.x, midY), ImVec2(end.x, midY), gbpPal::grid);

    // Regions (bands first, so everything else draws over them)
    for(auto& p : playheads) {
        float x0 = std::max(pos.x, xOf(relToDisplay(*p, inRelOf(*p))));
        float x1 = std::min(end.x, xOf(relToDisplay(*p, outRelOf(*p))));
        if(x1 > x0) dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, end.y), gbpAccent(p->index, 10));
    }

    // Grains of every playhead
    for(const auto& gh : grainHighlights) {
        if(gh.playhead < 0 || gh.playhead >= (int)playheads.size()) continue;
        if(playheads[gh.playhead]->liveInput && playheads[gh.playhead]->grainless.get()) continue;   // tap line (below)
        const SampleData& sd = sampleOf(*playheads[gh.playhead]);
        const float bufDur = sd.durationSecs > 0.0f ? sd.durationSecs : 1.0f;
        float x  = xOf(relToDisplay(*playheads[gh.playhead], gh.position));
        float wd = std::max(2.0f * zoom, std::min(w, gh.duration / bufDur / visW * w));
        float x2 = std::min(end.x, x + wd);
        if(x2 <= pos.x || x >= end.x) continue;
        float alpha = (gh.lifeTime / gh.maxLife) * (0.3f + 0.7f * gh.amp);
        dl->AddRectFilled(ImVec2(std::max(pos.x, x), pos.y), ImVec2(x2, end.y),
                          gbpAccent(gh.playhead, (int)(alpha * 100), gbpVoiceBrightness(gh.channel)));
    }

    // Waveforms: shared one neutral, own ones tinted
    auto drawPeaks = [&](const SampleData& sd, ImU32 col) {
        if(sd.peaks.empty()) return;
        const int bins = (int)sd.peaks.size();
        for(int i = 0; i < (int)w; i++) {
            float norm = visStart + ((float)i / w) * visW;
            const int b = std::max(0, std::min(bins - 1, (int)(norm * bins)));
            float peak = sd.peaks[b];
            float low  = sd.lows.empty() ? -peak : sd.lows[std::min(b, (int)sd.lows.size() - 1)];
            if(sd.lows.empty() && peak <= 0.0f) continue;
            if(peak <= low) { peak += 0.002f; low -= 0.002f; }
            dl->AddLine(ImVec2(pos.x + i, midY - peak * h * 0.45f), ImVec2(pos.x + i, midY - low * h * 0.45f), col);
        }
    };
    // Every waveform at the same alpha, in its playhead's accent. The shared
    // sample is playhead 1's: drawn once in its blue accent, also when other
    // playheads share it (their regions, cursors and grains keep their colours).
    {
        bool mainUsed = false;
        for(auto& p : playheads) mainUsed = mainUsed || &sampleOf(*p) == &mainSample;
        if(mainUsed) drawPeaks(mainSample, gbpAccent(0, gbpPal::waveAlpha));
    }
    for(auto& p : playheads) {
        if(p->liveInput)                          drawPeaks(p->live, gbpAccent(p->index, gbpPal::waveAlpha));
        else if(p->index > 0 && p->ownSample)     drawPeaks(p->own,  gbpAccent(p->index, gbpPal::waveAlpha));
    }

    // Position cursors + in/out lines, numbered at the top
    for(auto& p : playheads) {
        const float pIn = inRelOf(*p), pSpan = std::max(0.001f, outRelOf(*p) - pIn);
        const int nv = phVoices(*p);
        const auto positions = posSend(*p, nv);       // as sent (any time unit, + global offset)
        for(int i = 0; i < nv; i++) {
            float pp = positions.empty() ? 0.0f : positions[i % (int)positions.size()];
            pp       = std::max(0.0f, std::min(1.0f, pp));
            float x  = xOf(relToDisplay(*p, pIn + pp * pSpan));
            if(x < pos.x || x > end.x) continue;
            dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), gbpAccent(p->index, 110, gbpVoiceBrightness(i)), 1.0f * zoom);
        }
        const ImU32 lc = gbpAccent(p->index, 170);
        for(float v : {inRelOf(*p), outRelOf(*p)}) {
            float x = xOf(relToDisplay(*p, v));
            if(x < pos.x || x > end.x) continue;
            dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), lc, 1.0f * zoom);
        }
        float xi = xOf(relToDisplay(*p, inRelOf(*p)));
        if(xi >= pos.x && xi <= end.x) {
            std::string num = ofToString(p->index + 1);
            dl->AddText(ImVec2(xi + 3.0f * zoom, pos.y + (4.0f + 14.0f * p->index) * zoom), lc, num.c_str());
        }
    }

    // Grainless delay taps: pink lines over everything (see drawWaveformPanel)
    for(const auto& gh : grainHighlights) {
        if(gh.playhead < 0 || gh.playhead >= (int)playheads.size()) continue;
        Playhead& gp = *playheads[gh.playhead];
        if(!(gp.liveInput && gp.grainless.get())) continue;
        const float x = xOf(relToDisplay(gp, gh.position));
        if(x < pos.x || x > end.x) continue;
        const float a = std::max(0.0f, std::min(1.0f, gh.lifeTime / gh.maxLife));
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, end.y), IM_COL32(255, 90, 190, (int)(70 + 185 * a)), 2.0f * zoom);
    }

    // Label + border (+ drop feedback: the shared sample)
    {
        std::string label = mainSample.path.empty() ? std::string("no file loaded")
                                                   : std::filesystem::path(mainSample.path).filename().string();
        label = "All engines   ·   " + label;
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(end.x - ts.x - 6.0f * zoom, pos.y + 4.0f * zoom), gbpPal::textDim, label.c_str());
    }
    const bool dropHover = browser.isDraggingFile() &&
                           ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    dl->AddRect(pos, end, dropHover ? gbpPal::handleLight : gbpPal::border, 3.0f * zoom, 0,
                dropHover ? 2.0f * zoom : 1.0f);
    if(dropHover) {
        const char* msg = "Drop to load the shared sample (Engine 1)";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, midY - ts.y * 0.5f), gbpPal::handleLight, msg);
    }
}

// ── Global tab: controls, mute / solo, FX ────────────────────────────────────

namespace {
// Section header strip in the palette (inside a child window)
void gbpSectionHeader(const char* label, float w, float zoom) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cur = ImGui::GetCursorScreenPos();
    float  bh  = ImGui::GetTextLineHeight() + 6.0f * zoom;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), gbpPal::header, 3.0f * zoom);
    dl->AddText(ImVec2(cur.x + 6.0f * zoom, cur.y + 3.0f * zoom), gbpPal::headerText, label);
    ImGui::Dummy(ImVec2(w, bh));
}
} // namespace

void scGrainBoxPoly::drawGlobalPanel(float w) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 6.0f * zoom;
    float lw   = 70.0f * zoom;

    // Label | slider with the Publish to Node menu (scalar float parameter)
    auto slider = [&](const char* label, const char* id, ofParameter<float>& p, float width,
                      const char* fmt = "%.2f", ImGuiSliderFlags flags = 0) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(std::max(40.0f * zoom, width - lw - m));
        float v = p.get();
        if(gbpSliderFloat(id, &v, p.getMin(), p.getMax(), fmt, flags, &p == &transposeP)) p.set(v);
        drawNodePublishContextMenu(key);
    };

    // ── Global controls (left) | Engines mute / solo (right), one row ──────
    const float engW  = std::max(150.0f * zoom, w * 0.3f);   // Engines column
    const float gW    = w - engW - m;                        // Global column
    const float colW = (gW - 2.0f * m) / 3.0f;
    const float rowH = ImGui::GetFrameHeightWithSpacing() + 4.0f * zoom;
    ImGui::BeginGroup();
    gbpSectionHeader("Global", gW - m, zoom);
    auto cell = [&](const char* id, const char* label, const char* sid, ofParameter<float>& p,
                    const char* fmt, ImGuiSliderFlags flags, const char* tip, bool last) {
        ImGui::BeginChild(id, ImVec2(colW, rowH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        slider(label, sid, p, colW, fmt, flags);
        if(ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s\nShift+drag: fine (10x slower)%s", tip,
                              &p == &transposeP ? "\nCtrl/Cmd+drag (after starting it): whole semitones" : "");
        ImGui::EndChild();
        if(!last) ImGui::SameLine(0, m);
    };
    cell("##gbpGc0", "Transpose", "##gbpTr",  transposeP, "%.1f st", 0,
         "Added to every engine's pitch (semitones)", false);
    cell("##gbpGc1", "Volume",    "##gbpVol", volumeP,    "%.2f",    0,
         "Master gain of every engine", false);
    cell("##gbpGc2", "Speed",     "##gbpSpd", speedP,     "x%.3f",   ImGuiSliderFlags_Logarithmic,
         "Multiplies every auto-trigger rate (1 = as set)", true);
    const char* offFmt = timeUnitsCur == 1 ? "%+.0f ms" : timeUnitsCur == 2 ? "%+.2f b" : "%+.3f";
    cell("##gbpGc3", "Position",  "##gbpGPos", gPosP,     offFmt,    0,
         "Added to every engine's Position (the result is clipped to its region in SC)", false);
    cell("##gbpGc4", "Duration",  "##gbpGDur", gDurP,     offFmt,    0,
         "Added to every engine's Duration (the result is clipped to its region in SC)", false);
    cell("##gbpGc5", "Chance",    "##gbpGChc", gChanceP,  "%+.3f",   0,
         "Added to every engine's Chance", true);
    {   // Time units of the time-dependent parameters
        static const char* units[3] = {"Relative (0..1)", "ms", "Beats"};
        const std::string key = timeUnitsP.getEscapedName();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Time Units");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f * zoom);
        int tu = std::max(0, std::min(2, timeUnitsP.get()));
        if(ImGui::Combo("##gbpTimeUnits", &tu, units, 3)) timeUnitsP.set(tu);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Units of Position, Duration, In / Out, Pos / Dur jitter, the Pos / Dur LFO\n"
                              "strength and the global Position / Duration offsets.\n"
                              "Relative: 0..1 of the sample (Position / Duration: of the region).\n"
                              "ms / Beats: file engines: In / Out from the file start, Position from In;\n"
                              "Live engines: Position (and In / Out) = delay behind the live input.\n"
                              "Switching converts the values (no jump). Beats follow the node / transport tempo.");
        drawNodePublishContextMenu(key);
    }
    ImGui::EndGroup();
    const float globalH = ImGui::GetItemRectSize().y;
    ImGui::SameLine(0, m);

    // ── Mute / Solo (wraps onto more lines when there are many engines) ──
    ImGui::BeginChild("##gbpEngines", ImVec2(engW - m, globalH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    gbpSectionHeader("Engines", engW - m, zoom);
    const float engItemW = ImGui::CalcTextSize("8").x + 4.0f * zoom
                         + 2.0f * ImGui::GetFrameHeight() * 1.2f + 2.0f * zoom;
    for(auto& p : playheads) {
        ImGui::PushID(p->index);
        if(p->index > 0) {
            ImGui::SameLine(0, 14.0f * zoom);
            if(ImGui::GetContentRegionAvail().x < engItemW) ImGui::NewLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, gbpAccent(p->index, 255));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", p->index + 1);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4.0f * zoom);
        auto toggleBtn = [&](const char* lbl, ofParameter<bool>& b, ImU32 onCol, const char* tip) {
            const bool on = b.get();
            if(on) ImGui::PushStyleColor(ImGuiCol_Button, onCol);
            if(ImGui::Button(lbl, ImVec2(ImGui::GetFrameHeight() * 1.2f, 0))) b.set(!on);
            if(on) ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            drawPublishedCurrentItemUnderline(b.getEscapedName());
            drawNodePublishContextMenu(b.getEscapedName());
        };
        toggleBtn("M", p->mute, gbpAccent(p->index, 150, 0.6f), "Mute this engine");
        ImGui::SameLine(0, 2.0f * zoom);
        toggleBtn("S", p->solo, gbpAccent(p->index, 200, 0.9f), "Solo: only soloed engines sound");
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // ── Morph ─────────────────────────────────────────────────────────────
    drawMorphPanel(w);

    // ── FX chain ──────────────────────────────────────────────────────────
    drawFxPanel(w);
}

// ════════════════════════════════════════════════════════════════════════════
// Snapshots (as scBeatRepeatPro): every editor parameter of every engine and
// the Global tab, in its stored unit. Not stored: the samples, Live Length
// (a new length drops the live buffer), the trigger pulses, Snapshot itself.
// ════════════════════════════════════════════════════════════════════════════

bool scGrainBoxPoly::snapshotExcluded(const std::string& key) const {
    return key.rfind("SyncGate", 0) == 0 || key.rfind("Trigger", 0) == 0
        || key.rfind("Live Length", 0) == 0 || key.rfind("LiveLenBeats", 0) == 0
        || key.rfind("Morph", 0) == 0
        || key == snapshotP.getEscapedName();
}

ofJson scGrainBoxPoly::captureSnapshot() const {
    ofJson snap = ofJson::object();
    for(const auto& a : publishableEditorParameters)
        if(a.saveValue && !snapshotExcluded(a.key)) a.saveValue(snap);
    return snap;
}

// Values arrive in the snapshot's units (as a preset: no conversion), then
// one consistent send (timeDirty -> update)
void scGrainBoxPoly::applySnapshot(const ofJson& snap) {
    if(!snap.is_object()) return;
    const bool wasLoading = loadingPreset;
    loadingPreset = true;
    std::vector<std::pair<std::function<void(const ofJson&)>, ofJson>> todo;
    for(const auto& a : publishableEditorParameters)
        if(a.loadValue && !snapshotExcluded(a.key) && snap.contains(a.key))
            todo.emplace_back(a.loadValue, snap[a.key]);
    for(auto& [load, v] : todo) {
        try { load(v); } catch(...) {}
    }
    loadingPreset = wasLoading;
    timeDirty = true;
}

void scGrainBoxPoly::storeSnapshot(int slot) {
    if(slot < 0 || slot >= 16) return;
    const std::string keepName = snapUsed[slot] ? snapName(slot) : std::string();
    snapSlots[slot] = captureSnapshot();
    if(!keepName.empty()) snapSlots[slot]["name"] = keepName;
    snapUsed[slot]  = true;
    activeSnap      = slot;
    saveSnapshotLibrary();
}

void scGrainBoxPoly::recallSnapshot(int slot) {
    if(slot < 0 || slot >= 16 || !snapUsed[slot]) return;
    applySnapshot(snapSlots[slot]);
    activeSnap = slot;
    snapRecalling = true;
    snapshotP.set(slot + 1);   // shows / outputs the slot, without a second recall
    snapRecalling = false;
}

// A square 4 x 4 matrix of side w (left of the waveform)
// ── Snapshot library / banks ──────────────────────────────────────────────────
// {"gbpSnapshotBank": 1, "active": n, "slots": [16 x (object | null)]}
// As the other nodes (ChordSequence, PolyphonicArpeggiatorGUI...):
// data/nodeSnapshots/GrainBoxPoly/snapshot_<slot>.json, one file per slot,
// with its "name". Banks: data/nodeSnapshots/GrainBoxPoly/banks/<name>.json.
// What earlier builds saved (Supercollider/GrainBoxSnapshots, or a single
// _current.json) is taken over once, if there are no slot files yet.
std::string scGrainBoxPoly::snapSetDir() const {
    const std::string dir = ofToDataPath("nodeSnapshots/GrainBoxPoly", true);
    try { std::filesystem::create_directories(dir); } catch(...) {}
    return dir;
}
std::string scGrainBoxPoly::snapBankDir() const {
    const std::string dir = (std::filesystem::path(snapSetDir()) / "banks").string();
    static bool migrated = false;
    try {
        std::filesystem::create_directories(dir);
        if(!migrated) {
            migrated = true;
            for(const std::filesystem::path old : {std::filesystem::path(ofToDataPath("Supercollider/GrainBoxSnapshots", true)),
                                                   std::filesystem::path(snapSetDir())}) {
                if(!std::filesystem::exists(old)) continue;
                for(auto& e : std::filesystem::directory_iterator(old)) {
                    const std::string fn = e.path().filename().string();
                    if(!e.is_regular_file() || e.path().extension() != ".json") continue;
                    if(fn.rfind("snapshot_", 0) == 0 || fn == "_current.json") continue;
                    const auto to = std::filesystem::path(dir) / fn;
                    if(!std::filesystem::exists(to)) std::filesystem::copy_file(e.path(), to);
                }
            }
        }
    } catch(...) {}
    return dir;
}
std::string scGrainBoxPoly::snapLibPath() const {   // slot 0's file (see snapSlotPath)
    return (std::filesystem::path(snapSetDir()) / "snapshot_0.json").string();
}
std::string scGrainBoxPoly::snapName(int k) const {
    if(k < 0 || k >= 16 || !snapUsed[k] || !snapSlots[k].is_object() || !snapSlots[k].contains("name")) return "";
    try { return snapSlots[k]["name"].get<std::string>(); } catch(...) { return ""; }
}
// newest change among the slot files (another node / app wrote them?)
std::filesystem::file_time_type scGrainBoxPoly::snapSetTime() const {
    std::filesystem::file_time_type t{};
    try {
        for(auto& e : std::filesystem::directory_iterator(snapSetDir()))
            if(e.is_regular_file() && e.path().filename().string().rfind("snapshot_", 0) == 0)
                t = std::max(t, std::filesystem::last_write_time(e.path()));
    } catch(...) {}
    return t;
}

void scGrainBoxPoly::saveSnapshotLibrary() {
    const std::filesystem::path dir = snapSetDir();
    for(int k = 0; k < 16; k++) {
        const auto f = dir / ("snapshot_" + ofToString(k) + ".json");
        try {
            if(snapUsed[k]) ofSavePrettyJson(f.string(), snapSlots[k]);
            else if(std::filesystem::exists(f)) std::filesystem::remove(f);
        } catch(...) {}
    }
    snapLibTime = snapSetTime();
}

void scGrainBoxPoly::loadSnapshotLibrary() {
    const std::filesystem::path dir = snapSetDir();
    bool any = false;
    for(int k = 0; k < 16; k++)
        any = any || std::filesystem::exists(dir / ("snapshot_" + ofToString(k) + ".json"));
    if(!any) {
        // take over an earlier build's single file, once
        for(const std::filesystem::path old : {dir / "_current.json",
                 std::filesystem::path(ofToDataPath("Supercollider/GrainBoxSnapshots/_current.json", true))}) {
            if(!std::filesystem::exists(old)) continue;
            if(loadSnapshotBank(old.string())) { snapBankMsg.clear(); return; }   // (saves the slot files)
        }
        return;
    }
    snapUsed.fill(false);
    for(int k = 0; k < 16; k++) {
        const auto f = dir / ("snapshot_" + ofToString(k) + ".json");
        if(!std::filesystem::exists(f)) continue;
        try {
            ofJson j = ofLoadJson(f.string());
            if(!j.is_object()) continue;
            if(j.contains("__name")) { j["name"] = j["__name"]; j.erase("__name"); }
            snapSlots[k] = j; snapUsed[k] = true;
        } catch(...) {}
    }
    if(activeSnap >= 0 && !snapUsed[activeSnap]) activeSnap = -1;
    snapLibTime = snapSetTime();
}

void scGrainBoxPoly::saveSnapshotBank(const std::string& path) {
    ofJson j;
    j["gbpSnapshotBank"] = 1;
    j["active"] = activeSnap;
    j["slots"]  = ofJson::array();
    for(int k = 0; k < 16; k++) j["slots"].push_back(snapUsed[k] ? snapSlots[k] : ofJson(nullptr));
    std::string p = path;
    if(std::filesystem::path(p).extension().string() != ".json") p += ".json";
    snapBankMsg = ofSavePrettyJson(p, j) ? "Saved " + std::filesystem::path(p).filename().string()
                                         : "Could not save " + p;
}

bool scGrainBoxPoly::loadSnapshotBank(const std::string& path) {
    ofJson j;
    try { j = ofLoadJson(path); } catch(...) {}
    // a bank file, or an Oceanode preset / node json holding "gbpSnapshots"
    const ofJson* arr = nullptr;
    if(j.contains("slots") && j["slots"].is_array())               arr = &j["slots"];
    else if(j.contains("gbpSnapshots") && j["gbpSnapshots"].is_array()) arr = &j["gbpSnapshots"];
    if(!arr) { snapBankMsg = "No snapshots in " + std::filesystem::path(path).filename().string(); return false; }
    snapUsed.fill(false);
    for(int k = 0; k < std::min(16, (int)arr->size()); k++)
        if((*arr)[k].is_object()) {
            snapSlots[k] = (*arr)[k]; snapUsed[k] = true;
            if(snapSlots[k].contains("__name")) { snapSlots[k]["name"] = snapSlots[k]["__name"]; snapSlots[k].erase("__name"); }
        }
    int a = j.value("active", j.value("activeSnapshot", -1));
    activeSnap = (a >= 0 && a < 16 && snapUsed[a]) ? a : -1;
    snapBankMsg = "Loaded " + std::filesystem::path(path).stem().string();
    saveSnapshotLibrary();   // becomes the shared current set
    return true;
}

// ── Morph ─────────────────────────────────────────────────────────────────────
float scGrainBoxPoly::morphCurve(float u) const {
    u = std::max(0.0f, std::min(1.0f, u));
    switch(morphCurveP.get()) {
        case 1:  return u * u * (3.0f - 2.0f * u);              // Ease (in and out)
        case 2:  return u * u * u;                               // Exp: slow start
        case 3:  return 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);   // Log: fast start
        default: return u;                                       // Lin
    }
}

// Every parameter both states hold: continuous ones glide, switches jump at
// the switch point; a value in a time unit that differs between a and b jumps
// too (the two numbers are not comparable). As a preset: values arrive in
// their units (no conversion), one consistent send at the next update.
void scGrainBoxPoly::applyMorph(const ofJson& a, const ofJson& b, float t) {
    if(!a.is_object() || !b.is_object()) return;
    const int swm = morphSwitchP.get();
    const bool sw = swm == 0 ? t > 0.0f : swm == 1 ? t >= 0.5f : t >= 1.0f;
    auto differ = [&](const std::string& k) { return a.contains(k) && b.contains(k) && a[k] != b[k]; };
    const bool tuDiff = differ(timeUnitsP.getEscapedName());
    const bool wasLoading = loadingPreset;
    loadingPreset = true;
    std::vector<std::function<void()>> todo;
    for(const auto& act : publishableEditorParameters) {
        if(!act.morph || snapshotExcluded(act.key) || !a.contains(act.key) || !b.contains(act.key)) continue;
        const bool step = (act.unitClass == 1 && tuDiff) || (act.unitClass == 2 && differ(act.unitKey));
        const auto& x = a[act.key];
        const auto& y = b[act.key];
        auto fn = act.morph;
        todo.push_back([fn, &x, &y, t, sw, step]() { fn(x, y, t, sw, step); });
    }
    for(auto& f : todo) { try { f(); } catch(...) {} }
    loadingPreset = wasLoading;
    timeDirty = true;
}

void scGrainBoxPoly::startGlide(int slot) {
    if(slot < 0 || slot >= 16 || !snapUsed[slot]) return;
    const float bpm = std::max(1.0f, effectiveBpm());
    morphDur = morphBeatsP.get() ? morphTimeP.get() * 60.0f / bpm : morphTimeP.get();
    if(morphDur <= 0.001f) { morphRun = false; recallSnapshot(slot); return; }
    morphFrom     = captureSnapshot();
    morphTo       = snapSlots[slot];
    morphTarget   = slot;
    morphT0       = ofGetElapsedTimef();
    morphProgress = 0.0f;
    morphRun      = true;
}

void scGrainBoxPoly::applyAB() {
    const int a = morphAP.get() - 1, b = morphBP.get() - 1;
    if(a < 0 || b < 0 || a >= 16 || b >= 16 || !snapUsed[a] || !snapUsed[b]) return;
    morphRun = false;   // the slider takes over from a running glide
    applyMorph(snapSlots[a], snapSlots[b], morphCurve(morphP.get()));
}

void scGrainBoxPoly::drawMorphPanel(float w) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float m = 6.0f * zoom;
    gbpSectionHeader("Morph", w - m, zoom);
    auto slotCombo = [&](const char* id, ofParameter<int>& p, const char* tip) {
        const std::string key = p.getEscapedName();
        ImGui::SetNextItemWidth(48.0f * zoom);
        std::string prev = p.get() > 0 ? ofToString(p.get()) : std::string("-");
        if(ImGui::BeginCombo(id, prev.c_str())) {
            if(ImGui::Selectable("-", p.get() == 0)) p.set(0);
            for(int k = 0; k < 16; k++) {
                if(!snapUsed[k]) continue;
                if(ImGui::Selectable(ofToString(k + 1).c_str(), p.get() == k + 1)) p.set(k + 1);
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        drawPublishedCurrentItemUnderline(key);
        drawNodePublishContextMenu(key);
    };
    // Row 1: A / B crossfade
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("A");  ImGui::SameLine();
    slotCombo("##gbpMorphA", morphAP, "Snapshot at Morph 0");
    ImGui::SameLine(0, m);
    ImGui::TextUnformatted("B");  ImGui::SameLine();
    slotCombo("##gbpMorphB", morphBP, "Snapshot at Morph 1");
    ImGui::SameLine(0, m);
    {
        const std::string key = morphP.getEscapedName();
        ImGui::TextUnformatted("Morph");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(80.0f * zoom, w * 0.35f));
        float v = morphP.get();
        if(gbpSliderFloat("##gbpMorph", &v, 0.0f, 1.0f, "%.3f")) morphP.set(v);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Blend snapshot A (0) into B (1), through the Curve. Publish it\n"
                              "(right-click) to drive it from an LFO, envelope or sequence.");
        drawNodePublishContextMenu(key);
    }
    // Row 2: glide
    {
        bool g = morphGlideP.get();
        if(ImGui::Checkbox("Glide##gbpMG", &g)) morphGlideP.set(g);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("On: every recall (click, or the Snapshot input) glides from the current\n"
                              "state over Morph Time. Off: recalls jump; Alt-click a slot to glide.");
        drawNodePublishContextMenu(morphGlideP.getEscapedName(), "Glide##gbpMG", 0.0f, true);
    }
    ImGui::SameLine(0, m);
    {
        const std::string key = morphTimeP.getEscapedName();
        ImGui::TextUnformatted("Time");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f * zoom);
        float v = morphTimeP.get();
        const bool beats = morphBeatsP.get();
        if(gbpSliderFloat("##gbpMorphT", &v, 0.0f, 64.0f, beats ? "%.2f b" : "%.2f s", ImGuiSliderFlags_Logarithmic))
            morphTimeP.set(v);
        drawNodePublishContextMenu(key);
        ImGui::SameLine(0, 2.0f * zoom);
        if(ImGui::SmallButton(beats ? "b##gbpMTU" : "s##gbpMTU")) morphBeatsP.set(!beats);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Morph Time in beats (b, follows the tempo) or seconds (s)");
        drawNodePublishContextMenu(morphBeatsP.getEscapedName());
    }
    ImGui::SameLine(0, m);
    {
        static const char* curves[4] = {"Lin", "Ease", "Exp", "Log"};
        ImGui::TextUnformatted("Curve");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(62.0f * zoom);
        int c = std::max(0, std::min(3, morphCurveP.get()));
        if(ImGui::Combo("##gbpMorphC", &c, curves, 4)) morphCurveP.set(c);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Lin: even. Ease: soft start and end. Exp: slow start. Log: fast start.\n"
                              "(Glides and the Morph slider)");
        drawNodePublishContextMenu(morphCurveP.getEscapedName());
    }
    ImGui::SameLine(0, m);
    {
        static const char* sws[3] = {"Start", "Middle", "End"};
        ImGui::TextUnformatted("Switch");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(72.0f * zoom);
        int c = std::max(0, std::min(2, morphSwitchP.get()));
        if(ImGui::Combo("##gbpMorphS", &c, sws, 3)) morphSwitchP.set(c);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("When the things that can't blend jump: toggles, shapes, units,\n"
                              "playhead counts (and time values whose units differ).");
        drawNodePublishContextMenu(morphSwitchP.getEscapedName());
    }
    if(morphRun) {
        ImGui::SameLine(0, m);
        ImGui::Text("-> %d  %.0f%%", morphTarget + 1, morphProgress * 100.0f);
    }
    // Row 3: banks. The 16 slots are shared by every project / preset (saved
    // on every change); a bank is a named copy of them in the same folder.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bank");
    ImGui::SameLine(0, m);
    ImGui::SetNextItemWidth(120.0f * zoom);
    ImGui::InputTextWithHint("##gbpBankName", "bank name", snapBankName, sizeof(snapBankName));
    ImGui::SameLine(0, 4.0f * zoom);
    if(ImGui::Button("Save##gbpSB")) {
        std::string nm = snapBankName;
        for(char& c : nm) if(c == '/' || c == '\\' || c == ':') c = '_';
        if(!nm.empty() && nm != "_current") saveSnapshotBank((std::filesystem::path(snapBankDir()) / nm).string());
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Save the 16 snapshots (with their names) as a bank");
    ImGui::SameLine(0, m);
    // the banks in the folder
    std::vector<std::string> banks;
    try {
        for(auto& e : std::filesystem::directory_iterator(snapBankDir()))
            if(e.is_regular_file() && e.path().extension() == ".json" && e.path().stem() != "_current")
                banks.push_back(e.path().stem().string());
    } catch(...) {}
    std::sort(banks.begin(), banks.end());
    ImGui::SetNextItemWidth(130.0f * zoom);
    if(ImGui::BeginCombo("##gbpBanks", "Load bank...")) {
        if(banks.empty()) ImGui::TextDisabled("(no banks yet)");
        for(const auto& bk : banks) {
            if(ImGui::Selectable(bk.c_str())) {
                loadSnapshotBank((std::filesystem::path(snapBankDir()) / (bk + ".json")).string());
                std::snprintf(snapBankName, sizeof(snapBankName), "%s", bk.c_str());
            }
            if(ImGui::BeginPopupContextItem()) {   // right-click a bank: delete it
                if(ImGui::MenuItem("Delete this bank")) {
                    try { std::filesystem::remove(std::filesystem::path(snapBankDir()) / (bk + ".json")); } catch(...) {}
                    snapBankMsg = "Deleted " + bk;
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Load a bank into the 16 slots (replaces them). Right-click a bank\n"
                          "in the list to delete it.");
    ImGui::SameLine(0, 4.0f * zoom);
    if(ImGui::Button("From file...##gbpLB")) snapLoadDialog = true;
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Take the snapshots from a bank file or from an Oceanode preset file\n"
                          "(a GrainBox node's file that has snapshots)");
    ImGui::SameLine(0, 4.0f * zoom);
    if(ImGui::Button("Clear all##gbpCA")) ImGui::OpenPopup("##gbpClearAll");
    if(ImGui::BeginPopup("##gbpClearAll")) {
        ImGui::TextUnformatted("Clear all 16 snapshots?");
        if(ImGui::Button("Clear")) {
            for(int k = 0; k < 16; k++) { snapSlots[k] = ofJson(); snapUsed[k] = false; }
            activeSnap = -1;
            saveSnapshotLibrary();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(!snapBankMsg.empty()) {
        ImGui::SameLine(0, m);
        ImGui::TextDisabled("%s", snapBankMsg.c_str());
    }
    ImGui::Spacing();
}

void scGrainBoxPoly::drawSnapshotMatrix(float w) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float sp   = 3.0f * zoom;
    const float bw   = std::max(8.0f, (w - sp * 3.0f) / 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * zoom);
    for(int k = 0; k < 16; k++) {
        ImGui::PushID(k);
        if(k % 4 != 0) ImGui::SameLine();
        const bool used = snapUsed[k], act = activeSnap == k;
        ImGui::PushStyleColor(ImGuiCol_Button, act  ? gbpAccent(0, 230)
                                             : used ? gbpAccent(0, 110, 0.6f)
                                                    : ImGui::GetColorU32(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, gbpAccent(0, 170, 0.8f));
        std::string lbl = ofToString(k + 1);
        {
            const std::string nm = snapName(k);
            if(!nm.empty()) {   // the name, cut to the button's width
                std::string cut = nm;
                while(cut.size() > 1 && ImGui::CalcTextSize(cut.c_str()).x > bw - 4.0f * zoom) cut.pop_back();
                lbl = cut;
            }
        }
        lbl += "##snap";
        if(ImGui::Button(lbl.c_str(), ImVec2(bw, bw))) {
            const auto& io = ImGui::GetIO();
            if(io.KeyShift) storeSnapshot(k);
            else if(morphGlideP.get() != io.KeyAlt) startGlide(k);   // Alt: the other way
            else { morphRun = false; recallSnapshot(k); }
        }
        // Drag a slot onto another: move (swap with a used one); Alt: copy
        if(used && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("GBP_SNAP", &k, sizeof(int));
            const std::string nm = snapName(k);
            ImGui::Text("%s %d %s", ImGui::GetIO().KeyAlt ? "Copy" : "Move", k + 1, nm.c_str());
            ImGui::EndDragDropSource();
        }
        if(ImGui::BeginDragDropTarget()) {
            if(const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GBP_SNAP")) {
                const int src = *(const int*)pl->Data;
                if(src >= 0 && src < 16 && src != k) {
                    if(ImGui::GetIO().KeyAlt) {          // copy
                        snapSlots[k] = snapSlots[src]; snapUsed[k] = snapUsed[src];
                    } else {                             // move / swap
                        std::swap(snapSlots[k], snapSlots[src]);
                        std::swap(snapUsed[k], snapUsed[src]);
                        if(activeSnap == src) activeSnap = k; else if(activeSnap == k) activeSnap = src;
                        for(auto* mp : {&morphAP, &morphBP}) {
                            if(mp->get() == src + 1) mp->set(k + 1); else if(mp->get() == k + 1) mp->set(src + 1);
                        }
                    }
                    saveSnapshotLibrary();
                }
            }
            ImGui::EndDragDropTarget();
        }
        {   // A / B marks and the running glide's progress
            ImDrawList* bdl = ImGui::GetWindowDrawList();
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            if(morphAP.get() == k + 1) bdl->AddText(ImVec2(r0.x + 2.0f * zoom, r0.y + 1.0f * zoom), IM_COL32(255, 210, 120, 255), "A");
            if(morphBP.get() == k + 1) bdl->AddText(ImVec2(r1.x - 9.0f * zoom, r0.y + 1.0f * zoom), IM_COL32(120, 220, 255, 255), "B");
            if(morphRun && morphTarget == k)
                bdl->AddRectFilled(ImVec2(r0.x, r1.y - 3.0f * zoom), ImVec2(r0.x + (r1.x - r0.x) * morphProgress, r1.y),
                                   IM_COL32(255, 255, 255, 200));
        }
        ImGui::PopStyleColor(2);
        if(ImGui::BeginPopupContextItem("##gbpSnapMenu")) {
            if(ImGui::MenuItem("Store here")) storeSnapshot(k);
            if(used && ImGui::MenuItem("Glide here (Morph Time)")) startGlide(k);
            if(used && ImGui::MenuItem("Set as Morph A")) morphAP.set(k + 1);
            if(used && ImGui::MenuItem("Set as Morph B")) morphBP.set(k + 1);
            if(used) {   // name (shown on the button, kept when the slot is overwritten)
                if(ImGui::IsWindowAppearing()) {
                    const std::string nm = snapName(k);
                    std::snprintf(snapRenameBuf, sizeof(snapRenameBuf), "%s", nm.c_str());
                }
                ImGui::SetNextItemWidth(140.0f * zoom);
                if(ImGui::InputTextWithHint("##gbpSnapName", "Name", snapRenameBuf, sizeof(snapRenameBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                    snapSlots[k]["name"] = std::string(snapRenameBuf);
                    saveSnapshotLibrary();
                    ImGui::CloseCurrentPopup();
                }
                if(ImGui::IsItemDeactivatedAfterEdit()) {
                    snapSlots[k]["name"] = std::string(snapRenameBuf);
                    saveSnapshotLibrary();
                }
            }
            if(used && ImGui::MenuItem("Duplicate to next free slot")) {
                for(int d = 1; d < 16; d++) {
                    const int t = (k + d) % 16;
                    if(snapUsed[t]) continue;
                    snapSlots[t] = snapSlots[k]; snapUsed[t] = true;
                    const std::string nm = snapName(k);
                    if(!nm.empty()) snapSlots[t]["name"] = nm + " copy";
                    saveSnapshotLibrary();
                    break;
                }
            }
            if(used && ImGui::MenuItem("Clear slot")) {
                snapSlots[k] = ofJson();
                snapUsed[k]  = false;
                if(activeSnap == k) activeSnap = -1;
                saveSnapshotLibrary();
            }
            // the "Snapshot" parameter (1..16): publish it to recall from a connection
            ImGui::Separator();
            drawNodePublishMenuItems(snapshotP.getEscapedName());
            ImGui::EndPopup();
        }
        if(ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0)) {
            const std::string nm = snapName(k);
            if(used)
                ImGui::SetTooltip("%d%s%s\nClick: recall (Glide on: glide)\nAlt-click: glide over Morph Time (Glide on: jump)\n"
                                  "Shift-click: overwrite\nDrag onto another slot: move (Alt: copy; onto a used slot: swap)\n"
                                  "Right-click: name / store / duplicate / clear / Morph A, B / publish \"Snapshot\"",
                                  k + 1, nm.empty() ? "" : "  ", nm.c_str());
            else
                ImGui::SetTooltip("%d (empty)\nShift-click (or right-click > Store here): save a snapshot\n"
                                  "Drop a snapshot here to move it", k + 1);
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
}

// ── FX EQ: scGraphicEQ's curve display (scEQEditor) with draggable band
//    handles (drag: frequency / gain, wheel: Q / slope, double-click: 0 dB).
//    The band values stay saved / publishable parameters ("EQ Low Freq",
//    "EQ Low", "EQ Low Slope"...), with no table in the window. ──

void scGrainBoxPoly::drawEqSection(float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();

    // Parameters → editor
    for(int b = 0; b < 5; b++)
        eqEditor.bands[b] = { eqFreqP[b].get(), eqGainP[b].get(), eqShapeP[b].get() };
    eqEditor.dbRange   = 24.0f;
    eqEditor.gainLimit = eqGainP[0].getMax();
    eqEditor.qMin = 0.1f;  eqEditor.qMax = 20.0f;
    eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    if(eqCurveDirty) {
        float sr = (float)scPreferences().hardwareSampleRate;
        for(auto* sm : allServers)
            if(sm && sm->getServer()) { sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
        eqEditor.recompute(sr > 0.0f ? sr : 48000.0f);
        eqCurveDirty = false;
    }

    // Palette colours
    static scEQEditor::Colors col = [] {
        scEQEditor::Colors c = scEQEditor::defaultColors();
        c.bg = gbpPal::canvas;  c.border = gbpPal::border;
        c.gridLine = gbpPal::grid;  c.dbGridLine = gbpPal::grid;
        c.gridText = gbpPal::textDim;  c.dbGridText = gbpPal::textDim;
        c.zeroGrid = gbpPal::gridStrong;  c.zeroLine = gbpPal::gridStrong;
        c.curve = gbpPal::wave;  c.fft = gbpPal::grid;
        for(auto& bm : c.bandMarker) bm = gbpPal::grid;
        c.handle = gbpPal::wave;  c.handleActive = gbpPal::handleLight;
        return c;
    }();

    // Right-click a band handle: its Freq / Gain / Q (Slope) values + publish items
    eqEditor.bandMenu = [this](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        drawNodePublishMenuItems(eqFreqP[b].getEscapedName(),  true,  "Freq (Hz)");
        drawNodePublishMenuItems(eqGainP[b].getEscapedName(),  false, "Gain (dB)");
        drawNodePublishMenuItems(eqShapeP[b].getEscapedName(), false,
                                 scEQEditor::isShelf(b) ? "Slope" : "Q");
    };

    const bool off = !fxOnP[FX_EQ].get();
    if(off) ImGui::BeginDisabled();
    if(eqEditor.draw(std::max(120.0f * zoom, w), std::max(60.0f * zoom, h), col, !off,
                     nullptr, 0, "##gbpEqCurve")) {
        for(int b = 0; b < 5; b++) {
            eqFreqP[b].set (eqEditor.bands[b].freqHz);
            eqGainP[b].set (eqEditor.bands[b].gainDb);
            eqShapeP[b].set(eqEditor.bands[b].shape);
        }
    }
    if(off) ImGui::EndDisabled();
}

void scGrainBoxPoly::drawFxPanel(float w) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 6.0f * zoom;
    float lw   = 62.0f * zoom;
    // One row: EQ (half width) | Echo | Reverb (a quarter each)
    const float halfW = (w - m) * 0.5f;
    const float colW  = (halfW - 2.0f * m) * 0.5f;
    // Narrow columns: label above the slider, so the slider keeps its width
    const bool  stacked = colW - lw - m < 70.0f * zoom;
    // Height from the actual row count (Reverb: header + 7 sliders), so no
    // row is ever clipped at any window size / zoom
    const int   revRows = 7;
    const float rowH    = ImGui::GetFrameHeightWithSpacing()
                        + (stacked ? ImGui::GetTextLineHeightWithSpacing() : 0.0f);
    const float colH    = ImGui::GetFrameHeightWithSpacing() + revRows * rowH
                        + ImGui::GetStyle().WindowPadding.y * 2.0f + 6.0f * zoom;

    gbpSectionHeader("FX   (EQ -> Echo -> Reverb, after every engine)", w - m, zoom);

    auto slider = [&](const char* label, const char* id, ofParameter<float>& p,
                      const char* fmt = "%.2f", ImGuiSliderFlags flags = 0) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        if(stacked) ImGui::SetNextItemWidth(std::max(40.0f * zoom, colW - m));
        else {
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(std::max(40.0f * zoom, colW - lw - m));
        }
        float v = p.get();
        if(gbpSliderFloat(id, &v, p.getMin(), p.getMax(), fmt, flags)) p.set(v);
        drawNodePublishContextMenu(key);
    };
    // Status of a stage on the first server: active, waiting for its defs
    auto status = [&](int stage) -> std::string {
        if(!fxOnP[stage].get()) return "";
        for(auto& [srv, st] : fx)
            if(std::find(st.plan.begin(), st.plan.end(), stage) != st.plan.end()) return "";
        if(fx.empty()) return "";                  // graph not built (not connected)
        return "loading SynthDefs...";
    };
    auto header = [&](int stage, const char* title, const char* id) {
        bool on = fxOnP[stage].get();
        if(ImGui::Checkbox(id, &on)) fxOnP[stage].set(on);
        drawNodePublishContextMenu(fxOnP[stage].getEscapedName(), id, 0.0f, true);
        ImGui::SameLine();
        ImGui::TextUnformatted(title);
        const std::string st = status(stage);
        if(!st.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, gbpPal::textDim);
            ImGui::TextUnformatted(st.c_str());
            ImGui::PopStyleColor();
        }
    };

    // EQ: On + Mix, then the curve editor (drag the band handles)
    ImGui::BeginChild("##gbpFxEq", ImVec2(halfW, colH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    header(FX_EQ, "EQ", "On##fxeq");
    ImGui::SameLine(0, 2.0f * m);
    {
        if(!fxOnP[FX_EQ].get()) ImGui::BeginDisabled();
        const std::string key = eqMixP.getEscapedName();
        ImGui::TextUnformatted("Mix");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(50.0f * zoom, halfW * 0.25f));
        float v = eqMixP.get();
        if(gbpSliderFloat("##eqmix", &v, eqMixP.getMin(), eqMixP.getMax(), "%.2f")) eqMixP.set(v);
        drawNodePublishContextMenu(key);
        if(!fxOnP[FX_EQ].get()) ImGui::EndDisabled();
    }
    drawEqSection(halfW - 4.0f * zoom, colH - ImGui::GetCursorPosY() - 8.0f * zoom);
    ImGui::EndChild();
    ImGui::SameLine(0, m);

    ImGui::BeginChild("##gbpFxEcho", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
    header(FX_ECHO, "Echo", "On##fxecho");
    if(!fxOnP[FX_ECHO].get()) ImGui::BeginDisabled();
    {   // Delay: seconds, or a note value (b) that follows the tempo
        const bool beats = echoBeatsP.get();
        auto& p = beats ? echoBeatValP : echoDelayP;
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted("Delay");
        drawPublishedCurrentItemUnderline(key);
        const float bW = 18.0f * zoom;
        const float sw = stacked ? std::max(40.0f * zoom, colW - m - bW - 2.0f * zoom)
                                 : std::max(40.0f * zoom, colW - lw - m - bW - 2.0f * zoom);
        if(!stacked) ImGui::SameLine(lw);
        if(ImGui::Button(beats ? "b##ecu" : "s##ecu", ImVec2(bW, 0))) echoBeatsP.set(!beats);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Delay in seconds (s) or as a note value (b, follows the tempo; max 2 s)");
        drawNodePublishContextMenu(echoBeatsP.getEscapedName());
        ImGui::SameLine(0, 2.0f * zoom);
        ImGui::SetNextItemWidth(sw);
        if(beats) {
            int k = gbpNearestNote(echoBeatValP.get());
            if(ImGui::SliderInt("##ecdelb", &k, 0, kGbpNoteCount - 1, kGbpNotes[k].name, ImGuiSliderFlags_AlwaysClamp))
                echoBeatValP.set(kGbpNotes[k].beats);
        } else {
            float v = echoDelayP.get();
            if(gbpSliderFloat("##ecdel", &v, echoDelayP.getMin(), echoDelayP.getMax(), "%.3f s", ImGuiSliderFlags_Logarithmic))
                echoDelayP.set(v);
        }
        drawNodePublishContextMenu(key);
    }
    slider("Feed",   "##ecfb",  echoFeedP);
    slider("Cutoff", "##eccut", echoCutoffP, "%.0f (note)");
    {   // filter type (echo.scd's dropdown)
        static const char* echoFilters[4] = {"LowPass", "HighPass", "BandPass", "PeakEQ"};
        const std::string key = echoFilterP.getEscapedName();
        ImGui::TextUnformatted("Filter");
        drawPublishedCurrentItemUnderline(key);
        if(stacked) ImGui::SetNextItemWidth(std::max(40.0f * zoom, colW - m));
        else {
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(std::max(40.0f * zoom, colW - lw - m));
        }
        int ft = std::max(0, std::min(3, echoFilterP.get()));
        if(ImGui::Combo("##ecflt", &ft, echoFilters, 4)) echoFilterP.set(ft);
        drawNodePublishContextMenu(key);
    }
    slider("Reso",   "##ecres", echoResonanceP);
    slider("Mix",    "##ecmix", echoMixP);
    if(!fxOnP[FX_ECHO].get()) ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine(0, m);

    ImGui::BeginChild("##gbpFxRev", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
    header(FX_REVERB, "Reverb", "On##fxrev");
    if(!fxOnP[FX_REVERB].get()) ImGui::BeginDisabled();
    slider("Size",     "##rvsize", revSizeP,     "%.1f");
    slider("Decay",    "##rvdec",  revDecayP,    "%.2f s", ImGuiSliderFlags_Logarithmic);
    slider("Predelay", "##rvpre",  revPredelayP, "%.0f ms");
    slider("Lowpass",  "##rvlp",   revLowpassP,  "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    slider("Position", "##rvpos",  revPositionP, "%.2f");
    slider("Spread",   "##rvspr",  revSpreadP,   "%+.2f");
    slider("Mix",      "##rvmix",  revMixP);
    if(!fxOnP[FX_REVERB].get()) ImGui::EndDisabled();
    ImGui::EndChild();
}

// ── Envelope panel ────────────────────────────────────────────────────────────
// Self-contained: curve display (top) + Attack/Release/Shape sliders (bottom).

void scGrainBoxPoly::drawEnvelopePanel(Playhead& ph, float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 8.0f * zoom;   // inner margin
    // Clamp to actual available content width so drawing never overflows the right margin
    w = std::min(w, ImGui::GetContentRegionAvail().x);

    float curveH  = 100.0f * zoom;   // fixed, not derived from available h

    // ── Shape: Custom (attack / release / tension below) or a preset ──────
    {
        static const char* shapes[7] = {"Custom", "Hann", "Gaussian", "Percussive",
                                        "Reverse Perc", "Trapezoid", "Rectangular"};
        const std::string key = ph.envShape.getEscapedName();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shape");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60.0f * zoom, w - 2.0f * m - ImGui::CalcTextSize("Shape").x - 8.0f * zoom));
        int sh = std::max(0, std::min(6, ph.envShape.get()));
        if(ImGui::Combo("##gbpEnvShape", &sh, shapes, 7)) ph.envShape.set(sh);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Grain envelope. Custom: the attack / release / tension editor;\n"
                              "the others are fixed shapes (Rectangular: 3-point fades, no clicks).");
        drawNodePublishContextMenu(key);
    }
    const bool customEnv = ph.envShape.get() == 0;

    // ── Curve display ─────────────────────────────────────────────────────
    ImVec2 pos = ImGui::GetCursorScreenPos();
    // Inset the curve area by margin
    ImVec2 cPos = ImVec2(pos.x + m, pos.y + m);
    float  cW   = w - 2.0f * m;
    float  cH   = curveH - m;

    ImGui::InvisibleButton("##gbpEnv", ImVec2(std::max(1.0f, w), std::max(1.0f, curveH)));
    bool   active = ImGui::IsItemActive();
    ImVec2 mouse  = ImGui::GetIO().MousePos;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 acc = gbpAccent(ph.index, 255);

    // Background (inset curve area)
    dl->AddRectFilled(cPos, ImVec2(cPos.x + cW, cPos.y + cH), gbpPal::canvas, 3.0f * zoom);

    // Curve (CPU: one segment per ~pixel instead of per table point, and one
    // polyline for the outline)
    const auto& envData = ph.envData;
    if(!envData.empty()) {
        int N = (int)envData.size();
        const int step = std::max(1, (int)std::floor((float)(N - 1) / std::max(1.0f, cW)));
        std::vector<ImVec2> pts;
        pts.reserve(N / step + 2);
        for(int i = 0; i < N; i += step) {
            const int j = std::min(i, N - 1);
            pts.push_back(ImVec2(cPos.x + ((float)j / (N - 1)) * cW,
                                 cPos.y + cH - envData[j] * (cH - 2.0f * zoom)));
        }
        if(pts.back().x < cPos.x + cW)
            pts.push_back(ImVec2(cPos.x + cW, cPos.y + cH - envData[N - 1] * (cH - 2.0f * zoom)));
        for(size_t k = 0; k + 1 < pts.size(); k++)
            dl->AddRectFilled(ImVec2(pts[k].x, std::min(pts[k].y, pts[k + 1].y)),
                              ImVec2(pts[k + 1].x, cPos.y + cH), gbpAccent(ph.index, 38));
        dl->AddPolyline(pts.data(), (int)pts.size(), gbpAccent(ph.index, 220), 0, 1.5f * zoom);
    }

    // Attack / Release guide lines
    float rawAtk = ph.envAttack.get();   // 0..1
    float rawRel = ph.envRelease.get();  // 0..1
    float total  = rawAtk + rawRel;
    float atk    = (total > 1.0f) ? rawAtk / total : rawAtk;
    float rel    = (total > 1.0f) ? rawRel / total : rawRel;
    float tensionNorm = (ph.envTension.get() + 1.0f) * 0.5f;  // -1..1 → 0..1
    float atkX = cPos.x + atk * cW;
    float relX = cPos.x + (1.0f - rel) * cW;
    float tenY = cPos.y + (1.0f - tensionNorm) * cH;
    dl->AddLine(ImVec2(atkX, cPos.y), ImVec2(atkX, cPos.y + cH), gbpPal::gridStrong, 1.0f * zoom);
    dl->AddLine(ImVec2(relX, cPos.y), ImVec2(relX, cPos.y + cH), gbpPal::gridStrong, 1.0f * zoom);
    dl->AddRect(cPos, ImVec2(cPos.x + cW, cPos.y + cH), gbpPal::border, 3.0f * zoom);

    // Drag handles
    constexpr float HR = 5.0f;
    auto drawHandle = [&](ImVec2 p, ImU32 col) {
        dl->AddCircleFilled(p, HR * zoom, col);
        dl->AddCircle(p, HR * zoom, acc, 16, 1.5f * zoom);
    };
    // Attack / release: light handles; tension: accent (Custom shape only)
    if(customEnv) {
        drawHandle(ImVec2(atkX, cPos.y + cH * 0.25f), gbpPal::handleLight);
        drawHandle(ImVec2(relX, cPos.y + cH * 0.25f), gbpPal::handleLight);
        drawHandle(ImVec2(cPos.x + cW * 0.5f, tenY),  acc);
    }

    // Handle dragging
    if(customEnv && active && ImGui::IsMouseClicked(0)) {
        auto dist2 = [](ImVec2 a, ImVec2 b) {
            return (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y);
        };
        float thr = (HR * zoom + 5.0f) * (HR * zoom + 5.0f);
        if     (dist2(mouse, ImVec2(atkX, cPos.y + cH * 0.25f)) < thr) envDragHandle = 0;
        else if(dist2(mouse, ImVec2(relX, cPos.y + cH * 0.25f)) < thr) envDragHandle = 1;
        else if(dist2(mouse, ImVec2(cPos.x + cW * 0.5f, tenY))  < thr) envDragHandle = 2;
    }
    if(!ImGui::IsMouseDown(0)) envDragHandle = -1;
    if(envDragHandle >= 0 && ImGui::IsMouseDragging(0)) {
        // Relative drag (Shift: 10x slower): no jump when Shift changes
        const ImVec2 d     = ImGui::GetIO().MouseDelta;
        const float  speed = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
        const float  curX  = envDragHandle == 0 ? ph.envAttack.get() : 1.0f - ph.envRelease.get();
        float nx = std::max(0.f, std::min(1.f, curX + d.x / std::max(1.0f, cW) * speed));
        float ny = std::max(0.f, std::min(1.f, (ph.envTension.get() + 1.0f) * 0.5f - d.y / std::max(1.0f, cH) * speed));
        if(envDragHandle == 0)      ph.envAttack.set(nx);
        else if(envDragHandle == 1) ph.envRelease.set(1.0f - nx);
        else                        ph.envTension.set(ny * 2.0f - 1.0f);  // 0..1 → -1..1
    }

    // Label
    dl->AddText(ImVec2(cPos.x + 3*zoom, cPos.y + 2*zoom),
        gbpPal::textDim, "Envelope");

    // ── Sliders below the curve ───────────────────────────────────────────
    ImGui::SetCursorScreenPos(ImVec2(pos.x + m, pos.y + curveH + m * 0.5f));

    float sw      = w - 2.0f * m;
    float envLblW = 48.0f * zoom;   // fixed label column width within envelope panel
    auto envSlider = [&](const char* label, const char* id, ofParameter<float>& p,
                         float mn, float mx) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(envLblW);
        ImGui::SetNextItemWidth(sw - envLblW);
        float v = p.get();
        if(gbpSliderFloat(id, &v, mn, mx)) p.set(v);
        drawNodePublishContextMenu(key);
    };

    if(!customEnv) ImGui::BeginDisabled();
    envSlider("Atk",     "##envAtk", ph.envAttack,  0.0f, 1.0f);
    envSlider("Rel",     "##envRel", ph.envRelease, 0.0f, 1.0f);
    envSlider("Tension", "##envTen", ph.envTension, -1.0f, 1.0f);
    if(!customEnv) ImGui::EndDisabled();
}

// ── Controls panel ────────────────────────────────────────────────────────────
// Right-click any control: Publish to Node / Unpublish from Node (a published
// control's label is underlined).

void scGrainBoxPoly::drawControlsPanel(Playhead& ph, float w, float h) {
    float zoom   = ofxOceanodeShared::getZoomLevel();
    int   n      = phVoices(ph);     // the engine's playhead count
    // Grainless (Live delay taps): only Position, Amp, Pan, Region, Levels and
    // the Filter act; the rest is greyed out
    const bool glUI = ph.liveInput && ph.grainless.get();
    ctrlMeasure = 0.0f;
    float gap    = 6.0f * zoom;
    float colW   = (w - 2.0f * gap) / 3.0f;
    float m      = 4.0f * zoom;  // inner margin
    // Fixed label column width — keeps sliders aligned within each column
    float labelW = 58.0f * zoom;

    // Scalar value → broadcast to all n channels
    // A single slider stores ONE value: it applies to every playhead
    // (expanded when sent), so the window keeps showing a single slider
    auto setAllF = [&](ofParameter<vector<float>>& p, float val) {
        (void)n;
        p.set(vector<float>(1, val));
    };
    auto getF = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.f : p.get()[0];
    };

    // Label left | slider right — no overflow, respects margins
    auto sliderF = [&](const char* label, const char* id,
                       ofParameter<vector<float>>& p, float mn, float mx) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float v = getF(p);
        // Pitch / pitch jitter: Ctrl (Cmd) + drag snaps to whole semitones
        const bool semi = std::strcmp(id, "##pit") == 0 || std::strcmp(id, "##ijit") == 0;
        // time parameters: the range and format of the current Time Units
        const bool timeP = &p == &ph.position || &p == &ph.duration || &p == &ph.posJit || &p == &ph.durJit;
        if(timeP) {
            mn = p.getMin().empty() ? mn : p.getMin()[0];
            mx = p.getMax().empty() ? mx : p.getMax()[0];
        }
        // Multislider only while a connection drives it (one bar per playhead
        // of this engine, as the playheads read it); otherwise one slider
        if(n > 1 && hasInputConnection(p.getName())) {
            std::vector<float> vv = expandF(p.get(), n);
            if(gbpMultiSlider(id, vv, mn, mx, timeP ? timeFmt() : "%.3f")) p.set(vv);
        } else if(gbpSliderFloat(id, &v, mn, mx, timeP ? timeFmt() : "%.3f", 0, semi)) {
            setAllF(p, v);
        }
        drawNodePublishContextMenu(key);
    };
    auto sliderScalar = [&](const char* label, const char* id,
                             ofParameter<float>& p, float mn, float mx) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float v = p.get();
        // In / Out: the range and format of the current Time Units
        const bool timeP = &p == &ph.inPoint || &p == &ph.outPoint;
        if(timeP) { mn = p.getMin(); mx = p.getMax(); }
        if(gbpSliderFloat(id, &v, mn, mx, timeP ? timeFmt() : "%.3f")) p.set(v);
        drawNodePublishContextMenu(key);
    };
    auto toggle = [&](const char* label, ofParameter<bool>& p) {
        bool v = p.get();
        if(ImGui::Checkbox(label, &v)) p.set(v);
        drawNodePublishContextMenu(p.getEscapedName(), label, 0.0f, true);
    };

    // Filled section header: draws a dark rounded rect with bold label text.
    // Must be called inside a child window so GetWindowDrawList() is scoped.
    auto secHdr = [&](const char* label) {
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        ImVec2      cur  = ImGui::GetCursorScreenPos();
        float       lh   = ImGui::GetTextLineHeight();
        float       padV = 3.0f * zoom;
        float       padH = 6.0f * zoom;
        float       bh   = lh + padV * 2.0f;
        dl->AddRectFilled(
            ImVec2(cur.x, cur.y),
            ImVec2(cur.x + colW - m * 0.5f, cur.y + bh),
            gbpPal::header, 3.0f * zoom);
        dl->AddText(
            ImVec2(cur.x + padH, cur.y + padV),
            gbpPal::headerText, label);
        ImGui::Dummy(ImVec2(colW, bh));
        ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + m * 0.5f));
    };

    // ── Column 0: Trigger + Region ───────────────────────────────────────────
    ImGui::BeginChild("##gbpCol0", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    secHdr(glUI ? "Delay" : "Trigger");
    if(!glUI) {   // Grainless: only Reset (the LFOs) and the Region
    toggle("AutoTrig##c", ph.autoTrig);
    ImGui::SameLine(0, m);
    toggle("Link##ct", ph.uniqueTrig);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Link voices: every voice uses voice 1's chance roll,\n"
                          "so all voices fire (or skip) together.");
    ImGui::SameLine(0, m);
    toggle("Mono##ct", ph.monoTrig);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Mono: no overlap. Each playhead starts a grain only\n"
                          "once its previous grain has ended (triggers in between\n"
                          "are skipped). Duration = Interval + a hard envelope:\n"
                          "seamless loops / beat-repeat.");
    // Interval between auto triggers, in the engine's own unit: Relative (of
    // the region) / ms on a log slider, Beats on note values
    {
        auto& p = ph.autoTrigBeatDiv;
        const std::string key = p.getEscapedName();
        const int iu = ph.intervalUnitCur;
        ImGui::TextUnformatted("Interval");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        // the engine's own unit (independent of the global Time Units)
        const float unitW = 52.0f * zoom;
        ImGui::SetNextItemWidth(unitW);
        {
            int u = iu;
            const char* units[] = {"Rel", "ms", "Beats", "Pitch"};
            if(ImGui::Combo("##ivunit", &u, units, 4)) ph.intervalUnit.set(u);
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Interval unit (this engine only):\n"
                                  "Rel = fraction of the region, ms, Beats = note values,\n"
                                  "Pitch = a MIDI note: grains are triggered at its frequency\n"
                                  "(C2 = 65 Hz; audio-rate grain clouds / pulsar tones).");
            drawNodePublishContextMenu(ph.intervalUnit.getEscapedName());
        }
        ImGui::SameLine(0, m * 0.5f);
        ImGui::SetNextItemWidth(std::max(20.0f, colW - labelW - m - unitW - m * 0.5f));
        const float mn = p.getMin().empty() ? 0.001f : p.getMin()[0];
        const float mx = p.getMax().empty() ? 1.0f   : p.getMax()[0];
        const char* fmt = iu == 1 ? "%.0f ms" : iu == 2 ? "%.3f b" : iu == 3 ? "%.1f" : "%.3f";
        if(n > 1 && hasInputConnection(p.getName())) {
            std::vector<float> vv = expandF(p.get(), n);
            if(gbpMultiSlider("##ival", vv, mn, mx, fmt, true)) {
                if(iu == 2) for(auto& x : vv) x = kGbpNotes[gbpNearestNote(x)].beats;
                p.set(vv);
            }
        } else if(iu == 2) {
            int k = gbpNearestNote(getF(p));
            if(ImGui::SliderInt("##ival", &k, 0, kGbpNoteCount - 1, kGbpNotes[k].name, ImGuiSliderFlags_AlwaysClamp))
                setAllF(p, kGbpNotes[k].beats);
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
                ImGui::SetTooltip("Time between grains as a note value\n(T = triplet, D = dotted)");
        } else if(iu == 3) {
            // MIDI note (Ctrl / Cmd + drag: whole semitones), shown as note + Hz
            float v = getF(p);
            static const char* nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            const int   ni  = (int)std::lround(v);
            const float hz  = 440.0f * std::pow(2.0f, (v - 69.0f) / 12.0f);
            char lbl[48];
            if(std::abs(v - ni) < 0.01f)
                std::snprintf(lbl, sizeof(lbl), "%s%d  %.1f Hz", nn[((ni % 12) + 12) % 12], ni / 12 - 1, hz);
            else
                std::snprintf(lbl, sizeof(lbl), "%.2f  %.1f Hz", v, hz);
            if(gbpSliderFloat("##ival", &v, mn, mx, lbl, 0, true)) setAllF(p, v);
        } else {
            float v = getF(p);
            if(gbpSliderFloat("##ival", &v, mn, mx, fmt, ImGuiSliderFlags_Logarithmic)) setAllF(p, v);
        }
        drawNodePublishContextMenu(key);
    }
    sliderF("Phase",  "##tphase",  ph.trigPhase,       0.0f,   1.0f);
    sliderF("Chance", "##chance",  ph.chance,          0.0f,   1.0f);
    // Max Grains: overlap limit of the engine, with the grains sounding now
    {
        auto& p = ph.maxGrains;
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted("Max Gr");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        int mg = p.get();
        const int load = (int)std::round(ph.grainLoad);
        char fmt[64];
        if(mg <= 0) std::snprintf(fmt, sizeof(fmt), "Off  (%d now)", load);
        else        std::snprintf(fmt, sizeof(fmt), "%%d  (%d now%s)", load, ph.grainThin < 0.995f ? ", thin" : "");
        if(ImGui::SliderInt("##maxgr", &mg, 0, 512, fmt)) p.set(mg);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Most grains this engine keeps sounding at once (0 = no limit).\n"
                              "Above it, every playhead skips grains evenly (like a lower Chance).\n"
                              "\"now\" = grains sounding (estimate from rate x chance x duration).");
        drawNodePublishContextMenu(key);
    }
    // Manual trigger (all voices): a one-frame pulse on the Trigger parameter
    if(ImGui::Button("Trig##c")) {
        ph.trigger.set(vector<int>(std::max(1, n), 1));
        ph.triggerReset = true;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fire one grain on every voice");
    drawPublishedCurrentItemUnderline(ph.trigger.getEscapedName());
    drawNodePublishContextMenu(ph.trigger.getEscapedName());
    ImGui::SameLine(0, m);
    }
    if(ImGui::Button("Reset##c")) ph.syncGate.set(1);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(glUI ? (isSyncing() ? "Grainless: nothing to restart here; the LFOs follow the transport\n"
                                                "(Sync To Transport)."
                                              : "Restart the LFOs from their start phase (Pos / Amp / Pan / Cut\n"
                                                "move the delay taps).")
                        : isSyncing() ? "A grain at once on every playhead (also inside a long Mono grain).\n"
                                        "The clock and LFOs stay on the transport (Sync To Transport)."
                                      : "Restart: a grain at once on every playhead (also inside a long\n"
                                        "Mono grain), the trigger clock and the LFOs from their start phase.\n"
                                        "Tap it on the downbeat to line grains up with a loop.");
    drawPublishedCurrentItemUnderline(ph.syncGate.getEscapedName());
    drawNodePublishContextMenu(ph.syncGate.getEscapedName());
    ImGui::Spacing();
    secHdr("Region");
    sliderScalar("In",   "##inp",  ph.inPoint,  0.f, 1.f);
    sliderScalar("Out",  "##outp", ph.outPoint, 0.f, 1.f);
    ctrlMeasure = std::max(ctrlMeasure, ImGui::GetCursorPosY());   // content height
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    // ── Column 1: Grain ──────────────────────────────────────────────────────
    ImGui::BeginChild("##gbpCol1", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    secHdr(glUI ? "Delay tap" : "Grain");
    if(!glUI) {
    toggle("DynDur##c", ph.dynamicDur);
    ImGui::SameLine(0, m);
    toggle("TrgDur##c", ph.trigDur);
    }
    sliderF ("Amp",    "##amp", ph.amp,       0.f,  1.f);
    if(!glUI) {
    sliderF ("Pitch",  "##pit", ph.pitch,   -48.f, 48.f);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip("Pitch (semitones). With a Scale set, the total pitch (pitch +\n"
                          "jitter + LFO + transpose) snaps to the nearest scale note.\n"
                          "Shift+drag: fine (10x slower); Ctrl/Cmd+drag (after starting\n"
                          "it): whole semitones.");
    if(ph.trigDur.get()) {
        // TrgDur: the grain length follows the trigger period; Overlap scales it
        // (x1: back to back, x2: each grain overlaps the next by half -> with a
        // smooth envelope a seamless, gap-free stream / stutter loop)
        const std::string key = ph.overlap.getEscapedName();
        ImGui::TextUnformatted("Overlap");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float v = ph.overlap.get();
        if(gbpSliderFloat("##ovl", &v, 0.25f, 8.0f, "x%.2f", ImGuiSliderFlags_Logarithmic)) ph.overlap.set(v);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("TrgDur on: grain length = Overlap x the trigger interval.\n"
                              "x1: back to back (the envelope dips between grains), x2: each grain\n"
                              "overlaps the next by half (smooth envelope: no dips), <1: gaps.\n"
                              "Shift+drag: fine");
        drawNodePublishContextMenu(key);
    } else {
        sliderF ("Dur",    "##dur", ph.duration,  0.f,  1.f);
    }
    }
    sliderF ("Pos",    "##pos", ph.position,  0.f,  1.f);
    if(glUI && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip("Delay: 0 = now .. 1 = the whole Live Length (within In / Out)");
    sliderF ("PanAz",  "##pan", ph.panAz,     0.f,  2.f);
    if(!glUI) {
    sliderF ("Rev",    "##rev", ph.reverse,   0.f,  1.f);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip("Reverse: probability that a grain plays backwards (same material,\n"
                          "from its end). Linked voices (Trigger Link) reverse together.");
    {   // Scale + root (+ custom notes)
        static const char* scales[12] = {"Off", "Chromatic", "Major", "Minor", "Harm. minor", "Dorian",
                                         "Pent. major", "Pent. minor", "Whole tone", "Octaves",
                                         "Fifths+oct", "Custom"};
        static const char* notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const std::string sk = ph.scaleType.getEscapedName(), rk = ph.scaleRoot.getEscapedName();
        ImGui::TextUnformatted("Scale");
        drawPublishedCurrentItemUnderline(sk);
        ImGui::SameLine(labelW);
        const float rootW = 44.0f * zoom;
        ImGui::SetNextItemWidth(std::max(50.0f * zoom, colW - labelW - m - rootW - m));
        int st = std::max(0, std::min(11, ph.scaleType.get()));
        if(ImGui::Combo("##gbpScale", &st, scales, 12)) ph.scaleType.set(st);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Pitch to scale: the total pitch snaps to the nearest note of the scale\n"
                              "(ties go down). Root: the scale's tonic.");
        drawNodePublishContextMenu(sk);
        ImGui::SameLine(0, m);
        ImGui::SetNextItemWidth(rootW);
        int rt = std::max(0, std::min(11, ph.scaleRoot.get()));
        if(ImGui::Combo("##gbpRoot", &rt, notes, 12)) ph.scaleRoot.set(rt);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Root");
        drawPublishedCurrentItemUnderline(rk);
        drawNodePublishContextMenu(rk);
        if(st == 11) {   // Custom: one toggle per semitone above the root
            const std::string mk = ph.scaleMask.getEscapedName();
            const float bw = std::max(8.0f * zoom, (colW - m - 11.0f * 2.0f * zoom) / 12.0f);
            int mask = ph.scaleMask.get();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * zoom, m));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
            for(int k = 0; k < 12; k++) {
                if(k > 0) ImGui::SameLine();
                const bool on = (mask >> k) & 1;
                ImGui::PushID(k);
                if(on) ImGui::PushStyleColor(ImGuiCol_Button, gbpAccent(ph.index, 150, 0.7f));
                if(ImGui::Button(notes[(k + rt) % 12], ImVec2(bw, 0))) ph.scaleMask.set(mask ^ (1 << k));
                if(on) ImGui::PopStyleColor();
                drawNodePublishContextMenu(mk);
                ImGui::PopID();
            }
            ImGui::PopStyleVar(2);
        }
    }
    }
    ImGui::Spacing();
    secHdr("Levels");
    sliderF ("Levels", "##lvl", ph.levels,    0.f,  1.f);
    ctrlMeasure = std::max(ctrlMeasure, ImGui::GetCursorPosY());   // content height
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    // ── Column 2: Jitter ─────────────────────────────────────────────────────
    ImGui::BeginChild("##gbpCol2", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    if(!glUI) {   // Grainless: no jitter (Filter only)
    secHdr("Jitter");
    toggle("Link##cj", ph.uniqueJit);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Link voices: every voice uses voice 1's random jitter values,\n"
                          "so all voices jitter together.");
    sliderF ("Pos",   "##pjit",  ph.posJit,   0.f,  1.f);
    sliderF ("Pitch", "##ijit",  ph.pitchJit, 0.f,  12.f);
    sliderF ("Dur",   "##djit",  ph.durJit,   0.f,  1.f);
    sliderF ("Amp",   "##ajit",  ph.ampJit,   0.f,  1.f);
    sliderF ("PanAz", "##pnjit", ph.panAzJit, 0.f,  2.f);
    ImGui::Spacing();
    }
    // Filter (per voice, before panning; the Cut LFO is in the LFO row)
    secHdr("Filter");
    {
        const std::string key = ph.filterType.getEscapedName();
        ImGui::TextUnformatted("Type");
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        int ft = std::max(0, std::min(5, ph.filterType.get()));
        if(ImGui::Combo("##gbpFilt", &ft, kFilterNames, 6)) ph.filterType.set(ft);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Engine filter: LPF2 / BPF / Notch / HPF2 (2-pole), or LP-HP (DJ):\n"
                              "Cutoff left of the middle = lowpass closing, middle = open,\n"
                              "right of the middle = highpass opening.\n"
                              "Cutoff changes / Cut LFO jumps are smoothed (50 Hz one-pole lowpass), no clicks.");
        drawNodePublishContextMenu(key);
    }
    {
        const bool off = ph.filterType.get() == 0;
        if(off) ImGui::BeginDisabled();
        const std::string ck = ph.cutoff.getEscapedName();
        ImGui::TextUnformatted("Cutoff");
        drawPublishedCurrentItemUnderline(ck);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float c = ph.cutoff.get();
        // LP-HP: show which side of the middle the cutoff is on (as the SynthDef:
        // its place on the log slider, open between 0.45 and 0.55)
        char cfmt[48] = "%.0f Hz";
        if(ph.filterType.get() == 5) {
            const float xp = std::log(std::max(20.0f, c) / 20.0f) / std::log(1000.0f);
            if(xp < 0.45f)      std::snprintf(cfmt, sizeof(cfmt), "LP %.0f Hz", 40.0f * std::pow(500.0f, xp / 0.45f));
            else if(xp > 0.55f) std::snprintf(cfmt, sizeof(cfmt), "HP %.0f Hz", 20.0f * std::pow(600.0f, (xp - 0.55f) / 0.45f));
            else                std::snprintf(cfmt, sizeof(cfmt), "open");
        }
        if(gbpSliderFloat("##gbpCut", &c, 20.0f, 20000.0f, cfmt, ImGuiSliderFlags_Logarithmic))
            ph.cutoff.set(c);
        drawNodePublishContextMenu(ck);
        const std::string qk = ph.filterQ.getEscapedName();
        ImGui::TextUnformatted("Q");
        drawPublishedCurrentItemUnderline(qk);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float q = ph.filterQ.get();
        if(gbpSliderFloat("##gbpQ", &q, 0.5f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
            ph.filterQ.set(q);
        drawNodePublishContextMenu(qk);
        if(off) ImGui::EndDisabled();
    }
    ctrlMeasure = std::max(ctrlMeasure, ImGui::GetCursorPosY());   // content height
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ctrlContentH = ctrlMeasure;   // the row's height next frame (no empty gap)
}

// ── LFO live state ────────────────────────────────────────────────────────────
bool scGrainBoxPoly::drawLFOLive(ImDrawList* dl, ImVec2 pos, float w, float h, Playhead& ph, int t,
                                 int shape, float phaseOff, float speed, bool overlayOnly) {
    const float now = ofGetElapsedTimef();
    if(ph.lfoStateAt < 0.0f || now - ph.lfoStateAt > 0.5f) return false;   // no live data
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const ImU32 curve = gbpAccent(ph.index, 230);
    const ImU32 hot   = IM_COL32(255, 255, 255, 230);
    auto yOf = [&](float v) {
        return std::max(pos.y + 1.0f, std::min(pos.y + h - 1.0f, pos.y + h - std::max(0.0f, std::min(1.0f, v)) * h));
    };
    if(shape >= 4) {
        if(overlayOnly) return true;
        // Random shapes: the real output of the last ~4 cycles, newest at the right
        const float cycleSec = 240.0f / std::max(1.0f, effectiveBpm()) / std::max(0.001f, speed);
        const float span     = std::max(0.5f, std::min(30.0f, 4.0f * cycleSec));
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), gbpPal::canvas, 2.0f);
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), gbpPal::border, 2.0f);
        std::vector<ImVec2> pts;
        const auto& hst = ph.lfoHist[t];
        for(const auto& [tt, v] : hst) {
            const float age = now - tt;
            if(age > span) continue;
            const float x = pos.x + w - age / span * w;
            const float y = yOf(v);
            if(shape == 4 && !pts.empty()) pts.emplace_back(x, pts.back().y);   // steps: hold, then jump
            pts.emplace_back(x, y);
        }
        if(!pts.empty()) pts.emplace_back(pos.x + w, pts.back().y);
        if(pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), curve, 0, 1.5f * zoom);
        dl->AddCircleFilled(ImVec2(pos.x + w - 3.0f * zoom, yOf(ph.lfoValNow[t])), 3.0f * zoom, hot);
        return true;
    }
    // Periodic shapes: where the LFO is now on the drawn cycle (x = its phase
    // before the offset, as the preview draws it) and its actual value
    float ph0 = std::fmod(ph.lfoPhaseNow[t] - phaseOff + 2.0f, 1.0f);
    const float x = pos.x + ph0 * w;
    dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + h), gbpAccent(ph.index, 120), 1.0f * zoom);
    dl->AddCircleFilled(ImVec2(x, yOf(ph.lfoValNow[t])), 3.5f * zoom, hot);
    return true;
}

// ── LFO preview canvas ────────────────────────────────────────────────────────

void scGrainBoxPoly::drawLFOPreview(ImDrawList* dl, ImVec2 pos, float w, float h,
                                 int shape, float phase, float quant, float strength, float barDiv,
                                 int playhead, float powExp) {
    const ImU32 curve    = gbpAccent(playhead, 215);
    const ImU32 curveDim = gbpAccent(playhead, 90);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), gbpPal::canvas, 2.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), gbpPal::border, 2.0f);

    // ── Tempo grid (behind waveform) ─────────────────────────────────────────
    // Display = 1 LFO cycle = 1/barDiv bars = 4/barDiv beats.
    // Beat lines at phase k*barDiv/4, bar lines at phase k*barDiv.
    {
        float safeBarDiv = std::max(0.001f, barDiv);
        // Beat lines (faint) — cap at 128 to avoid overdraw
        for(int k = 1; k < 128; k++) {
            float frac = k * safeBarDiv / 4.0f;
            if(frac >= 1.0f) break;
            float xPx = pos.x + frac * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), gbpPal::grid, 1.0f);
        }
        // Bar lines (brighter)
        for(int k = 1; k < 128; k++) {
            float frac = k * safeBarDiv;
            if(frac >= 1.0f) break;
            float xPx = pos.x + frac * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), gbpPal::gridStrong, 1.5f);
        }
    }

    // Zero-strength: draw baseline only
    if(strength < 0.001f) {
        float y = pos.y + h - 1.0f;
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + w, y), curveDim);
        // Still draw phase line so user sees where phase is set
        if(phase > 0.001f) {
            float xPx = pos.x + phase * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), gbpPal::handleLight, 1.0f);
        }
        return;
    }

    // Fixed random values for deterministic randStep / randCurve preview
    static const float rv[5] = {0.30f, 0.80f, 0.15f, 0.65f, 0.45f};

    auto lfoAt = [&](float ph) -> float {
        ph = std::fmod(ph, 1.0f);
        float raw = 0.5f;
        switch(shape) {
            case 0: raw = std::sin(ph * 2.0f * float(M_PI)) * 0.5f + 0.5f; break;
            case 1: raw = 1.0f - std::abs(ph * 2.0f - 1.0f); break;
            case 2: raw = ph; break;
            case 3: raw = 1.0f - ph; break;
            case 4: { // randomStep
                int seg = std::min(3, (int)(ph * 4));
                raw = rv[seg];
            } break;
            case 5: { // randomCurve
                float idx = ph * 4.0f;
                int   i0  = std::min(3, (int)idx);
                float frac = idx - (float)i0;
                raw = rv[i0] * (1.0f - frac) + rv[i0 + 1] * frac;
            } break;
        }
        // Pow on the 0..1 shape value (as the SynthDef: identity at 1)
        if(std::abs(powExp - 1.0f) > 0.00001f)
            raw = std::pow(std::max(0.0f, std::min(1.0f, raw)), std::max(0.1f, std::min(10.0f, powExp)));
        // Quantize: N steps 0, 1/N, ..., (N-1)/N
        if(quant >= 1.0f) {
            int qi = (int)quant;
            raw = std::floor(raw * qi);
            raw = std::min(raw, (float)(qi - 1));
            raw /= qi;
        }
        return raw;
    };

    // ── Waveform (1 cycle, phase-offset applied) ──────────────────────────────
    // CPU: the per-pixel values only change on edit: cached by their inputs
    int    N    = (int)w;
    static std::map<std::string, std::vector<float>> previewCache;
    char keyBuf[128];
    std::snprintf(keyBuf, sizeof(keyBuf), "%d|%.5f|%.3f|%.4f|%d", shape, phase, quant, powExp, N);
    if(previewCache.size() > 256) previewCache.clear();
    std::vector<float>& vals = previewCache[keyBuf];
    if((int)vals.size() != N) {
        vals.resize(std::max(0, N));
        for(int i = 0; i < N; i++) vals[i] = lfoAt(std::fmod((float)i / (float)N + phase, 1.0f));
    }
    std::vector<ImVec2> line;
    if(shape != 4) line.reserve(N);
    ImVec2 prev = {-1.0f, -1.0f};
    for(int i = 0; i < N; i++) {
        // Map pixel i → waveform phase, offset by initPhase
        float val = vals[i];
        float x   = pos.x + (float)i;
        float y   = pos.y + h - val * h;
        y = std::max(pos.y + 1.0f, std::min(pos.y + h - 1.0f, y));
        if(shape == 4) {
            if(prev.x >= 0) {
                dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(x, prev.y), curve,    1.5f);
                dl->AddLine(ImVec2(x, prev.y),      ImVec2(x, y),      curveDim, 1.0f);
            }
        } else {
            line.push_back(ImVec2(x, y));
        }
        prev = {x, y};
    }
    if(line.size() > 1) dl->AddPolyline(line.data(), (int)line.size(), curve, 0, 1.5f);

    // ── Baseline ──────────────────────────────────────────────────────────────
    float by = pos.y + h - 1.0f;
    dl->AddLine(ImVec2(pos.x, by), ImVec2(pos.x + w, by), gbpPal::grid);

    // ── Phase marker (where the cycle starts / sync-reset target) ────────────
    // Drawn last so it's always visible on top of the waveform.
    // Phase=0 is at the left edge (no line needed); draw for any meaningful offset.
    if(phase > 0.001f) {
        float xPx = pos.x + phase * w;
        dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), gbpPal::handleLight, 1.0f);
        // Small tick at bottom
        dl->AddTriangleFilled(
            ImVec2(xPx, pos.y + h),
            ImVec2(xPx - 3.0f, pos.y + h - 5.0f),
            ImVec2(xPx + 3.0f, pos.y + h - 5.0f),
            gbpPal::handleLight);
    }
}


// ── Modulation row ────────────────────────────────────────────────────────────

void scGrainBoxPoly::drawModRow(Playhead& ph, float w, float h) {
    float zoom  = ofxOceanodeShared::getZoomLevel();
    float gap   = 4.0f * zoom;
    // Grainless: only the Pos / Amp / Pan / Cut LFOs (the others do nothing)
    const bool glMod = ph.liveInput && ph.grainless.get();
    auto lfoShown = [&](int t) { return !(glMod && (t == 1 || t == 2 || t == 5)); };
    int shownN = 0;
    for(int t = 0; t < NUM_LFO; t++) if(lfoShown(t)) shownN++;
    float colW  = (w - gap * (shownN - 1)) / std::max(1, shownN);
    float m     = 4.0f * zoom;
    float lw    = 30.0f * zoom;   // label column width inside each cell (7 columns)

    static const char* shapeLabels[6] = {"sin","tri","saw","iSw","rSt","rCv"};

    auto getF0 = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.f : p.get()[0];
    };
    const int nv = phVoices(ph);
    auto setAllF0 = [nv](ofParameter<vector<float>>& p, float v) {
        p.set(vector<float>(nv, v));
    };
    // Label | slider row with the Publish to Node menu on the slider
    auto lfoSlider = [&](const char* label, const char* id, ofParameter<vector<float>>& p,
                         float mn, float mx, const char* fmt, bool roundValue,
                         bool semitones = false, ImGuiSliderFlags flags = 0) {
        const std::string key = p.getEscapedName();
        ImGui::TextUnformatted(label);
        drawPublishedCurrentItemUnderline(key);
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(colW - lw - m);
        float v = getF0(p);
        if(gbpSliderFloat(id, &v, mn, mx, fmt, flags, semitones))
            setAllF0(p, roundValue ? std::round(v) : v);
        drawNodePublishContextMenu(key);
    };

    bool firstCol = true;
    for(int t = 0; t < NUM_LFO; t++) {
        if(!lfoShown(t)) continue;
        LfoGroup& lfo = ph.lfo[t];
        if(!firstCol) ImGui::SameLine(0.0f, gap);
        firstCol = false;

        ImGui::PushID(t);
        ImGui::BeginChild("##gbpMod", ImVec2(colW, h), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m * 0.5f, m * 0.5f));

        // Header: filled background label + Unique toggle on the right
        {
            ImDrawList* hdl  = ImGui::GetWindowDrawList();
            ImVec2      hcur = ImGui::GetCursorScreenPos();
            float       lh   = ImGui::GetTextLineHeight();
            float       padV = 3.0f * zoom;
            float       bh   = lh + padV * 2.0f;
            hdl->AddRectFilled(
                ImVec2(hcur.x, hcur.y),
                ImVec2(hcur.x + colW - m * 0.5f, hcur.y + bh),
                gbpPal::header, 3.0f * zoom);
            std::string title = LFO_TARGET_NAMES[t];
            if(t == LFO_CUT && ph.filterType.get() == 0) title += "  (filter off)";
            hdl->AddText(ImVec2(hcur.x + 5.0f * zoom, hcur.y + padV),
                         (t == LFO_CUT && ph.filterType.get() == 0) ? gbpPal::textDim : gbpPal::headerText,
                         title.c_str());
            ImGui::SetCursorScreenPos(ImVec2(hcur.x, hcur.y + bh + m * 0.5f));
        }

        // Link voices (the "Unique" parameter) + Latch
        {
            // GrainBoxPoly: one LFO per engine, added to every playhead, so
            // there is nothing to link (per-playhead motion: external vectors)
            bool uv = true;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Link##lfu", &uv);
            ImGui::EndDisabled();
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("This LFO is one per engine: its value is added to every playhead.\n"
                                  "For per-playhead motion, connect a vector (e.g. oscillator + indexer)\n"
                                  "to the published parameter.");
            if(t == LFO_AMP) {
                ofParameter<bool>& lp = ph.latchAmp;
                bool lv = lp.get();
                if(ImGui::Checkbox("Latch##lfl", &lv)) lp.set(lv);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip(t == LFO_AMP
                        ? "Latch: each grain keeps the amp (base + LFO + jitter) of its start;\n"
                          "off: the amp follows the LFO during the grain."
                        : "");
                drawNodePublishContextMenu(lp.getEscapedName(), "Latch##lfl", 0.0f, true);
            } else {
                // Pos / Dur / Pitch / Pan are always per grain; TrRt and Cut
                // (a filter on the playhead output) have no latch
                const bool always = (t != LFO_TRRT && t != LFO_CUT);
                bool lv = always;
                ImGui::BeginDisabled();
                ImGui::Checkbox("Latch##lfl", &lv);
                ImGui::EndDisabled();
                if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(always
                        ? "Always latched: a grain takes this value when it starts\n"
                          "and keeps it for its whole duration."
                        : (t == LFO_CUT
                           ? "Not available: the filter works on the engine's output,\n"
                             "not per grain; the cutoff follows the LFO continuously."
                           : "Not available: the trigger rate is not tied to one grain."));
            }
        }

        // Shape selector — 6 small toggle buttons; one Publish menu for the row
        // (right-click any of them)
        const std::string shapeKey = lfo.shape.getEscapedName();
        float btnW = (colW - 2.0f * m - 5.0f * gap * 0.5f) / 6.0f;
        int curShape = (int)std::round(getF0(lfo.shape));
        bool openShapeMenu = false;
        ImVec2 shapeRowMin(0, 0), shapeRowMax(0, 0);
        for(int s = 0; s < 6; s++) {
            if(s > 0) ImGui::SameLine(0.0f, gap * 0.5f);
            bool active = (s == curShape);
            if(active) ImGui::PushStyleColor(ImGuiCol_Button, gbpAccent(ph.index, 120, 0.8f));
            ImGui::PushID(s);
            if(ImGui::Button(shapeLabels[s], ImVec2(btnW, 0))) {
                setAllF0(lfo.shape, (float)s);
            }
            if(ImGui::IsItemClicked(ImGuiMouseButton_Right)) openShapeMenu = true;
            if(s == 0) shapeRowMin = ImGui::GetItemRectMin();
            shapeRowMax = ImGui::GetItemRectMax();
            ImGui::PopID();
            if(active) ImGui::PopStyleColor();
        }
        if(isEditorParameterPublished(shapeKey))
            ImGui::GetWindowDrawList()->AddLine(ImVec2(shapeRowMin.x, shapeRowMax.y + 1.0f),
                                                ImVec2(shapeRowMax.x, shapeRowMax.y + 1.0f),
                                                ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
        const std::string shapePopup = "##PublishToNode_" + shapeKey;
        if(openShapeMenu) ImGui::OpenPopup(shapePopup.c_str());
        if(ImGui::BeginPopup(shapePopup.c_str())) {
            drawNodePublishMenuItems(shapeKey);
            ImGui::EndPopup();
        }

        // Speed (bar divisions: 0.125=1/8 bar cycle, 1=1/bar, 4=4×/bar)
        // Speed: musical ratios on a slider centred at x1 (fractions to the
        // left, multiples to the right); the small button switches between
        // cycles per bar (Bar) and per region length (Smp: x1 = one cycle
        // while the region plays at its own speed)
        {
            static const float kRatios[] = {1.f/128, 1.f/96, 1.f/64, 1.f/48, 1.f/32, 1.f/24, 1.f/16, 1.f/12, 1.f/8, 1.f/6,
                                            1.f/4, 1.f/3, 1.f/2, 2.f/3, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f,
                                            8.f, 12.f, 16.f, 24.f, 32.f, 48.f, 64.f, 96.f, 128.f};
            constexpr int kN = (int)(sizeof(kRatios) / sizeof(kRatios[0]));
            const std::string key = lfo.speed.getEscapedName();
            ImGui::TextUnformatted("Spd");
            drawPublishedCurrentItemUnderline(key);
            ImGui::SameLine(lw);
            const bool rel = lfo.speedRel.get();
            const float bW = 30.0f * zoom;
            if(ImGui::Button(rel ? "Smp##spdm" : "Bar##spdm", ImVec2(bW, 0))) lfo.speedRel.set(!rel);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Bar: Speed = cycles per bar (1/8 = one cycle every 8 bars).\n"
                                  "Smp: Speed = cycles per region length: x1 = one cycle in the time the\n"
                                  "region takes to play at its own speed (a saw on Pos then reads it\n"
                                  "like normal playback), 1/8 = 8 x slower, x2 = twice as fast.\n"
                                  "Pos LFO in Smp: Str becomes Len, the stretch the saw sweeps; x1 sweeps\n"
                                  "it in real time, so a short Len = a short stutter loop (Live: the read\n"
                                  "point holds still, then jumps back by Len).");
            drawNodePublishContextMenu(lfo.speedRel.getEscapedName());
            ImGui::SameLine(0, 2.0f * zoom);
            ImGui::SetNextItemWidth(std::max(20.0f, colW - lw - m - bW - 2.0f * zoom));
            const float v = getF0(lfo.speed);
            int k = 0; float bd = 1e9f;
            for(int i = 0; i < kN; i++) {
                const float d = std::abs(std::log(kRatios[i]) - std::log(std::max(1e-4f, v)));
                if(d < bd) { bd = d; k = i; }
            }
            char lbl[32];
            const float r = kRatios[k];
            const bool exact = bd < 0.01f;
            if(!exact)        std::snprintf(lbl, sizeof(lbl), "x%.3g", v);
            else if(r < 1.0f) std::snprintf(lbl, sizeof(lbl), "1/%g", std::round(1.0f / r * 100.0f) / 100.0f);
            else              std::snprintf(lbl, sizeof(lbl), "x%g", r);
            if(r < 1.0f && exact && std::abs(1.0f / r - std::round(1.0f / r)) > 0.01f)
                std::snprintf(lbl, sizeof(lbl), "%d/%d", (int)std::round(r * 3.0f), 3);   // 2/3
            if(ImGui::SliderInt("##spd", &k, 0, kN - 1, lbl, ImGuiSliderFlags_AlwaysClamp))
                setAllF0(lfo.speed, kRatios[k]);
            drawNodePublishContextMenu(key);
        }
        // Phase (initial phase / sync reset target)
        lfoSlider("Phs", "##phs", lfo.phase, 0.0f, 1.0f, "%.2f", false);
        // Quant (integer)
        lfoSlider("Qnt", "##qnt", lfo.quant, 0.0f, 32.0f, "%.0f", true);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Quant: N steps 0, 1/N ... (N-1)/N of Str (0 = off, smooth).\n"
                              "E.g. Pos, Qnt 4, Str 1: 0, .25, .5, .75 of the region.");
        // Strength (range depends on target): pitch=semitones, TrRt=beatdiv
        if(t <= 1)   // Pos / Dur strength: a time amount in the current Time Units
            // (Pos in Smp mode: "Len", the loop length the saw sweeps at its speed)
            lfoSlider((t == 0 && lfo.speedRel.get()) ? "Len" : "Str", "##str", lfo.strength, lfo.strength.getMin().empty() ? 0.0f : lfo.strength.getMin()[0],
                      lfo.strength.getMax().empty() ? 1.0f : lfo.strength.getMax()[0],
                      timeUnitsCur == 0 ? "%.2f" : timeFmt(), false);
        else
            lfoSlider("Str", "##str", lfo.strength, 0.0f, kLfoStrMax[t],
                      (t == 2) ? "%.1fst" : (t == 5) ? "%.2fbd" : (t == LFO_CUT) ? "%.2foct" : "%.2f", false,
                      t == 2);
        if(t == 2 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Pitch LFO strength (semitones). With a Scale set, the total pitch\n"
                              "snaps to the scale. Shift+drag: fine; Ctrl/Cmd+drag: whole semitones.");
        // Pow: >1 favours low values, <1 high ones (1 = off)
        lfoSlider("Pow", "##pow", lfo.pow, 0.1f, 10.0f, "%.2f", false, false, ImGuiSliderFlags_Logarithmic);

        // LFO preview
        lfoCtrlBottom = std::max(lfoCtrlBottom, ImGui::GetCursorPosY());
        float previewH = h - ImGui::GetCursorPosY() - m;
        if(previewH > 16.0f * zoom) {
            ImVec2 pPos = ImGui::GetCursorScreenPos();
            float  pW   = colW - 2.0f * m;
            ImGui::Dummy(ImVec2(pW, previewH));
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            // random shapes with live data: the real recent output instead
            const bool liveRandom = curShape >= 4 &&
                drawLFOLive(pdl, pPos, pW, previewH, ph, t, curShape, getF0(lfo.phase), (float)lfoSpeedBar(ph, t), false);
            if(!liveRandom) {
                drawLFOPreview(pdl, pPos, pW, previewH,
                               curShape,
                               getF0(lfo.phase),
                               getF0(lfo.quant),
                               getF0(lfo.strength),
                               (float)lfoSpeedBar(ph, t),   // cycles per bar (Smp: converted)
                               ph.index,
                               getF0(lfo.pow));
                if(getF0(lfo.strength) > 0.0f)
                    drawLFOLive(pdl, pPos, pW, previewH, ph, t, curShape, getF0(lfo.phase), (float)lfoSpeedBar(ph, t), true);
            }
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopID();
    }
}
