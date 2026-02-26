//
//  scFFT.h
//  ofxOceanodeSupercollider
//
//  Standalone 128-bin FFT spectrum analyzer.
//  Displays a frequency spectrum and outputs band magnitudes as a float vector.
//  Toggle 'Enable' to start/stop polling (avoids OSC flooding when not needed).
//  'Show' opens a floating/dockable ImGui window (same pattern as scWavescope).
//

#ifndef scFFT_h
#define scFFT_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
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

class scFFT : public ofxOceanodeNodeModel {
public:
    static constexpr int   NUM_BINS     = 128;
    static constexpr float SAMPLE_RATE  = 44100.0f;
    static constexpr float FREQ_MIN     = 20.0f;
    static constexpr float FREQ_MAX     = 22050.0f;

    scFFT(vector<serverManager*> outputServers)
        : ofxOceanodeNodeModel("SC FFT")
        , servers(outputServers)
    {
        displayMagnitudes.fill(0.0f);
    }

    ~scFFT() {
        clearSynth();
    }

    void setup() override {
        addParameter(showWindow.set("Show", false));
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0, (int)servers.size() > 0 ? 0 : 0,
                                               (int)servers.size() > 0 ? (int)servers.size()-1 : 0));
        addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
        addParameter(enabled.set("Enable",    true));

        addParameter(logScale.set("Log X",    true));
        addParameter(dbScale.set("dB Y",      true));
        addParameter(dbFloor.set("dB Floor", -80.0f, -120.0f, -40.0f));
        addParameter(smoothing.set("Smooth",   0.7f,    0.0f,   0.99f));
        addParameter(lineMode.set("Line Mode", false));

        addOutputParameter(spectrumData.set("Spectrum",
            vector<float>(NUM_BINS, 0.0f),
            vector<float>(NUM_BINS, 0.0f),
            vector<float>(NUM_BINS, 1.0f)));

        addInspectorParameter(widgetWidth.set("Widget Width",  240.0f, 100.0f, 800.0f));
        addInspectorParameter(widgetHeight.set("Widget Height", 140.0f,  60.0f, 400.0f));

        // Embedded spectrum display (shown when node is expanded)
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

        listeners.push(numChannels.newListener([this](int &i) {
            if(i < 1 || i > MAX_NODE_CHANNELS) return;
            if(input->getNodeRef() && enabled.get()) recreateSynth();
        }));

        listeners.push(enabled.newListener([this](bool &v) {
            if(v && input->getNodeRef()) recreateSynth();
            else clearSynth();
        }));
    }

    void update(ofEventArgs &args) override {
        if(!synth || !fftBus || !enabled.get()) return;

        const vector<float>& raw = fftBus->readValues;
        if((int)raw.size() == NUM_BINS) {
            float coeff = smoothing.get();
            vector<float> out(NUM_BINS);
            for(int i = 0; i < NUM_BINS; i++) {
                displayMagnitudes[i] = displayMagnitudes[i] * coeff
                                     + raw[i] * (1.0f - coeff);
                out[i] = displayMagnitudes[i];
            }
            spectrumData = out;
        }
        fftBus->requestValues();
    }

    // Floating / dockable window — same pattern as scWavescope
    void draw(ofEventArgs &) override {
        if(!showWindow) return;
        string title = (canvasID == "Canvas" ? "" : canvasID + "/")
                       + "SC FFT " + ofToString(getNumIdentifier());
        if(ImGui::Begin(title.c_str(), (bool*)&showWindow.get())) {
            ImVec2 avail  = ImGui::GetContentRegionAvail();
            float  W      = std::max(avail.x, 100.0f);
            float  H      = std::max(avail.y,  60.0f);
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            drawSpectrumFull(ImGui::GetWindowDrawList(), cursor.x, cursor.y, W, H);
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
    ofParameter<bool>     logScale;
    ofParameter<bool>     dbScale;
    ofParameter<float>    dbFloor;
    ofParameter<float>    smoothing;
    ofParameter<bool>     lineMode;
    ofParameter<float>    widgetWidth;
    ofParameter<float>    widgetHeight;

    ofParameter<vector<float>> spectrumData;

    // ── Listeners ──────────────────────────────────────────────────────────
    ofEventListeners listeners;
    ofEventListener  serverGraphListener;

    // ── State ──────────────────────────────────────────────────────────────
    std::array<float, NUM_BINS> displayMagnitudes;
    vector<serverManager*>      servers;

    // ── SC management ──────────────────────────────────────────────────────
    void recreateSynth() {
        clearSynth();
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
        if(!input->getNodeRef()) return;
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;

        ofxSCServer* srv = servers[serverIndex]->getServer();

        fftBus = new ofxSCBus(RATE_CONTROL, NUM_BINS, srv);
        if(!fftBus || fftBus->index < 0 || fftBus->index >= 4096) {
            if(fftBus) { delete fftBus; fftBus = nullptr; }
            return;
        }
        if(!srv->controlBusses[fftBus->index]) {
            fftBus->free(); delete fftBus; fftBus = nullptr;
            return;
        }

        string defName = "fftanalyzer" + ofToString(numChannels.get());
        synth = new ofxSCSynth(defName, srv);
        synth->createAndRun(1, 1, getActive()); //addToTail
        synth->set("in",     input->getBusIndex(srv));
        synth->set("fftbus", fftBus->index);

        fftBus->requestValues();
    }

    void activate() override {
        if(synth) synth->run(true);
    }

    void deactivate() override {
        if(synth) synth->run(false);
    }

    void clearSynth() {
        if(synth) { synth->free(); delete synth; synth = nullptr; }
        if(fftBus) { fftBus->free(); delete fftBus; fftBus = nullptr; }
        displayMagnitudes.fill(0.0f);
    }

    // ── Core drawing ───────────────────────────────────────────────────────
    //
    // drawSpectrumFull: renders bars/lines + freq labels into (xS,yS)→(xS+W, yS+totalH).
    // The bottom LABEL_H pixels are reserved for the frequency labels; the rest is
    // the spectrum bars area. This is shared between the embedded widget and the
    // floating window so both stay in sync visually.
    //
    void drawSpectrumFull(ImDrawList* dl,
                          float xS, float yS,
                          float W,  float totalH)
    {
        static constexpr float LABEL_H = 14.0f;

        const float specH     = totalH - LABEL_H;   // height of bars area
        const float xE        = xS + W;
        const float yE        = yS + specH;          // bottom of bars area
        const float yLabelTop = yE + 1.0f;
        const float nyquist   = SAMPLE_RATE / 2.0f;
        const float floorVal  = dbFloor.get();
        const bool  useLog    = logScale.get();
        const bool  useDb     = dbScale.get();
        const bool  useLine   = lineMode.get();

        // Clip everything to prevent drawing outside our bounds
        dl->PushClipRect(ImVec2(xS, yS), ImVec2(xE, yLabelTop + LABEL_H), true);

        // ── Backgrounds ────────────────────────────────────────────────────
        dl->AddRectFilled(ImVec2(xS, yS), ImVec2(xE, yE),                  IM_COL32(10, 12, 18, 255));
        dl->AddRectFilled(ImVec2(xS, yE), ImVec2(xE, yLabelTop + LABEL_H), IM_COL32( 8, 10, 16, 255));
        dl->AddRect(      ImVec2(xS, yS), ImVec2(xE, yLabelTop + LABEL_H), IM_COL32(60, 60, 80, 255));

        // ── Frequency grid + labels ─────────────────────────────────────────
        static const float gridFreqs[]  = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        static const char* gridLabels[] = { "50","100","200","500","1k","2k","5k","10k","20k" };
        const float logRatio = std::log(FREQ_MAX / FREQ_MIN);

        for(int g = 0; g < 9; g++) {
            if(gridFreqs[g] > FREQ_MAX) break;
            float gx = useLog
                ? xS + W * (std::log(gridFreqs[g] / FREQ_MIN) / logRatio)
                : xS + W * (gridFreqs[g] / nyquist);

            dl->AddLine(ImVec2(gx, yS),       ImVec2(gx, yE),   IM_COL32(35, 35,  50, 200));
            dl->AddText(ImVec2(gx + 2.0f, yLabelTop),            IM_COL32(90, 90, 110, 210), gridLabels[g]);
        }

        // ── Helpers ────────────────────────────────────────────────────────
        auto bandToX = [&](float b) -> float {
            float t = b / (float)NUM_BINS;
            if(useLog) return xS + W * t;
            float fc = FREQ_MIN * std::pow(FREQ_MAX / FREQ_MIN, t);
            return xS + W * (fc / nyquist);
        };

        auto magToY = [&](float mag) -> float {
            if(useDb) {
                float db = 20.0f * std::log10f(std::max(mag, 1e-12f));
                return yE - specH * ofClamp((db - floorVal) / (-floorVal), 0.0f, 1.0f);
            }
            return yE - specH * ofClamp(mag, 0.0f, 1.0f);
        };

        auto bandColor = [&](int b, int alpha) -> ImU32 {
            float t = (float)b / (float)(NUM_BINS - 1);
            return IM_COL32((int)(40+200*t), (int)(200-160*t), (int)(255-220*t), alpha);
        };

        // ── Spectrum ───────────────────────────────────────────────────────
        if(useLine) {
            // Filled area under curve
            for(int b = 0; b < NUM_BINS - 1; b++) {
                float x1 = bandToX(b + 0.5f),    x2 = bandToX(b + 1.5f);
                float y1 = magToY(displayMagnitudes[b]),
                      y2 = magToY(displayMagnitudes[b+1]);
                dl->AddQuadFilled(ImVec2(x1,y1), ImVec2(x2,y2),
                                  ImVec2(x2,yE),  ImVec2(x1,yE), IM_COL32(60,180,220,40));
            }
            // Coloured line on top
            for(int b = 0; b < NUM_BINS - 1; b++) {
                float x1 = bandToX(b + 0.5f),    x2 = bandToX(b + 1.5f);
                float y1 = magToY(displayMagnitudes[b]),
                      y2 = magToY(displayMagnitudes[b+1]);
                dl->AddLine(ImVec2(x1,y1), ImVec2(x2,y2), bandColor(b, 230), 1.5f);
            }
        } else {
            for(int b = 0; b < NUM_BINS; b++) {
                float t1 = (float)b       / (float)NUM_BINS;
                float t2 = (float)(b + 1) / (float)NUM_BINS;

                float x1, x2;
                if(useLog) {
                    x1 = xS + W * t1;
                    x2 = xS + W * t2;
                } else {
                    float fc1 = FREQ_MIN * std::pow(FREQ_MAX / FREQ_MIN, t1);
                    float fc2 = FREQ_MIN * std::pow(FREQ_MAX / FREQ_MIN, t2);
                    x1 = xS + W * (fc1 / nyquist);
                    x2 = xS + W * (fc2 / nyquist);
                }
                if(x2 <= x1 + 0.3f) continue;

                float barTop = magToY(displayMagnitudes[b]);
                dl->AddRectFilled(ImVec2(x1, barTop), ImVec2(x2 - 0.5f, yE), bandColor(b, 210));
            }
        }

        dl->PopClipRect();
    }

    // ── Embedded node widget ────────────────────────────────────────────────
    void drawSpectrumWidget() {
        static constexpr float LABEL_H  = 14.0f;
        static constexpr float PAD      =  2.0f;

        ImDrawList* dl     = ImGui::GetWindowDrawList();
        ImVec2      cursor = ImGui::GetCursorScreenPos();

        const float W      = widgetWidth.get();
        const float specH  = widgetHeight.get();    // bars-only height set by user
        const float totalH = specH + LABEL_H;       // bars + label row

        drawSpectrumFull(dl, cursor.x + PAD, cursor.y + PAD, W, totalH);

        // Claim the full area so ImGui lays out correctly
        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + totalH + PAD * 2.0f));
        ImGui::Dummy(ImVec2(W, 4.0f));
    }
};

#endif /* scFFT_h */
