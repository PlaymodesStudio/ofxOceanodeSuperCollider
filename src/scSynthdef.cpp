//
//  scSynthdef.cpp
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scSynthdef.h"
#include "ofxSCSynth.h"


scSynthdef::scSynthdef(synthdefDesc _synthDescription) : synthDescription(_synthDescription), synthdefName(_synthDescription.name), scNode(_synthDescription.name + "*"){
    description = synthDescription.description;
    variableChanged = false;
}

void scSynthdef::setup(){
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
    
    oldNumChannels = numChannels;
    listeners.push(numChannels.newListener([this](int &i){
        if(i < 1 || i > MAX_NODE_CHANNELS) return;
        if(oldNumChannels != numChannels || variableChanged){
            for(auto &synth : synths){
                ofxSCSynth *newSynth = new ofxSCSynth(getSynthdefFilename(), synth.first);
                newSynth->create(4, synth.second->nodeID); //replace synth
                newSynth->run(getActive());
                delete synth.second;
                synth.second = newSynth;
            }
            resendParams.notify();
        }
        oldNumChannels = numChannels;
        variableChanged = false;
    }));
    
    
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
                for(auto synthServer : synths){
                    if(vi->size() == 1) synthServer.second->setMultiple(toSendName, vi->at(0), numChannels);
                    else synthServer.second->set(toSendName, vi);
                }
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
                for(auto synthServer : synths){
                    if(vf->size() == 1) synthServer.second->setMultiple(toSendName, vf->at(0), numChannels);
                    else synthServer.second->set(toSendName, vf);
                }
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
                for(auto synthServer : synths){
                    synthServer.second->set(toSendName, i);
                }
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
                for(auto synthServer : synths){
                    synthServer.second->set(toSendName, f);
                }
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
                for(auto synthServer : synths){
                    synthServer.second->set(toSendName, b);
                }
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
                for(auto synthServer : synths){
                    if(vi->size() == 1) synthServer.second->setMultiple(toSendName, vi->at(0), numChannels);
                    else synthServer.second->set(toSendName, vi);
                }
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
                for(auto synthServer : synths){
                    if(vi->size() == 1) synthServer.second->setMultiple(toSendName, vi->at(0), numChannels);
                    else synthServer.second->set(toSendName, vi);
                }
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
                for(auto synthServer : synths){
                    if(vf->size() == 1) synthServer.second->setMultiple(toSendName, vf->at(0), numChannels);
                    else synthServer.second->set(toSendName, vf);
                }
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
            
            listeners.push(resendParams.newListener([this, toSendName, availableInput]{
                if(availableInput->getNodeRef() != nullptr){
                    for(auto synthServer : synths){
                        synthServer.second->set(toSendName + "_sel", 1);
                        synthServer.second->mapan(toSendName + "_ar", availableInput->getBusIndex(synthServer.first), MAX_NODE_CHANNELS);
                    }
                }else{
                    for(auto synthServer : synths){
                        synthServer.second->set(toSendName + "_sel", 0);
                        synthServer.second->mapan(toSendName + "_ar", -1, MAX_NODE_CHANNELS);
                    }
                }
            }));
        }
        listeners.push(resendParams.newListener([setValuesToSynths]{
            setValuesToSynths();
        }));
    }
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            if(synthServer.second != nullptr){
                synthServer.second->set("inChannels", numChannels);
            }
        }
        for(auto synthServer : synths){
            for(int i = 0; i < inputs.size(); i++){
                if(inputBuses[synthServer.first].count(inputs[i]->getNodeRef()) == 1){
                    string paramName = ofToLower(inputs[i].getName());
                    if(synthServer.second != nullptr){
                        synthServer.second->set(paramName, inputBuses[synthServer.first][inputs[i]->getNodeRef()]);
                    }
                }
            }
            for(int i = 0; i < outputs.size(); i++){
                if(outputBuses[synthServer.first].count(outputs[i]->getIndex()) == 1){
                    string paramName = ofToLower(outputs[i].getName());
                    if(synthServer.second != nullptr){
                        synthServer.second->set(paramName, outputBuses[synthServer.first][outputs[i]->getIndex()]);
                    }
                }
            }
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
    for(auto &synth : synths) synth.second->run(true);
}

void scSynthdef::deactivate(){
    for(auto &synth : synths) synth.second->run(false);
}

void scSynthdef::buildSynth(ofxSCServer* server){
    synths[server] = new ofxSCSynth(getSynthdefFilename(), server);
}

void scSynthdef::createSynth(ofxSCServer* server){
    if(synths.count(server) == 0) return;
    resendParams.notify();
    synths[server]->create();
    synths[server]->run(getActive());
}

void scSynthdef::moveSynthBefore(ofxSCServer* server, int nodeID){
    if(synths.count(server) == 0) return;
    resendParams.notify();
    synths[server]->moveBefore(nodeID);
}

void scSynthdef::free(ofxSCServer* server){
    if(synths.count(server) == 1){
        synths[server]->free();
        delete synths[server];
        synths.erase(server);
    }
}

void scSynthdef::freeAll(){
    for(auto &synth : synths) synth.second->free();
    synths.clear();
}

void scSynthdef::setOutputBus(ofxSCServer* server, int index, int bus){
    outputBuses[server][index] = bus;
    for(int i = 0; i < outputs.size(); i++){
        if(outputs[i]->getIndex() == index){
            string paramName = ofToLower(outputs[i].getName());
            if(synths.count(server) != 0){
                synths[server]->set(paramName, bus);
            }
        }
    }
}

void scSynthdef::setInputBus(ofxSCServer* server, scNode* node, int bus){
    inputBuses[server][node] = bus;
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i]->getNodeRef() == node){
            string paramName = ofToLower(inputs[i].getName());
            if(synths.count(server) != 0){
                synths[server]->set(paramName, bus);
            }
        }
    }
}

void scSynthdef::resetInputBusses(ofxSCServer* server, int targetBus){
    inputBuses[server].clear();
    for(int i = 0; i < inputs.size(); i++){
        string paramName = ofToLower(inputs[i].getName());
        if(synths.count(server) != 0){
            synths[server]->set(paramName, targetBus);
        }
    }
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
