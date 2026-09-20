//
//  ofxOceanodeSuperColliderController.h
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#ifndef ofxOceanodeSuperColliderController_h
#define ofxOceanodeSuperColliderController_h


#include "ofxOceanodeBaseController.h"
#include "ofxOceanodeSuperColliderConfig.h"
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
    // Starts/stops a frame-stepped NRT capture controlled by an external node.
    // When manualStop is true, endNRTRecording() defines the exact duration.
    // Needs ofxOceanode's frame-stepped transport, so it is only available
    // together with the timeline.
    bool beginNRTRecording(int serverIndex, int outputChannels, const std::string& outputPath, bool manualStop = true);
    bool endNRTRecording(bool cancelled = false);
    bool isNRTRecordingActive() const { return nrtCaptureActive; }
    bool isNRTRendering() const { return nrtRendering.load(); }
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
    bool nrtCaptureActive = false;
    std::atomic<bool> nrtRendering{false};
    std::thread nrtRenderThread;
    std::string nrtOutputPath = "Supercollider/NRT/oceanode-render.wav";
    std::string nrtStatus;
    float nrtDuration = 10.0f;
    int nrtOutputChannels = 2;
    int nrtServer = 0;
    int nrtRenderResult = 0;
    bool nrtManualStop = false;
    std::string nrtCaptureOutputPath;
    int nrtCaptureOutputChannels = 2;
#endif // OFXOCEANODESC_HAS_TIMELINE
};

#endif /* ofxOceanodeSuperColliderController_h */
