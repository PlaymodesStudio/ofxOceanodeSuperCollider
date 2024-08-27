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
#include "scOutput.h"
#include "scNode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSCServer.h"

namespace ofxOceanodeSuperCollider{

static void registerModels(ofxOceanode &o){
    std::function<void(ofDirectory dir)> readSynthdefsInDirectory = [&o, &readSynthdefsInDirectory](ofDirectory dir){
        for(auto f : dir.getFiles()){
            if(f.isDirectory()){
                readSynthdefsInDirectory(ofDirectory(f.path()));
            }else{
                //Get synthdefs
                if(f.getExtension() == "txarcmeta"){
                    auto desc = scSynthdef::readAndCreateSynthdef(f.getAbsolutePath());
                    if(desc.type == "multi"){

                    }else if(desc.type == "events"){

                    }else{
                        if(desc.category != ""){
                            o.registerModel<scSynthdef>("SuperCollider/" + desc.category, desc);
                        }else{
                            o.registerModel<scSynthdef>("SuperCollider", desc);
                        }
                    }
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
    
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

    o.registerModel<scInfo>("SuperCollider", controller->getServers());
    o.registerModel<scBuffer>("SuperCollider", controller->getServers());
    o.registerModel<scCustomBuffer>("SuperCollider", controller->getServers());
    o.registerModel<scOutput>("SuperCollider", controller->getServers());
    o.registerModel<scPitch>("SuperCollider");
    o.registerModel<scChord>("SuperCollider");
}
static void registerType(ofxOceanode &o){
    o.registerType<nodePort>("ScBus");
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
    
    controller->createServers();
    
    registerModels(o);
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
