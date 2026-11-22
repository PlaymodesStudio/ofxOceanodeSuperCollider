//
//  scLissajous.h
//  ofxOceanodeSupercollider
//
//  XY / Lissajous display — plots channel 0 (X) vs channel 1 (Y).
//  Reuses the existing wavescope3_2 SynthDef (64 samples per channel,
//  variable time-window, A2K→Out.kr pattern).
//
//  Parameters (main interface):
//    In          — audio input port (must be ≥ 2 channels)
//    Server      — server index
//    Time Window — how many seconds of history to display (0.001–1 s)
//    Gain        — scale factor applied to both axes
//    Trails      — most frames kept (a cap for Fade)
//    Fade        — seconds a drawn trace takes to fade out
//    Stroke      — line width / point size in screen pixels (1 = one pixel at any size)
//    Draw        — "Lines" joins the samples; "Points" plots each sample as a dot
//                  (like Reaktor's XY scope)
//    Dot Size    — radius of the newest point
//    Color       — line/dot colour
//    Axes        — "X / Y": channel 0 across, channel 1 up;
//                  "L / R-L": channel 0 across, channel 1 minus channel 0 up,
//                  the XY display of the Reaktor ensemble t'wanGer
//
//  In a custom GUI's CustomRegion widget the display fills the region.
//  Inspector parameters:
//    Widget Width / Height
//

#ifndef scLissajous_h
#define scLissajous_h

#include "../../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "../../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "scAnalyzerBinding.h"
#include "imgui.h"
#include <array>
#include <deque>
#include <cmath>
#include <algorithm>

class scLissajous : public ofxOceanodeNodeModel {
public:
    static constexpr int   SAMPLES_PER_CH = 64;     // must match wavescope3 SCD
    static constexpr int   NUM_CH         = 2;       // X + Y
    static constexpr int   TOTAL_BUSES    = SAMPLES_PER_CH * NUM_CH;  // 128

    scLissajous(vector<serverManager*> outputServers)
        : ofxOceanodeNodeModel("Lissajous")
        , servers(outputServers)
    {}

    ~scLissajous() { clearSynth(); }

    void setup() override {
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0,
            servers.empty() ? 0 : 0,
            servers.empty() ? 0 : (int)servers.size() - 1));
        addParameter(timeWindow.set("Time Window", 0.02f, 0.001f, 1.0f));
        addParameter(gain.set("Gain",       1.0f,  0.01f, 10.0f));
        addParameter(trails.set("Trails",   256,   1,     1024));
        addParameter(fade.set("Fade",       0.25f, 0.f,   10.f));
        addParameter(stroke.set("Stroke",   1.f,   0.5f,  10.f));
        addParameter(dotSize.set("Dot Size",2.0f,  0.5f,  10.0f));
        addParameterDropdown(axes, "Axes", 0, {"X / Y", "L / R-L"});
        addParameterDropdown(drawMode, "Draw", 0, {"Lines", "Points"});
        addParameter(lineColor.set("Color",
            ofColor(0, 220, 180, 200),
            ofColor(0,   0,   0, 0),
            ofColor(255, 255, 255, 255)));

        addInspectorParameter(widgetWidth.set("Widget Width",  240.0f, 80.0f, 800.0f));
        addInspectorParameter(widgetHeight.set("Widget Height",240.0f, 80.0f, 800.0f));

        // XY display widget
        addCustomRegion(
            ofParameter<std::function<void()>>().set("XY", [this](){ drawWidget(); }),
            ofParameter<std::function<void()>>().set("XY", [this](){ drawWidget(); })
        );

        // ── Listeners ───────────────────────────────────────────────────────
        listeners.push(input.newListener([this](nodePort& p) {
            if(p.getNodeRef() != nullptr) recreateSynth();
            else clearSynth();
        }));

        listeners.push(serverIndex.newListener([this](int& i) {
            if(input->getNodeRef()) recreateSynth();
            serverGraphListener.unsubscribe();
            if(i >= 0 && i < (int)servers.size()) {
                serverGraphListener = servers[i]->graphComputed.newListener([this](){
                    if(input->getNodeRef()) rebindOrRecreateSynth();
                });
            }
        }));
        // From the start too (not only when Server changes): a node added from
        // the menu never re-bound after a server reboot or an NRT capture
        if(serverIndex.get() >= 0 && serverIndex.get() < (int)servers.size()) {
            serverGraphListener = servers[serverIndex.get()]->graphComputed.newListener([this](){
                if(input->getNodeRef()) rebindOrRecreateSynth();
            });
        }

        listeners.push(timeWindow.newListener([this](float& v) {
            if(synth) synth->set("timeWindow", v);
        }));
    }

    void update(ofEventArgs&) override {
        if(!synth || !scopeBus) return;

        // One request for the whole block (it used to be one per sample)
        scopeBus->requestValues();

        // Read frame: first 64 = X, next 64 = Y (wavescope3 layout)
        std::array<float, SAMPLES_PER_CH> xBuf, yBuf;
        const auto& values = scopeBus->readValues;
        for(int i = 0; i < SAMPLES_PER_CH; i++) {
            xBuf[i] = i < (int)values.size() ? values[i] : 0.f;
            const int y = SAMPLES_PER_CH + i;
            yBuf[i] = y < (int)values.size() ? values[y] : 0.f;
        }

        // Push frame into trail deque
        const float now = ofGetElapsedTimef();
        history.push_back({ xBuf, yBuf, now });
        int maxTrails = trails.get();
        while((int)history.size() > maxTrails)
            history.pop_front();
        // frames older than Fade are gone (Fade 0 keeps only the newest)
        while(history.size() > 1 && now - history.front().time > fade.get())
            history.pop_front();
    }

private:
    // ── SC resources ────────────────────────────────────────────────────────
    ofxSCSynth*             synth = nullptr;
    // wavescope3_2 writes TOTAL_BUSES consecutive control buses starting at
    // "out". One bus of that many channels is that block, allocated contiguous
    // by construction, and read with a single request per frame.
    ofxSCBus*               scopeBus = nullptr;
    scAnalyzerBinding       binding;

    // ── Parameters ──────────────────────────────────────────────────────────
    ofParameter<nodePort>   input;
    ofParameter<int>        serverIndex;
    ofParameter<float>      timeWindow;
    ofParameter<float>      gain;
    ofParameter<int>        trails;
    ofParameter<float>      fade;
    ofParameter<float>      stroke;
    ofParameter<float>      dotSize;
    ofParameter<ofColor>    lineColor;
    ofParameter<int>        axes;
    ofParameter<int>        drawMode;
    ofParameter<float>      widgetWidth;
    ofParameter<float>      widgetHeight;

    // ── Listeners ────────────────────────────────────────────────────────────
    ofEventListeners        listeners;
    ofEventListener         serverGraphListener;

    // ── State ───────────────────────────────────────────────────────────────
    struct Frame {
        std::array<float, SAMPLES_PER_CH> x, y;
        float time = 0;
    };
    std::deque<Frame>       history;
    vector<serverManager*>  servers;

    // ── SC management ────────────────────────────────────────────────────────
    void recreateSynth() {
        clearSynth();
        if(!input->getNodeRef()) return;
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;

        ofxSCServer* srv = servers[serverIndex]->getServer();

        // The contiguous block wavescope3_2 writes into
        scopeBus = new ofxSCBus(RATE_CONTROL, TOTAL_BUSES, srv);
        if(scopeBus->index < 0) { clearSynth(); return; }

        // Use wavescope3_2 SynthDef (2 channels = X + Y)
        const int inBus = input->getBusIndex(srv);
        synth = new ofxSCSynth("wavescope3_2", srv);
        synth->set("in",         inBus);
        synth->set("out",        scopeBus->index);
        synth->set("timeWindow", timeWindow.get());
        synth->createAndRun(1, 1, getActive()); //addToTail
        binding.bind(servers[serverIndex], inBus);
    }

    // After a graph rebuild: the synth keeps running and only follows its
    // input to the new bus (see scAnalyzerBinding.h).
    void rebindOrRecreateSynth() {
        if(!synth || serverIndex < 0 || serverIndex >= (int)servers.size()
           || !binding.canRebind(servers[serverIndex])) {
            recreateSynth();
            return;
        }
        const int inBus = input->getBusIndex(servers[serverIndex]->getServer());
        if(inBus != binding.inBus()) {
            synth->set("in", inBus);
            binding.setInBus(inBus);
        }
    }

    void activate() override {
        if(synth) synth->run(true);
    }

    void deactivate() override {
        if(synth) synth->run(false);
    }

    void clearSynth() {
        if(synth) { synth->free(); delete synth; synth = nullptr; }
        if(scopeBus) { scopeBus->free(); delete scopeBus; scopeBus = nullptr; }
        binding.clear();
        history.clear();
    }

    // ── Drawing ──────────────────────────────────────────────────────────────
    void drawWidget() {
        float zoom = ofxOceanodeShared::getZoomLevel();
        ImDrawList* dl     = ImGui::GetWindowDrawList();
        ImVec2      cursor = ImGui::GetCursorScreenPos();

        const auto& region = ofxOceanodeShared::getCustomRegionRenderContext();
        const float W  = region.active ? std::max(1.f, region.width - 4.f * zoom) : widgetWidth.get() * zoom;
        const float H  = region.active ? std::max(1.f, region.height - 8.f * zoom) : widgetHeight.get() * zoom;
        const float xS = cursor.x + 2.f * zoom;
        const float yS = cursor.y + 2.f * zoom;
        const float xE = xS + W;
        const float yE = yS + H;
        const float cx = xS + W * 0.5f;
        const float cy = yS + H * 0.5f;
        const float rx = W * 0.5f;
        const float ry = H * 0.5f;

        // Background
        dl->AddRectFilled(ImVec2(xS, yS), ImVec2(xE, yE), IM_COL32(8, 8, 14, 255));
        dl->AddRect(ImVec2(xS, yS), ImVec2(xE, yE), IM_COL32(50, 50, 70, 255));

        // Cross-hair grid
        dl->AddLine(ImVec2(cx, yS), ImVec2(cx, yE), IM_COL32(35, 35, 50, 200));
        dl->AddLine(ImVec2(xS, cy), ImVec2(xE, cy), IM_COL32(35, 35, 50, 200));
        // Diagonal guides
        dl->AddLine(ImVec2(xS, yS), ImVec2(xE, yE), IM_COL32(25, 25, 40, 120));
        dl->AddLine(ImVec2(xE, yS), ImVec2(xS, yE), IM_COL32(25, 25, 40, 120));

        if(history.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 4.f * zoom));
            ImGui::Dummy(ImVec2(W, 4.f * zoom));
            return;
        }

        const float g     = gain.get();
        const bool  diffY = axes.get() == 1;   // Y = channel 1 - channel 0
        const float ds    = dotSize.get();
        const int   nFr   = (int)history.size();
        const ofColor& col = lineColor.get();
        const float now = ofGetElapsedTimef();
        const float fadeTime = fade.get();
        const float lineW = stroke.get();

        for(int fi = 0; fi < nFr; fi++) {
            const Frame& fr = history[fi];
            // a trace fades out linearly over Fade seconds
            const float life = (fi == nFr - 1 || fadeTime <= 0.f) ? 1.f
                             : ofClamp(1.f - (now - fr.time) / fadeTime, 0.f, 1.f);
            if(life <= 0.f) continue;
            ImU32 c = IM_COL32(col.r, col.g, col.b, (int)(col.a * life));

            if(drawMode.get() == 1) {
                // one dot per sample, Stroke pixels square
                const float h = std::max(0.5f, lineW * 0.5f);
                for(int s = 0; s < SAMPLES_PER_CH; s++) {
                    const float yv = diffY ? fr.y[s] - fr.x[s] : fr.y[s];
                    const float px = cx + ofClamp(fr.x[s] * g, -1.f, 1.f) * rx;
                    const float py = cy - ofClamp(yv * g, -1.f, 1.f) * ry;
                    dl->AddRectFilled(ImVec2(px - h, py - h), ImVec2(px + h, py + h), c);
                }
                continue;
            }
            // Draw polyline connecting the 64 samples in this frame
            for(int s = 0; s < SAMPLES_PER_CH - 1; s++) {
                const float ya = diffY ? fr.y[s]   - fr.x[s]   : fr.y[s];
                const float yb = diffY ? fr.y[s+1] - fr.x[s+1] : fr.y[s+1];
                float x1 = cx + ofClamp(fr.x[s]   * g, -1.f, 1.f) * rx;
                float y1 = cy - ofClamp(ya * g, -1.f, 1.f) * ry;
                float x2 = cx + ofClamp(fr.x[s+1] * g, -1.f, 1.f) * rx;
                float y2 = cy - ofClamp(yb * g, -1.f, 1.f) * ry;
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), c, lineW);
            }

            // Brightest dot at the newest sample of newest frame
            if(fi == nFr - 1) {
                float xp = cx + ofClamp(fr.x[SAMPLES_PER_CH-1] * g, -1.f, 1.f) * rx;
                const float yl = diffY ? fr.y[SAMPLES_PER_CH-1] - fr.x[SAMPLES_PER_CH-1] : fr.y[SAMPLES_PER_CH-1];
                float yp = cy - ofClamp(yl * g, -1.f, 1.f) * ry;
                dl->AddCircleFilled(ImVec2(xp, yp), ds * zoom, IM_COL32(col.r, col.g, col.b, col.a));
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 4.f * zoom));
        ImGui::Dummy(ImVec2(W, 4.f * zoom));
    }
};

#endif /* scLissajous_h */
