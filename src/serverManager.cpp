//
//  serverManager.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#include "serverManager.h"
#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSuperCollider.h"
#include "scNode.h"
#include "scStart.h"
#include "scOutput.h"
#include "ofxOceanodeShared.h"

serverManager::serverManager(){
    volume = 1;
    mute = false;
    delay = 0;
    stereomix = false;
    stereomixSize = 2;
    audioDevice = 0;
    dumpOsc = false;
    numRecomputeGraphOnce = 0;
};

serverManager::~serverManager(){
    for(auto node : nodesList) node->free(server);
    nodesList.clear();
    for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
    busses.clear();
}

void serverManager::setup(){
    server = new ofxSCServer(preferences.bindAddress, preferences.udpPort);
    if(preferences.local){
        sc = new scStart(preferences);
    }
    boot();
    
	listeners.push(ofxOceanodeShared::getPresetWillBeLoadedEvent().newListener([this](){
			backupNodesList = nodesList; // Save state
			nodesList.clear();           // Clear main list to prevent crash
			// Note: We do NOT clear busses here, preventing the glitch.
		}));
	
    listeners.push(ofxOceanodeShared::getPresetHasLoadedEvent().newListener([this](){
        recomputeGraph();
    }));
    
    listeners.push(server->serverBootedEvent.newListener([this](){
        initialize();
    }));
    listeners.push(server->serverInitializedEvent.newListener([this](){
        recomputeGraph();
    }));
}

void serverManager::draw(){
    if(ImGui::Button("Boot Server")){
        boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        if(preferences.local){
            kill();
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Local", &preferences.local)){
        if(!preferences.local){
            sc->killServer();
            delete sc;
        }else{
            sc = new scStart(preferences);
        }
    }
    
    if(ImGui::Button("Load Defs")){
        loadDefs();
    }
    
    ImGui::Separator();
    
    if(ImGui::SliderFloat("Volume", &volume, 0, 1)){
        setVolume(volume);
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Mute", &mute)){
        if(mute) setVolume(0);
        else setVolume(volume);
    }
    
    if(ImGui::SliderInt("Delay", &delay, 0, 5000)){
        setDelay(delay);
    }
    
    ImGui::Separator();
    
    ImGui::InputInt("Udp Port", &preferences.udpPort);
    
    
    int intaddress[4] = {0, 0, 0, 0};
    vector<string> splitAddress = ofSplitString(preferences.bindAddress, ".");
    for(int i = 0; i < 4; i++){
        intaddress[i] = ofToInt(splitAddress[i]);
    }
    if(ImGui::InputInt4("Bind Address", &intaddress[0])){
        string newAddress = ofToString(intaddress[0]) + "."
                            + ofToString(intaddress[1]) + "."
                            + ofToString(intaddress[2]) + "."
                            + ofToString(intaddress[3]) + ".";
        preferences.bindAddress = newAddress;
    }
    
    ImGui::InputInt("Audio Busses", &preferences.numAudioBusChannels);
    ImGui::InputInt("Control Busses", &preferences.numControlBusChannels);
    ImGui::InputInt("Input Channels", &preferences.numInputBusChannels);
    ImGui::InputInt("Output Channels", &preferences.numOutputBusChannels);
    ImGui::InputInt("Block Size", &preferences.blockSize);
    ImGui::InputInt("Buffer Size", &preferences.hardwareBufferSize);
    ImGui::InputInt("Sampling Rate", &preferences.hardwareSampleRate);
    ImGui::InputInt("Num Buffers", &preferences.numBuffers);
    ImGui::InputInt("Max Nodes", &preferences.maxNodes);
    ImGui::InputInt("Max Synthdefs", &preferences.maxSynthDefs);
    ImGui::InputInt("Mem Size", &preferences.memSize);
    ImGui::InputInt("Num Wire Bufs", &preferences.numWireBufs);
    ImGui::InputInt("Num R Gens", &preferences.numRGens);
    ImGui::InputInt("Max Logins", &preferences.maxLogins);
    ImGui::InputFloat("Safety Clip Th", &preferences.safetyClipThreshold);
    
    auto vector_getter = [](void* vec, int idx, const char** out_text)
    {
        auto& vector = *static_cast<std::vector<std::string>*>(vec);
        if (idx < 0 || idx >= static_cast<int>(vector.size())) { return false; }
        *out_text = vector.at(idx).c_str();
        return true;
    };
    
    if(ImGui::Combo("Audio Device", &audioDevice, vector_getter, static_cast<void*>(&audioDeviceNames), audioDeviceNames.size())){
        if(audioDevice == 0){
            preferences.deviceName = "nil";
        }else{
            preferences.deviceName = audioDeviceNames[audioDevice];
        }
    }
    
    //Device name;
    bool verb = preferences.verbosity;
    if(ImGui::Checkbox("Verbosity", &verb)){
        if(verb) preferences.verbosity = 1;
        else preferences.verbosity = 0;
    }

    bool localUgens = (preferences.ugensPlugins != "");
    if(ImGui::Checkbox("LocalUgens", &localUgens)){
        if(localUgens)
            preferences.ugensPlugins = ofToDataPath("Supercollider/Ugens", true);
        else
            preferences.ugensPlugins = "";
    }
    
    if(ImGui::Checkbox("Dump Osc", &dumpOsc)){
        ofxOscMessage m;
        m.setAddress("/dumpOSC");
        if(dumpOsc) m.addIntArg(1);
        else m.addIntArg(0);
        server->sendMsg(m);
    }
}

void serverManager::boot(){
    if(preferences.local){
        sc->start();
    }
}

void serverManager::initialize(){
    server->notify();
    
    ofxOscMessage m2;
    m2.setAddress("/g_new");
    m2.addIntArg(1);
    m2.addIntArg(0);
    m2.addIntArg(0);
    server->sendMsg(m2);
    
    loadDefs();
    
    setVolume(volume);
    setDelay(delay);
    setStereoMix(stereomix);
    setStereoMixSize(stereomixSize);
}

void serverManager::kill(){
    if(preferences.local){
        ofxOscMessage m;
        m.setAddress("/quit");
        server->sendMsg(m);
        sc->killServer();
    }
}

void serverManager::loadDefs(){
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
	m.addStringArg(ofToDataPath(SYNTHDEF_DIRECTORY, true));
    m.addIntArg(0);
    server->sendMsg(m);
}

void serverManager::setVolume(float _volume){
    volume = _volume;
    for(auto &o : outputs) o->setVolume(volume);
    
}

void serverManager::setDelay(int _delay){
    delay = _delay;
    for(auto &o : outputs) o->setDelay(delay);
}

void serverManager::setStereoMix(bool _stereomix){
    stereomix = _stereomix;
    for(auto &o : outputs) o->setStereoMix(stereomix);
}

void serverManager::setStereoMixSize(int _stereomixSize){
    stereomixSize = _stereomixSize;
    for(auto &o : outputs) o->setStereoMixSize(stereomixSize);
}

void serverManager::addOutput(scOutput *output){
    outputs.push_back(output);
    setVolume(volume);
    setDelay(delay);
    setStereoMix(stereomix);
    setStereoMixSize(stereomixSize);
}

void serverManager::removeOutput(scOutput *output){
    outputs.erase(std::remove(outputs.begin(), outputs.end(), output), outputs.end());
    recomputeGraph();
}

void serverManager::recomputeGraph(){
	if(ofxOceanodeShared::isPresetLoading()) return;

	// 1. Calculate the NEW topology based on valid outputs
	std::vector<scNode*> newNodesList;
	std::map<scNode*, std::pair<int, std::vector<int>>> nodeChilds;
	
	if(outputs.size() > 0){
		for(int i = 0; i < outputs.size(); i++){
			// Check for null to be extra safe
			if(outputs[i] && outputs[i]->isInputConnected()){
				outputs[i]->getInputNode()->appendOrderedNodes(newNodesList, nodeChilds);
			}
		}
		for(int i = 0; i < outputs.size(); i++){
			if(outputs[i] && outputs[i]->isInputConnected()){
				newNodesList.push_back(outputs[i]);
			}
		}
	}

	// 2. COMPARE: New Topology vs Backup
	bool canRestore = false;
	
	// If the count matches, check the pointers
	if(backupNodesList.size() == newNodesList.size()){
		canRestore = true;
		for(size_t i = 0; i < backupNodesList.size(); ++i){
			// If pointers are identical, it means the objects are the same (alive).
			// This happens during Copy/Paste of non-audio nodes.
			if(backupNodesList[i] != newNodesList[i]){
				canRestore = false;
				break;
			}
		}
	}

	if(canRestore){
		// [SCENARIO: SOFT UPDATE]
		// The audio nodes are identical. We just restore the list.
		// No audio glitch, no silence.
		nodesList = newNodesList;
		backupNodesList.clear();
		return;
	}

	// [SCENARIO: HARD REBUILD]
	// The nodes are different (Preset Load or Audio Graph Change).
	// We must rebuild everything.
	
	// Clear backup (these are likely dead pointers now)
	backupNodesList.clear();

	if(outputs.size() == 0){
		// Full Clear
		ofxOscMessage m;
		m.setAddress("/g_freeAll");
		m.addIntArg(1);
		server->sendMsg(m);

		for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
		busses.clear();
	}
	else{
		server->setBLatency(true);

		// [DISTORTION FIX]
		// Since we couldn't free the old C++ nodes (they were deleted),
		// the old synths are still running. We must wipe the server group.
		// Group 1 is the default container created in initialize().
		ofxOscMessage m;
		m.setAddress("/g_freeAll");
		m.addIntArg(1);
		server->sendMsg(m);

		// Reset Busses
		for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
		busses.clear();
		outputBussesRefToNode.clear();
		inputBussesRefToNode.clear();
		
		std::vector<scNode*> toCreateNodes;
		
		// All nodes in the new list need creation
		for(auto &node : newNodesList){
			toCreateNodes.push_back(node);
		}
		
		// Rebuild Connections
		std::map<nodePort, std::vector<scNode*>> connections;
		for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
			(*it)->getConnections(connections);
			(*it)->buildSynth(server);
		}
			
		// Alloc Busses
		for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
			for(int i = 0; i < (*it)->getNumOutputs() ; i++){
				busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server);
				int busindex = busses.back().index;
				(*it)->setOutputBus(server, i, busindex);
				outputBussesRefToNode[(*it)][i] = busindex;
			}
		}
		
		// Link Busses
		for(auto &c : connections){
			for(auto &dest : c.second){
				int busindex = outputBussesRefToNode[c.first.getNodeRef()][c.first.getIndex()];
				dest->setInputBus(server, c.first.getNodeRef(), busindex);
				inputBussesRefToNode[dest].push_back(busindex);
			}
		}
		
		// Create Synths on Server
		scNode* lastNode = nullptr;
		for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
			(*it)->createSynth(server);
			
			if(lastNode != nullptr){
				(*it)->moveSynthBefore(server, lastNode->getLastSynthID(server));
			}
			lastNode = (*it);
		}
		
		nodesList = newNodesList;
		server->setBLatency(false);
	}
	graphComputed.notify();
}



//int serverManager::getOutputBusForNode(scNode* node){
//    if(outputBussesRefToNode.count(node) == 1){
//        return outputBussesRefToNode[node];
//    }
//    else{
//        return -1;
//    }
//}
