//
//  scSampleBox.h
//  ofxOceanodeSuperCollider
//
//  SampleBox: a sampler instrument for one audio file.
//
//  • The node shows the performance controls (Show, Trigger/Gate, Pitch,
//    Velocity, Gain, N Chan, the audio Trig/Gate input) and whatever is
//    published. The dockable window holds the rest: the browser, the
//    waveform with the In / Out region and a playhead per sounding voice, a
//    transport strip, and tabs (Playback, Envelopes, Filter & EQ, Dynamics,
//    Space) laid out in columns as GrainBox's. Right-click any control there
//    to publish it to the node.
//  • Playing: a rising edge of Trigger/Gate (or of the audio input) starts a
//    note. One-shot plays the region once (fading out over its last 4 ms, so
//    the end never clicks), Loop repeats it until Stop (optionally
//    crossfading the seam), Gate Mode holds the note while the gate is high.
//    Polyphonic rotates over up to 8 voices; mono chokes the previous note
//    (5 ms fade) instead of jumping it back to the start.
//  • Pitch 60 = the file's own pitch. Resample couples speed and pitch;
//    Stretch (Warp1) keeps them apart. Reverse plays the region backwards.
//  • Modulation: the shared envelopes, LFOs and routing matrix (scModulation),
//    per voice for the voice targets, the latest note's for the effects.
//  • Sound: SampleBox_<K>_<N>_<R|T> (synthdefs/Defaults/SampleBox).
//  • Files: anything macOS can decode (WAV, AIFF, FLAC, MP3, M4A, CAF...),
//    decoded on a worker thread; from the browser, Load..., or a Finder drop.
//

#pragma once

#include "scNode.h"
#include "serverManager.h"
#include "scSampleBrowser.h"
#include "scSlicerBox.h"
#include "scEQEditor.h"
#include "scModulation.h"
#include "scAudioParam.h"
#include "ofxSCGroup.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include <array>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

class scSampleBox : public scNode {
public:
    static constexpr int MaxChannels = 16;
    static constexpr int MaxVoices = 8;
    explicit scSampleBox(std::vector<serverManager*> servers);
    ~scSampleBox() override;

    void setup() override;
    void update(ofEventArgs&) override;
    void setBpm(float bpm) override { mod.setBpm(bpm); }   // tempo-synced LFOs
    void draw(ofEventArgs&) override;
    void buildSynth(ofxSCServer*) override;
    void createSynth(ofxSCServer*) override;
    void free(ofxSCServer*) override;
    void setOutputBus(ofxSCServer*, int, int) override;
    int getOutputBusIndex(ofxSCServer*, int) override;
    void moveSynthBefore(ofxSCServer*, int) override;
    int getLastSynthID(ofxSCServer*) override;
    void setInputBus(ofxSCServer*, scNode*, int) override;
    void resetInputBusses(ofxSCServer*, int) override;
    void resendParametersForNRT() override;
    void presetSave(ofJson&) override;
    void loadBeforeConnections(ofJson&) override;
    void presetRecallAfterSettingParameters(ofJson&) override;

private:
    struct Control {
        std::string key;
        std::string scName;
        ofParameter<float>* f = nullptr;
        ofParameter<int>* i = nullptr;
        ofParameter<bool>* b = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<std::string()> save;
        std::function<void(const std::string&)> load;
    };
    // per server: the node is a group: the voices, then the modulation's
    // sources and routes (they read the voices' gates)
    struct Groups {
        ofxSCGroup* root = nullptr;
        ofxSCGroup* voices = nullptr;
        ofxSCGroup* sources = nullptr;
        ofxSCGroup* routes = nullptr;
        ofxSCBus* gateBus = nullptr;      // the 8 voices' gates (audio)
        bool created = false;
    };
    std::map<ofxSCServer*, Groups> groups;
    // the shared modulation: envelopes, LFOs, routing (Envelopes / LFOs / Routing tabs)
    scModulation mod;
    void freeGroups(ofxSCServer* srv);

    std::vector<serverManager*> servers;
    scSampleBrowser browser{"scSampleBox", {".wav", ".aif", ".aiff", ".flac"}};

    // ── The sample ───────────────────────────────────────────────────────────
    std::string samplePath;              // as chosen (kept in presets)
    std::string playPath;                // what scsynth reads
    int sampleChannels = 1;
    float sampleSeconds = 0.0f, sampleRate = 0.0f;
    static constexpr int WaveformBins = 4096;
    std::vector<float> waveformPeaks;
    std::future<scSlicerBox::Audio> decodeTask;
    std::string decodingPath, pendingFile, loadError;
    bool pendingLoadDialog = false;

    // ── Synths ───────────────────────────────────────────────────────────────
    std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> buffers;
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    std::map<ofxSCServer*, ofxSCBus*> visBuses;      // playheads (control, MaxVoices)
    std::map<ofxSCServer*, int> outputBuses, inputBuses;
    std::vector<float> playheads;                    // position 0..1 per voice, -1 silent
    float nextPlayheadRead = 0.0f;
    ofEventListeners nodeListeners;
    ofEventListener dragListener;

    // ── Parameters ───────────────────────────────────────────────────────────
    ofParameter<bool> showWindow;
    ofParameter<float> trigger, pitch, velocity, gain;
    ofParameter<int> channels;
    ofParameter<float> inPoint, outPoint, loopCrossfade, rate, stretch, warpWindow, transpose;
    ofParameter<bool> loop, poly, gateMode, reverse;
    ofParameter<int> voices, stretchMode;
    ofParameter<float> attack, release, pan;
    ofParameter<bool> filterOn, eqOn, compOn, satOn, reverbOn;
    ofParameter<int> filterType;
    ofParameter<float> cutoff, resonance;
    std::array<ofParameter<float>, 5> eqGain, eqFreq, eqShape;
    ofParameter<float> eqMix;
    scEQEditor eqEditor;
    ofParameter<float> compThreshold, compRatio, compAttack, compRelease, compMakeup;
    ofParameter<float> satDrive, satMix;
    ofParameter<float> revSize, revDecay, revPredelay, revLowpass, revPosition, revSpread, revMix;
    ofParameter<float> width, diffuse, bassMono, bassFreq;

    // ── Window state ─────────────────────────────────────────────────────────
    float browserWidth = 220.0f;
    float waveformHeight = 0.0f;         // screen pixels; set when the window first opens
    int regionDrag = -1;                 // 0 In, 1 Out, 2 the whole region
    float regionDragOffset = 0.0f;
    bool windowHovered = false;          // for Finder drops
    bool auditionHeld = false;           // Play held in Gate Mode
    int fireCount = 0, stopCount = 0;

    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    bool publishedSeparator = false;
    bool restoring = false;

    // Pitch and Velocity also take audio cables (mapped onto the synth's controls)
    scAudioParam pitchIn, velIn;
    void applyCables(ofxSCServer* srv, ofxSCSynth* s);
    bool cabled(ofxSCServer* srv, const std::string& scName) const {
        return (scName == "pitch" && pitchIn.bus(srv) >= 0) || (scName == "velocity" && velIn.bus(srv) >= 0);
    }
    template<typename T> void registerControl(ofParameter<T>& p, const char* scName) {
        Control c;
        c.key = p.getEscapedName();
        c.scName = scName;
        if constexpr(std::is_same<T, float>::value) c.f = &p;
        else if constexpr(std::is_same<T, int>::value) c.i = &p;
        else c.b = &p;
        c.add = [this, &p]() { return addParameter(p); };
        c.save = [&p]() { return p.toString(); };
        c.load = [&p](const std::string& s) { p.fromString(s); };
        const std::string name = c.scName;
        controls.push_back(std::move(c));
        nodeListeners.push(p.newListener([this, name](T& value) {
            if(restoring) return;
            for(auto& [srv, synth] : synths) if(synth && !cabled(srv, name)) {   // (a set would unmap a cable)
                if constexpr(std::is_same<T, bool>::value) synth->set(name, int(value));
                else synth->set(name, value);
            }
        }));
    }

    // a control of the shared modulation: publishable, saved; send on change
    template<typename T> void regMod(ofParameter<T>& p, std::function<void()> send) {
        Control c;
        c.key = p.getEscapedName();
        c.add = [this, &p]() { return addParameter(p); };
        if constexpr(std::is_same<T, std::vector<float>>::value){
            c.save = [&p]() { std::string out; for(size_t i = 0; i < p.get().size(); i++) out += (i ? "," : "") + ofToString(p.get()[i]); return out; };
            c.load = [&p](const std::string& s) { std::vector<float> v; for(auto& t : ofSplitString(s, ",", true, true)) v.push_back(ofToFloat(t)); if(!v.empty()) p.set(v); };
        }else{
            c.save = [&p]() { return p.toString(); };
            c.load = [&p](const std::string& s) { p.fromString(s); };
        }
        controls.push_back(std::move(c));
        nodeListeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
    }

    // ── Publishing ───────────────────────────────────────────────────────────
    void publish(const std::string&);
    void unpublish(const std::string&);
    bool isConnected(const std::string&) const;
    void drawPublishMenu(const std::string&);
    void drawPublishedUnderline(const std::string&) const;

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawHeader(float w);
    void drawWaveform(float w);
    void drawTransport(float w);
    void drawPlaybackTab(float w);
    void drawFilterEqTab(float w);
    void drawDynamicsTab(float w);
    void drawSpaceTab(float w);
    void drawEqEditor(float w, float h);
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f",
                bool logarithmic = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items);
    void toggle(ofParameter<bool>& p, const char* label);
    void beginColumn(const char* id);
    void endColumn(bool last = false);
    float labelW = 74.0f;                // label column of the sliders (unzoomed)

    // ── Sample and synths ────────────────────────────────────────────────────
    void loadSample(const std::string&);
    void applyDecoded(scSlicerBox::Audio&& audio);
    void ensureBuffers(ofxSCServer*);
    void releaseBuffers(ofxSCServer*);
    void ensureVisBus(ofxSCServer*);
    void replaceSynth(ofxSCServer*);
    void replaceAllSynths();
    void sendAll();
    void sendAll(ofxSCServer*, ofxSCSynth*);
    void fire();
    void stop();
    void requireSynthdefs();
    std::string synthName() const;
};
