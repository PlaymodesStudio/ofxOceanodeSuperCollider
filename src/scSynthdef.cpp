//
//  scSynthdef.cpp
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scSynthdef.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include <cmath>

namespace {
const int MAX_SERIAL_ITERATIONS = 32;
}

scSynthdef::scSynthdef(synthdefDesc _synthDescription) : synthDescription(_synthDescription), synthdefName(_synthDescription.name), scNode(_synthDescription.name + "*"){
    description = synthDescription.description;
    variableChanged = false;
}

void scSynthdef::setup(){
    // Keep metadata-backed descriptions available in the inspector even if the
    // model lifecycle changes and setup() runs after construction-only state.
    description = synthDescription.description;

    //First check for inputs
    for(auto spec : synthDescription.params){
        auto specMap = spec.second;
        if(specMap["units"] == "input"){
            string paramName = spec.first;
            //Modify name to have capital letters
            //TODO: make pattern like master_level be converted to Master Level
            paramName[0] = toupper(paramName[0]);
            scNode::addInput(paramName);
        }
    }
    
    addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
    const bool canUseSerialIterations = supportsSerialIterations();
    iterations.set("Iter", 1, 1, canUseSerialIterations ? MAX_SERIAL_ITERATIONS : 1);
    if(canUseSerialIterations){
        addInspectorParameter(iterations);
    }
    
    oldNumChannels = numChannels;
    oldIterations = iterations;
    listeners.push(numChannels.newListener([this](int &i){
        if(i < 1 || i > MAX_NODE_CHANNELS) return;
        if(oldNumChannels != numChannels || variableChanged){
            for(auto &synthServer : synths){
                replaceSynthChain(synthServer.first);
            }
            resendParams.notify();
        }
        oldNumChannels = numChannels;
        variableChanged = false;
    }));
    if(canUseSerialIterations){
        listeners.push(iterations.newListener([this](int &i){
            if(i < 1 || i > MAX_SERIAL_ITERATIONS) return;
            if(oldIterations != iterations){
                for(auto &synthServer : synths){
                    replaceSynthChain(synthServer.first);
                }
                resendParams.notify();
            }
            oldIterations = iterations;
        }));
    }
    
    
    for(auto variable : synthDescription.variables){
        ofParameter<int> var;
        addParameter(var.set(variable.first, 1, 1, variable.second));
        shared_ptr<int> oldVar(new int(var));
        
        listeners.push(var.newListener([this, oldVar](int &i){
            if(*oldVar != i){
                variableChanged = true;
                numChannels = numChannels; //To trigger recreation of synth
            }
            *oldVar = i;
        }));
    }
    
    
    for(auto spec : synthDescription.params){
        auto specMap = spec.second;
        string paramName = spec.first;
        string toSendName = ofToLower(paramName);
        //Modify name to have capital letters
        //TODO: make pattern like master_level be converted to Master Level
        paramName[0] = toupper(paramName[0]);
        
        bool hasAudioRate = false;
        string unitWithoutAudio = specMap["units"];
        shared_ptr<ofxOceanodeAbstractParameter> parameterReference = nullptr;
        if(specMap["units"][0] == 'a'){ //Audio rate
            unitWithoutAudio = specMap["units"].substr(1, specMap["units"].size()-1);
            hasAudioRate = true;
        }
        
        std::function<void()> setValuesToSynths;
        
        if(unitWithoutAudio == "vi"){
            ofParameter<vector<int>> vi;
            
            parameterReference = addParameter(vi.set(paramName,
                                vector<int>(1, ofToInt(specMap["default"])),
                                vector<int>(1, ofToInt(specMap["minval"])),
                                vector<int>(1, ofToInt(specMap["maxval"]))));
            
            setValuesToSynths = [this, toSendName, vi](){
                for(auto i : vi.get()) {
                    if(std::isnan(i)){
                        ofLog() << "Trying to send a nan value";
                        return;
                    }
                }
                forEachSynth([&](ofxSCSynth* synth){
                    if(vi->size() == 1) synth->setMultiple(toSendName, vi->at(0), numChannels);
                    else synth->set(toSendName, vi);
                });
            };
            
            listeners.push(vi.newListener([setValuesToSynths](vector<int> &vi_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio == "vf"){
            ofParameter<vector<float>> vf;
            parameterReference = addParameter(vf.set(paramName,
                                vector<float>(1, ofToFloat(specMap["default"])),
                                vector<float>(1, ofToFloat(specMap["minval"])),
                                vector<float>(1, ofToFloat(specMap["maxval"]))));
            
            setValuesToSynths = [this, toSendName, vf](){
                for(auto f : vf.get()) {
                    if(std::isnan(f)){
                        ofLog() << "Trying to send a nan value";
                        return;
                    }
                }
                forEachSynth([&](ofxSCSynth* synth){
                    if(vf->size() == 1) synth->setMultiple(toSendName, vf->at(0), numChannels);
                    else synth->set(toSendName, vf);
                });
            };
            
            listeners.push(vf.newListener([setValuesToSynths](vector<float> &vf_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio == "i"){
            ofParameter<int> i;
            
            parameterReference = addParameter(i.set(paramName,
                                ofToInt(specMap["default"]),
                                ofToInt(specMap["minval"]),
                                ofToInt(specMap["maxval"])));
            
            setValuesToSynths = [this, toSendName, i](){
                if(std::isnan(i.get())){
                    ofLog() << "Trying to send a nan value";
                    return;
                }
                forEachSynth([&](ofxSCSynth* synth){
                    synth->set(toSendName, i);
                });
            };
            
            listeners.push(i.newListener([setValuesToSynths](int &i_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio == "f"){
            ofParameter<float> f;
            parameterReference = addParameter(f.set(paramName,
                                ofToFloat(specMap["default"]),
                                ofToFloat(specMap["minval"]),
                                ofToFloat(specMap["maxval"])));
            
            setValuesToSynths = [this, toSendName, f](){
                if(std::isnan(f.get())){
                    ofLog() << "Trying to send a nan value";
                    return;
                }
                forEachSynth([&](ofxSCSynth* synth){
                    synth->set(toSendName, f);
                });
            };
            
            listeners.push(f.newListener([setValuesToSynths](float &f_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio == "b"){
            ofParameter<bool> b;
            parameterReference = addParameter(b.set(paramName,
                                ofToBool(specMap["default"]),
                                ofToBool(specMap["minval"]),
                                ofToBool(specMap["maxval"])));
            
            setValuesToSynths = [this, toSendName, b](){
                if(std::isnan(b.get())){
                    ofLog() << "Trying to send a nan value";
                    return;
                }
                forEachSynth([&](ofxSCSynth* synth){
                    synth->set(toSendName, b);
                });
            };
            
            listeners.push(b.newListener([this, setValuesToSynths](bool &b_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio == "buffer"){
            ofParameter<vector<int>> vi;
            addParameter(vi.set(paramName, {-1}, {-1}, {INT_MAX}));
            
            setValuesToSynths = [this, toSendName, vi](){
                for(auto i : vi.get()) {
                    if(std::isnan(i)){
                        ofLog() << "Trying to send a nan value";
                        return;
                    }
                }
                forEachSynth([&](ofxSCSynth* synth){
                    if(vi->size() == 1) synth->setMultiple(toSendName, vi->at(0), numChannels);
                    else synth->set(toSendName, vi);
                });
            };
            
            listeners.push(vi.newListener([setValuesToSynths](vector<int> &vi_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio.substr(0, 2) == "d:"){ //Is dropdown
            ofParameter<vector<int>> vi;
            vector<string> splitString = ofSplitString(specMap["units"], ":");
            splitString.erase(splitString.begin());
            parameterReference = addParameterDropdown(vi, paramName, ofToInt(specMap["default"]), splitString);
            
            setValuesToSynths = [this, toSendName, vi](){
                for(auto i : vi.get()) {
                    if(std::isnan(i)){
                        ofLog() << "Trying to send a nan value";
                        return;
                    }
                }
                forEachSynth([&](ofxSCSynth* synth){
                    if(vi->size() == 1) synth->setMultiple(toSendName, vi->at(0), numChannels);
                    else synth->set(toSendName, vi);
                });
            };
            
            listeners.push(vi.newListener([this, setValuesToSynths](vector<int> &vi_){
                setValuesToSynths();
            }));
        }
        else if(unitWithoutAudio.substr(0, 3) == "df:"){ //Is dropdown
            ofParameter<vector<float>> vf;
            vector<string> splitString = ofSplitString(specMap["units"], ":");
            splitString.erase(splitString.begin());
//            vector<string> options = ofSplitString(splitString[1], ", ");
            parameterReference = addParameterDropdown(vf, paramName, ofToInt(specMap["default"]), splitString);
            
            setValuesToSynths = [this, toSendName, vf](){
                for(auto f : vf.get()) {
                    if(std::isnan(f)){
                        ofLog() << "Trying to send a nan value";
                        return;
                    }
                }
                forEachSynth([&](ofxSCSynth* synth){
                    if(vf->size() == 1) synth->setMultiple(toSendName, vf->at(0), numChannels);
                    else synth->set(toSendName, vf);
                });
            };
            
            listeners.push(vf.newListener([this, setValuesToSynths](vector<float> &vf_){
                setValuesToSynths();
            }));
        }
        if(hasAudioRate && parameterReference != nullptr){
            auto availableInput = availableInputs.emplace_back(std::make_shared<nodePort>());
            parameterReference->addReceiveFunc<nodePort>([this, toSendName, availableInput](nodePort const &port){
                //TODO: Check why it triggers to times
                *availableInput = port;
                for(auto &output : outputs) output = output;
            });
            parameterReference->addDisconnectFunc([this, toSendName, availableInput](){
                *availableInput = nodePort();
                for(auto &output : outputs) output = output;
            });
            
            
            listeners.push(resetAudioRateBusAssignments.newListener([this, toSendName, availableInput](std::pair<ofxSCServer*, int> busAssignmentInfo){
                forEachSynth(busAssignmentInfo.first, [&](ofxSCSynth* synth){
                    synth->set(toSendName + "_sel", 0);
                    synth->mapan(toSendName + "_ar", busAssignmentInfo.second, MAX_NODE_CHANNELS);
                });
            }));
            
            listeners.push(setAudioRateBusAssignment.newListener([this, toSendName, availableInput](std::tuple<ofxSCServer*, scNode*, int> busAssignmentInfo){
                const auto [server, node, bus] = busAssignmentInfo;
                if(availableInput->getNodeRef() == node){
                    forEachSynth(server, [&](ofxSCSynth* synth){
                        synth->set(toSendName + "_sel", 1);
                        synth->mapan(toSendName + "_ar", bus, MAX_NODE_CHANNELS);
                    });
                }
            }));
        }
        listeners.push(resendParams.newListener([setValuesToSynths]{
            setValuesToSynths();
        }));
    }
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            configureSynthRouting(synthServer.first, synthServer.second, 0);
            if(serialSynths.count(synthServer.first) != 0){
                for(int i = 0; i < serialSynths[synthServer.first].size(); i++){
                    configureSynthRouting(synthServer.first, serialSynths[synthServer.first][i], i + 1);
                }
            }
            configureCompensationRouting(synthServer.first);
        }
    }));
    
    //Last check for outputs
    for(auto spec : synthDescription.params){
        auto specMap = spec.second;
        if(specMap["units"] == "output"){
            string paramName = spec.first;
            //Modify name to have capital letters
            //TODO: make pattern like master_level be converted to Master Level
            paramName[0] = toupper(paramName[0]);
            scNode::addOutput(paramName);
        }
    }
}

void scSynthdef::activate(){
    forEachSynth([](ofxSCSynth* synth){
        synth->run(true);
    });
    for(auto &synthServer : compensationSynths){
        if(synthServer.second != nullptr) synthServer.second->run(true);
    }
}

void scSynthdef::deactivate(){
    forEachSynth([](ofxSCSynth* synth){
        synth->run(false);
    });
    for(auto &synthServer : compensationSynths){
        if(synthServer.second != nullptr) synthServer.second->run(false);
    }
}

void scSynthdef::buildSynth(ofxSCServer* server){
    buildSynthChain(server);
}

void scSynthdef::createSynth(ofxSCServer* server){
    if(synths.count(server) == 0) return;
    resendParams.notify();
    createSynthChain(server);
}

void scSynthdef::moveSynthBefore(ofxSCServer* server, int nodeID){
    if(synths.count(server) == 0) return;
    resendParams.notify();
    int nextTarget = nodeID;
    if(compensationSynths.count(server) != 0 && compensationSynths[server] != nullptr){
        compensationSynths[server]->moveBefore(nextTarget);
        nextTarget = compensationSynths[server]->nodeID;
    }
    auto &chain = serialSynths[server];
    for(auto it = chain.rbegin(); it != chain.rend(); ++it){
        if(*it == nullptr) continue;
        (*it)->moveBefore(nextTarget);
        nextTarget = (*it)->nodeID;
    }
    if(synths[server] != nullptr) synths[server]->moveBefore(nextTarget);
}

void scSynthdef::free(ofxSCServer* server){
    freeCompensationSynth(server);
    freeSynthChain(server);
    freeSerialBuses(server);
    compensationSynths.erase(server);
    synths.erase(server);
    serialSynths.erase(server);
    serialBuses.erase(server);
    inputBuses.erase(server);
    outputBuses.erase(server);
    defaultInputBuses.erase(server);
}

void scSynthdef::freeAll(){
    for(auto &synthServer : compensationSynths){
        if(synthServer.second == nullptr) continue;
        synthServer.second->free();
        delete synthServer.second;
    }
    for(auto &synthServer : synths){
        if(synthServer.second == nullptr) continue;
        synthServer.second->free();
        delete synthServer.second;
    }
    for(auto &synthServer : serialSynths){
        for(auto synth : synthServer.second){
            if(synth == nullptr) continue;
            synth->free();
            delete synth;
        }
    }
    for(auto &busServer : serialBuses){
        for(auto bus : busServer.second){
            if(bus == nullptr) continue;
            bus->free();
            delete bus;
        }
    }
    compensationSynths.clear();
    synths.clear();
    serialSynths.clear();
    serialBuses.clear();
    inputBuses.clear();
    outputBuses.clear();
    defaultInputBuses.clear();
}

bool scSynthdef::supportsSerialIterations() const{
    int inputCount = 0;
    int outputCount = 0;
    for(auto spec : synthDescription.params){
        const auto &specMap = spec.second;
        auto unitsIt = specMap.find("units");
        if(unitsIt == specMap.end()) continue;
        if(unitsIt->second == "input") inputCount++;
        else if(unitsIt->second == "output") outputCount++;
    }
    return inputCount == 1 && outputCount == 1;
}

int scSynthdef::getEffectiveIterations() const{
    if(!supportsSerialIterations()) return 1;
    return ofClamp(iterations.get(), 1, MAX_SERIAL_ITERATIONS);
}

bool scSynthdef::usesIterationCompensation() const{
    return getEffectiveIterations() > 1;
}

void scSynthdef::freeSerialBuses(ofxSCServer* server){
    if(serialBuses.count(server) == 0) return;
    for(auto bus : serialBuses[server]){
        if(bus == nullptr) continue;
        bus->free();
        delete bus;
    }
    serialBuses[server].clear();
}

void scSynthdef::freeCompensationSynth(ofxSCServer* server){
    if(compensationSynths.count(server) == 0) return;
    if(compensationSynths[server] != nullptr){
        compensationSynths[server]->free();
        delete compensationSynths[server];
    }
    compensationSynths[server] = nullptr;
}

void scSynthdef::freeSynthChain(ofxSCServer* server){
    if(synths.count(server) == 1 && synths[server] != nullptr){
        synths[server]->free();
        delete synths[server];
        synths.erase(server);
    }
    freeSerialSynths(server);
}

void scSynthdef::freeSerialSynths(ofxSCServer* server){
    if(serialSynths.count(server) == 0) return;
    for(auto synth : serialSynths[server]){
        if(synth == nullptr) continue;
        synth->free();
        delete synth;
    }
    serialSynths[server].clear();
}

void scSynthdef::createSerialResources(ofxSCServer* server){
    const int chainSize = getEffectiveIterations();
    auto &tail = serialSynths[server];
    tail.reserve(chainSize - 1);
    for(int i = 1; i < chainSize; i++){
        tail.push_back(new ofxSCSynth(getSynthdefFilename(), server));
    }

    auto &buses = serialBuses[server];
    const int busCount = usesIterationCompensation() ? chainSize : chainSize - 1;
    buses.reserve(busCount);
    for(int i = 0; i < busCount; i++){
        buses.push_back(new ofxSCBus(RATE_AUDIO, MAX_NODE_CHANNELS, server));
    }

    if(usesIterationCompensation()){
        compensationSynths[server] = new ofxSCSynth(getCompensationSynthdefFilename(), server);
    }
}

void scSynthdef::buildSynthChain(ofxSCServer* server){
    if(server == nullptr) return;
    freeCompensationSynth(server);
    freeSynthChain(server);
    freeSerialBuses(server);
    synths[server] = new ofxSCSynth(getSynthdefFilename(), server);
    createSerialResources(server);
}

void scSynthdef::replaceSynthChain(ofxSCServer* server){
    if(server == nullptr) return;

    ofxSCSynth* oldCompensationSynth = compensationSynths.count(server) == 0 ? nullptr : compensationSynths[server];
    auto oldBuses = serialBuses[server];
    ofxSCSynth* oldHead = synths.count(server) == 0 ? nullptr : synths[server];
    const int anchorNodeID = oldHead == nullptr ? -1 : oldHead->nodeID;

    // Remove the old serial tail before inserting the replacement chain so a
    // live Iter change cannot leave old and new final stages summing together.
    freeSerialSynths(server);
    if(oldCompensationSynth != nullptr){
        oldCompensationSynth->free();
        delete oldCompensationSynth;
    }

    compensationSynths.erase(server);
    serialBuses[server].clear();

    auto newSynth = new ofxSCSynth(getSynthdefFilename(), server);
    synths[server] = newSynth;
    createSerialResources(server);

    resendParams.notify();

    configureSynthRouting(server, newSynth, 0);
    if(anchorNodeID > 0) newSynth->createAndRun(4, anchorNodeID, getActive());
    else newSynth->createAndRun(0, 1, getActive());

    int previousNodeID = newSynth->nodeID;
    auto &tail = serialSynths[server];
    for(int i = 0; i < tail.size(); i++){
        auto synth = tail[i];
        if(synth == nullptr) continue;
        configureSynthRouting(server, synth, i + 1);
        synth->createAndRun(3, previousNodeID, getActive());
        previousNodeID = synth->nodeID;
    }
    configureCompensationRouting(server);
    if(compensationSynths.count(server) != 0 && compensationSynths[server] != nullptr){
        compensationSynths[server]->createAndRun(3, previousNodeID, getActive());
    }

    if(oldHead != nullptr){
        delete oldHead;
    }
    for(auto bus : oldBuses){
        if(bus == nullptr) continue;
        bus->free();
        delete bus;
    }
}

void scSynthdef::createSynthChain(ofxSCServer* server){
    if(server == nullptr) return;
    if(synths.count(server) == 0 || synths[server] == nullptr){
        buildSynthChain(server);
    }

    configureSynthRouting(server, synths[server], 0);
    synths[server]->createAndRun(0, 1, getActive());

    int previousNodeID = synths[server]->nodeID;
    auto &tail = serialSynths[server];
    for(int i = 0; i < tail.size(); i++){
        auto synth = tail[i];
        if(synth == nullptr) continue;
        configureSynthRouting(server, synth, i + 1);
        synth->createAndRun(3, previousNodeID, getActive());
        previousNodeID = synth->nodeID;
    }
    configureCompensationRouting(server);
    if(compensationSynths.count(server) != 0 && compensationSynths[server] != nullptr){
        compensationSynths[server]->createAndRun(3, previousNodeID, getActive());
    }
}

void scSynthdef::configureSynthRouting(ofxSCServer* server, ofxSCSynth* synth, int iterationIndex){
    if(server == nullptr || synth == nullptr) return;

    synth->set("inChannels", numChannels);

    const int chainSize = getEffectiveIterations();
    if(chainSize > 1 && inputs.size() == 1 && outputs.size() == 1){
        string inputParamName = ofToLower(inputs[0].getName());
        string outputParamName = ofToLower(outputs[0].getName());

        int inputBus = defaultInputBuses.count(server) == 1 ? defaultInputBuses[server] : 0;
        scNode* inputNode = inputs[0]->getNodeRef();
        if(inputBuses[server].count(inputNode) == 1){
            inputBus = inputBuses[server][inputNode];
        }

        int outputBus = 0;
        if(outputBuses[server].count(outputs[0]->getIndex()) == 1){
            outputBus = outputBuses[server][outputs[0]->getIndex()];
        }

        int currentInputBus = inputBus;
        int currentOutputBus = outputBus;
        if(iterationIndex > 0 && iterationIndex - 1 < serialBuses[server].size()){
            currentInputBus = serialBuses[server][iterationIndex - 1]->index;
        }
        if(iterationIndex < serialBuses[server].size()){
            currentOutputBus = serialBuses[server][iterationIndex]->index;
        }

        synth->set(inputParamName, currentInputBus);
        synth->set(outputParamName, currentOutputBus);
        return;
    }

    for(int i = 0; i < inputs.size(); i++){
        string paramName = ofToLower(inputs[i].getName());
        int bus = defaultInputBuses.count(server) == 1 ? defaultInputBuses[server] : 0;
        scNode* inputNode = inputs[i]->getNodeRef();
        if(inputBuses[server].count(inputNode) == 1){
            bus = inputBuses[server][inputNode];
        }
        synth->set(paramName, bus);
    }

    for(int i = 0; i < outputs.size(); i++){
        if(outputBuses[server].count(outputs[i]->getIndex()) == 1){
            string paramName = ofToLower(outputs[i].getName());
            synth->set(paramName, outputBuses[server][outputs[i]->getIndex()]);
        }
    }
}

void scSynthdef::configureCompensationRouting(ofxSCServer* server){
    if(server == nullptr || !usesIterationCompensation()) return;
    if(compensationSynths.count(server) == 0 || compensationSynths[server] == nullptr) return;

    const int chainSize = getEffectiveIterations();
    if(serialBuses[server].size() < chainSize || outputs.size() != 1) return;

    int outputBus = 0;
    if(outputBuses[server].count(outputs[0]->getIndex()) == 1){
        outputBus = outputBuses[server][outputs[0]->getIndex()];
    }

    auto synth = compensationSynths[server];
    const float gain = 1.0f / std::sqrt(static_cast<float>(chainSize));
    synth->set("inChannels", numChannels);
    synth->set("in", serialBuses[server][chainSize - 1]->index);
    synth->set("out", outputBus);
    synth->setMultiple("gain", gain, numChannels.get());
}

void scSynthdef::setOutputBus(ofxSCServer* server, int index, int bus){
    outputBuses[server][index] = bus;
    for(int i = 0; i < outputs.size(); i++){
        if(outputs[i]->getIndex() == index){
            if(synths.count(server) != 0){
                resendParams.notify();
            }
        }
    }
}

void scSynthdef::setInputBus(ofxSCServer* server, scNode* node, int bus){
    inputBuses[server][node] = bus;
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i]->getNodeRef() == node){
            if(synths.count(server) != 0){
                resendParams.notify();
            }
        }
    }
    auto args = std::make_tuple(server, node, bus);
    setAudioRateBusAssignment.notify(args);
}

void scSynthdef::resetInputBusses(ofxSCServer* server, int targetBus){
    if(synths.count(server) == 0) return;
    inputBuses[server].clear();
    defaultInputBuses[server] = targetBus;
    resendParams.notify();
    auto args = std::make_pair(server, targetBus);
    resetAudioRateBusAssignments.notify(args);
}

int scSynthdef::getOutputBusIndex(ofxSCServer* server, int index){
    return outputBuses[server][index];
}

int scSynthdef::getLastSynthID(ofxSCServer* server){
    if(synths.count(server) == 0) return -1;
    return synths[server]->nodeID;
}

synthdefDesc scSynthdef::readAndCreateSynthdef(string file){
    ofBuffer fileBuffer = ofBufferFromFile(file);
    
    auto lineIterator = fileBuffer.getLines().begin();
    std::string lineString = (*lineIterator);
    ofStringReplace(lineString, "var ", "");
    ofStringReplace(lineString, " ", "");
    ofStringReplace(lineString, ";", "");
    vector<string> vars = ofSplitString(lineString, ",");
    
    //next line
    lineIterator++;
    
    //o
    bool varFound = false;
    while(!varFound){
        if(ofStringTimesInString(*lineIterator, "o = ") == 1){
            varFound = true;
        }
        lineIterator++;
    }
    string oarray = *lineIterator;
    while((*++lineIterator) != "];"){
        oarray += (*lineIterator);
    }
    ofStringReplace(oarray, "\t", "");
    ofStringReplace(oarray, " ", "");
    vector<string> odata = ofSplitString(oarray, ",");
    for(int i = 0; i < odata.size(); i++){
        if(ofStringTimesInString(odata[i], "\"") > 0){
            ofStringReplace(odata[i], "\"", "");
        }else{
            odata[i] = "p[" + ofToString(i) + "]";
        }
    }
    
    //p
    lineIterator++;
    lineIterator++;
    // skip event line
    vector<vector<string>> pStringData;
    string array;
    while((*++lineIterator) != "];"){
        if(ofStringTimesInString(*lineIterator, "//") == 1){
            ofStringReplace(array, " [ ", "");
            ofStringReplace(array, " ], ", "");
            ofStringReplace(array, " ]", "");
            ofStringReplace(array, "\t", "");
            ofStringReplace(array, "\'", "");
            ofStringReplace(array, "\'", "");
            ofStringReplace(array, " ", "");
            string arrayId = ofSplitString(array, ",").front();
            array.erase(0, arrayId.length()+1);
            pStringData.resize(ofToInt(arrayId)+1);
            pStringData[ofToInt(arrayId)] = ofSplitString(array, ",");
            array = "";
        }else{
            array += (*lineIterator);
        }
    }
    ofStringReplace(array, " [ ", "");
    ofStringReplace(array, " ], ", "");
    ofStringReplace(array, " ]", "");
    ofStringReplace(array, "\t", "");
    ofStringReplace(array, "\'", "");
    ofStringReplace(array, "\'", "");
    ofStringReplace(array, " ", "");
    string arrayId = ofSplitString(array, ",").front();
    array.erase(0, arrayId.length()+1);
    pStringData.resize(ofToInt(arrayId)+1);
    pStringData[ofToInt(arrayId)] = ofSplitString(array, ",");
    
    
    vector<map<string, string>> pdata;
    pdata.resize(pStringData.size());
    for(int i = 0; i < pStringData.size(); i++){
        if(pStringData[i].size() == 0) continue;
        if(ofStringTimesInString(pStringData[i][0], ":")){
            for(auto comb : pStringData[i]){
                auto split = ofSplitString(comb, ":");
                if(split[0] != "nil")
                    pdata[i][split[0]] = split[1];
             }
        }else if (pStringData[i].size()>=2) //
		{
            for(int j = 0; j < pStringData[i].size(); j = j+2){
                if(pStringData[i][j] != "nil")
                    pdata[i][pStringData[i][j]] = pStringData[i][j+1];
            }
        }
		else ofLog()<< "readAndCreateSynthdef : Error in file : " << file;
    }
    
    std::function<string(string)> getStringFromData = [odata, pdata, &getStringFromData](string checkvalue) -> string{
        if(ofStringTimesInString(checkvalue, "o[")){
            checkvalue.erase(0, 2); //remove o[
            checkvalue.erase(checkvalue.length()-1); //remove ]
            return getStringFromData(odata[ofToInt(checkvalue)]);
        }else if(ofStringTimesInString(checkvalue, "p[")){
            checkvalue.erase(0, 2); //remove o[
            checkvalue.erase(checkvalue.length()-1); //remove ]
            return "";
        }
        return checkvalue;
    };
    
    std::function<std::map<string, string>(string)> getMapFromData = [odata, pdata, &getMapFromData](string checkvalue) -> std::map<string, string>{
        if(ofStringTimesInString(checkvalue, "o[")){
            checkvalue.erase(0, 2); //remove o[
            checkvalue.erase(checkvalue.length()-1); //remove ]
            return getMapFromData(odata[ofToInt(checkvalue)]);
        }else if(ofStringTimesInString(checkvalue, "p[")){
            checkvalue.erase(0, 2); //remove o[
            checkvalue.erase(checkvalue.length()-1); //remove ]
            return pdata[ofToInt(checkvalue)];
        }
        return map<string, string>();
    };
    
    //We are interested in pdata[1]?
    synthdefDesc currentDescription;
    currentDescription.name = getStringFromData(pdata[1]["name"]);
    currentDescription.type = getStringFromData(pdata[1]["type"]);
    currentDescription.description = getStringFromData(pdata[1]["description"]);
    ofStringReplace(currentDescription.description, "_", " ");
    ofStringReplace(currentDescription.description, "|", ",");
    currentDescription.category = getStringFromData(pdata[1]["category"]);
    vector<string> variableNames = ofSplitString(getStringFromData(pdata[1]["variables"]), ":");
    vector<string> variableDimensions = ofSplitString(getStringFromData(pdata[1]["variableDimensions"]), ":");
    if(variableNames.size() == variableDimensions.size() && variableNames[0] != ""){
        currentDescription.variables.resize(variableNames.size());
        for(int i = 0; i < currentDescription.variables.size(); i++){
            currentDescription.variables[i] = std::make_pair(variableNames[i], ofToInt(variableDimensions[i]));
        }
    }
    if(pdata.size() > 5){
        auto specsPos = pdata[1]["specs"];
        specsPos.erase(0, 2); //remove o[
        specsPos.erase(1); //remove ]
        specsPos = pdata[ofToInt(specsPos)]["array"];
        specsPos.erase(0, 2); //remove o[
        specsPos.erase(1); //remove ]
        auto specsList = pdata[ofToInt(specsPos)];//getMapFromData(pdata[1]["specs"]);
        for(auto spec : specsList){
            currentDescription.params[spec.first] = getMapFromData(spec.second);
            // replace units for text in odata.
            currentDescription.params[spec.first]["units"] = getStringFromData(currentDescription.params[spec.first]["units"]);
        }
    }
    return currentDescription;
}

string scSynthdef::getSynthdefFilename(){
    string filename = synthdefName + ofToString(numChannels);
    for(auto variable : synthDescription.variables){
        filename += "_" + ofToString(getParameter<int>(variable.first));
    }
    return filename;
}

string scSynthdef::getCompensationSynthdefFilename(){
    return "LinearGain" + ofToString(numChannels);
}
