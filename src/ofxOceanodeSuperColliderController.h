//
//  ofxOceanodeSuperColliderController.h
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#ifndef ofxOceanodeSuperColliderController_h
#define ofxOceanodeSuperColliderController_h


#include "ofxOceanodeBaseController.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#if OFXOCEANODESC_HAS_TIMELINE
#include <atomic>
#include <thread>
#endif // OFXOCEANODESC_HAS_TIMELINE

class scStart;
class ofxSCServer;
class serverManager;
class scPreferences;

class ofxOceanodeSuperColliderController : public ofxOceanodeBaseController{
public:
    ofxOceanodeSuperColliderController();
    ~ofxOceanodeSuperColliderController();
    
    void createServers();
    
    void setup();
#if OFXOCEANODESC_HAS_TIMELINE
    void update() override;
#endif // OFXOCEANODESC_HAS_TIMELINE
    void draw();
    
    void killServers();

#if OFXOCEANODESC_HAS_TIMELINE
    // A capture happens in two stages, because the slow half cannot be done at
    // the moment recording starts.
    //
    // Arming rebuilds the graph for NRT and then waits, over the following
    // frames, for everything that finishes asynchronously: the server's
    // /synced handshake, /d_loadDir completions, the VST's /vst_open, and the
    // per-node parameter resend. The transport stays stopped and the time
    // provider stays disabled throughout, so all of it is stamped at score
    // time zero -- which is where a patch's opening state belongs. Recording
    // then only has to start the clock, which is instant, so it can be driven
    // from another node without the first seconds arriving late.
    //
    // Every server with something connected to an Output is captured and
    // rendered, whatever serverIndex says; the argument stays for existing
    // callers. The audio device plays all the servers at once, so recording
    // one of them alone leaves part of the patch out. With more than one, the
    // master is their sum, sample by sample, and each server's stems come
    // from its own render.
    bool armNRTRecording(int serverIndex, int outputChannels, const std::string& outputPath);
    void disarmNRTRecording();
    bool isNRTArmed() const { return nrtArmState == NRTArmState::Armed; }
    bool isNRTSettling() const { return nrtArmState == NRTArmState::Settling; }
    std::string getNRTStatus() const { return nrtStatus; }

    // Starts/stops a frame-stepped NRT capture controlled by an external node.
    // When manualStop is true, endNRTRecording() defines the exact duration.
    // Needs ofxOceanode's frame-stepped transport, so it is only available
    // together with the timeline.
    bool beginNRTRecording(int serverIndex, int outputChannels, const std::string& outputPath, bool manualStop = true);
    // Also write the full mix alongside whatever Source selects. The master
    // uses the requested output path exactly; stems append their own names.
    void setNRTRecordStems(bool recordStems){ nrtRecordStems = recordStems; }
    // With more than one server captured, also keep each server's own master
    // beside the summed one, as <name>_S<n>_MasterMix.wav. They are rendered
    // anyway to build the sum, so keeping them costs no extra render.
    void setNRTServerMasters(bool keep){ nrtServerMasters = keep; }
    int getNRTServerCount() const { return (int)outputServers.size(); }

    // One entry of a server's Source dropdown, in the order they are listed:
    // no stems, every stem at once, then one entry per mixer covering that
    // mixer's inputs, then each stem on its own. Stem busses are pre-fader,
    // so they do not sum back to the master.
    struct NRTSource {
        enum class Kind { None, AllStems, Mixer, Stem };
        Kind kind = Kind::None;
        std::string label;
        std::string mixerName;  // set for Mixer and Stem
        int stemIndex = -1;     // set for Stem
    };
    std::vector<NRTSource> getNRTSources(int serverIndex) const;
    // Just the labels, for the dropdown.
    std::vector<std::string> getNRTSourceNames(int serverIndex) const;
    // The choice travels as its label, never as a position. Arming rebuilds
    // the graph, so the list the render resolves against is not the same list
    // object the dropdown was filled from, and a position in one can mean a
    // different stem in the other. An empty label, or one that is no longer in
    // the graph, takes no stems from that server. When no server gives any
    // stems, the master is rendered whatever setNRTRecordStems() says.
    //
    // setNRTSources() takes one label per server, by server index;
    // setNRTSource() is the single-server form and sets server 0 only.
    void setNRTSources(const std::vector<std::string>& labels){ nrtSourceLabels = labels; }
    void setNRTSource(const std::string& label){ nrtSourceLabels.assign(1, label); }
    bool endNRTRecording(bool cancelled = false);
    bool isNRTRecordingActive() const { return nrtCaptureActive; }
    bool isNRTRendering() const { return nrtRendering.load(); }

    // 0..1 across every file of the current render, or -1 when nothing is
    // rendering. scsynth reports no progress of its own, so the only live
    // signal is the output files growing towards their expected size.
    float getNRTRenderProgress() const;
    // Files finished, and how many there are, for "3 of 5".
    int getNRTRenderJobsDone() const { return nrtRenderJobsDone.load(); }
    int getNRTRenderJobCount() const { return (int)nrtRenderOutputs.size(); }
    // How many scsynth processes the render may run at once. Each is a full
    // pass over the patch and they are independent processes, so a set of
    // stems costs roughly one render's wall time when this matches the file
    // count. Each also loads the plugins, so a heavy VST patch may want fewer.
    void setNRTMaxParallelRenders(int count){ nrtMaxParallelRenders = count; }
    // Run a DC blocker over every rendered file before it is handed over, the
    // same one-pole difference filter SuperCollider's LeakDC uses. Offline
    // renders of self-oscillating and asymmetric material often settle a few
    // LSBs away from zero, which costs headroom and thumps on edit points.
    void setNRTRemoveDC(bool remove){ nrtRemoveDC = remove; }
    // 0..1 through the timeline while capturing, or -1. Only meaningful when
    // the capture has a fixed duration; an externally stopped one has none.
    float getNRTCaptureProgress() const;
#endif // OFXOCEANODESC_HAS_TIMELINE

    void saveConfig(std::string filepath, scPreferences prefs);
	void saveControllerConfig(std::string filepath);
	void loadConfig(std::string filepath, scPreferences &prefs);
	
    
    vector<serverManager*> getServers(){return outputServers;}
private:
    
    void reloadAudioDevices();
    void syncAudioDeviceSelection();
    void syncAudioInputDeviceSelection();
    void syncSampleRateSelection();
    bool hasAvailableAudioDevice(const std::string& deviceName) const;
    bool hasAvailableAudioInputDevice(const std::string& deviceName) const;
    std::string getSuperColliderDeviceName(const std::string& deviceName) const;
    std::string getAudioDeviceNameFromSelection() const;
    std::string getAudioInputDeviceNameFromSelection() const;
    int getSampleRateFromSelection() const;
    void applyAudioDeviceToServers(bool restartServers);
#if OFXOCEANODESC_HAS_TIMELINE
    void startNRTRender();
    void completeNRTCapture(bool cancelled = false, double durationOverride = -1.0);
    void joinFinishedNRTThread();
    void updateNRTArming();
    // Same list as getNRTSources(), built straight from a manager so that the
    // dropdown and the render agree on what every index means.
    std::vector<NRTSource> buildNRTSources(serverManager* manager) const;
    // The servers a capture covers: every one with an Output that has
    // something patched into it, by index.
    std::vector<int> findNRTServers() const;
    // Adds 32-bit float WAVs sample by sample into a new file at `output`.
    // They must agree on channel count and rate; a shorter one counts as
    // silence past its end. Returns false, and writes nothing usable, on any
    // file it cannot read.
    static bool sumFloatWavs(const std::vector<std::string>& inputs, const std::string& output);
    // In-place DC removal on a 32-bit float WAV, per channel, streamed so the
    // file is never held in memory. The filter is LeakDC's, but its corner is
    // given in Hz and the coefficient derived from the file's own sample rate:
    // SuperCollider's default 0.995 puts the corner at 35 Hz, which is a real
    // high-pass that would thin a kick, not a DC fix. Returns false and leaves
    // the file untouched if it is not a format this understands, so an
    // unexpected file is never half-rewritten.
    static bool removeDCOffsetInPlace(const std::string& wavPath, double cornerHz = 3.5);
#endif // OFXOCEANODESC_HAS_TIMELINE

    float volume;
    bool mute;
    int delay;
    bool stereomix;
    int stereomixSize;
    int audioDevice;
    int audioInputDevice;
    int sampleRate;
    int selectedSampleRate;
    std::string selectedAudioDeviceName;
    std::string selectedAudioInputDeviceName;
    vector<string> audioDeviceNames;
    vector<string> audioDeviceSuperColliderNames;
    vector<int> audioDeviceInputChannels;
    vector<int> audioDeviceOutputChannels;
    vector<vector<int>> audioDeviceSampleRates;
    vector<string> inputDeviceNames;
    vector<string> inputDeviceSuperColliderNames;
    vector<int> inputDeviceFullIndices;
    vector<string> sampleRateNames;
    vector<int> sampleRateValues;
    vector<serverManager*> outputServers;

#if OFXOCEANODESC_HAS_TIMELINE
    enum class NRTArmState { Disarmed, Settling, Armed };
    NRTArmState nrtArmState = NRTArmState::Disarmed;
    uint64_t nrtSettleDeadline = 0;
    // Arming rebuilds the graph itself, which fires graphComputed; that must
    // not be mistaken for the patch changing under us.
    bool nrtSelfRebuild = false;
    ofEventListeners nrtGraphListeners;

    bool nrtCaptureActive = false;
    std::atomic<bool> nrtRendering{false};
    std::thread nrtRenderThread;
    std::string nrtOutputPath = "Supercollider/NRT/oceanode-render.wav";
    std::string nrtStatus;
    float nrtDuration = 10.0f;
    int nrtOutputChannels = 2;
    // Indices of the servers being captured, set when arming.
    std::vector<int> nrtServers;
    // Written by every render worker, so it cannot be a plain int.
    std::atomic<int> nrtRenderResult{0};
    bool nrtManualStop = false;
    bool nrtRecordStems = false;
    bool nrtServerMasters = false;
    // One per server, by server index.
    std::vector<std::string> nrtSourceLabels;
    // Filled on the GUI thread before the workers start and never written
    // again, so the workers only ever read it.
    std::vector<std::string> nrtRenderOutputs;
    // Bumped by whichever worker finishes a file, read by the GUI.
    std::atomic<int> nrtRenderJobsDone{0};
    std::atomic<long long> nrtRenderExpectedBytes{0};
    int nrtMaxParallelRenders = 4;
    bool nrtRemoveDC = false;
    std::string nrtCaptureOutputPath;
    int nrtCaptureOutputChannels = 2;
#endif // OFXOCEANODESC_HAS_TIMELINE
};

#endif /* ofxOceanodeSuperColliderController_h */
