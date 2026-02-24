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

serverManager::serverManager(){
    volume = 1;
    mute = false;
    delay = 0;
    stereomix = false;
    stereomixSize = 2;
    audioDevice = 0;
    dumpOsc = false;
    numRecomputeGraphOnce = 0;
    busFromSilent = nullptr;
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
    }));
    
    listeners.push(ofxOceanodeShared::getPresetHasLoadedEvent().newListener([this](){
        if(preferences.loadOnPreset){
            loadSynthdefsFromPreset(ofxOceanodeShared::getCurrentPresetPath());
        }
        recomputeGraph();
    }));
    
    listeners.push(server->serverBootedEvent.newListener([this](){
        initialize();
    }));
    listeners.push(server->serverInitializedEvent.newListener([this](){
        recomputeGraph();
    }));
	
	if(busFromSilent == nullptr) busFromSilent = std::make_unique<ofxSCBus>(RATE_AUDIO, MAX_NODE_CHANNELS, server);
    listeners.push(server->queryTreeReplyEvent.newListener(this, &serverManager::checkGraph));
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
    
    if(ImGui::Button("Check graph")){
        ofxOscMessage m;
        m.setAddress("/g_queryTree");
        m.addIntArg(0);
        m.addIntArg(1);
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
    if(preferences.loadOnPreset){
        m.addStringArg(ofToDataPath(std::string(SYNTHDEF_DIRECTORY) + "/Defaults", true));
    }else{
        m.addStringArg(ofToDataPath(SYNTHDEF_DIRECTORY, true));
    }
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
                    int busindex = outputBussesRefToNode[c.first.getNodeRef()][c.first.getIndex()];
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

void serverManager::checkGraph(ofxOscMessage &m){
    struct synthControl{
        bool usesIndex;
        std::string name = "";
        int index = 0;
        bool usesAssignment;
        float value;
        std::string assignment;
    };
    
    struct synthStruct{
        synthStruct(){};
        synthStruct(int _synthId, std::string _synthName) : synthId(_synthId), synthName(_synthName){};
        int synthId;
        std::string synthName;
        std::vector<synthControl> controls;
    };
    
    std::map<int, synthStruct> synths;
    
    int readIndex = 0;
    bool controlValuesIncluded = m.getArgAsInt(readIndex++);
    int nodeIdOfRequestedGroup = m.getArgAsInt(readIndex++);
    int numChildNodesInGroup = m.getArgAsInt(readIndex++);
    
    std::function<void()> readChild = [m, &readChild, &readIndex, controlValuesIncluded, &synths](){
        int nodeId = m.getArgAsInt(readIndex++);
        int numChildNodes = m.getArgAsInt(readIndex++);
        if(numChildNodes != -1){
            for(int i = 0; i < numChildNodes; i++){
                readChild();
            }
        }
        else{
            std::string synthName = m.getArgAsString(readIndex++);
            synths[nodeId] = synthStruct(nodeId, synthName);
            if(controlValuesIncluded){
                int numControlValues = m.getArgAsInt(readIndex++);
                for(int i = 0; i < numControlValues; i++){
                    auto &control = synths[nodeId].controls.emplace_back();
                    ofxOscArgType controlArgType = m.getArgType(readIndex);
                    if(controlArgType == OFXOSC_TYPE_INT32){
                        control.usesIndex = true;
                        control.index = m.getArgAsInt(readIndex++);
                    }
                    else if(controlArgType == OFXOSC_TYPE_STRING){
                        control.usesIndex = false;
                        control.name = m.getArgAsString(readIndex++);
                    }
                    
                    ofxOscArgType controlValueArgType = m.getArgType(readIndex);
                    if(controlValueArgType == OFXOSC_TYPE_FLOAT){
                        control.usesAssignment = false;
                        control.value = m.getArgAsFloat(readIndex++);
                    }
                    else if(controlValueArgType == OFXOSC_TYPE_STRING){
                        control.usesAssignment = true;
                        control.assignment = m.getArgAsString(readIndex++);
                    }
                }
            }
        }
    };
    
    for(int i = 0; i < numChildNodesInGroup; i++){
        readChild();
    }
    
    
    auto getNodeNameid = [](scNode* node) -> std::string {
        std::string result = node->getParameterGroup().getName();
        std::string parents = node->getParents();
        if(parents != "Canvas"){
            result = result + " " + parents;
        }
        ofStringReplace(result, " ", "_");
        ofStringReplace(result, "_/_", "_");
        ofStringReplace(result, "*", "");
        ofStringReplace(result, "SC ", "");
        return result;
    };
    
    auto returnReplaceSpaces = [](std::string s) -> std::string {
        ofStringReplace(s, " ", "_");
        return s;
    };

    if(nodesList.size() != 0){ //Create graphvix diagram
        
        std::ofstream fout(ofToDataPath("foo.dot"));

//        file_out << "This is my output\n";
//        fout << "-------------- Begin Dot --------------" << endl;
        fout << "digraph G {" << endl;
        fout << "rankdir=\"LR\";" << endl;
        fout << "node [ shape = rectangle ]" << endl;
        fout << "graph [ splines=polyline ]" << endl;
        fout << endl;
        
        struct macromap{
            std::map<std::string, macromap> childs;
            std::vector<std::string> elements;
        };
        
        macromap mm;
        std::map<std::string, std::string> nodesMap;
        std::map<int, std::pair<std::string, std::vector<std::string>>> busesConnections;
        int i = nodesList.size();
        for(auto &node : nodesList){
            std::string nodename = node->getParameterGroup().getName();
            std::string parents = node->getParents();
            std::string nodeid = getNodeNameid(node);
            int nodeOrder = i;
            vector<int> nodeInServerIDs = node->getNodeIDs(server);
            std::string nodeelement;// = "subgraph cluster_" + nodeid + " {\n";
//            nodeelement += "label=\"" + nodename + " \\n Order: " + ofToString(i, 2, '0') + "\"\n";
            for(auto scNode : nodeInServerIDs){
                
                
                /*
                 synth1 [shape=plaintext label=<
                                 <TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0">
                                 <TR><TD COLSPAN="3">Panner 3</TD></TR>
                                 <TR><TD COLSPAN="3">Order: 64</TD></TR>
                                 <TR><TD COLSPAN="3">ID: 2001</TD></TR>
                                 <TR><TD COLSPAN="3">2001</TD></TR>
                                 <TR><TD PORT="in1" BGCOLOR="lightgray" WIDTH="1"></TD><TD>in1</TD></TR>
                                 <TR><TD PORT="levels"></TD><TD COLSPAN="1">Levels</TD></TR>
                                 <TR><TD SIDES="R"></TD><TD>output</TD><TD PORT="output" BGCOLOR="lightgray" WIDTH="1"></TD></TR>
                                 </TABLE>>];
                 */
                
                
                if(scNode != -1 && synths.count(scNode) == 1){
                    synthStruct synth = synths[scNode];
                    
                    nodeelement += nodeid + "_" + ofToString(scNode) + " [shape=plaintext label=<\n";
                    nodeelement += "<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\">\n";
                    nodeelement += "<TR><TD COLSPAN=\"3\">" + parents + "</TD></TR>";
                    nodeelement += "<TR><TD COLSPAN=\"3\">" + nodename + "</TD></TR>";
                    nodeelement += "<TR><TD COLSPAN=\"3\">Order: " + ofToString(i, 2, '0') + "</TD></TR>";
                    nodeelement += "<TR><TD COLSPAN=\"3\">ID: " + ofToString(scNode) + "</TD></TR>\n";
                    
//                    for(auto c : synth.controls){
//                        nodeelement += "<TR><TD PORT=\"" + c.name + "\"></TD><TD COLSPAN=\"1\">" + c.name + "</TD></TR>\n";
//                    }
                    
                    for(auto p : node->getParameterGroup()){
                        if(std::find_if(node->getInputs().begin(), node->getInputs().end(), [p](auto &v){return p->getName() == v.getName();}) != node->getInputs().end()){
                            nodeelement += "<TR><TD PORT=\"" + p->getName() + "\" WIDTH=\"1\"></TD><TD WIDTH=\"100\">" + p->getName() + "</TD></TR>\n";
                            auto search = std::find_if(synth.controls.begin(), synth.controls.end(), [p](auto &contr){
                                return ofToLower(p->getName()) == contr.name;
                            });
                            if(search != synth.controls.end()){
                                busesConnections[search->value].second.push_back(nodeid + "_" + ofToString(scNode) + ":" + p->getName());
                            }
                        }
                        else if(std::find_if(node->getOutputs().begin(), node->getOutputs().end(), [p](auto &v){return p->getName() == v.getName();}) != node->getOutputs().end()){
                            nodeelement += "<TR><TD SIDES=\"R\"></TD><TD WIDTH=\"100\">" + p->getName() + "</TD><TD PORT=\"" + p->getName() + "\" WIDTH=\"1\"></TD></TR>\n";
                            auto search = std::find_if(synth.controls.begin(), synth.controls.end(), [p](auto &contr){
                                return ofToLower(p->getName()) == contr.name;
                            });
                            if(search != synth.controls.end()){
                                busesConnections[search->value].first = nodeid + "_" + ofToString(scNode) + ":" + p->getName();
                            }
                        }
                        else{
                            //TODO: Draw if modulable ar
//                            nodeelement += "<TR><TD PORT=\"" + p->getName() + "\"></TD><TD COLSPAN=\"1\">" + p->getName() + "</TD></TR>\n";
                            auto search = std::find_if(synth.controls.begin(), synth.controls.end(), [p](auto &contr){
                                return (ofToLower(p->getName()) + "_sel") == contr.name;
                            });
                            if(search != synth.controls.end() && search->value == 1){
                                auto search2 = std::find_if(synth.controls.begin(), synth.controls.end(), [p](auto &contr){
                                    return (ofToLower(p->getName()) + "_ar") == contr.name;
                                });
                                nodeelement += "<TR><TD PORT=\"" + p->getName() + "\" WIDTH=\"1\"></TD><TD WIDTH=\"100\">" + p->getName() + "</TD></TR>\n";
                                
                                std::string assignment = search2->assignment;
                                assignment.erase(assignment.begin());
                                busesConnections[ofToInt(assignment)].second.push_back(nodeid + "_" + ofToString(scNode) + ":" + p->getName());
                                
                            }
                        }
                        
                    }
                    
                    nodeelement += "</TABLE>>];\n";
                }
                else{
                   //TODO: DRAW RED SYNTH
                    ofLog() << "Not fount node " << nodename;
                }
            }
//            nodeelement += "}";
            
//            std::string parents = node->getParents();
            if(parents == "Canvas"){
                mm.elements.push_back(nodeelement);
            }else{
                std::vector<std::string> splittedParents = ofSplitString(parents, " / ");
                macromap* mm_ref = &mm;
                for(auto &parent : splittedParents){
                    mm_ref = &mm_ref->childs[parent];
                }
                mm_ref->elements.push_back(nodeelement);
            }
            i--;
        }
        
        int clusterid = 0;
        
        std::function<void(macromap)> printAllElements = [&printAllElements, &clusterid, &fout](macromap mm){
            for(auto &e : mm.elements){
                fout << e << endl;
            }
            for(auto &c : mm.childs){
                std::string childname = c.first;
//                fout << "subgraph cluster_" << clusterid++ << " {" << endl;
//                fout << "label = \"" << childname << "\"" << endl;
//                fout << "style=filled;" << endl;
//                fout << "node [style=filled,color=white];" << endl;
                printAllElements(c.second);
                
//                fout << "}" << endl;
            }
        };
        
        printAllElements(mm);
        
        fout << endl;
        
//        for (auto it = nodesList.rbegin(); it != nodesList.rend(); ++it) {
//            int i = 0;
//            for(auto in : (*it)->getInputs()){
//                std::string tonodeid = getNodeNameid(*it);
//                std::string fromnodeid = "";
//                if(in->getNodeRef() != nullptr){
//                    fromnodeid = getNodeNameid(in->getNodeRef());
//                }else{//Create empty node
//                    fromnodeid = tonodeid + ofToString(i);
//                    //n0 [label= "", shape=none,height=.0,width=.0]
//                    cout << fromnodeid << " [label= \"\", shape=none,height=0,width=0]" << endl;
//                }
//                cout << fromnodeid << " -> " << tonodeid << " [label = \"" << in->getBusIndex(server) << "\", headlabel = \"" << in.getName() << "\"]" <<  endl;
//                i++;
//            }
//        }
        
//            for(auto &c : connections){
//                std::string fromnodeid = getNodeNameid(c.first.getNodeRef());
//                for(auto &dest : c.second){
//                    int busindex = outputBussesRefToNode[c.first.getNodeRef()][c.first.getIndex()];
//                    std::string tonodeid = getNodeNameid(dest);
//                    cout << fromnodeid << " -> " << tonodeid << " [label = \"" << busindex << "\"]" << endl;
//                }
//            }
        
        for(auto c : busesConnections){
            if(c.second.first == ""){
                for(auto dest : c.second.second){
                    std::string destNoDots = dest.substr(0, dest.find(':'));
                    fout << "null_" + ofToString(c.first) + "_" + destNoDots + " [label= \"\", shape=none,height=0,width=0]\n";
                    
                    fout << "null_" + ofToString(c.first) + "_" + destNoDots << " -> " << dest << " [label = \"" << c.first << "\"]" << endl;
                }
            }
            else{
                for(auto dest : c.second.second){
                    fout << c.second.first << " -> " << dest << " [label = \"" << c.first << "\"]" << endl;
                }
            }
        }
        fout << "}" << endl;
        ofSystem("/opt/homebrew/bin/dot -Tpdf \"" + ofToDataPath("foo.dot") + "\" -o \"" + ofToDataPath("foo.pdf") + "\"");
//        fout << "-------------- End Dot --------------" << endl;
    }
}


//int serverManager::getOutputBusForNode(scNode* node){
//    if(outputBussesRefToNode.count(node) == 1){
//        return outputBussesRefToNode[node];
//    }
//    else{
//        return -1;
//    }
//}
