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

	// 1. Calculate NEW Topology
	std::vector<scNode*> newNodesList;
	std::map<scNode*, std::pair<int, std::vector<int>>> nodeChilds;
	
	if(outputs.size() > 0){
		for(int i = 0; i < outputs.size(); i++){
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

	// =========================================================
	// PATH A: RECOVERY MODE (Preset Load or Copy/Paste)
	// We have data in backupNodesList, meaning a Load event started this.
	// =========================================================
	if(!backupNodesList.empty()){
		
		// Check for Copy/Paste Match (Soft Update)
		bool canRestore = (backupNodesList.size() == newNodesList.size());
		if(canRestore){
			for(size_t i = 0; i < backupNodesList.size(); ++i){
				if(backupNodesList[i] != newNodesList[i]){
					canRestore = false;
					break;
				}
			}
		}

		if(canRestore){
			// [GLITCH FIX] Identical graph. Restore and exit silent.
			nodesList = newNodesList;
			backupNodesList.clear();
			return;
		}

		// [CRASH FIX] Mismatch = Full Preset Load.
		// Old pointers in backupNodesList are likely ZOMBIES.
		// We DO NOT call free() on them. We wipe the server group instead.
		ofxOscMessage m;
		m.setAddress("/g_freeAll");
		m.addIntArg(1);
		server->sendMsg(m);
		
		// Force full rebuild below
		backupNodesList.clear();
		// nodesList is already empty from the listener
	}
	
	// =========================================================
	// PATH B: MANUAL MODE (Wire Disconnect/Connect)
	// backupNodesList is empty. nodesList contains current live nodes.
	// We must identify what changed and update surgically.
	// =========================================================
	
	server->setBLatency(true);

	// 1. Clean up Busses (Necessary for re-routing)
	for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
	busses.clear();
	outputBussesRefToNode.clear();
	inputBussesRefToNode.clear();
	
	std::vector<scNode*> toCreateNodes;
	std::vector<scNode*> toUpdateNodes;
	
	// 2. Diff: New vs Old
	for(auto &node : newNodesList){
		auto nodeInListIter = std::find(nodesList.begin(), nodesList.end(), node);
		if(nodeInListIter != nodesList.end()){
			toUpdateNodes.push_back(node);
			nodesList.erase(nodeInListIter); // Remove from list so only "Dead" nodes remain
		}else{
			toCreateNodes.push_back(node);   // New node found
		}
	}
	
	// 3. [STUCK SOUND FIX] Free Removed Nodes
	// Any node remaining in nodesList was disconnected.
	// We MUST free it to stop the sound.
	for(auto node : nodesList){
		if(node != nullptr)
			node->free(server);
	}
	nodesList.clear(); // Clear the dead list
	
	// 4. Rebuild Connections
	std::map<nodePort, std::vector<scNode*>> connections;
	for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
		(*it)->getConnections(connections);
		
		if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
			(*it)->buildSynth(server); // Build new
		}else{
			(*it)->resetInputBusses(server); // Reset existing
		}
	}
		
	// 5. Alloc Busses
	for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
		for(int i = 0; i < (*it)->getNumOutputs() ; i++){
			busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server);
			int busindex = busses.back().index;
			(*it)->setOutputBus(server, i, busindex);
			outputBussesRefToNode[(*it)][i] = busindex;
		}
	}
	
	// 6. Link Busses
	for(auto &c : connections){
		for(auto &dest : c.second){
			int busindex = outputBussesRefToNode[c.first.getNodeRef()][c.first.getIndex()];
			dest->setInputBus(server, c.first.getNodeRef(), busindex);
			inputBussesRefToNode[dest].push_back(busindex);
		}
	}
	
	// 7. Create/Order Synths
	scNode* lastNode = nullptr;
	for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
		if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
			(*it)->createSynth(server);
		}
		else{
			if(lastNode != nullptr){
				(*it)->moveSynthBefore(server, lastNode->getLastSynthID(server));
			}
		}
		lastNode = (*it);
	}
	
	// Update main list
	nodesList = newNodesList;
	server->setBLatency(false);

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
