//
//  scFFTHD.h
//  ofxOceanodeSupercollider
//
//  High-resolution FFT spectrum analyzer — 1080 log-spaced bands.
//  Uses a 4096-point FFT (2048 real bins; bin width follows the active server sample rate).
//  Log-band aggregation (max) and normalization happen inside the SynthDef,
//  matching the proven scFFT / fftanalyzer.scd pattern exactly.
//  All 1080 output bands map to ≥1 real FFT bin — no interpolation.
//  Output is a float vector suitable for driving a texture column → spectrogramShift.
//
//  IMPORTANT: Before using this node, generate the required SynthDef binaries
//  by running "fftanalyzerHD_synthdefs.scd" once in SuperCollider.
//  Delete any old fftanalyzerHD/*.scsyndef files first.
//

#ifndef scFFTHD_h
#define scFFTHD_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "imgui.h"
#include <array>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class scFFTHD : public ofxOceanodeNodeModel {
public:
    static constexpr int   NUM_BANDS    = 1080;   // matches SynthDef output (log-aggregated)
    static constexpr float FREQ_MIN     = 20.0f;

    scFFTHD(vector<serverManager*> outputServers)
        : ofxOceanodeNodeModel("SC FFT HD")
        , servers(outputServers)
    {
        displayMagnitudes.fill(0.0f);
        outputBuffer.assign(NUM_BANDS, 0.0f);
    }

    ~scFFTHD() {
        clearSynth();
    }

    void setup() override {
        addParameter(showWindow.set("Show", false));
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0,
                                     (int)servers.size() > 0 ? 0 : 0,
                                     (int)servers.size() > 0 ? (int)servers.size()-1 : 0));
        addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
        addParameter(enabled.set("Enable", true));

        addParameter(dbScale.set("dB Y",     true));
        addParameter(dbFloor.set("dB Floor", -80.0f, -120.0f, -40.0f));
        addParameter(smoothing.set("Smooth",  0.7f,    0.0f,   0.99f));

        addOutputParameter(spectrumData.set("Spectrum",
            vector<float>(NUM_BANDS, 0.0f),
            vector<float>(NUM_BANDS, 0.0f),
            vector<float>(NUM_BANDS, 1.0f)));
        spectrumData.setSerializable(false); // never save 1080 stale floats to preset JSON

        addInspectorParameter(widgetWidth.set("Widget Width",  240.0f, 100.0f, 800.0f));
        addInspectorParameter(widgetHeight.set("Widget Height", 140.0f,  60.0f, 400.0f));

        addCustomRegion(
            ofParameter<std::function<void()>>().set("Spectrum", [this](){ drawSpectrumWidget(); }),
            ofParameter<std::function<void()>>().set("Spectrum", [this](){ drawSpectrumWidget(); })
        );

        // ── Listeners ──────────────────────────────────────────────────────
        listeners.push(input.newListener([this](nodePort &p) {
            if(p.getNodeRef() != nullptr && enabled.get()) recreateSynth();
            else clearSynth();
        }));

        listeners.push(serverIndex.newListener([this](int &i) {
            if(input->getNodeRef() && enabled.get()) recreateSynth();
            serverGraphListener.unsubscribe();
            if(i >= 0 && i < (int)servers.size()) {
                serverGraphListener = servers[i]->graphComputed.newListener([this](){
                    if(input->getNodeRef() && enabled.get()) recreateSynth();
                });
            }
        }));

        listeners.push(numChannels.newListener([this](int &) {
            if(input->getNodeRef() && enabled.get()) recreateSynth();
        }));

        listeners.push(enabled.newListener([this](bool &v) {
            if(v && input->getNodeRef()) recreateSynth();
            else clearSynth();
        }));
    }

    void update(ofEventArgs &) override {
        if(!synth || !fftBus || !enabled.get()) return;

        const vector<float>& raw = fftBus->readValues;
        if((int)raw.size() == NUM_BANDS) {
            const float coeff        = smoothing.get();
            const float oneMinusCoeff = 1.0f - coeff;
            for(int i = 0; i < NUM_BANDS; i++) {
                displayMagnitudes[i] = displayMagnitudes[i] * coeff
                                     + raw[i] * oneMinusCoeff;
                outputBuffer[i] = displayMagnitudes[i];
            }
            spectrumData = outputBuffer;
        }
        fftBus->requestValues();
    }

    void draw(ofEventArgs &) override {
        if(!showWindow) return;
        string title = (canvasID == "Canvas" ? "" : canvasID + "/")
                       + "SC FFT HD " + ofToString(getNumIdentifier());
        if(ImGui::Begin(title.c_str(), (bool*)&showWindow.get())) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            drawSpectrumFull(ImGui::GetWindowDrawList(),
                             ImGui::GetCursorScreenPos().x,
                             ImGui::GetCursorScreenPos().y,
                             std::max(avail.x, 100.0f),
                             std::max(avail.y,  60.0f));
            ImGui::Dummy(avail);
        }
        ImGui::End();
    }

private:
    // ── SC resources ───────────────────────────────────────────────────────
    ofxSCSynth* synth  = nullptr;
    ofxSCBus*   fftBus = nullptr;

    // ── Parameters ─────────────────────────────────────────────────────────
    ofParameter<bool>     showWindow;
    ofParameter<nodePort> input;
    ofParameter<int>      serverIndex;
    ofParameter<int>      numChannels;
    ofParameter<bool>     enabled;
    ofParameter<bool>     dbScale;
    ofParameter<float>    dbFloor;
    ofParameter<float>    smoothing;
    ofParameter<float>    widgetWidth;
    ofParameter<float>    widgetHeight;

    ofParameter<vector<float>> spectrumData;

    ofEventListeners listeners;
    ofEventListener  serverGraphListener;

    std::array<float, NUM_BANDS> displayMagnitudes;
    vector<float>                outputBuffer;       // pre-allocated, avoids per-frame heap alloc
    vector<serverManager*>       servers;

    float getCurrentSampleRate() const {
        if(serverIndex >= 0 && serverIndex < (int)servers.size() && servers[serverIndex] != nullptr){
            return (float)std::max(1, servers[serverIndex]->getSampleRate());
        }
        return (float)scPreferences().hardwareSampleRate;
    }

    // ── SC management ──────────────────────────────────────────────────────
    void recreateSynth() {
        clearSynth();
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
        if(!input->getNodeRef()) return;
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;

        ofxSCServer* srv = servers[serverIndex]->getServer();

        fftBus = new ofxSCBus(RATE_CONTROL, NUM_BANDS, srv);
        if(!fftBus || fftBus->index < 0 || fftBus->index >= 4096) {
            if(fftBus) { delete fftBus; fftBus = nullptr; }
            return;
        }
        if(!srv->controlBusses[fftBus->index]) {
            fftBus->free(); delete fftBus; fftBus = nullptr;
            return;
        }

        string defName = "fftanalyzerHD" + ofToString(numChannels.get());
        synth = new ofxSCSynth(defName, srv);
        synth->createAndRun(1, 1, getActive());
        synth->set("in",     input->getBusIndex(srv));
        synth->set("fftbus", fftBus->index);

        fftBus->requestValues();
    }

    void activate()   override { if(synth) synth->run(true);  }
    void deactivate() override { if(synth) synth->run(false); }

    // Called after ALL parameters are set AND all connections are remade.
    // At this point input->getNodeRef() is guaranteed to be valid if the
    // connection exists, so recreateSynth() can safely start the FFT synth.
    void presetHasLoaded() override {
        if(input->getNodeRef() && enabled.get()) recreateSynth();
    }

    void clearSynth() {
        if(synth)  { synth->free();  delete synth;  synth  = nullptr; }
        if(fftBus) { fftBus->free(); delete fftBus; fftBus = nullptr; }
        displayMagnitudes.fill(0.0f);
    }

    // ── Visualization ───────────────────────────────────────────────────────
    void drawSpectrumFull(ImDrawList* dl, float xS, float yS, float W, float totalH) {
        static constexpr float LABEL_H = 14.0f;
        const float specH     = totalH - LABEL_H;
        const float xE        = xS + W;
        const float yE        = yS + specH;
        const float yLabelTop = yE + 1.0f;
        const float floorVal  = dbFloor.get();
        const bool  useDb     = dbScale.get();
        const float freqMax   = std::max(FREQ_MIN * 1.01f, getCurrentSampleRate() / 2.0f);
        const float logRatio  = std::log(freqMax / FREQ_MIN);

        dl->PushClipRect(ImVec2(xS, yS), ImVec2(xE, yLabelTop + LABEL_H), true);

        dl->AddRectFilled(ImVec2(xS, yS), ImVec2(xE, yE),                  IM_COL32(10, 12, 18, 255));
        dl->AddRectFilled(ImVec2(xS, yE), ImVec2(xE, yLabelTop + LABEL_H), IM_COL32( 8, 10, 16, 255));
        dl->AddRect(      ImVec2(xS, yS), ImVec2(xE, yLabelTop + LABEL_H), IM_COL32(60, 60, 80, 255));

        static const float gridFreqs[]  = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        static const char* gridLabels[] = { "50","100","200","500","1k","2k","5k","10k","20k" };

        for(int g = 0; g < 9; g++) {
            if(gridFreqs[g] > freqMax) break;
            float gx = xS + W * (std::log(gridFreqs[g] / FREQ_MIN) / logRatio);
            dl->AddLine(ImVec2(gx, yS),      ImVec2(gx, yE),    IM_COL32(35, 35,  50, 200));
            dl->AddText(ImVec2(gx + 2.0f, yLabelTop),           IM_COL32(90, 90, 110, 210), gridLabels[g]);
        }

        auto magToY = [&](float mag) -> float {
            if(useDb) {
                float db = 20.0f * std::log10f(std::max(mag, 1e-12f));
                return yE - specH * ofClamp((db - floorVal) / (-floorVal), 0.0f, 1.0f);
            }
            return yE - specH * ofClamp(mag, 0.0f, 1.0f);
        };

        const int N = NUM_BANDS;
        for(int j = 0; j < N - 1; j++) {
            float x1 = xS + W * (float)j       / (float)N;
            float x2 = xS + W * (float)(j + 1) / (float)N;
            float y1 = magToY(displayMagnitudes[j]);
            float y2 = magToY(displayMagnitudes[j + 1]);
            float t  = (float)j / (float)(N - 1);
            ImU32 col = IM_COL32((int)(40+200*t), (int)(200-160*t), (int)(255-220*t), 210);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, 1.0f);
        }

        dl->PopClipRect();
    }

    void drawSpectrumWidget() {
        float zoom = ofxOceanodeShared::getZoomLevel();
        static constexpr float LABEL_H = 14.0f;
        static constexpr float PAD     =  2.0f;
        ImDrawList* dl     = ImGui::GetWindowDrawList();
        ImVec2      cursor = ImGui::GetCursorScreenPos();
        const float W      = widgetWidth.get() * zoom;
        const float totalH = (widgetHeight.get() + LABEL_H) * zoom;
        drawSpectrumFull(dl, cursor.x + PAD * zoom, cursor.y + PAD * zoom, W, totalH);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + totalH + PAD * zoom * 2.0f));
        ImGui::Dummy(ImVec2(W, 4.0f * zoom));
    }
};

#endif /* scFFTHD_h */
