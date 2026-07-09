//
//  serverManager.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#include "serverManager.h"
#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSuperCollider.h"
#include "scNode.h"
#include "scStart.h"
#include "scOutput.h"
#include "ofxOceanodeShared.h"

std::map<ofxSCServer*, int> serverManager::serverSampleRates;

serverManager::serverManager(){
    initialized = false;
    volume = 1;
    mute = false;
    delay = 0;
    stereomix = false;
    stereomixSize = 2;
    audioDevice = 0;
    dumpOsc = false;
    numRecomputeGraphOnce = 0;
    busFromSilent = nullptr;
    server = nullptr;
    sc = nullptr;
    configuredNumInputBusChannels = -1;
    configuredNumOutputBusChannels = -1;
};

serverManager::~serverManager(){
    serverSampleRates.erase(server);
    for(auto node : nodesList) node->free(server);
    nodesList.clear();
    for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
    busses.clear();
}

void serverManager::setup(){
    if(configuredNumInputBusChannels < 0) configuredNumInputBusChannels = preferences.numInputBusChannels;
    if(configuredNumOutputBusChannels < 0) configuredNumOutputBusChannels = preferences.numOutputBusChannels;

    server = new ofxSCServer(preferences.bindAddress, preferences.udpPort, preferences.udpPort+20, preferences.numInputBusChannels, preferences.numOutputBusChannels, preferences.numAudioBusChannels, preferences.numControlBusChannels, preferences.numBuffers);
    serverSampleRates[server] = preferences.hardwareSampleRate;
    if(preferences.local){
        sc = new scStart(preferences);
    }
    boot();
    
    listeners.push(ofxOceanodeShared::getPresetWillBeLoadedEvent().newListener([this](){
        if(ofxOceanodeShared::getPresetLoadType() == ofxOceanodePresetLoadType_ClipboardPaste) return;
        teardownGraphForPresetLoad();
    }));
    
    listeners.push(ofxOceanodeShared::getPresetHasLoadedEvent().newListener([this](){
        if(preferences.loadOnPreset){
            loadSynthdefsFromPreset(ofxOceanodeShared::getCurrentPresetPath());
        }
        recomputeGraph();
    }));
    
    listeners.push(server->serverBootedEvent.newListener([this](){
        initialized = false;
        initialize();
    }));
    listeners.push(server->serverInitializedEvent.newListener([this](){
        initialized = true;
        nodesList.clear();
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();

        // Server initialization resets the audio bus allocator, so the old
        // reserved silent bus address may now point to a live allocation.
        busFromSilent.reset();
        busFromSilent = std::make_unique<ofxSCBus>(RATE_AUDIO, MAX_NODE_CHANNELS, server);
        recomputeGraph();
        
        setVolume(volume);
        setDelay(delay);
        setStereoMix(stereomix);
        setStereoMixSize(stereomixSize);
    }));
	
	if(busFromSilent == nullptr) busFromSilent = std::make_unique<ofxSCBus>(RATE_AUDIO, MAX_NODE_CHANNELS, server);
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
            if(sc != nullptr){
                sc->killServer();
                delete sc;
                sc = nullptr;
            }
        }else{
            sc = new scStart(preferences);
        }
    }
    
    if(ImGui::Checkbox("Load Synthdefs On Preset", &preferences.loadOnPreset)){
        if(!preferences.loadOnPreset){
            loadDefs();
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
    
    ImGui::Text("Output Device: %s", preferences.deviceName == "nil" ? "Default" : preferences.deviceName.c_str());
    ImGui::Text("Input Device: %s", preferences.inputDeviceName == "nil" ? "Default" : preferences.inputDeviceName.c_str());
    
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
    if(preferences.local && sc != nullptr){
        sc->setPreferences(preferences);
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
    
    server->sendInitializationSyncMessage();
}

void serverManager::kill(){
    if(preferences.local && sc != nullptr){
        ofxOscMessage m;
        m.setAddress("/quit");
        server->sendMsg(m);
        sc->killServer();
    }
}

void serverManager::prepareForRestart(){
    initialized = false;
    if(preferences.local){
        ofxOscMessage m;
        m.setAddress("/quit");
        if(server != nullptr) server->sendMsg(m);
        if(sc == nullptr) sc = new scStart(preferences);
        sc->setPreferences(preferences);
        sc->killServer();
    }
}

void serverManager::setAudioDeviceNames(const std::string& outputDeviceName, const std::string& inputDeviceName, int inputChannels, int outputChannels){
    if(configuredNumInputBusChannels < 0) configuredNumInputBusChannels = preferences.numInputBusChannels;
    if(configuredNumOutputBusChannels < 0) configuredNumOutputBusChannels = preferences.numOutputBusChannels;

    preferences.deviceName = (outputDeviceName.empty() || outputDeviceName == "Default") ? "nil" : outputDeviceName;
    preferences.inputDeviceName = (inputDeviceName.empty() || inputDeviceName == "Default") ? "nil" : inputDeviceName;
    preferences.numInputBusChannels = configuredNumInputBusChannels;
    preferences.numOutputBusChannels = configuredNumOutputBusChannels;

    if(inputChannels > 0 && configuredNumInputBusChannels > inputChannels){
        ofLogNotice("serverManager") << "Clamping SC input channels for "
            << (preferences.inputDeviceName != "nil" ? preferences.inputDeviceName : preferences.deviceName)
            << " from " << configuredNumInputBusChannels << " to " << inputChannels;
        preferences.numInputBusChannels = inputChannels;
    }
    if(preferences.deviceName != "nil" && outputChannels > 0 && configuredNumOutputBusChannels > outputChannels){
        ofLogNotice("serverManager") << "Clamping SC output channels for " << preferences.deviceName
            << " from " << configuredNumOutputBusChannels << " to " << outputChannels;
        preferences.numOutputBusChannels = outputChannels;
    }
}

void serverManager::loadDefs(){
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    if(preferences.loadOnPreset){
        m.addStringArg(ofToDataPath(std::string(SYNTHDEF_DIRECTORY) + "/Defaults", true));
    }else{
        m.addStringArg(ofToDataPath(SYNTHDEF_DIRECTORY, true));
    }
    m.addIntArg(0);
    server->sendMsg(m);

    // DynGen slot SynthDefs (DynGenWrapper_N_S) live in their own subdir.
    // Copy/symlink the CompiledSynthdefs/dyngen/ output from dyngen.scd to
    // [data]/Supercollider/Synthdefs/dyngen/ so they are picked up here.
    std::string dyngenDir = ofToDataPath(std::string(SYNTHDEF_DIRECTORY) + "/dyngen", true);
    if(ofDirectory::doesDirectoryExist(dyngenDir)){
        ofxOscMessage m2;
        m2.setAddress("/d_loadDir");
        m2.addStringArg(dyngenDir);
        server->sendMsg(m2);
    }
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

void serverManager::setHardwareSampleRate(int sampleRate){
    if(sampleRate > 0){
        preferences.hardwareSampleRate = sampleRate;
        if(server != nullptr){
            serverSampleRates[server] = sampleRate;
        }
    }
}

int serverManager::getSampleRateForServer(ofxSCServer* server){
    auto it = serverSampleRates.find(server);
    if(it != serverSampleRates.end() && it->second > 0){
        return it->second;
    }
    return scPreferences().hardwareSampleRate;
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

void serverManager::teardownGraphForPresetLoad(){
    if(server == nullptr) return;

    outputBussesRefToNode.clear();
    inputBussesRefToNode.clear();
    connections.clear();
    nodesListChanged = true;

    const int silentBusIndex = busFromSilent != nullptr ? busFromSilent->index : 0;
    for(auto node : nodesList){
        if(node != nullptr){
            node->resetInputBusses(server, silentBusIndex);
            node->free(server);
        }
    }
    nodesList.clear();

    for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
    busses.clear();

    if(initialized){
        ofxOscMessage m;
        m.setAddress("/g_freeAll");
        m.addIntArg(1);
        server->sendMsg(m);
    }
}

void serverManager::recomputeGraph(){
    if(ofxOceanodeShared::isPresetLoading()) return;
    if(!initialized) return;
//    ofLog() << "Recompute Graph";
    if(outputs.size() == 0){
        for(auto node : nodesList) node->free(server);
        nodesList.clear();
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();
    }else{
        
//        server->setWaitToSend(true);
        
        
//        for(auto &b : busses) b.free();
        
        outputBussesRefToNode.clear();
        inputBussesRefToNode.clear();
        
        std::vector<scNode*> newNodesList;
        std::map<scNode*, std::pair<int, std::vector<int>>> nodeChilds;
        for(int i = 0; i < outputs.size(); i++){
            if(outputs[i]->isInputConnected()){
                outputs[i]->getInputNode()->appendOrderedNodes(newNodesList, nodeChilds);
            }
        }
        for(int i = 0; i < outputs.size(); i++){
            if(outputs[i]->isInputConnected()){
                newNodesList.push_back(outputs[i]);
            }
        }
        
        std::map<nodePort, std::vector<scNode*>> newConnections;
        
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            (*it)->getConnections(newConnections);
        }
        
        //Skip all proceses if the list is the same and if the nodeList has not changed (due to destroyed nodes)
        if(nodesList == newNodesList && !nodesListChanged && newConnections == connections){
            return;
        }
        
        nodesListChanged = false;
        
        std::vector<scNode*> toCreateNodes;
        std::vector<scNode*> toUpdateNodes;
        
        for(auto &node : newNodesList){
            auto nodeInListIter = std::find(nodesList.begin(), nodesList.end(), node);
            if(nodeInListIter != nodesList.end()){
                toUpdateNodes.push_back(node);
                nodesList.erase(nodeInListIter);
            }else{
                toCreateNodes.push_back(node);
            }
        }
        server->setBLatency(true);
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();
        
        for(auto node : nodesList){
            if(node != nullptr)
                node->free(server);
        }
        nodesList.clear();
        
        for(auto &node : toCreateNodes){
            nodeDestroyedListeners[node] = node->destroyedNode.newListener([this, node](){
                nodesList.erase(std::remove(nodesList.begin(), nodesList.end(), node), nodesList.end());
                nodesListChanged = true;
            });
        }
        
        for(auto &node : nodesList){
            nodeDestroyedListeners.erase(node);
        }
        
            
            for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
                if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
                    (*it)->buildSynth(server);
                }
                (*it)->resetInputBusses(server, busFromSilent->index);
            }
                
        //Create outputBusses for all nodes except scOutput
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            for(int i = 0; i < (*it)->getNumOutputs() ; i++){
                busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server);
                int busindex = busses.back().index;
                (*it)->setOutputBus(server, i, busindex);
                outputBussesRefToNode[(*it)][i] = busindex;
            }
        }
        
            for(auto &c : newConnections){
                for(auto &dest : c.second){
                    int busindex;
                    auto& nodeMap = outputBussesRefToNode[c.first.getNodeRef()];
                    if(nodeMap.count(c.first.getIndex()))
                        busindex = nodeMap[c.first.getIndex()];
                    else
                        busindex = c.first.getBusIndex(server); // fallback for self-managed buses (e.g. mix bus)
                    dest->setInputBus(server, c.first.getNodeRef(), busindex);
                    inputBussesRefToNode[dest].push_back(busindex);
                }
            }
        
        scNode* lastNode = nullptr;
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
                (*it)->createSynth(server);
            }
            else{
                if(lastNode == nullptr){
//                    (*it)->moveSynthAfter(server, -1);
                }
                else{
                    (*it)->moveSynthBefore(server, lastNode->getLastSynthID(server));
                }
            }
            lastNode = (*it);
        }
        
        nodesList = newNodesList;
        connections = newConnections;
//            nodesList.insert(nodesList.end(), newNodesList.begin(), newNodesList.end());
//        }
//        server->sendStoredBundle();
//        server->setWaitToSend(false);
        server->setBLatency(false);
    }
    graphComputed.notify();
}

void serverManager::loadSynthdefsFromPreset(std::string path){
    //TODO: clear all loaded definitions, via /d_free message https://doc.sccode.org/Reference/Server-Command-Reference.html
    
//    std::set<std::string> synthsList;
    std::set<std::string> synthsList;
    
    std::function<void(std::string)> checkSynthsInPreset = [this, &checkSynthsInPreset, &synthsList](std::string path){
        ofJson json = ofLoadJson(path + "/modules.json");
        
        if(json.empty()){
            return;
        }
        
        for (ofJson::iterator node = json.begin(); node != json.end(); ++node) {
            if(node.key().rfind("SC ", 0) == 0){
                std::string synthdefName = node.key();
                bool version2 = ofStringTimesInString(node.key(), "*") == 1;
                ofStringReplace(synthdefName, "SC ", "");
                ofStringReplace(synthdefName, "*", "");
                
                if(version2){
                    if(alreadyLoadedSynthsList.count(synthdefName) == 0){
                        synthsList.insert(synthdefName);
                    }
                }
//                else{
//                    synthsList.insert(synthdefName);
//                }
            }
            else if(node.key() == "Macro"){
                for (ofJson::iterator nodeID = node.value().begin(); nodeID != node.value().end(); ++nodeID) {
                    int id = ofToInt(nodeID.key());
                    
                    ofJson macroJson = ofLoadJson(path + "/Macro_" + ofToString(id) + ".json");
                    
                    if(macroJson.empty()) continue;
                    
                    if(macroJson["LocalPreset"]){
                        checkSynthsInPreset(path + "/Macro_" + ofToString(id));
                    }else{
                        std::string macroPath = macroJson["MacroPath"];
//                        ofLog() << macroPath;
                        std::vector<std::string> macroPathSplit = ofSplitString(macroPath, "/");
                        std::string recreatedMacroPath = "";
                        for(auto it = macroPathSplit.rbegin(); it !=macroPathSplit.rend(); it++){
                            recreatedMacroPath = *it + "/" + recreatedMacroPath;
                            if(*it == "Macros"){
//                                ofLog() << "Recreated Macro Path: " << recreatedMacroPath;
                                checkSynthsInPreset(recreatedMacroPath);
                                continue;
                            }
                        }
                    }
                }
            }
        }
    };
    
    checkSynthsInPreset(path);
    
    
    std::function<std::string(std::string, std::string)> searchForPathInDirectory = [&searchForPathInDirectory](std::string synthdefName, std::string searchPath)->std::string{
        ofDirectory dir(searchPath);
        dir.sort();
        for(auto &file : dir.getFiles()){
            if(file.isDirectory()){
                string path = searchForPathInDirectory(synthdefName, file.getAbsolutePath());
                if(path != "") return path;
            }else{
                if(file.getFileName() == (synthdefName + ".txarcmeta")){
                    return dir.getAbsolutePath();
                }
            }
        }
        return "";
    };
    
    for(auto &synthdef : synthsList){
        alreadyLoadedSynthsList.insert(synthdef);
        std::string path = synthdefFolders[synthdef];
        
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(path);
        server->sendMsg(m);
    }
    
    ofSleepMillis(100 * synthsList.size());
}


//int serverManager::getOutputBusForNode(scNode* node){
//    if(outputBussesRefToNode.count(node) == 1){
//        return outputBussesRefToNode[node];
//    }
//    else{
//        return -1;
//    }
//}
