//
//  scDataBox.h
//  ofxOceanodeSuperCollider
//
//  SC DataBox: plays any file as audio. The file's bytes are decoded in C++
//  (int8, uint8, int16, int32 or float32, interleaved into data channels),
//  written as float WAVs and loaded into one mono buffer per data channel.
//  Each output channel is a playhead (a DataBoxVoice synth, see
//  synthdefs/Defaults/DataBox/databox.scd) that loops a window around its
//  position; Position / Window / Rate / Level are vectors, one value per
//  playhead.
//
//  The window (Show) maps the whole file, lets the loaded region be dragged
//  over it, shows the decoded region with the live playheads, and holds the
//  decode settings. Those settings are inspector parameters, so presets keep
//  them and rebuild the buffers on load.
//

#ifndef scDataBox_h
#define scDataBox_h

#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include <future>

class serverManager;

class scDataBox : public scNode {
public:
    static constexpr int MAX_HEADS = 64;
    // Upper bound of decoded samples, all data channels together (64 MB of floats)
    static constexpr int64_t MAX_SAMPLES = 16 * 1024 * 1024;
    static constexpr int MAP_COLUMNS = 1024;
    static constexpr int PREVIEW_COLUMNS = 1024;

    enum Format { Int8, UInt8, Int16LE, Int16BE, Int32LE, Float32LE, NumFormats };

    explicit scDataBox(std::vector<serverManager*> servers);
    ~scDataBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;

    // scNode interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void setInputBus(ofxSCServer*, scNode*, int) override {}
    void resetInputBusses(ofxSCServer*, int) override {}
    void activate() override;
    void deactivate() override;

private:
    // ── Decoding (background) ────────────────────────────────────────────────
    struct DecodeSettings {
        std::string path;
        int format = Int8;
        int channels = 1;
        int64_t offset = 0;
        int64_t length = 0;
        int sampleRate = 44100;
        bool normalize = false;
    };
    struct DecodeResult {
        uint64_t generation = 0;
        std::vector<std::string> wavPaths;   // one mono WAV per data channel
        std::vector<std::vector<std::pair<float, float>>> preview; // min/max per column, per channel
        int64_t framesPerChannel = 0;
        bool capped = false;
        std::string error;
    };
    struct FileMap {
        std::string path;
        int64_t size = 0;
        std::vector<float> mean;       // 0..1 average byte value per column
        std::vector<float> roughness;  // 0..1 average |delta| between bytes per column
    };
    static DecodeResult decode(DecodeSettings settings, uint64_t generation, std::string tmpDir, std::string prefix);
    static FileMap scanFile(std::string path);
    static int bytesPerSample(int format);

    void requestDecode();
    void startDecode();
    void applyDecodeResult(DecodeResult result);
    void loadBuffersOnServer(ofxSCServer* server);
    void swapBuffers(ofxSCServer* server);
    void freeBuffers(std::vector<ofxSCBuffer*>& buffers);
    void deleteWavFiles(const std::vector<std::string>& paths);
    std::string getTmpDir() const;

    // ── Playheads ────────────────────────────────────────────────────────────
    ofxSCSynth* makeVoice(ofxSCServer* server, int head);
    void setNumHeads(int n);
    int bufferForHead(ofxSCServer* server, int head) const;
    template<typename T>
    static T valueFor(const std::vector<T>& v, int head, T fallback){
        return v.empty() ? fallback : v[head % v.size()];
    }
    void sendVectorToVoices(const std::string& control, const std::vector<float>& values, float fallback);
    void sendToVoices(const std::string& control, float value);

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawFileMap(float width, float height);
    void drawRegion(float width, float height);
    void drawSettings();
    void openFile();
    void resetRegionToFile();

    // ── Parameters ───────────────────────────────────────────────────────────
    ofParameter<bool> showWindow;
    ofParameter<int> numHeads;
    ofParameter<std::vector<float>> position, window, rate, level;
    ofParameter<std::string> path;
    ofParameter<int> format, dataChannels, sampleRate;
    ofParameter<int> offset, length;
    ofParameter<bool> normalize, dcRemove;
    ofParameter<float> lag, fade;

    // ── State ────────────────────────────────────────────────────────────────
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, std::vector<ofxSCSynth*>> voices;
    std::map<ofxSCServer*, ofxSCBus*> positionBuses;
    std::map<ofxSCServer*, int> outputBuses;
    // Buffers the voices play, and buffers loaded but not yet synced
    std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> buffers;
    struct PendingLoad {
        std::vector<ofxSCBuffer*> buffers;
        float startTime = 0;
    };
    std::map<ofxSCServer*, PendingLoad> pendingLoads;

    std::vector<std::string> wavPaths;      // decoded data currently in use
    std::vector<std::string> oldWavPaths;   // replaced, deleted once every server swapped
    std::vector<std::vector<std::pair<float, float>>> preview;
    int64_t framesPerChannel = 0;
    bool capped = false;
    std::string status = "No file";

    uint64_t decodeGeneration = 0;
    bool decodeRequested = false;
    float decodeRequestTime = 0;
    std::future<DecodeResult> decodeTask;
    std::future<FileMap> mapTask;
    FileMap fileMap;

    // Map interaction
    enum class Drag { None, Move, Start, End } drag = Drag::None;
    int64_t dragAnchorOffset = 0;
    float dragAnchorX = 0;

    // Last vector sent per control, so a parameter re-notified with the same
    // value (anything driven per frame) does not resend to every voice
    std::map<std::string, std::vector<float>> lastSent;
};

#endif /* scDataBox_h */
