//
//  scSynthdef.cpp
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#include "scSynthdef.h"
#include "ofxSCSynth.h"

scSynthdef::scSynthdef(synthdefDesc _description) : description(_description), synthdefName(_description.name), scNode(_description.name){
    
}

void scSynthdef::setup(){
    scNode::addInputs(description.numInputs);
    addParameter(numChannels.set("N Chan", 1, 1, 100));
    
    buffers.resize(description.numBuffers);
    for(int i = 0; i < buffers.size(); i++){
        string paramName = "Buf";
        if(i > 0) paramName += ofToString(i+1);
        addParameter(buffers[i].set(paramName, {0}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(buffers[i].newListener([this, i, paramName](vector<int> &buffs){
            for(auto synthServer : synths){
                if(buffs.size() != 0){
                    for(int i = 0; i < synthServer.second.size(); i++){
                        if(buffs.size() == 1) synthServer.second[i]->set(ofToLower(paramName), buffs[0]);
                        else synthServer.second[i]->set(ofToLower(paramName), buffs[i]);
                    }
                }
            }
        }));
    }
    
    oldNumChannels = numChannels;
    listeners.push(numChannels.newListener([this](int &i){
        if(oldNumChannels != numChannels){
            bool remove = oldNumChannels > numChannels;
            
            if(remove){
                for(int j = oldNumChannels-1; j >= numChannels; j--){
                    for(auto &synthServer : synths){
                        synthServer.second.back()->free();
                        synthServer.second.pop_back();
                    }
                }
            }else{
                for(auto &synthServer : synths){
                    int lastSynthId = synthServer.second.back()->nodeID;
                    for(int j = 0; j < numChannels-oldNumChannels; j++){
                        synthServer.second.emplace_back(new ofxSCSynth(ofToLower(synthdefName), synthServer.first))->create(2, lastSynthId); //put node just before last one;
                    }
                }
            }
            
            for(auto synthServer : synths){
                for(int j = 0; j < synthServer.second.size(); j++){
                    if(synthServer.second[j] != nullptr){
                        synthServer.second[j]->set("out", outputBus[synthServer.first] + (doNotDistributeOutputs ? 0 : j));
                    }
                    for(int i = 0; i < inputs.size(); i++){
                        if(inputBuses[synthServer.first].count(inputs[i].get()) == 1){
                            string paramName = "in";
                            if(i > 0) paramName += ofToString(i+1);
                            for(int j = 0; j < synthServer.second.size(); j++){
                                if(synthServer.second[j] != nullptr){
                                    synthServer.second[j]->set(paramName, inputBuses[synthServer.first][inputs[i].get()] + (doNotDistributeInputs ? 0 : j));
                                }
                            }
                        }
                    }
                }
                
            }
            
            resendParams.notify();
        }
        oldNumChannels = numChannels;
    }));
    
    
    
    for(auto spec : description.params){
        auto specMap = spec.second;
        string paramName = spec.first;
        paramName[0] = toupper(paramName[0]);
        if(ofToFloat(specMap["step"]) == 1.0){
            ofParameter<vector<int>> vi;
            
            addParameter(vi.set(paramName,
                                vector<int>(1, ofToInt(specMap["default"])),
                                vector<int>(1, ofToInt(specMap["minval"])),
                                vector<int>(1, ofToInt(specMap["maxval"]))));
            string toSendName = spec.first;
            listeners.push(vi.newListener([this, toSendName](vector<int> &vi_){
                for(auto synthServer : synths){
                    for(int i = 0; i < synthServer.second.size(); i++){
                        if(vi_.size() == 1) synthServer.second[i]->set(toSendName, vi_[0]);
                        else synthServer.second[i]->set(toSendName, vi_[i]);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, vi, toSendName]{
                for(auto synthServer : synths){
                    for(int i = 0; i < synthServer.second.size(); i++){
                        if(vi->size() == 1) synthServer.second[i]->set(toSendName, vi->at(0));
                        else synthServer.second[i]->set(toSendName, vi->at(i));
                    }
                }
            }));
        }else{
            ofParameter<vector<float>> vf;
            addParameter(vf.set(paramName,
                                vector<float>(1, ofToFloat(specMap["default"])),
                                vector<float>(1, ofToFloat(specMap["minval"])),
                                vector<float>(1, ofToFloat(specMap["maxval"]))));
            string toSendName = spec.first;
            listeners.push(vf.newListener([this, toSendName](vector<float> &vf_){
                for(auto synthServer : synths){
                    for(int i = 0; i < synthServer.second.size(); i++){
                        if(vf_.size() == 1) synthServer.second[i]->set(toSendName, vf_[0]);
                        else synthServer.second[i]->set(toSendName, vf_[i]);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, vf, toSendName]{
                for(auto synthServer : synths){
                    for(int i = 0; i < synthServer.second.size(); i++){
                        if(i >= vf->size()) synthServer.second[i]->set(toSendName, vf->at(0));
                        else synthServer.second[i]->set(toSendName, vf->at(i));
                    }
                }
            }));
        }
    }
    
    addInspectorParameter(doNotDistributeInputs.set("Not Dist In", false));
    addInspectorParameter(doNotDistributeOutputs.set("Not Dist Out", false));
    
    listeners.push(doNotDistributeInputs.newListener([this](bool &b){
        for(auto synthServer : synths){
            for(int j = 0; j < synthServer.second.size(); j++){
                for(int i = 0; i < inputs.size(); i++){
                    if(inputBuses[synthServer.first].count(inputs[i].get()) == 1){
                        string paramName = "in";
                        if(i > 0) paramName += ofToString(i+1);
                        for(int j = 0; j < synthServer.second.size(); j++){
                            if(synthServer.second[j] != nullptr){
                                synthServer.second[j]->set(paramName, inputBuses[synthServer.first][inputs[i].get()] + (doNotDistributeInputs ? 0 : j));
                            }
                        }
                    }
                }
            }
        }
    }));
        
    listeners.push(doNotDistributeOutputs.newListener([this](bool &b){
        for(auto synthServer : synths){
            for(int j = 0; j < synthServer.second.size(); j++){
                if(synthServer.second[j] != nullptr){
                    synthServer.second[j]->set("out", outputBus[synthServer.first] + (doNotDistributeOutputs ? 0 : j));
                }
            }
        }
    }));
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            for(int j = 0; j < synthServer.second.size(); j++){
                if(synthServer.second[j] != nullptr){
                    synthServer.second[j]->set("inChannels", numChannels);
                    synthServer.second[j]->set("index", j);
                }
            }
        }
        
    }));
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            for(int b = 0; b < buffers.size(); b++){
                for(int i = 0; i < synthServer.second.size(); i++){
                    if(buffers[b]->size() == 1) synthServer.second[i]->set(ofToLower(buffers[b].getName()), buffers[b]->at(0));
                    else synthServer.second[i]->set(ofToLower(buffers[b].getName()), buffers[b]->at(i));
                }
            }
        }
    }));
    
    scNode::addOutput();
}


void scSynthdef::createSynth(ofxSCServer* server){
    for(int i = 0; i < numChannels; i++){
        synths[server].emplace_back(new ofxSCSynth(ofToLower(synthdefName), server))->create();
    }
    resendParams.notify();
}

void scSynthdef::free(ofxSCServer* server){
    for(auto s : synths[server]){
        s->free();
    }
    synths.erase(server);
}

void scSynthdef::setOutputBus(ofxSCServer* server, int bus){
    outputBus[server] = bus;
    for(int i = 0; i < synths[server].size(); i++){
        if(synths[server][i] != nullptr){
            synths[server][i]->set("out", bus + (doNotDistributeOutputs ? 0 : i));
        }
    }
}

void scSynthdef::setInputBus(ofxSCServer* server, scNode* node, int bus){
    inputBuses[server][node] = bus;
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i].get() == node){
            string paramName = "in";
            if(i > 0) paramName += ofToString(i+1);
            for(int j = 0; j < synths[server].size(); j++){
                if(synths[server][j] != nullptr){
                    synths[server][j]->set(paramName, bus + (doNotDistributeInputs ? 0 : j));
                }
            }
        }
    }
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
        }else{
            for(int j = 0; j < pStringData[i].size(); j = j+2){
                if(pStringData[i][j] != "nil")
                    pdata[i][pStringData[i][j]] = pStringData[i][j+1];
            }
        }
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
    currentDescription.numInputs = ofToInt(getStringFromData(pdata[1]["numInputs"]));
    currentDescription.numBuffers = ofToInt(getStringFromData(pdata[1]["numBuffers"]));
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
        }
    }
    return currentDescription;
}
