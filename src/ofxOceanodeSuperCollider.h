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
#include "scTonal.h"
#include "scBuffer.h"
#include "scCustomBuffer.h"
#include "scInfo.h"
#include "scServer.h"
#include "scNode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSCServer.h"

namespace ofxOceanodeSuperCollider{

static vector<string> loadSamples(){
    std::function<vector<string>(ofDirectory dir)> readWavsInDirectory = [&readWavsInDirectory](ofDirectory dir){
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
    
    ofDirectory dir("Supercollider/Samples");
    dir.sort();
    vector<string> wavs;
    if(dir.exists()){
        wavs = readWavsInDirectory(dir);
    }else{
        dir.create();
    }
    return wavs;
}

static void registerModels(ofxOceanode &o, vector<string> wavs){
//    o.registerModel<scPitch>("SuperCollider");
//    o.registerModel<scChord>("SuperCollider");
//    o.registerModel<scOut>("SuperCollider", scServer, controller.get());
    
    
//    std::function<void(ofDirectory dir)> readSynthdefsInDirectory = [&o, &readSynthdefsInDirectory](ofDirectory dir){
//        for(auto f : dir.getFiles()){
//            if(f.isDirectory()){
//                readSynthdefsInDirectory(ofDirectory(f.path()));
//            }else{
//                //Get synthdefs
//                if(f.getExtension() == "txarcmeta"){
//                    auto desc = scSynthdef::readAndCreateSynthdef(f.getAbsolutePath());
//                    if(desc.type == "multi"){
//
//                    }else if(desc.type == "events"){
//
//                    }else{
//                        o.registerModel<scSynthdef>("Supercollider", desc);
//                    }
//                }
//            }
//        }
//    };
//
//    ofDirectory dir("Supercollider/Synthdefs");
//    if(dir.exists()){
//        readSynthdefsInDirectory(dir);
//    }else{
//        dir.create();
//    }
    
    ofJson json = ofLoadJson("Supercollider/Synthdefs.json");
    for(ofJson::iterator it = json.begin(); it != json.end(); it++){
        synthdefDesc currentDescription;
        currentDescription.name = it.key();
        currentDescription.numInputs = it.value()["In"];
        currentDescription.numBuffers = it.value()["Buf"];
        currentDescription.numChannels = it.value()["Out_Size"];
        std::string params = it.value()["Params"];
        if(params != ""){
        std::vector<std::string> splittedParams = ofSplitString(params, ", ");
            for(string &s : splittedParams){
                vector<string> ss = ofSplitString(s, ":");
//                if(ss[0] == "vi") currentDescription.params[ss[1]]["step"] = 1.0;
//                else currentDescription.params[ss[1]]["step"] = 0.0;
                currentDescription.params[ss[1]]["type"] = ss[0];
                currentDescription.params[ss[1]]["default"] = ss[2];
                currentDescription.params[ss[1]]["minval"] = ss[3];
                currentDescription.params[ss[1]]["maxval"] = ss[4];
            }
        }
        
        
        o.registerModel<scSynthdef>("SuperCollider",
                                    currentDescription);
    }
    
    
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

    o.registerModel<scInfo>("SuperCollider", controller->getServers());
    o.registerModel<scBuffer>("SuperCollider", wavs);
    o.registerModel<scCustomBuffer>("SuperCollider", controller->getServers());
    o.registerModel<scServer>("SuperCollider", controller->getServers());
    o.registerModel<scPitch>("SuperCollider");
    o.registerModel<scChord>("SuperCollider");
}
static void registerType(ofxOceanode &o){
    o.registerType<scNode*>("ScBus");
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
    auto controller = o.addController<ofxOceanodeSuperColliderController>();
    
    vector<string> wavs = loadSamples();
    controller->createServers(wavs);
    
    registerModels(o, wavs);
    registerType(o);
    registerScope(o);
}

static void setup(ofxOceanode &o){
    o.getController<ofxOceanodeSuperColliderController>()->setup();
}

static void kill(ofxOceanode& o){
    o.getController<ofxOceanodeSuperColliderController>()->killServers();
}
}

#endif /* ofxOceanodeSuperCollider_h */
