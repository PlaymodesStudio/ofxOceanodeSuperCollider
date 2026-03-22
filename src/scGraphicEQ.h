//
//  scGraphicEQ.h
//  ofxOceanodeSupercollider
//
//  5-band parametric EQ node with real-time curve visualization
//  and optional FFT spectrum overlay.
//
//  Per-channel modulation:
//    All band parameters (gain, freq, Q/slope) are vector<float>.
//    When the vector length == numChannels, each element is sent to the
//    corresponding channel via SC's /n_setn → NamedControl array mapping.
//    When the vector has length 1, the single value is broadcast to all channels.
//

#ifndef scGraphicEQ_h
#define scGraphicEQ_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include <map>
#include <array>
#include <cmath>

class ofxSCSynth;
class ofxSCServer;
class ofxSCBus;

class scGraphicEQ : public scNode {
public:
    scGraphicEQ();
    ~scGraphicEQ();

    void setup() override;
    void update(ofEventArgs &args) override;

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

    // Events for parameter synchronization (matching scSynthdef pattern)
    ofEvent<void> resendParams;
    ofEvent<std::pair<ofxSCServer*, int>> resetAudioRateBusAssignments;

private:
    // --- SuperCollider resources ---
    std::map<ofxSCServer*, ofxSCSynth*>            synthInstances;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, std::map<int, int>>     outputBuses;

    // Optional FFT analysis (per-server, when showFFT is true)
    std::map<ofxSCServer*, ofxSCSynth*> fftSynthInstances;
    std::map<ofxSCServer*, ofxSCBus*>   fftBuses;

    // --- EQ parameters (main interface) ---
    ofParameter<int> numChannels;
    int oldNumChannels = 0;

    // Band gains — vector<float> for per-channel modulation
    ofParameter<vector<float>> b1gain, b2gain, b3gain, b4gain, b5gain;

    // Band pitches (MIDI note, 0–135) — vector<float> for per-channel modulation
    // Converted to Hz before sending to SuperCollider via pitchToHz()
    ofParameter<vector<float>> b1pitch, b2pitch, b3pitch, b4pitch, b5pitch;

    // Band shapes: slope for shelves, Q for peaks — vector<float>
    ofParameter<vector<float>> b1slope;
    ofParameter<vector<float>> b2q, b3q, b4q;
    ofParameter<vector<float>> b5slope;

    // Dry/wet mix (0 = dry, 1 = wet) — vector for per-channel modulation
    ofParameter<vector<float>> mix;

    // --- Inspector-only parameters ---
    ofParameter<bool>  showFFT;
    ofParameter<float> widgetWidth;
    ofParameter<float> widgetHeight;

    // --- EQ curve visualization ---
    static constexpr int   NUM_FREQ_POINTS = 256;
    static constexpr float FREQ_MIN        = 20.0f;
    static constexpr float FREQ_MAX        = 20000.0f;
    static constexpr float GAIN_MIN_DB     = -48.0f;
    static constexpr float GAIN_MAX_DB     = +48.0f;
    // Pitch (MIDI note) range: note 0 ≈ 8 Hz, note 135 ≈ 19912 Hz
    static constexpr float PITCH_MIN       = 0.0f;
    static constexpr float PITCH_MAX       = 135.0f;

    // Convert MIDI note (float, supports fractional) to Hz
    static float pitchToHz(float midi) {
        return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
    }
    static constexpr int   NUM_BINS        = 128;
    static constexpr float FFT_FREQ_MIN    = 20.0f;
    static constexpr float FFT_FREQ_MAX    = 22050.0f;
    static constexpr float FFT_DB_FLOOR    = -80.0f;
    static constexpr float SAMPLE_RATE     = 44100.0f;

    std::array<float, NUM_FREQ_POINTS> combinedCurveDb;
    std::array<float, NUM_BINS>        fftMagnitudes;
    float                              fftSmoothingCoeff = 0.65f;
    bool                               curveNeedsUpdate  = true;

    // Biquad coefficient set
    struct BiquadCoeffs { float b0, b1, b2, a1, a2; };

    static BiquadCoeffs computeLowShelf (float freqHz, float gainDb, float slope,   float sr);
    static BiquadCoeffs computeHighShelf(float freqHz, float gainDb, float slope,   float sr);
    static BiquadCoeffs computePeakEQ   (float freqHz, float gainDb, float qFactor, float sr);
    static float        computeMagnitudeDb(const BiquadCoeffs& c, float freqHz, float sr);

    void recomputeEQCurve();

    // --- ImGui visualization ---
    void drawEQWidget();
    static float logFreqToX(float freq, float xStart, float width);
    static float dbToY(float db, float yStart, float height);

    // --- SC helpers ---
    std::string getSynthDefName() const;
    void sendAllParamsToSynth(ofxSCServer* server);
    void restoreFullState(ofxSCServer* server);

    // Helper: expand a vector to numChannels length (broadcast scalar or pass-through)
    vector<float> expandToChannels(const vector<float>& v, int nCh) const;
    // Helper: expand a pitch vector to Hz per channel
    vector<float> pitchVecToHz(const vector<float>& pitchVec, int nCh) const;
    // Helper: expand a vector of Q values to rq (1/Q) per channel
    vector<float> qToRq(const vector<float>& qVec, int nCh) const;

    // FFT synth management
    void createFFTSynth(ofxSCServer* server);
    void freeFFTSynth(ofxSCServer* server);
    void freeAllFFTSynths();

    ofEventListeners listeners;
};

#endif /* scGraphicEQ_h */
