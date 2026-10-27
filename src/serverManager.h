//
//  serverManager.h
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#ifndef serverManager_h
#define serverManager_h

#include <vector>
#include <map>
#include <string>
#include <ofConstants.h>
#include <ofEvent.h>
#include <set>
#include <memory>
#include <cstdint>
#include <functional>

class ofxSCServer;
class ofxSCSynth;
class ofxSCBus;
class scNode;
class scStart;
class scOutput;
class nodePort;

struct scPreferences{
    bool local = true;
    bool loadOnPreset = false;
    int udpPort = 57110; //u (0-65535)
    std::string bindAddress = "127.0.0.1"; //-B (set to 0.0.0.0 to listen to all)
    int numControlBusChannels = 16384; //c
    // Every output port in the graph takes MAX_NODE_CHANNELS (128) of these:
    // 4096 left room for only ~30 ports. ~8 MB of server memory at -z 64.
    int numAudioBusChannels = 32768; //a
    int numInputBusChannels = 2; //i
    int numOutputBusChannels = 16; //o
    int blockSize = 64; //z
    int hardwareBufferSize = 64; //Z
    int hardwareSampleRate = 44100; //S
    int numBuffers = 1024; //b
    int maxNodes = 1024; //n
    int maxSynthDefs = 1024; //d
    int memSize = 2048576; //m
    int numWireBufs = 8192; //w
    int numRGens = 64; //r
    int maxLogins = 1; //l
    float safetyClipThreshold = 1.26; //s
    std::string deviceName = "nil"; //H (output device, second arg)
    std::string inputDeviceName = "nil"; //H (input device, first arg)
    int verbosity = 0;
    std::string ugensPlugins = ""; //-U (list of paths separated by :)
};

// scsynth's -U: the custom UGen folders (a preference), else the app's own
// data/Supercollider/Extensions, plus scsynth's bundled plugins (scsynth does
// not search its standard locations once -U is given). Empty: no -U (the
// standard locations, ~/Library/Application Support/SuperCollider/Extensions).
// macOS uses a temporary symlink view that excludes Supernova plugin variants;
// installed packages remain unchanged, including across Sync updates.
std::string scUgenPluginPaths(const std::string& ugensPlugins, const std::string& scsynthPath);

class serverManager{
public:
    serverManager();
    ~serverManager();
    
    void setup();
    void draw();
    
    void boot();
    void initialize();
    void kill();
    void prepareForRestart();
    void loadDefs(bool loadAll = false);
    void setSynthdefReloadCallback(std::function<void()> callback){ synthdefReloadCallback = callback; }

    // Capture the same OSC graph used by realtime SC and prepare it for NRT.
    // The graph is restored to realtime operation when endNRTCapture() is
    // called, so nodes and SynthDefs do not need a second implementation.
    bool beginNRTCapture();
    void endNRTCapture(double endTime = -1.0);
    // Writes the patch's complete parameter state into the score. Called the
    // moment a capture starts rolling, while the clock still reads zero.
    void resendAllParametersForNRT();

    // One renderable layer: the audio arriving on `bus`, named after the
    // subgraph that produces it.
    struct NRTStem {
        std::string name;
        int bus = -1;
        // The mixer this input arrives at, so stems can be grouped by mixer.
        std::string mixerName;
    };
    // Every connected input of every mixer point in the graph. Empty inputs
    // are skipped, so a track with nothing patched in produces no file.
    std::vector<NRTStem> getNRTStems() const;
    bool writeNRTScore(const std::string& path, double endTime = -1.0) const;
    // Same capture, but the file-writing synth reads `bus` instead.
    bool writeNRTStemScore(const std::string& path, double endTime, int bus) const;
    std::size_t getNRTEventCount() const;
    // Whether any Output on this server has something patched into it. A
    // server with none makes no sound, so an offline render can skip it.
    bool hasConnectedOutput() const;
    int renderNRT(const std::string& scorePath, const std::string& outputPath, int outputChannels = 2) const;
    
    void setVolume(float volume);
    void setDelay(int delay);
    
    void setStereoMix(bool stereomix);
    void setStereoMixSize(int stereomixSize);
    void setHardwareSampleRate(int sampleRate);
    int getSampleRate() const { return preferences.hardwareSampleRate; }
    
    void setOutputChannel(int channel);
    void recomputeGraph();
    void teardownGraphForPresetLoad();
    // Visible scope renderers renew a lease; all viewers of a bus share one
    // capture. No polling or synths for folded nodes outside the viewport.
    const std::vector<float>* requestFoldedWaveform(const nodePort& port);
    
    void addOutput(scOutput* output);
    void removeOutput(scOutput* output);
    
    void setAudioDevices(std::vector<std::string> audioDevices){audioDeviceNames = audioDevices;}
    void setAudioDeviceNames(const std::string& outputDeviceName, const std::string& inputDeviceName, int inputChannels = 0, int outputChannels = 0);
    
    ofxSCServer* getServer(){return server;}
    // Bumped whenever every synth in the default group is gone at once: a
    // teardown (/g_freeAll) or a server (re)initialisation, which also resets
    // the bus allocators. Nodes that keep synths outside the graph compare it
    // with the value they built against (see scAnalyzerBinding).
    uint64_t getGraphGeneration() const { return graphGeneration; }
    static int getSampleRateForServer(ofxSCServer* server);
//    int getOutputBusForNode(scNode* node);
    
    // Coalesced rebuild: an Output's input changing (a connection made or
    // removed) only marks the graph; it is rebuilt once, on the next update.
    // Rewiring several connections in one go (portalize, a node moving its
    // inputs) then costs one rebuild, and sources that are disconnected and
    // reconnected within the same frame are not freed and recreated.
    void requestRecomputeGraph(){ recomputePending = true; }

    bool hasGraphNodes() const { return !nodesList.empty(); }
    // Tickets are submitted only by a successful rebuild. An older /synced
    // cannot release resources retired after that rebuild was submitted.
    uint64_t requestMixerRouting(std::shared_ptr<void> lease = {}) {
        if(!initialized) return 0;
        const auto ticket=++mixerRoutingRequested;
        if(lease) mixerRetirements.emplace_back(ticket,std::move(lease));
        requestRecomputeGraph(); return ticket;
    }
    bool mixerRoutingReady(uint64_t ticket) const { return mixerRoutingCompleted>=ticket; }

    void recomputeGraphOnce(){
        numRecomputeGraphOnce++;
        if(numRecomputeGraphOnce == outputs.size()){
            recomputeGraph();
            numRecomputeGraphOnce = 0;
        }
    }
    
    void resetRecomputeGraphOnce(){
        numRecomputeGraphOnce = 0;
    }
    
    void setSynthdefFolders(std::map<std::string, std::string> _synthdefFolders){
        synthdefFolders = _synthdefFolders;
    }

    // A node that creates synths from SynthDefs outside the Defaults folder
    // (e.g. GrainBoxPoly's FX: Echo, SpaceMaster) registers their folder here.
    // Registered folders are loaded by loadDefs() — at boot, before the
    // initialization /sync, and at NRT capture start (score time 0) — and, when
    // registered while the server runs, right away followed by a /sync:
    // areRequiredSynthdefsLoading() is true until scsynth answers it, i.e.
    // until every one of those /d_loadDir has completed.
    void requireSynthdefFolder(const std::string& folder);
    bool areRequiredSynthdefsLoading() const;
    // Loads one .scsyndef into the running server (nothing if it is not
    // running: it is in a folder loaded at boot). Followed by a /sync, so
    // areRequiredSynthdefsLoading() is true until scsynth has read it.
    void loadSynthdefFile(const std::string& path);
    // The same for every .scsyndef in a folder (and its subfolders)
    void loadSynthdefFolder(const std::string& folder);
    // Re-reads the SynthDef metadata and registers new node types, as the
    // "Load Defs" menu does
    void reloadSynthdefModels(){ if(synthdefReloadCallback) synthdefReloadCallback(); }
    
    scPreferences preferences;
    ofEvent<void> graphComputed;
private:
    // True for folders under the data SynthDef directory, which the full
    // scan in loadDefs() already covers
    static bool isInsideSynthdefRoot(const std::string& folder);
    static constexpr int foldedWaveformSamples = 64;
    struct FoldedWaveform {
        std::unique_ptr<ofxSCSynth> synth;
        std::unique_ptr<ofxSCBus> bus;
        uint64_t lastDrawFrame = 0;
    };
    std::map<int, FoldedWaveform> foldedWaveforms;
    bool foldedWaveformDefAttempted = false;
    bool foldedWaveformDefLoaded = false;
    double lastFoldedWaveformPoll = -1;
    void updateFoldedWaveforms();
    void clearFoldedWaveforms(bool serverReset = false);
    bool loadFoldedWaveformDef();
    void loadNRTSynthdefs();
    void prepareNodesForNRTCapture();
    
    void loadSynthdefsFromPreset(const std::string& path, bool forceLoad = false, bool waitForLoad = true);

    ofEventListeners listeners;
    
    ofxSCSynth *synth;
    ofxSCServer *server;
    
    bool initialized;
    
    std::vector<scOutput*> outputs;
    std::vector<scNode*> nodesList;
    std::vector<bool> graphAudioEnabled;

    static std::map<ofxSCServer*, int> serverSampleRates;
    
    // Stable allocation per node/output: adding a MacroMixer destination must
    // not renumber the origin's or an unrelated graph's audio buses.
    std::map<scNode*,std::map<int,std::unique_ptr<ofxSCBus>>> busses;
    void releaseGraphBusses();
    void watchMixerGraphSync();
    ofEventListener mixerGraphSyncListener;
    int mixerGraphSyncID=-1;
    std::map<scNode*, std::map<int, int>> outputBussesRefToNode;
    // source -> destination, with the bus between them, recorded as the graph
    // is built. Gives both the stem list and the chain to walk for its name.
    struct nodeLink { scNode* source = nullptr; scNode* destination = nullptr; int bus = -1; };
    std::vector<nodeLink> nodeLinks;
    std::map<nodePort, std::vector<scNode*>> connections;
    
    std::unique_ptr<ofxSCBus> busFromSilent;
    std::unique_ptr<ofxSCBus> transposeBus;   // Global Transpose / Scale (scGlobalPitch.h)
    void allocateTransposeBus();
    
    float volume;
    bool mute;
    int delay;
    bool stereomix;
    int stereomixSize;
    int audioDevice;
    bool dumpOsc;
    std::vector<std::string> audioDeviceNames;
    scStart* sc;
    int configuredNumInputBusChannels;
    int configuredNumOutputBusChannels;
    
    int numRecomputeGraphOnce;
    bool recomputePending = false;
    std::map<scNode*, ofEventListener> nodeDestroyedListeners;
    bool nodesListChanged = false;
    uint64_t graphGeneration = 0;
    uint64_t mixerRoutingRequested=0,mixerRoutingSubmitted=0,mixerRoutingCompleted=0;
    std::vector<std::pair<uint64_t,std::shared_ptr<void>>> mixerRetirements;
    void completeMixerRouting(uint64_t ticket) {
        mixerRoutingCompleted=std::max(mixerRoutingCompleted,ticket);
        // Destroy leases after compacting the pending list: native destruction
        // can request another graph rebuild from an event callback.
        std::vector<std::shared_ptr<void>> released;
        for(auto it=mixerRetirements.begin();it!=mixerRetirements.end();) {
            if(it->first<=mixerRoutingCompleted) { released.push_back(std::move(it->second)); it=mixerRetirements.erase(it); }
            else ++it;
        }
    }
    std::map<std::string, std::string> synthdefFolders;
    std::function<void()> synthdefReloadCallback;
    
    std::set<std::string> alreadyLoadedSynthsList;
    std::set<std::string> requiredSynthdefFolders;   // see requireSynthdefFolder
    // Required after loadDefs() ran but before the server was initialized
    std::set<std::string> foldersRequiredDuringInit;
};

#endif /* serverManager_h */
