#ifndef timelineWaveTrack_h
#define timelineWaveTrack_h

#include "scNode.h"
#include "ofxOceanodeTimeline.h"
#include "ofxOceanodeContainer.h"
#include "ofxSuperCollider.h"
#include "serverManager.h"

#include <map>
#include <set>
#include <algorithm>
#include <cmath>

// Audio source for the new timeline Wave-track type. The timeline owns the
// clips and transport mapping; this node only turns the selected clips into
// SC synths and exposes their mixed audio through one regular scNode output.
class timelineWaveTrack : public scNode, public ofxOceanodeTimelineWaveAudioProvider {
public:
    explicit timelineWaveTrack(std::vector<serverManager*> outputServers)
    : scNode("Timeline Wave Track"), servers(std::move(outputServers)) {
        color = ofColor(180, 120, 255);
    }

    ~timelineWaveTrack() override {
        if(getHostContainer() != nullptr)
            getHostContainer()->getTimelineManager().setWaveAudioProvider(nullptr);
        for(auto& serverVoices : voices) {
            for(auto& entry : serverVoices.second) freeVoice(entry.second);
        }
        destroyedNode.notify();
    }

    void setup() override {
        scNode::addOutput("Out");
        refreshTrackOptions();
        trackSelectorParameter = addParameterDropdown(trackSelector, "Wave Track", 0, trackOptions);
        addParameter(volume.set("Volume", 1.0f, 0.0f, 4.0f));
        addParameter(playbackRate.set("Playback Rate", 1.0f, 0.1f, 4.0f));
        if(getHostContainer() != nullptr)
            getHostContainer()->getTimelineManager().setWaveAudioProvider(this);
    }

    void update(ofEventArgs&) override {
        refreshTrackOptions();
    }

    void buildSynth(ofxSCServer* server) override {
        if(server == nullptr) return;
        syncVoicesFromTimeline(server);
    }

    void createSynth(ofxSCServer* server) override {
        if(server == nullptr) return;
        serverReady[server] = true;
        syncVoicesFromTimeline(server);
        for(auto& entry : voices[server]) createVoice(server, entry.first, entry.second);
    }

    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = voices.find(server);
        if(it == voices.end()) return;
        for(auto& entry : it->second)
            if(entry.second.synth != nullptr && entry.second.created) entry.second.synth->moveBefore(nodeID);
    }

    void free(ofxSCServer* server) override {
        auto it = voices.find(server);
        if(it != voices.end()) {
            for(auto& entry : it->second) freeVoice(entry.second);
            voices.erase(it);
        }
        serverReady.erase(server);
    }

    void activate() override {
        for(auto& serverVoices : voices)
            for(auto& entry : serverVoices.second)
                if(entry.second.synth != nullptr) entry.second.synth->run(true);
    }

    void deactivate() override {
        for(auto& serverVoices : voices)
            for(auto& entry : serverVoices.second)
                if(entry.second.synth != nullptr) entry.second.synth->run(false);
    }

    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        if(index != 0) return;
        outputBuses[server] = bus;
        auto it = voices.find(server);
        if(it != voices.end()) {
            for(auto& entry : it->second)
                if(entry.second.synth != nullptr) entry.second.synth->set("out", bus);
        }
    }

    int getOutputBusIndex(ofxSCServer* server, int index) override {
        if(index != 0) return -1;
        auto it = outputBuses.find(server);
        return it == outputBuses.end() ? -1 : it->second;
    }

    int getLastSynthID(ofxSCServer* server) override {
        int last = -1;
        auto it = voices.find(server);
        if(it == voices.end()) return last;
        for(auto& entry : it->second)
            if(entry.second.synth != nullptr) last = std::max(last, entry.second.synth->nodeID);
        return last;
    }

    void updateWaveClip(const std::string& trackId, const std::string& clipId,
                        const std::string& filePath, float gain, float trackVolume,
                        int numChannels, double sourceBeat, double contentDurationBeats,
                        double sourceStartBeat, double sourceFileDurationBeats,
                        float clipPlaybackRate, bool reverse, float bpm, bool isPlaying, bool active,
                        bool forceTransportSync) override {
        const std::string key = makeKey(trackId, clipId);
        DesiredState& state = states[key];
        const bool selected = selectedTrack(trackId);
        state.filePath = filePath;
        state.numChannels = numChannels;
        state.sourceBeat = sourceBeat;
        state.contentDurationBeats = std::max(1.0 / 24.0, contentDurationBeats);
        state.sourceStartBeat = std::max(0.0, sourceStartBeat);
        state.sourceFileDurationBeats = std::max(1.0 / 24.0, sourceFileDurationBeats);
        state.gain = gain * trackVolume * volume.get();
        state.clipPlaybackRate = std::max(0.0f, clipPlaybackRate);
        state.playbackRate = clipPlaybackRate * playbackRate.get();
        state.reverse = reverse;
        state.bpm = bpm > 0.0f ? bpm : 120.0f;
        state.transportPlaying = isPlaying;
        state.audible = selected && state.gain > 0.0f;
        state.playing = isPlaying && active && state.audible;
        // The synthdef owns an audio-rate read head. The transport is used
        // only for one-shot phase correction at discontinuities.
        state.forceTransportSync = forceTransportSync;

        for(auto* manager : servers) {
            if(manager == nullptr || manager->getServer() == nullptr) continue;
            auto* server = manager->getServer();
            if(serverReady[server]) ensureVoice(server, key, state);
            auto it = voices.find(server);
            if(it == voices.end()) continue;
            auto voiceIt = it->second.find(key);
            if(voiceIt == it->second.end() || voiceIt->second.synth == nullptr) continue;
            applyState(voiceIt->second, state);
        }
    }

    void releaseWaveClip(const std::string& trackId, const std::string& clipId) override {
        const std::string key = makeKey(trackId, clipId);
        states.erase(key);
        for(auto& serverVoices : voices) {
            auto it = serverVoices.second.find(key);
            if(it != serverVoices.second.end()) {
                freeVoice(it->second);
                serverVoices.second.erase(it);
            }
        }
    }

private:
    struct DesiredState {
        std::string filePath;
        int numChannels = 0;
        double sourceBeat = 0.0;
        double contentDurationBeats = 4.0;
        double sourceStartBeat = 0.0;
        double sourceFileDurationBeats = 4.0;
        float gain = 0.0f;
        // Source beats per timeline beat, as computed by the timeline. Used
        // to predict where the transport should be on the next frame.
        float clipPlaybackRate = 1.0f;
        // Effective synth rate (clip rate times this node's Playback Rate).
        float playbackRate = 1.0f;
        float bpm = 120.0f;
        bool reverse = false;
        bool playing = false;
        bool transportPlaying = false;
        bool audible = false;
        bool forceTransportSync = false;
    };

    struct Voice {
        std::string filePath;
        int numChannels = 0;
        std::vector<ofxSCBuffer*> buffers;
        ofxSCSynth* synth = nullptr;
        // Server this voice lives on (ofxSCNode::getServer() is protected).
        ofxSCServer* server = nullptr;
        double lastSourceBeat = -1.0;
        double lastUpdateTime = -1.0;
        // Reset requests are sent as a changing counter so two consecutive
        // resets can never collapse into one held flag.
        int jumpCounter = 0;
        // After the playhead leaves a clip at its natural end the synth keeps
        // its gate open until its own audio-rate read head has reached the
        // end of the slice (the synthdef closes the output there itself).
        bool tailing = false;
        double tailUntil = 0.0;
        bool created = false;
    };

    std::vector<serverManager*> servers;
    ofParameter<int> trackSelector;
    ofParameter<float> volume;
    ofParameter<float> playbackRate;
    std::shared_ptr<ofxOceanodeParameter<int>> trackSelectorParameter;
    std::vector<std::string> trackOptions;
    std::map<std::string, DesiredState> states;
    std::map<ofxSCServer*, std::map<std::string, Voice>> voices;
    std::map<ofxSCServer*, int> outputBuses;
    std::map<ofxSCServer*, bool> serverReady;
    std::set<std::string> warnedMissingPaths;
    static std::string makeKey(const std::string& trackId, const std::string& clipId) {
        return trackId + "\n" + clipId;
    }

    bool selectedTrack(const std::string& trackId) const {
        if(trackSelector.get() == 0 || getHostContainer() == nullptr) return true;
        const auto& timeline = getHostContainer()->getTimelineManager();
        int option = 1;
        for(const auto& track : timeline.getTracks()) {
            if(!track.isWaveTrack) continue;
            if(option++ == trackSelector.get()) return track.id == trackId;
        }
        return false;
    }

    void refreshTrackOptions() {
        std::vector<std::string> next{"Mix"};
        if(getHostContainer() != nullptr) {
            for(const auto& track : getHostContainer()->getTimelineManager().getTracks())
                if(track.isWaveTrack) next.push_back(track.name);
        }
        if(next == trackOptions) return;
        trackOptions = std::move(next);
        const int selected = ofClamp(trackSelector.get(), 0, static_cast<int>(trackOptions.size()) - 1);
        trackSelector.set("Wave Track", selected, 0, static_cast<int>(trackOptions.size()) - 1);
        if(trackSelectorParameter != nullptr) trackSelectorParameter->setDropdownOptions(trackOptions);
    }

    std::string resolvePath(const std::string& input) const {
        if(input.empty()) return {};
        if(input[0] == '/') return input;
        const std::string dataPath = ofToDataPath(input, true);
        if(ofFile::doesFileExist(dataPath)) return dataPath;
        return ofToDataPath("Supercollider/Samples/" + input, true);
    }

    void syncVoicesFromTimeline(ofxSCServer* server) {
        if(getHostContainer() == nullptr) return;
        for(const auto& track : getHostContainer()->getTimelineManager().getTracks()) {
            if(!track.isWaveTrack) continue;
            for(const auto& clip : track.clips) {
                if(clip.waveFilePath.empty() || clip.waveNumChannels <= 0) continue;
                DesiredState state;
                const std::string key = makeKey(track.id, clip.id);
                auto stateIt = states.find(key);
                if(stateIt != states.end()) state = stateIt->second;
                state.filePath = clip.waveFilePath;
                state.numChannels = clip.waveNumChannels;
                state.contentDurationBeats = std::max(1.0 / 24.0, clip.contentDurationBeats);
                state.sourceStartBeat = std::max(0.0, clip.waveSourceStartBeat);
                state.sourceFileDurationBeats = std::max(1.0 / 24.0,
                    clip.waveFileDurationBeats > 0.0
                        ? clip.waveFileDurationBeats
                        : clip.waveFileDurationMs * std::max(1.0f, state.bpm) / 60000.0);
                state.reverse = clip.waveReverse;
                states[key] = state;
                ensureVoice(server, key, state);
            }
        }
    }

    void ensureVoice(ofxSCServer* server, const std::string& key, const DesiredState& state) {
        if(server == nullptr || state.numChannels <= 0 || state.numChannels > 16 || state.filePath.empty()) return;
        auto& voice = voices[server][key];
        if(voice.synth != nullptr && (voice.filePath != state.filePath || voice.numChannels != state.numChannels))
            freeVoice(voice);
        if(voice.synth != nullptr) return;
        const std::string path = resolvePath(state.filePath);
        if(!ofFile::doesFileExist(path)) {
            if(warnedMissingPaths.insert(path).second)
                ofLogWarning("timelineWaveTrack") << "Wave file is unavailable to SuperCollider: " << path;
            return;
        }
        voice.filePath = state.filePath;
        voice.numChannels = state.numChannels;
        for(int ch = 0; ch < state.numChannels; ++ch) {
            auto* buffer = new ofxSCBuffer(0, 0, server);
            buffer->readChannel(path, {ch});
            voice.buffers.push_back(buffer);
        }
        voice.synth = new ofxSCSynth("waveTrackPlayer" + ofToString(state.numChannels), server);
        voice.server = server;
        for(int ch = 0; ch < state.numChannels; ++ch)
            voice.synth->set("buf" + ofToString(ch), voice.buffers[ch]->index);
        if(outputBuses.count(server)) voice.synth->set("out", outputBuses[server]);
        if(serverReady[server]) createVoice(server, key, voice);
    }

    void createVoice(ofxSCServer* server, const std::string& key, Voice& voice) {
        if(voice.synth == nullptr || voice.created) return;
        voice.synth->createAndRun(0, 1, getActive());
        voice.created = true;
        auto stateIt = states.find(key);
        if(stateIt != states.end()) applyState(voice, stateIt->second);
    }

    // Seconds of GUI/OSC latency that a clip end may lag behind the audio
    // read head. The synth is silent after its own slice end, so holding the
    // gate slightly longer than needed is inaudible.
    static constexpr double kNaturalEndWindowSec = 0.25;
    static constexpr double kTailGraceSec = 0.25;
    // Maximum disagreement between the predicted and the reported transport
    // position before it is treated as a seek.
    static constexpr double kJumpToleranceSec = 0.1;

    void sendParams(Voice& voice, const std::vector<std::pair<std::string, float>>& params) {
        if(voice.synth == nullptr) return;
        auto* server = voice.server;
        if(voice.created && voice.synth->isCreated() && server != nullptr) {
            // One /n_set message: every value (in particular beatTransport
            // and the jump counter) is applied in the same control block,
            // so the reset latches the phase that belongs to this frame.
            ofxOscMessage m;
            m.setAddress("/n_set");
            m.addIntArg(voice.synth->nodeID);
            for(const auto& param : params) {
                m.addStringArg(param.first);
                m.addFloatArg(param.second);
            }
            server->sendMsg(m);
        } else {
            for(const auto& param : params)
                voice.synth->set(param.first, static_cast<double>(param.second));
        }
    }

    void applyState(Voice& voice, const DesiredState& state) {
        if(voice.synth == nullptr) return;
        const double now = ofGetElapsedTimeMillis() / 1000.0;
        const double dt = voice.lastUpdateTime < 0.0 ? 0.0 : std::max(0.0, now - voice.lastUpdateTime);
        voice.lastUpdateTime = now;
        const double sourceBeatsPerSecond =
            std::max(1e-6, static_cast<double>(state.clipPlaybackRate)) * state.bpm / 60.0;

        if(!state.playing) {
            // The clip became inactive. If that happened because the playhead
            // simply ran past the clip end, the audio read head may still be
            // a few milliseconds (one GUI frame plus OSC latency) away from
            // the slice end. Cutting the gate here was what dropped the end
            // of the last slice, so let the synth finish on its own.
            if(voice.lastSourceBeat >= 0.0 && state.transportPlaying && state.audible) {
                const double remainingSec =
                    (state.contentDurationBeats - voice.lastSourceBeat) / sourceBeatsPerSecond;
                if(remainingSec < kNaturalEndWindowSec) {
                    voice.tailing = true;
                    // Remaining time at the synth's actual rate (which
                    // includes this node's Playback Rate multiplier).
                    const double audioBeatsPerSecond =
                        std::max(1e-3, static_cast<double>(state.playbackRate)) * state.bpm / 60.0;
                    const double remainingAudioSec = std::max(0.0,
                        (state.contentDurationBeats - voice.lastSourceBeat) / audioBeatsPerSecond);
                    voice.tailUntil = now + std::min(remainingAudioSec, kNaturalEndWindowSec) + kTailGraceSec;
                }
            }
            voice.lastSourceBeat = -1.0;
            if(voice.tailing && state.transportPlaying && state.audible && now < voice.tailUntil) {
                return; // leave play=1; the synth's own end fade closes it
            }
            voice.tailing = false;
            sendParams(voice, {
                {"clipStartBeat", 0.0f},
                {"clipLenBeats", static_cast<float>(state.contentDurationBeats)},
                {"sourceStartBeat", static_cast<float>(state.sourceStartBeat)},
                {"sourceFileDurationBeats", static_cast<float>(state.sourceFileDurationBeats)},
                {"bpm", state.bpm},
                {"playbackRate", state.playbackRate},
                {"reverse", state.reverse ? 1.0f : 0.0f},
                {"gain", state.gain},
                {"play", 0.0f}
            });
            return;
        }

        // A reset is required when the voice (re)starts, the timeline reports
        // a discontinuity (loop wrap), or the reported source position does
        // not follow from the previous one (a seek, even inside the clip).
        bool jumped = voice.tailing || state.forceTransportSync || voice.lastSourceBeat < 0.0;
        if(!jumped) {
            const double expected = voice.lastSourceBeat + dt * sourceBeatsPerSecond;
            const double tolerance = std::max(kJumpToleranceSec, dt * 0.5) * sourceBeatsPerSecond;
            jumped = std::abs(state.sourceBeat - expected) > tolerance;
        }
        voice.tailing = false;
        if(jumped) voice.jumpCounter = (voice.jumpCounter + 1) % (1 << 20);

        // beatTransport is sent in the same message as the reset request;
        // otherwise it only feeds the legacy in-range gate.
        sendParams(voice, {
            {"clipStartBeat", 0.0f},
            {"clipLenBeats", static_cast<float>(state.contentDurationBeats)},
            {"sourceStartBeat", static_cast<float>(state.sourceStartBeat)},
            {"sourceFileDurationBeats", static_cast<float>(state.sourceFileDurationBeats)},
            {"bpm", state.bpm},
            {"playbackRate", state.playbackRate},
            {"reverse", state.reverse ? 1.0f : 0.0f},
            {"gain", state.gain},
            {"beatTransport", static_cast<float>(state.sourceBeat)},
            {"jump", static_cast<float>(voice.jumpCounter)},
            {"play", 1.0f}
        });
        voice.lastSourceBeat = state.sourceBeat;
    }

    static void freeVoice(Voice& voice) {
        if(voice.synth != nullptr) {
            voice.synth->free();
            delete voice.synth;
            voice.synth = nullptr;
        }
        for(auto* buffer : voice.buffers) {
            if(buffer != nullptr) {
                buffer->free();
                delete buffer;
            }
        }
        voice.buffers.clear();
        voice.created = false;
        voice.lastSourceBeat = -1.0;
        voice.lastUpdateTime = -1.0;
        voice.tailing = false;
    }
};

#endif
