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
//    Trails      — number of frames to keep (persistence / afterglow)
//    Dot Size    — radius of each plotted point
//    Color       — line/dot colour
//
//  Inspector parameters:
//    Widget Width / Height
//

#ifndef scLissajous_h
#define scLissajous_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
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
        addParameter(trails.set("Trails",   8,     1,     64));
        addParameter(dotSize.set("Dot Size",2.0f,  0.5f,  10.0f));
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
                    if(input->getNodeRef()) recreateSynth();
                });
            }
        }));

        listeners.push(timeWindow.newListener([this](float& v) {
            if(synth) synth->set("timeWindow", v);
        }));
    }

    void update(ofEventArgs&) override {
        if(!synth || controlBuses.empty()) return;

        // Request all buses
        for(auto* bus : controlBuses)
            if(bus) bus->requestValues();

        // Find lowest physical bus index so we can map correctly
        int lowestIdx = controlBuses[0]->index;
        for(auto* bus : controlBuses)
            if(bus && bus->index < lowestIdx) lowestIdx = bus->index;

        // Read frame: first 64 = X, next 64 = Y (wavescope3 layout)
        std::array<float, SAMPLES_PER_CH> xBuf, yBuf;
        for(int i = 0; i < TOTAL_BUSES; i++) {
            if(!controlBuses[i]) continue;
            int pos = controlBuses[i]->index - lowestIdx;
            if(pos < 0 || pos >= TOTAL_BUSES) continue;
            float val = controlBuses[i]->readValues.empty() ? 0.f
                                                             : controlBuses[i]->readValues[0];
            if(pos < SAMPLES_PER_CH)
                xBuf[pos] = val;
            else
                yBuf[pos - SAMPLES_PER_CH] = val;
        }

        // Push frame into trail deque
        history.push_back({ xBuf, yBuf });
        int maxTrails = trails.get();
        while((int)history.size() > maxTrails)
            history.pop_front();
    }

private:
    // ── SC resources ────────────────────────────────────────────────────────
    ofxSCSynth*             synth = nullptr;
    vector<ofxSCBus*>       controlBuses;

    // ── Parameters ──────────────────────────────────────────────────────────
    ofParameter<nodePort>   input;
    ofParameter<int>        serverIndex;
    ofParameter<float>      timeWindow;
    ofParameter<float>      gain;
    ofParameter<int>        trails;
    ofParameter<float>      dotSize;
    ofParameter<ofColor>    lineColor;
    ofParameter<float>      widgetWidth;
    ofParameter<float>      widgetHeight;

    // ── Listeners ────────────────────────────────────────────────────────────
    ofEventListeners        listeners;
    ofEventListener         serverGraphListener;

    // ── State ───────────────────────────────────────────────────────────────
    struct Frame {
        std::array<float, SAMPLES_PER_CH> x, y;
    };
    std::deque<Frame>       history;
    vector<serverManager*>  servers;

    // ── SC management ────────────────────────────────────────────────────────
    void recreateSynth() {
        clearSynth();
        if(!input->getNodeRef()) return;
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;

        ofxSCServer* srv = servers[serverIndex]->getServer();

        // Allocate TOTAL_BUSES individual 1-channel KR buses
        controlBuses.resize(TOTAL_BUSES, nullptr);
        for(int i = 0; i < TOTAL_BUSES; i++) {
            controlBuses[i] = new ofxSCBus(RATE_CONTROL, 1, srv);
            if(!controlBuses[i] || controlBuses[i]->index < 0) {
                clearSynth(); return;
            }
        }

        // Find lowest bus index — wavescope3 expects contiguous bus block
        int lowestIdx = controlBuses[0]->index;
        for(auto* bus : controlBuses)
            if(bus && bus->index < lowestIdx) lowestIdx = bus->index;

        // Use wavescope3_2 SynthDef (2 channels = X + Y)
        synth = new ofxSCSynth("wavescope3_2", srv);
        synth->set("in",         input->getBusIndex(srv));
        synth->set("out",        lowestIdx);
        synth->set("timeWindow", timeWindow.get());
        synth->addToTail();
    }

    void clearSynth() {
        if(synth) { synth->free(); delete synth; synth = nullptr; }
        for(auto* bus : controlBuses)
            if(bus) { bus->free(); delete bus; }
        controlBuses.clear();
        history.clear();
    }

    // ── Drawing ──────────────────────────────────────────────────────────────
    void drawWidget() {
        ImDrawList* dl     = ImGui::GetWindowDrawList();
        ImVec2      cursor = ImGui::GetCursorScreenPos();

        const float W  = widgetWidth.get();
        const float H  = widgetHeight.get();
        const float xS = cursor.x + 2.f;
        const float yS = cursor.y + 2.f;
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
            ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 4.f));
            ImGui::Dummy(ImVec2(W, 4.f));
            return;
        }

        const float g     = gain.get();
        const float ds    = dotSize.get();
        const int   nFr   = (int)history.size();
        const ofColor& col = lineColor.get();
        const int maxTrailFrames = trails.get();

        for(int fi = 0; fi < nFr; fi++) {
            const Frame& fr = history[fi];
            // Older frames are more transparent
            float ageFrac = (float)(fi + 1) / (float)nFr; // 0=oldest, 1=newest
            ImU32 c = IM_COL32(
                col.r,
                col.g,
                col.b,
                (int)(col.a * ageFrac * ageFrac)  // quadratic fade
            );

            // Draw polyline connecting the 64 samples in this frame
            for(int s = 0; s < SAMPLES_PER_CH - 1; s++) {
                float x1 = cx + ofClamp(fr.x[s]   * g, -1.f, 1.f) * rx;
                float y1 = cy - ofClamp(fr.y[s]   * g, -1.f, 1.f) * ry;
                float x2 = cx + ofClamp(fr.x[s+1] * g, -1.f, 1.f) * rx;
                float y2 = cy - ofClamp(fr.y[s+1] * g, -1.f, 1.f) * ry;
                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), c, ds * 0.5f);
            }

            // Brightest dot at the newest sample of newest frame
            if(fi == nFr - 1) {
                float xp = cx + ofClamp(fr.x[SAMPLES_PER_CH-1] * g, -1.f, 1.f) * rx;
                float yp = cy - ofClamp(fr.y[SAMPLES_PER_CH-1] * g, -1.f, 1.f) * ry;
                dl->AddCircleFilled(ImVec2(xp, yp), ds, IM_COL32(col.r, col.g, col.b, col.a));
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 4.f));
        ImGui::Dummy(ImVec2(W, 4.f));
    }
};

#endif /* scLissajous_h */
