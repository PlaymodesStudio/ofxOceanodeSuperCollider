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
    
    float volume;
    bool mute;
    int delay;
    bool stereomix;
    int stereomixSize;
    vector<string> audioDeviceNames;
    vector<serverManager*> outputServers;
};

#endif /* ofxOceanodeSuperColliderController_h */
