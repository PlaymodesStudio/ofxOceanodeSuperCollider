//
//  ofxOceanodeSuperColliderController.h
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#ifndef ofxOceanodeSuperColliderController_h
#define ofxOceanodeSuperColliderController_h


#include "ofxOceanodeBaseController.h"

class scStart;
class ofxSCServer;
class serverManager;
class scPreferences;

class ofxOceanodeSuperColliderController : public ofxOceanodeBaseController{
public:
    ofxOceanodeSuperColliderController();
    ~ofxOceanodeSuperColliderController(){};
    
    void createServers();
    
    void setup();
    void draw();
    
    void killServers();

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
};

#endif /* ofxOceanodeSuperColliderController_h */
