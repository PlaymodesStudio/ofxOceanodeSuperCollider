//
//  ofxOceanodeSuperCollider.h
//
//
//  Created by Eduard Frigola Bagué on 14/01/2022.
//

#ifndef ofxOceanodeSuperCollider_h
#define ofxOceanodeSuperCollider_h

#include "ofxOceanode.h"
#include "scStart.h"
#include "scSynthdef.h"
#include "scOut.h"
#include "scTonal.h"
#include "scBuffer.h"
#include "scInfo.h"
#include "ofxOceanodeSuperColliderController.h"

#include <sys/sysctl.h>

//TODO: Add a controller for changing parameters

namespace ofxOceanodeSuperCollider{
static ofxSCServer* scServer = nullptr;
static scStart  sc;

static void registerModels(ofxOceanode &o){
    if(scServer == nullptr) ofLog() << "ERROR - Call start before registering";
    auto controller = o.addController<ofxOceanodeSuperColliderController>();
    controller->setScEngine(&sc);
    controller->setScServer(scServer);
    o.registerModel<scPitch>("SuperCollider");
    o.registerModel<scChord>("SuperCollider");
    o.registerModel<scOut>("SuperCollider", scServer, controller.get());
    o.registerModel<scBuffer>("SuperCollider", scServer);
    o.registerModel<scInfo>("SuperCollider", scServer);
    
    ofJson json = ofLoadJson("Synthdefs.json");
    for(ofJson::iterator it = json.begin(); it != json.end(); it++){
        o.registerModel<scSynthdef>("SuperCollider",
                                    it.key(),
                                    it.value()["In_Size"],
                                    it.value()["Out_Size"],
                                    it.value()["Params"],
                                    it.value()["In"],
                                    it.value()["Out"],
                                    it.value()["Buf"],
                                    scServer);
    }
}
static void registerType(ofxOceanode &o){
    o.registerType<std::pair<ofxSCBus*, scSynthdef*>>("ScBus");
    o.registerType<vector<ofxSCBuffer*>>("ScBuffer");
}
static void registerScope(ofxOceanode &o){
    o.registerScope<std::pair<ofxSCBus*, scSynthdef*>>([](ofxOceanodeAbstractParameter *p, ImVec2 size){
        auto pair = p->cast<std::pair<ofxSCBus*, scSynthdef*>>().getParameter().get();
        auto size2 = ImGui::GetContentRegionAvail();

        ImGui::Text("%i, %s", pair.first->index, pair.second->nodeName().c_str());
        });
}
static void registerCollection(ofxOceanode &o){
    registerModels(o);
    registerType(o);
    registerScope(o);
}

static void setup(bool local = true, std::string ip = "127.0.0.1", int port = 57110, string synthdefsPath = ofToDataPath("Synthdefs", true)){
    
    scServer = new ofxSCServer(ip, port);
    
    if(local){
        //https://stackoverflow.com/questions/66256300/c-c-code-to-have-different-execution-block-on-m1-and-intel
        cpu_type_t type;
        size_t size = sizeof(type);
        sysctlbyname("hw.cputype", &type, &size, NULL, 0);
        
        int procTranslated;
        size = sizeof(procTranslated);
        // Checks whether process is translated by Rosetta
        sysctlbyname("sysctl.proc_translated", &procTranslated, &size, NULL, 0);
        
        // Removes CPU_ARCH_ABI64 or CPU_ARCH_ABI64_32 encoded with the Type
        cpu_type_t typeWithABIInfoRemoved = type & ~CPU_ARCH_MASK;
        
        if (typeWithABIInfoRemoved == CPU_TYPE_X86)
        {
            if (procTranslated == 1)
            {
                sc.setup(ofToDataPath("Scsynth/aarch/bin/scsynth", true));
            }
            else
            {
                sc.setup(ofToDataPath("Scsynth/intel/bin/scsynth", true));
            }
        }
        else if (typeWithABIInfoRemoved == CPU_TYPE_ARM)
        {
            sc.setup(ofToDataPath("Scsynth/aarch/bin/scsynth", true));
        }
        sleep(5);
        
        ofxOscMessage m2;
        m2.setAddress("/g_new");
        m2.addIntArg(1);
        m2.addIntArg(0);
        m2.addIntArg(0);
        scServer->sendMsg(m2);
    }
    
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    m.addStringArg(synthdefsPath);
    m.addIntArg(0);
    scServer->sendMsg(m);
}

static void kill(){
    ofxOscMessage m;
    m.setAddress("/quit");
    scServer->sendMsg(m);
    sleep(1);
}
}

#endif /* ofxOceanodeSuperCollider_h */
