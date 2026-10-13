//
//  scSpectralFilterBox.h
//  ofxOceanodeSuperCollider
//
//  SC Spectral Filter Box: a spectral filter drawn by a float vector.
//
//  • Server: SpectralFilterBox_<fft>_<n> (synthdefs/Defaults/SpectralFilterBox)
//    runs per channel FFT -> PV_MaskFilter -> IFFT, PV_MaskFilter being a
//    custom UGen (ugens/OceanodeSpectral) that scales each bin by a 256-point
//    mask read on a log-frequency axis, moved by a transpose. About 0.5% CPU
//    per channel.
//  • Here, at no server cost: the Mask vector (any size, e.g. a Manual Vector)
//    is resampled to 256 points (linear / smooth / steps), its two ends crossfaded into each other (Circular XFade: no
//    seam when it scrolls round), blurred, curved (Contrast), quantised, inverted, mapped to
//    a dB range and scaled by Depth; it is sent only when it changes.
//    Transpose (semitones) plus Scroll (semitones per second, wrapping round
//    the mask) plus Spread (an offset per channel) give each channel's
//    transpose, sent at the frame rate and smoothed on the server.
//  • Node: the main controls and a preview of the shaped mask where it sits
//    now; the rest is in the inspector.
//

#pragma once

#include "scNode.h"
#include "ofxSCSynth.h"
#include <map>
#include <vector>

class ofxSCServer;

class scSpectralFilterBox : public scNode {
public:
    static constexpr int MAX_CHANNELS = 16;   // SpectralFilterBox_<fft>_<n> exists for n = 1..16
    static constexpr int MASK_SIZE = 256;     // the SynthDefs' mask control

    scSpectralFilterBox();
    ~scSpectralFilterBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;

    // scNode interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void resendParametersForNRT() override;

private:
    std::string synthDefName() const;
    int channels() const;
    void rebuild();                                   // N Chan / FFT changed: replace the synths in place
    void sendAll(ofxSCSynth* synth);
    void sendMask(bool force);
    void sendTransposes(bool force);
    void sendScalar(const std::string& name, float value);
    std::vector<float> shapeMask() const;             // Mask -> the 256 gains sent
    std::vector<float> channelTransposes() const;
    float spanSemitones() const;
    void drawPreview();

    // ── Node ─────────────────────────────────────────────────────────────────
    ofParameter<int> numChannels, fftSize;            // fftSize: 0 = 2048, 1 = 4096
    ofParameter<std::vector<float>> mask;
    ofParameter<float> circularXFade, transpose, scroll, spread, depth, gainDb, mix;
    // ── Inspector ────────────────────────────────────────────────────────────
    ofParameter<int> interp;                          // 0 Linear, 1 Smooth, 2 Steps
    ofParameter<float> contrast, rangeDb, blur, lowFreq, highFreq, attack, release;
    ofParameter<int> quantize;
    ofParameter<bool> invert, wrap, hold;

    // ── State ────────────────────────────────────────────────────────────────
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;
    std::vector<float> shaped;                        // the last mask sent (and drawn)
    std::vector<float> sentTransposes;
    std::map<std::string, float> sentScalars;
    float scrollOffset = 0;                           // semitones the Scroll has moved the mask
    float lastTime = -1;
    bool maskDirty = true;
    int builtChannels = 0, builtFft = -1;
    ofEventListeners listeners;
};
