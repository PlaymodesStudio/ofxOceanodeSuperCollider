//
//  ofxOceanodePlaymodes.h
//
//
//  Created by Eduard Frigola Bagué on 14/01/2022.
//

#ifndef ofxOceanodePlaymodes_h
#define ofxOceanodePlaymodes_h

#include "ofxOceanode.h"
#include "scStart.h"
#include "scSynthdef.h"
#include "scOut.h"
#include "scTonal.h"

//TODO: Add a controller for changing parameters

namespace ofxOceanodeSuperCollider{
static ofxSCServer* scServer = nullptr;
static scStart  sc;

static void registerModels(ofxOceanode &o){
    if(scServer == nullptr) ofLog() << "ERROR - Call start before registering";
    o.registerModel<scPitch>("SuperCollider");
    o.registerModel<scChord>("SuperCollider");
    o.registerModel<scOut>("Supercollider", scServer);
    
    ofJson json = ofLoadJson("Synthdefs.json");
    for(ofJson::iterator it = json.begin(); it != json.end(); it++){
        o.registerModel<scSynthdef>("SuperCollider",
                                    it.key(),
                                    it.value()["Size"],
                                    it.value()["Params"],
                                    it.value()["In"],
                                    it.value()["Out"],
                                    scServer);
    }
}
static void registerType(ofxOceanode &o){
    o.registerType<std::pair<ofxSCBus*, ofxSCSynth*>>("ScBus");
}
static void registerScope(ofxOceanode &o){
    
}
static void registerCollection(ofxOceanode &o){
    registerModels(o);
    registerType(o);
    registerScope(o);
}

static void start(bool local = true, std::string ip = "127.0.0.1", int port = 57110, string synthdefsPath = ofToDataPath("Synthdefs", true)){
    
    scServer = new ofxSCServer(ip, port);
    
    if(local){
        sc.start(ofToDataPath("sc/bin/scsynth", true));
        sleep(5);
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(synthdefsPath);
        scServer->sendMsg(m);
        
        ofxOscMessage m2;
        m2.setAddress("/g_new");
        m2.addIntArg(1);
        m2.addIntArg(0);
        m2.addIntArg(0);
        scServer->sendMsg(m2);
    }
}

static void kill(){
    ofxOscMessage m;
    m.setAddress("/quit");
    scServer->sendMsg(m);
}
}

#endif /* ofxOceanodePlaymodes_h */
