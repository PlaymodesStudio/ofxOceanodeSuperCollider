//
//  ofxOceanodeSuperCollider.h
//
//
//  Created by Eduard Frigola Bagué on 14/01/2022.
//

#ifndef ofxOceanodeSuperCollider_h
#define ofxOceanodeSuperCollider_h

#define MAX_NODE_CHANNELS 100

#include "ofxOceanode.h"
#include "scStart.h"
#include "scSynthdef.h"
//#include "scOut.h"
//#include "scTonal.h"
#include "scBuffer.h"
//#include "scInfo.h"
#include "scServer.h"
#include "scNode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSCServer.h"

namespace ofxOceanodeSuperCollider{
static ofxSCServer* server = nullptr;
static scStart  sc;

static void registerModels(ofxOceanode &o){
    if(server == nullptr) ofLog() << "ERROR - Call start before registering";
    auto controller = o.addController<ofxOceanodeSuperColliderController>();
    controller->setScEngine(&sc);
    controller->setScServer(server);
//    o.registerModel<scPitch>("SuperCollider");
//    o.registerModel<scChord>("SuperCollider");
//    o.registerModel<scOut>("SuperCollider", scServer, controller.get());
//    o.registerModel<scInfo>("SuperCollider", scServer);
//    o.registerModel<scNode>("Supercollider", "test");
//    o.registerModel<scSynthdef>("Supercollider", "Simple", "vf:Pitch:40:0:127, vf:Level:0:0:1");
//    o.registerModel<scSynthdef>("Supercollider", "Filter", "vf:Pitch:127:0:127, vf:Q:1:0:1");
//    o.registerModel<scSynthdef>("Supercollider", "Stereomix", "");
    
    
    std::function<void(ofDirectory dir)> readSynthdefsInDirectory = [&o, &readSynthdefsInDirectory](ofDirectory dir){
        for(auto f : dir.getFiles()){
            if(f.isDirectory()){
                readSynthdefsInDirectory(ofDirectory(f.path()));
            }else{
                //Get synthdefs
                if(f.getExtension() == "txarcmeta"){
                    o.registerModel<scSynthdef>("Supercollider", scSynthdef::readAndCreateSynthdef(f.getAbsolutePath()));
                }
            }
        }
    };
    
    ofDirectory dir("Supercollider/Synthdefs");
    if(dir.exists()){
        readSynthdefsInDirectory(dir);
    }else{
        dir.create();
    }
    
    std::function<vector<string>(ofDirectory dir)> readWavsInDirectory = [&o, &readWavsInDirectory](ofDirectory dir){
        vector<string> wavs;
        for(auto f : dir.getFiles()){
            if(f.isDirectory()){
                ofDirectory dir2(f.path());
                dir2.sort();
                vector<string> newWavs = readWavsInDirectory(dir2);
                wavs.insert(wavs.end(), newWavs.begin(), newWavs.end());
            }else{
                //Get synthdefs
                string wavPath = f.getAbsolutePath();
                ofStringReplace(wavPath, ofToDataPath("Supercollider/Samples", true), "");
                wavs.push_back(wavPath);
            }
        }
        return wavs;
    };
    
    ofDirectory dir2("Supercollider/Samples");
    dir2.sort();
    vector<string> wavs;
    if(dir2.exists()){
        wavs = readWavsInDirectory(dir2);
    }else{
        dir2.create();
    }

    int z = 0;
    z = z+1;
    
    
    o.registerModel<scBuffer>("SuperCollider", wavs);
    o.registerModel<scServer>("Supercollider", server, controller.get(), 1, wavs);
}
static void registerType(ofxOceanode &o){
    o.registerType<scNode*>("ScBus");
    //o.registerType<vector<ofxSCBuffer*>>("ScBuffer");
}
static void registerScope(ofxOceanode &o){
//    o.registerScope<std::pair<ofxSCBus*, scSynthdef*>>([](ofxOceanodeAbstractParameter *p, ImVec2 size){
//        auto pair = p->cast<std::pair<ofxSCBus*, scSynthdef*>>().getParameter().get();
//        auto size2 = ImGui::GetContentRegionAvail();
//
//        ImGui::Text("%i, %s", pair.first->index, pair.second->nodeName().c_str());
//        });
}
static void registerCollection(ofxOceanode &o){
    registerModels(o);
    registerType(o);
    registerScope(o);
}

static void setup(bool local = true, std::string ip = "127.0.0.1", int port = 57110, string synthdefsPath = ofToDataPath("Supercollider/Synthdefs", true)){
    
    server = new ofxSCServer(ip, port);
    
    if(local){
        if(ofDirectory::doesDirectoryExist(ofToDataPath("Supercollider/Scsynth/bin/"))){
            sc.setup(ofToDataPath("Supercollider/Scsynth/bin/scsynth", true));
        }else if(ofDirectory::doesDirectoryExist("/Applications/SuperCollider.app/Contents/Resources/")){
            sc.setup("/Applications/SuperCollider.app/Contents/Resources/scsynth");
        }else{
            ofLog() << "No ScSynth found on the system";
        }
        sleep(5);
        
        ofxOscMessage m2;
        m2.setAddress("/g_new");
        m2.addIntArg(1);
        m2.addIntArg(0);
        m2.addIntArg(0);
        server->sendMsg(m2);
    }
    
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    m.addStringArg(synthdefsPath);
    m.addIntArg(0);
    server->sendMsg(m);
}

static void kill(){
    ofxOscMessage m;
    m.setAddress("/quit");
    server->sendMsg(m);
    sleep(1);
}
}

#endif /* ofxOceanodeSuperCollider_h */
