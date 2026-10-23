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
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSCServer.h"
#include "serverManager.h"
#include <cmath>
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
    registerMacroMixerSCBus(o.getController<ofxOceanodeSuperColliderController>()->getServers());
}

void registerScope(ofxOceanode &o){
    const auto servers = o.getController<ofxOceanodeSuperColliderController>()->getServers();
    o.registerScope<nodePort>([servers](ofxOceanodeAbstractParameter* parameter, ImVec2 size){
        const auto pos = ImGui::GetCursorScreenPos();
        // The folded node can be outside the canvas viewport. Do not even
        // renew its capture lease unless its display is actually visible.
        if(!ImGui::IsRectVisible(size)) { ImGui::Dummy(size); return; }
        const auto port = parameter->cast<nodePort>().getParameter().get();
        const std::vector<float>* samples = nullptr;
        for(auto* manager : servers){
            samples = manager->requestFoldedWaveform(port);
            if(samples) break;
        }
        auto* draw = ImGui::GetWindowDrawList();
        const float middle = pos.y + size.y * 0.5f;
        draw->AddLine(ImVec2(pos.x, middle), ImVec2(pos.x + size.x, middle), IM_COL32(100, 110, 120, 70));
        if(samples && samples->size() > 1){
            // Fixed small stack buffer: no per-frame vectors, FBOs or textures.
            ImVec2 points[64];
            const int count = std::min(64, int(samples->size()));
            for(int i = 0; i < count; ++i){
                const float value = std::isfinite((*samples)[i]) ? ofClamp((*samples)[i], -1.0f, 1.0f) : 0;
                points[i] = ImVec2(pos.x + size.x * i / (count - 1), middle - value * size.y * 0.45f);
            }
            draw->AddPolyline(points, count, IM_COL32(100, 220, 175, 255), 0, 1.0f);
        }
        ImGui::Dummy(size);
    });
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
