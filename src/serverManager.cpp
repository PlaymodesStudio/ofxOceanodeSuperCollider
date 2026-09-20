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
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

std::map<ofxSCServer*, int> serverManager::serverSampleRates;

namespace {
std::string shellQuote(const std::string& value){
    std::string quoted = "'";
    for(char c : value){
        if(c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

std::string getScPluginPath(const std::string& scsynthPath){
    const std::string scRoot = scsynthPath.substr(0, scsynthPath.size() - 7);
    return scRoot.find("/SuperCollider.app/") != std::string::npos
        ? scRoot + "plugins"
        : scRoot;
}
}

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

bool serverManager::beginNRTCapture(){
    if(server == nullptr || !initialized) return false;

    // Must happen first: some nodes hold their real state inside the server
    // rather than in their own parameters, and pulling it back needs a reply.
    // Once capture starts nothing is transmitted, so no reply can arrive.
    prepareNodesForNRTCapture();

    // Keep the live graph out of the score while using the normal graph
    // builder to replay its complete state at score time zero.
    server->beginNRTCapture(true);
    teardownGraphForPresetLoad();
    server->clearNRTScore();
    server->setNRTTime(0.0);

    ofxOscMessage group;
    group.setAddress("/g_new");
    group.addIntArg(1);
    group.addIntArg(0);
    group.addIntArg(0);
    server->sendMsg(group);

    loadNRTSynthdefs();
    recomputeGraph();
    return true;
}

void serverManager::prepareNodesForNRTCapture(){
    if(server == nullptr) return;

    bool anyPending = false;
    for(auto node : connectedNodes){
        if(node == nullptr) continue;
        node->prepareForNRTCapture();
        anyPending = anyPending || node->isNRTCapturePreparationPending();
    }
    if(!anyPending) return;

    // Pump the server so the replies that complete those pulls arrive. The
    // timeout keeps a plugin that never answers from blocking the render; the
    // capture then simply falls back to the last state the node had cached.
    const uint64_t deadline = ofGetElapsedTimeMillis() + 2000;
    while(ofGetElapsedTimeMillis() < deadline){
        server->process();
        anyPending = false;
        for(auto node : connectedNodes){
            if(node != nullptr && node->isNRTCapturePreparationPending()){
                anyPending = true;
                break;
            }
        }
        if(!anyPending) return;
        ofSleepMillis(5);
    }
    ofLogWarning("serverManager") << "NRT capture: some nodes did not finish saving their state in time";
}

void serverManager::loadNRTSynthdefs(){
    // The NRT renderer is a separate scsynth process. Load only the current
    // preset's definitions instead of replaying the entire Synthdefs tree.
    const std::string presetPath = ofxOceanodeShared::getCurrentPresetPath();
    const std::string absolutePresetPath = ofToDataPath(presetPath, true);
    if(!presetPath.empty() && ofDirectory::doesDirectoryExist(absolutePresetPath)){
        loadSynthdefsFromPreset(presetPath, true, false);
    }else{
        ofLogWarning("serverManager") << "NRT capture has no valid current preset; loading the complete Synthdefs tree";
        loadDefs();
    }

    // Output is a legacy SynthDef without a .txarcmeta descriptor.
    const std::string outputSynthDef = ofToDataPath(
        std::string(SYNTHDEF_DIRECTORY) + "/Defaults/output.scsyndef", true);
    if(ofFile::doesFileExist(outputSynthDef)){
        ofxOscMessage message;
        message.setAddress("/d_load");
        message.addStringArg(outputSynthDef);
        server->sendMsg(message);
    }
}

void serverManager::endNRTCapture(double endTime){
    if(server == nullptr) return;
    server->endNRTCapture(endTime);

    // The model graph was rebuilt for the score. Rebuild it once more against
    // the realtime server so normal playback continues without stale node
    // handles or bus allocations.
    teardownGraphForPresetLoad();
    recomputeGraph();
}

bool serverManager::writeNRTScore(const std::string& path, double endTime) const{
    return server != nullptr && server->writeNRTScore(path, endTime);
}

std::size_t serverManager::getNRTEventCount() const{
    return server != nullptr ? server->getNRTEventCount() : 0;
}

int serverManager::renderNRT(const std::string& scorePath, const std::string& outputPath, int outputChannels) const{
    if(server == nullptr) return -1;

    std::string scPath = ofToDataPath("Supercollider/Scsynth/bin/scsynth", true);
    if(!ofFile::doesFileExist(scPath)) scPath = "/Applications/SuperCollider.app/Contents/Resources/scsynth";

    const auto &p = preferences;
    outputChannels = std::max(1, std::min(outputChannels, p.numOutputBusChannels));
    std::ostringstream command;
    command << shellQuote(scPath) << " -N " << shellQuote(scorePath) << " _ "
            << shellQuote(outputPath) << " " << p.hardwareSampleRate
            << " WAVE float"
            << " -o " << outputChannels
            << " -i " << p.numInputBusChannels
            << " -a " << p.numAudioBusChannels
            << " -c " << p.numControlBusChannels
            << " -b " << p.numBuffers
            << " -n " << p.maxNodes
            << " -d " << p.maxSynthDefs
            << " -m " << p.memSize
            << " -w " << p.numWireBufs
            << " -r " << p.numRGens
            << " -l " << p.maxLogins
            << " -z " << p.blockSize
            << " -D 0";
    if(!p.ugensPlugins.empty()){
        // scsynth does not search its standard plugin locations when -U is
        // supplied. Keep the bundled SuperCollider plugins available in NRT
        // renders, in addition to Oceanode's optional custom UGen folder.
        command << " -U " << shellQuote(ofToDataPath(p.ugensPlugins, true) + ":" + getScPluginPath(scPath));
    }
    // scsynth prints one progress line per score packet in NRT mode, which
    // would bury the console -- but its failures ("SynthDef not found", bus
    // and node errors) go to the same stream, and those are exactly what is
    // needed when a render comes out silent. Keep the lot in a log beside the
    // audio rather than discarding it.
    const std::string logPath = outputPath + ".log";
    command << " > " << shellQuote(logPath) << " 2>&1";

    ofLogNotice("serverManager") << "Rendering SuperCollider NRT score: " << outputPath;
    const int result = std::system(command.str().c_str());

    // Surface anything that looks like a failure, so a silent render explains
    // itself in the console instead of only in the log.
    std::ifstream log(logPath);
    if(log.is_open()){
        int reported = 0;
        for(std::string line; std::getline(log, line); ){
            if(line.find("FAILURE") == std::string::npos &&
               line.find("ERROR") == std::string::npos &&
               line.find("exception") == std::string::npos) continue;
            if(++reported > 20){
                ofLogError("serverManager") << "NRT render: further errors in " << logPath;
                break;
            }
            ofLogError("serverManager") << "NRT render: " << line;
        }
        if(reported == 0 && result != 0){
            ofLogError("serverManager") << "NRT render: scsynth exited with " << result
                                        << "; see " << logPath;
        }
    }
    return result;
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

void serverManager::loadSynthdefsFromPreset(const std::string& path, bool forceLoad, bool waitForLoad){
    //TODO: clear all loaded definitions, via /d_free message https://doc.sccode.org/Reference/Server-Command-Reference.html
    
//    std::set<std::string> synthsList;
    std::set<std::string> synthsList;
    
    std::function<void(const std::string&)> checkSynthsInPreset = [this, &checkSynthsInPreset, &synthsList, forceLoad](const std::string& path){
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
                    if(forceLoad || alreadyLoadedSynthsList.count(synthdefName) == 0){
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

    for(auto &synthdef : synthsList){
        auto folder = synthdefFolders.find(synthdef);
        if(folder == synthdefFolders.end() || folder->second.empty()){
            ofLogWarning("serverManager") << "Could not find SynthDef folder for " << synthdef;
            continue;
        }
        if(!forceLoad) alreadyLoadedSynthsList.insert(synthdef);
        std::string path = folder->second;
        
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(path);
        server->sendMsg(m);
    }
    
    if(waitForLoad) ofSleepMillis(100 * synthsList.size());
}


//int serverManager::getOutputBusForNode(scNode* node){
//    if(outputBussesRefToNode.count(node) == 1){
//        return outputBussesRefToNode[node];
//    }
//    else{
//        return -1;
//    }
//}
