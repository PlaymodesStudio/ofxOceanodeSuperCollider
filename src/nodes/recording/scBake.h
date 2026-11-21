//
//  scBake.h
//  ofxOceanodeSuperCollider
//
//  SC Bake: a chain frozen into a looping sample, to free the CPU it used.
//
//  Right-click any SC node with an output -> "Bake to Texture Loop": the node's
//  output is rendered offline on its own (the NRT recorder's frame-stepped
//  capture, from the transport's start; ofxOceanodeSuperColliderController::
//  requestNRTBake), turned into a seamless loop (scBakeLoop.h: the start of
//  the render is skipped, the crossfade is written into the file), and this
//  node takes the original's place: every cable that left the original's
//  output now leaves this node's Out. The original stays in the patch,
//  unplugged; with no path to an Output it, and whatever fed only it, leaves
//  the server -- that is the CPU saved.
//
//  Texture mode is a snapshot: right for pads, drones and textures, whose
//  phase against the bars does not matter. It does not follow modulation or
//  the transport any more (that is the point).
//
//  Synced Loop ("Bake to Synced Loop", N bars): for sequences. Three cycles
//  are rendered from the transport's start: the first is the intro (the
//  sequence starting from silence), the second the loop (it carries the
//  first one's tails, so it wraps seamlessly as it is), the third a check of
//  how well the source repeats every N bars (shown as "repeats NN %").
//  Playback follows the transport to the sample (scTransportSync anchors,
//  BakeSync<n>): the intro from the start, then the loop; seeks land where
//  they should; stopped, it is silent. A tempo change after the bake
//  resamples the loop (the node says so: Rebake).
//
//  Timeline Span ("Bake Timeline Span", from a bar, so many bars, a tail):
//  a freeze. The render runs from the transport's start to the span's end
//  plus the tail; the file keeps the span and the tail, and plays at the
//  transport's position (BakeSpan<n>), silent before and after it -- what
//  the original played there, including its history (a reverb from before
//  the span) and its tails. Not with tempo automation (placed by beats at
//  one tempo); at most 2^24 frames (5.8 minutes at 48 kHz).
//
//  Unbake puts the cables back on the original; Rebake renders it again with
//  the node's current settings (Inspector). The file is saved with the preset
//  (copied into its samples folder, like SampleBox's samples).
//
//  Synths: synthdefs/Defaults/Bake (BakeLoop<n>, BakeSync<n>, BakeTap<n> for
//  the render).
//

#pragma once

#include "../../ofxOceanodeSuperColliderConfig.h"
#include "../../scNode.h"
#include "../../timing/scTransportSync.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class ofxOceanodeSuperColliderController;
class ofxSCSynth;
class ofxSCBuffer;
class ofxOceanodeContainer;
class ofxOceanodeAbstractParameter;

class scBake : public scNode {
public:
    explicit scBake(ofxOceanodeSuperColliderController* controller);
    ~scBake();

    // Set at registration: the menu on every node needs the controller.
    static void setController(ofxOceanodeSuperColliderController* controller){ sharedController = controller; }
    // The "Bake to Texture Loop" menu (scNode::drawNodeContextMenu), and this
    // node's own Unbake/Rebake. True when the node deleted itself.
    static bool drawNodeMenu(scNode& node);

    void setup() override;
    void update(ofEventArgs&) override;

    void presetSave(ofJson& json) override;
    void macroSave(ofJson& json, std::string path) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

    // scNode
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int getOutputBusIndex(ofxSCServer* server, int index) override;
    int getOutputNumChannels(int) override { return fileChannels > 0 ? fileChannels : -1; }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int getLastSynthID(ofxSCServer* server) override;
    void setInputBus(ofxSCServer*, scNode*, int) override {}
    void resetInputBusses(ofxSCServer*, int) override {}
    void applyGraphSuspended(ofxSCServer* server, bool suspended) override;
    std::string requiredSynthdefFolder() const override { return synthdefFolder(); }

    struct Settings {
        float length = 20.0f;
        float crossfade = 2.0f;
        float skip = 4.0f;
        int channels = 0;        // 0: what the source says it writes
        bool matchLoop = true;
        // Synced Loop
        bool synced = false;
        int bars = 4;
        double beatsPerBar = 4.0;   // the transport's, when baking
        float bpm = 120.0f;         // the tempo the loop is rendered at
        double loopBeats() const { return bars * beatsPerBar; }
        double cycleSeconds() const { return loopBeats() * 60.0 / std::max(1.0f, bpm); }
        // Timeline Span
        bool span = false;
        float fromBar = 1.0f;       // 1 = the start
        float lengthBars = 16.0f;
        float tail = 4.0f;          // seconds past the span's end
        double spanStartBeat() const { return (fromBar - 1.0) * beatsPerBar; }
        double spanEndBeat() const { return spanStartBeat() + lengthBars * beatsPerBar; }
        double beatSeconds() const { return 60.0 / std::max(1.0f, bpm); }
    };
    // Renders `source`'s output `outputIndex` and takes its place when done.
    void startBake(scNode& source, int outputIndex, const Settings& settings);

private:
    enum class State { Empty, Rendering, Looping, Baked, Unbaked, Failed };

    static ofxOceanodeSuperColliderController* sharedController;
    static std::string synthdefFolder();
    static int guessChannels(scNode& source, int outputIndex);

    void drawStatus();
    bool drawOwnMenu();
    Settings currentSettings() const;
    float transportBpm() const;
    double transportBeatsPerBar() const;
    void sendAnchors();
    void onRendered(bool ok, const std::string& message, const std::vector<std::string>& files, int filesChannels);
    void applyLoop();
    scNode* findSource() const;
    ofxOceanodeAbstractParameter* sourceOutputParameter(scNode& source) const;
    ofxOceanodeAbstractParameter* ownOutputParameter();
    void moveCables(ofxOceanodeAbstractParameter& from, ofxOceanodeAbstractParameter& to);
    bool unbake();
    // A SampleBox node takes this one's place, its pad 1 playing the baked
    // loop always; this node is deleted. True when it was.
    bool replaceWithSampleBox();
    void rebake();
    void releaseServerState(ofxSCServer* server);
    void forgetBakeSource();

    ofxOceanodeSuperColliderController* controller = nullptr;

    ofParameter<float> level;
    customGuiRegion statusRegion;
    ofParameter<float> loopLength, crossfade, skipStart;
    ofParameter<int> renderChannels;
    ofParameter<bool> matchLoop;
    ofParameter<bool> syncedMode;
    ofParameter<int> barsParam;
    ofParameter<bool> spanMode;
    ofParameter<float> fromBarParam, lengthBarsParam, tailParam;

    // What plays, and what it was baked from (saved with the preset)
    std::string filePath;
    int fileChannels = 0;
    // Synced Loop: the intro file, the loop's length in beats and frames,
    // and the tempo it was rendered at
    bool synced = false;
    std::string introPath;
    double loopBeats = 0.0;
    int loopFrames = 0;
    float bakedBpm = 0.0f;
    // Timeline Span: where the file starts (beats), its length (loopFrames)
    bool span = false;
    double spanStartBeat = 0.0;
    int fileRate = 0;           // the file's sample rate (frames per second)
    scTransportSync::Follower follower;
    int anchorIdCounter = 0;
    int anchorHardCounter = 0;
    bool followsTransport() const { return synced || span; }
    std::string sourceName;        // the original node's name in this canvas
    std::string sourceOutput;      // its output's name
    std::string loopInfo;

    State state = State::Empty;
    std::string statusText;

    // The bake in flight
    scNode* bakingSource = nullptr;
    int bakingOutput = 0;
    Settings bakingSettings;
    std::string renderPath, loopPath, introOutPath;
    ofEventListener sourceDestroyedListener;
    std::thread loopThread;
    std::atomic<bool> loopDone{false};
    bool loopOk = false;
    std::string loopMessage;
    int loopChannels = 0;
    int loopFramesOut = 0;
    int loopRateOut = 0;

    struct ServerState {
        ofxSCSynth* synth = nullptr;
        ofxSCBuffer* buffer = nullptr;
        std::string bufferPath;
        ofxSCBuffer* introBuffer = nullptr;
        std::string introBufferPath;
        bool fresh = false;     // created since the last anchor: decide its intro
        int outputBus = -1;
    };
    std::map<ofxSCServer*, ServerState> serverStates;
};
