//
//  scSpectrogram.h
//  ofxOceanodeSupercollider
//
//  Self-contained scrolling spectrogram.
//  SC analysis identical to scFFTHD (4096-pt FFT → 1080 log-spaced bands),
//  plus GPU ping-pong FBO scrolling: new data always enters on the right,
//  history scrolls left. Classic spectrogram orientation: low freq at bottom.
//
//  Requires the same fftanalyzerHD SynthDefs as scFFTHD.
//

#ifndef scSpectrogram_h
#define scSpectrogram_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "imgui.h"
#include "ofFbo.h"
#include "ofShader.h"
#include "ofPixels.h"
#include "ofTexture.h"
#include <array>
#include <cmath>
#include <algorithm>

class scSpectrogram : public ofxOceanodeNodeModel {
public:
    static constexpr int   NUM_BANDS   = 1080;
    static constexpr int   FBO_WIDTH   = 1920;
    static constexpr int   FBO_HEIGHT  = NUM_BANDS;
    static constexpr float SAMPLE_RATE = 44100.0f;
    static constexpr float FREQ_MIN    = 20.0f;
    static constexpr float FREQ_MAX    = 22050.0f;

    scSpectrogram(vector<serverManager*> outputServers)
        : ofxOceanodeNodeModel("SC Spectrogram")
        , servers(outputServers)
    {
        displayMagnitudes.fill(0.0f);
    }

    ~scSpectrogram() {
        clearSynth();
    }

    void setup() override {
        // ── SC params (same as scFFTHD) ────────────────────────────────────
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0,
                                     (int)servers.size() > 0 ? 0 : 0,
                                     (int)servers.size() > 0 ? (int)servers.size()-1 : 0));
        addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
        addParameter(enabled.set("Enable", true));
        addParameter(smoothing.set("Smooth", 0.5f, 0.0f, 0.99f));

        // ── Display params ─────────────────────────────────────────────────
        addParameter(showWindow.set("Show", false));
        addParameter(speed.set("Speed", 1.0f, 0.0f, 16.0f));
        addParameter(dbFloor.set("dB Floor", -80.0f, -120.0f, 0.0f));
        addParameter(dbCeil.set("dB Ceil",     0.0f,  -60.0f, 0.0f));
        addParameter(colormapIdx.set("Colormap", 0, 0, 3)); // 0=Hot 1=Viridis 2=Phosphor 3=Gray

        addOutputParameter(textureOut.set("Texture", nullptr, nullptr, nullptr));

        addInspectorParameter(widgetWidth.set("Widget Width",   480.0f, 100.0f, 1920.0f));
        addInspectorParameter(widgetHeight.set("Widget Height", 200.0f,  40.0f,  600.0f));

        addCustomRegion(
            ofParameter<std::function<void()>>().set("Spectrogram", [this](){ drawWidget(); }),
            ofParameter<std::function<void()>>().set("Spectrogram", [this](){ drawWidget(); })
        );

        // ── Listeners ──────────────────────────────────────────────────────
        listeners.push(input.newListener([this](nodePort &p) {
            if(p.getNodeRef() && enabled.get()) recreateSynth();
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

        // ── Graphics init ──────────────────────────────────────────────────
        initFbos();
        initShader();
        initColumnTex();
    }

    void update(ofEventArgs &) override {
        if(!synth || !fftBus || !enabled.get()) return;

        const vector<float>& raw = fftBus->readValues;
        if((int)raw.size() == NUM_BANDS) {
            float coeff = smoothing.get();
            for(int i = 0; i < NUM_BANDS; i++) {
                displayMagnitudes[i] = displayMagnitudes[i] * coeff
                                     + raw[i] * (1.0f - coeff);
            }
            scrollAccum += speed.get();
            int pixels = (int)scrollAccum;
            if(pixels >= 1) {
                scrollAccum -= (float)pixels;
                doScrollPass(pixels);
            }
        }
        fftBus->requestValues();
    }

    void draw(ofEventArgs &) override {
        if(!showWindow) return;
        string title = (canvasID == "Canvas" ? "" : canvasID + "/")
                       + "SC Spectrogram " + ofToString(getNumIdentifier());
        bool open = showWindow.get();
        if(ImGui::Begin(title.c_str(), &open)) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            drawSpectrogramImage(ImGui::GetCursorScreenPos().x,
                                 ImGui::GetCursorScreenPos().y,
                                 std::max(avail.x, 100.0f),
                                 std::max(avail.y,  40.0f));
            ImGui::Dummy(avail);
        }
        ImGui::End();
        if(!open) showWindow = false;
    }

private:
    // ── SC resources ───────────────────────────────────────────────────────
    ofxSCSynth* synth  = nullptr;
    ofxSCBus*   fftBus = nullptr;

    // ── Parameters ─────────────────────────────────────────────────────────
    ofParameter<nodePort> input;
    ofParameter<int>      serverIndex;
    ofParameter<int>      numChannels;
    ofParameter<bool>     enabled;
    ofParameter<float>    smoothing;
    ofParameter<bool>     showWindow;
    ofParameter<float>    speed;
    ofParameter<float>    dbFloor;
    ofParameter<float>    dbCeil;
    ofParameter<int>      colormapIdx;
    ofParameter<float>    widgetWidth;
    ofParameter<float>    widgetHeight;
    ofParameter<ofTexture*> textureOut;

    ofEventListeners listeners;
    ofEventListener  serverGraphListener;

    std::array<float, NUM_BANDS> displayMagnitudes;
    vector<serverManager*> servers;

    // ── Graphics ───────────────────────────────────────────────────────────
    ofFbo     fbos[2];
    int       ping        = 0;
    ofShader  scrollShader;
    ofTexture columnTex;
    ofFloatPixels columnPix;
    float     scrollAccum = 0.0f;

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

    void clearSynth() {
        if(synth)  { synth->free();  delete synth;  synth  = nullptr; }
        if(fftBus) { fftBus->free(); delete fftBus; fftBus = nullptr; }
        displayMagnitudes.fill(0.0f);
    }

    // ── Graphics init ───────────────────────────────────────────────────────
    void initFbos() {
        ofFbo::Settings s;
        s.width          = FBO_WIDTH;
        s.height         = FBO_HEIGHT;
        s.internalformat = GL_RGBA8;
        s.useDepth       = false;
        s.useStencil     = false;
        s.minFilter      = GL_LINEAR;
        s.maxFilter      = GL_LINEAR;
        s.textureTarget  = GL_TEXTURE_2D;
        for(int i = 0; i < 2; i++) {
            fbos[i].allocate(s);
            fbos[i].begin();
            ofClear(0, 0, 0, 255);
            fbos[i].end();
        }
        textureOut = &fbos[ping].getTexture();
    }

    void initColumnTex() {
        // 1-pixel wide, NUM_BANDS tall, single float channel (magnitude)
        columnPix.allocate(1, NUM_BANDS, OF_PIXELS_R);
        columnPix.set(0.0f);
        columnTex.allocate(columnPix);
    }

    void initShader() {
        // Standard OF vertex shader
        string vert = R"(
#version 150
uniform mat4 modelViewProjectionMatrix;
in vec4 position;
void main() {
    gl_Position = modelViewProjectionMatrix * position;
}
)";
        // Scroll + colormap fragment shader
        string frag = R"(
#version 150

uniform sampler2D tHistory;   // previous frame (whole FBO)
uniform sampler2D tColumn;    // new FFT column  (1 × NUM_BANDS, magnitude 0..1)
uniform vec2  size;           // FBO dimensions in pixels
uniform float normShift;      // shift / FBO_WIDTH (normalized pixels to shift left)
uniform float dbFloor;        // lower dB bound (e.g. -80)
uniform float dbCeil;         // upper dB bound (e.g.   0)
uniform int   colormapIdx;    // 0=Hot 1=Viridis 2=Phosphor 3=Gray

out vec4 out_color;

// ── Colormaps ───────────────────────────────────────────────────────────────
// Hot: black → red → yellow → white
vec3 colorHot(float t) {
    return vec3(
        clamp(t * 3.0,       0.0, 1.0),
        clamp(t * 3.0 - 1.0, 0.0, 1.0),
        clamp(t * 3.0 - 2.0, 0.0, 1.0)
    );
}

// Viridis approximation
vec3 colorViridis(float t) {
    vec3 c0 = vec3(0.267, 0.005, 0.329);
    vec3 c1 = vec3(0.128, 0.567, 0.551);
    vec3 c2 = vec3(0.993, 0.906, 0.144);
    return t < 0.5 ? mix(c0, c1, t * 2.0) : mix(c1, c2, (t - 0.5) * 2.0);
}

// Green phosphor
vec3 colorPhosphor(float t) {
    return vec3(t * 0.1, t, t * 0.3);
}

vec3 applyColormap(float t) {
    if      (colormapIdx == 1) return colorViridis(t);
    else if (colormapIdx == 2) return colorPhosphor(t);
    else if (colormapIdx == 3) return vec3(t);
    return colorHot(t);
}

void main() {
    vec2 uv = gl_FragCoord.xy / size;

    if (uv.x >= 1.0 - normShift) {
        // ── New FFT column ─────────────────────────────────────────────────
        // Invert Y: band 1079 (high freq) → FBO bottom (GL y=0), band 0 (low freq) → FBO top.
        // With UV (0,0)→(1,1), AddImage maps GL y=0 to screen top → high freq at top. ✓
        float mag = texture(tColumn, vec2(0.5, 1.0 - uv.y)).r;
        float db  = 20.0 * log(max(mag, 1e-12)) / log(10.0);
        float t   = clamp((db - dbFloor) / (dbCeil - dbFloor), 0.0, 1.0);
        out_color = vec4(applyColormap(t), 1.0);
    } else {
        // ── Scroll history left ────────────────────────────────────────────
        out_color = texture(tHistory, vec2(uv.x + normShift, uv.y));
    }
}
)";
        scrollShader.setupShaderFromSource(GL_VERTEX_SHADER,   vert);
        scrollShader.setupShaderFromSource(GL_FRAGMENT_SHADER, frag);
        scrollShader.linkProgram();
    }

    // ── Per-frame scroll pass ───────────────────────────────────────────────
    void doScrollPass(int pixels) {
        // Upload new column: band 0 (lowest freq) → row 0 → GL texture bottom → FBO bottom
        for(int i = 0; i < NUM_BANDS; i++) {
            columnPix.setColor(0, i, ofFloatColor(displayMagnitudes[i]));
        }
        columnTex.loadData(columnPix);

        const float normShiftVal = (float)pixels / (float)FBO_WIDTH;

        ofFbo& readFbo  = fbos[ping];
        ofFbo& writeFbo = fbos[1 - ping];

        writeFbo.begin();
        ofClear(0, 0, 0, 255);
        scrollShader.begin();
        scrollShader.setUniformTexture("tHistory",   readFbo.getTexture(),  0);
        scrollShader.setUniformTexture("tColumn",    columnTex,             1);
        scrollShader.setUniform2f("size",            (float)FBO_WIDTH, (float)FBO_HEIGHT);
        scrollShader.setUniform1f("normShift",       normShiftVal);
        scrollShader.setUniform1f("dbFloor",         dbFloor.get());
        scrollShader.setUniform1f("dbCeil",          dbCeil.get());
        scrollShader.setUniform1i("colormapIdx",     colormapIdx.get());
        ofDrawRectangle(0, 0, FBO_WIDTH, FBO_HEIGHT);
        scrollShader.end();
        writeFbo.end();

        ping = 1 - ping;
        textureOut = &fbos[ping].getTexture();
    }

    // ── Display helpers ─────────────────────────────────────────────────────
    // Orientation is baked into the FBO (high freq at GL y=0 bottom).
    // UV (0,0)→(1,1): AddImage maps GL y=0 → screen top → high freq at top everywhere.
    void drawSpectrogramImage(float x, float y, float W, float H) {
        GLuint texId = fbos[ping].getTexture().getTextureData().textureID;
        ImVec2 p0(x, y);
        ImVec2 p1(x + W, y + H);
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)(uintptr_t)texId, p0, p1,
            ImVec2(0, 0), ImVec2(1, 1)
        );
        ImGui::SetCursorScreenPos(ImVec2(x, y + H + 2.0f));
    }

    void drawWidget() {
        static constexpr float PAD = 2.0f;
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float W = widgetWidth.get();
        const float H = widgetHeight.get();
        drawSpectrogramImage(cursor.x + PAD, cursor.y + PAD, W, H);
        ImGui::Dummy(ImVec2(W + PAD * 2.0f, 4.0f));
    }
};

#endif /* scSpectrogram_h */
