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
class scOut;

class ofxOceanodeSuperColliderController : public ofxOceanodeBaseController{
public:
    ofxOceanodeSuperColliderController();
    ~ofxOceanodeSuperColliderController(){};
    
    void setScEngine(scStart* _scEngine);
    void setScServer(ofxSCServer* _scServer);
    void draw();
    
    void addOutput(scOut* node);
    void removeOutput(scOut* node);
    
private:
    
    void reloadAudioDevices();
    
    int audioDevice;
    float volume;
    bool mute;
    int delay;
    bool dumpOsc = false;
    vector<string> audioDeviceNames;
    scStart* sc;
    ofxSCServer* scServer;
    vector<scOut*> outputNodes;
};

#endif /* ofxOceanodeSuperColliderController_h */
