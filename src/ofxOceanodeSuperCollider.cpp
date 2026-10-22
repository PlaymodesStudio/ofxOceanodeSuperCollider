//
//  ofxOceanodeSuperCollider.cpp
//
//
//  Created by Eduard Frigola Bagué on 14/01/2022.
//

#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "scStart.h"
#include "scSynthdef.h"
#include "scOldSynthdef.h"
#include "scNode.h"
#include "MacroMixerSCBus.h"
#include "scAudioBusMixer.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSCServer.h"
#include "nodes/scFeedbackNode.h"   // refreshSynthdefModels

namespace ofxOceanodeSuperCollider{

// ofxOceanodeSuperColliderNodes<Group>.cpp
void registerSynthNodes(ofxOceanode &o);
void registerEffectNodes(ofxOceanode &o);
void registerSequencerNodes(ofxOceanode &o);
void registerAnalysisNodes(ofxOceanode &o);
void registerUtilNodes(ofxOceanode &o);

void refreshSynthdefModels(ofxOceanode &o){
    std::map<std::string, std::string> synthdefFolders;
	std::function<void(ofDirectory dir)> readSynthdefsInDirectory = [&o, &readSynthdefsInDirectory, &synthdefFolders](ofDirectory dir){
		for(auto f : dir.getFiles()){
			if(f.isDirectory()){
				readSynthdefsInDirectory(ofDirectory(f.path()));
			}else{
				//Get synthdefs
				if(f.getExtension() == "txarcmeta"){
					synthdefDesc desc;
					try {
						desc = scSynthdef::readAndCreateSynthdef(f.getAbsolutePath());
					} catch(const std::exception& error) {
						// Not an Oceanode description (VSTPlugin writes its own .txarcmeta)
						ofLogWarning("ofxOceanodeSuperCollider") << "Skipping " << f.getAbsolutePath() << ": " << error.what();
						continue;
					}
					if(desc.type == "multi"){

					}else if(desc.type == "events"){

					}else{
						// Refreshed, not just registered: registerModel keeps the
						// first description of a name, so a SynthDef rebuilt with
						// other parameters (SC Code's Promote, Load Defs) went on
						// creating nodes with the old ones until a restart. The
						// name is the scNode's "SC <name>*".
						const std::string category = desc.category.empty() ? "SuperCollider" : "SuperCollider/" + desc.category;
						const std::string nodeName = "SC " + desc.name + "*";
						if(desc.name == "FeedbackNode"){
							o.refreshModelNamed<scFeedbackNode>(nodeName, category, desc);
						}else{
							o.refreshModelNamed<scSynthdef>(nodeName, category, desc);
						}
					}
                    synthdefFolders[desc.name] = dir.getAbsolutePath();
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
	
	
	//Register old synthdef format
	ofJson json = ofLoadJson("Supercollider/Synthdefs.json");
		for(ofJson::iterator it = json.begin(); it != json.end(); it++){
			oldSynthdefDesc currentDescription;
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
			
			
			o.registerModel<scOldSynthdef>("SuperCollider/Old",
										currentDescription);
		}
	
	auto controller = o.getController<ofxOceanodeSuperColliderController>();
    
    for(auto &server : controller->getServers()){
        server->setSynthdefFolders(synthdefFolders);
    }
}

void registerModels(ofxOceanode &o){
    refreshSynthdefModels(o);
    registerSynthNodes(o);
    registerEffectNodes(o);
    registerSequencerNodes(o);
    registerAnalysisNodes(o);
    registerUtilNodes(o);
}

void registerType(ofxOceanode &o){
	o.registerType<nodePort>("ScBus");
    registerSCAudioInputs(o.getController<ofxOceanodeSuperColliderController>()->getServers());
    registerMacroMixerSCBus(o.getController<ofxOceanodeSuperColliderController>()->getServers());
}

void registerScope(ofxOceanode &o){
//    o.registerScope<std::pair<ofxSCBus*, scSynthdef*>>([](ofxOceanodeAbstractParameter *p, ImVec2 size){
//        auto pair = p->cast<std::pair<ofxSCBus*, scSynthdef*>>().getParameter().get();
//        auto size2 = ImGui::GetContentRegionAvail();
//
//        ImGui::Text("%i, %s", pair.first->index, pair.second->nodeName().c_str());
//        });
}

void registerCollection(ofxOceanode &o){
	auto controller = o.addController<ofxOceanodeSuperColliderController>();
	
	controller->createServers();
	auto refreshModels = [&o](){ refreshSynthdefModels(o); };
	controller->setSynthdefReloadCallback(refreshModels);
	for(auto *server : controller->getServers()) server->setSynthdefReloadCallback(refreshModels);
	
	registerModels(o);
	registerType(o);
	registerScope(o);
}

void setup(ofxOceanode &o){
	auto controller = o.getController<ofxOceanodeSuperColliderController>();
	controller->setup();
}

void kill(ofxOceanode& o){
	o.getController<ofxOceanodeSuperColliderController>()->killServers();
}

}
