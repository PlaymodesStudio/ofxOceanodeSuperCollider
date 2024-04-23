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
    numChannels = description.numChannels;
    
    buffers.resize(description.numBuffers);
    for(int i = 0; i < buffers.size(); i++){
        string paramName = "Bufnum";
        if(i > 0) paramName += ofToString(i+1);
        addParameter(buffers[i].set(paramName, {0}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(buffers[i].newListener([this, i, paramName](vector<int> &buffs){
            for(auto synthServer : synths){
                bool serverWaitToSendWasTrue = false;
                if(synthServer.first->getWaitToSend()){
                    serverWaitToSendWasTrue = true;
                }else{
                    synthServer.first->setWaitToSend(true);
                }
                if(buffs.size() != 0){
                    if(buffs.size() == 1) synthServer.second->setMultiple(ofToLower(paramName), buffs[0], numChannels);
                    else synthServer.second->set(ofToLower(paramName), buffs);
                }
                if(!serverWaitToSendWasTrue){
                    synthServer.first->sendStoredBundle();
                    synthServer.first->setWaitToSend(false);
                }
            }
        }));
    }
    
    for(auto spec : description.params){
        auto specMap = spec.second;
        string paramName = spec.first;
//        paramName[0] = toupper(paramName[0]);
        if(specMap["type"] == "vi"){
            ofParameter<vector<int>> vi;
            
            addParameter(vi.set(paramName,
                                vector<int>(1, ofToInt(specMap["default"])),
                                vector<int>(1, ofToInt(specMap["minval"])),
                                vector<int>(1, ofToInt(specMap["maxval"]))));
            string toSendName = ofToLower(spec.first);
            listeners.push(vi.newListener([this, toSendName](vector<int> &vi_){
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    if(vi_.size() == 1) synthServer.second->setMultiple(toSendName, vi_[0], numChannels);
                    else synthServer.second->set(toSendName, vi_);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, vi, toSendName]{
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    if(vi->size() == 1) synthServer.second->setMultiple(toSendName, vi->at(0), numChannels);
                    else synthServer.second->set(toSendName, vi);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
        }else if(specMap["type"] == "vf"){
            ofParameter<vector<float>> vf;
            addParameter(vf.set(paramName,
                                vector<float>(1, ofToFloat(specMap["default"])),
                                vector<float>(1, ofToFloat(specMap["minval"])),
                                vector<float>(1, ofToFloat(specMap["maxval"]))));
            string toSendName = ofToLower(spec.first);
            listeners.push(vf.newListener([this, toSendName](vector<float> &vf_){
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    if(vf_.size() == 1) synthServer.second->setMultiple(toSendName, vf_[0], numChannels);
                    else synthServer.second->set(toSendName, vf_);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, vf, toSendName]{
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    if(vf->size() == 1) synthServer.second->setMultiple(toSendName, vf->at(0), numChannels);
                    else synthServer.second->set(toSendName, vf);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
        }else if(specMap["type"] == "i"){
            ofParameter<int> i;
            
            addParameter(i.set(paramName,
                                ofToInt(specMap["default"]),
                                ofToInt(specMap["minval"]),
                                ofToInt(specMap["maxval"])));
            string toSendName = ofToLower(spec.first);
            listeners.push(i.newListener([this, toSendName](int &i_){
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    synthServer.second->set(toSendName, i_);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, i, toSendName]{
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    synthServer.second->set(toSendName, i);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
        }else if(specMap["type"] == "f"){
            ofParameter<float> f;
            addParameter(f.set(paramName,
                                ofToFloat(specMap["default"]),
                                ofToFloat(specMap["minval"]),
                                ofToFloat(specMap["maxval"])));
            string toSendName = ofToLower(spec.first);
            listeners.push(f.newListener([this, toSendName](float &f_){
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    synthServer.second->set(toSendName, f_);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
            listeners.push(resendParams.newListener([this, f, toSendName]{
                for(auto synthServer : synths){
                    bool serverWaitToSendWasTrue = false;
                    if(synthServer.first->getWaitToSend()){
                        serverWaitToSendWasTrue = true;
                    }else{
                        synthServer.first->setWaitToSend(true);
                    }
                    synthServer.second->set(toSendName, f);
                    if(!serverWaitToSendWasTrue){
                        synthServer.first->sendStoredBundle();
                        synthServer.first->setWaitToSend(false);
                    }
                }
            }));
        }
    }
    
    addInspectorParameter(doNotDistributeInputs.set("Not Dist In", false));
    addInspectorParameter(doNotDistributeOutputs.set("Not Dist Out", false));
    
    listeners.push(doNotDistributeInputs.newListener([this](bool &b){
        for(auto synthServer : synths){
            bool serverWaitToSendWasTrue = false;
            if(synthServer.first->getWaitToSend()){
                serverWaitToSendWasTrue = true;
            }else{
                synthServer.first->setWaitToSend(true);
            }
            for(int i = 0; i < inputs.size(); i++){
                if(inputBuses[synthServer.first].count(inputs[i].get()) == 1){
                    string paramName = "in";
                    if(i > 0) paramName += ofToString(i+1);
                    if(synthServer.second != nullptr){
                        synthServer.second->set(paramName, inputBuses[synthServer.first][inputs[i].get()]);
                    }
                }
            }
            if(!serverWaitToSendWasTrue){
                synthServer.first->sendStoredBundle();
                synthServer.first->setWaitToSend(false);
            }
        }
    }));
        
    listeners.push(doNotDistributeOutputs.newListener([this](bool &b){
        for(auto synthServer : synths){
            bool serverWaitToSendWasTrue = false;
            if(synthServer.first->getWaitToSend()){
                serverWaitToSendWasTrue = true;
            }else{
                synthServer.first->setWaitToSend(true);
            }
            if(synthServer.second != nullptr){
                synthServer.second->set("out", outputBus[synthServer.first]);
            }
            if(!serverWaitToSendWasTrue){
                synthServer.first->sendStoredBundle();
                synthServer.first->setWaitToSend(false);
            }
        }
    }));
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            bool serverWaitToSendWasTrue = false;
            if(synthServer.first->getWaitToSend()){
                serverWaitToSendWasTrue = true;
            }else{
                synthServer.first->setWaitToSend(true);
            }
            if(synthServer.second != nullptr){
                synthServer.second->set("inChannels", numChannels);
            }
            if(!serverWaitToSendWasTrue){
                synthServer.first->sendStoredBundle();
                synthServer.first->setWaitToSend(false);
            }
        }
        
    }));
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            bool serverWaitToSendWasTrue = false;
            if(synthServer.first->getWaitToSend()){
                serverWaitToSendWasTrue = true;
            }else{
                synthServer.first->setWaitToSend(true);
            }
            for(int b = 0; b < buffers.size(); b++){
                if(buffers[b]->size() == 1) synthServer.second->set(ofToLower(buffers[b].getName()), buffers[b]->at(0));
                else synthServer.second->set(ofToLower(buffers[b].getName()), buffers[b]);
            }
            if(!serverWaitToSendWasTrue){
                synthServer.first->sendStoredBundle();
                synthServer.first->setWaitToSend(false);
            }
        }
    }));
    
    scNode::addOutput();
}


void scSynthdef::createSynth(ofxSCServer* server){
    synths[server] = new ofxSCSynth(ofToLower(synthdefName), server);
    synths[server]->create();
    resendParams.notify();
}

void scSynthdef::free(ofxSCServer* server){
    synths[server]->free();
    delete synths[server];
    synths.erase(server);
}

void scSynthdef::setOutputBus(ofxSCServer* server, int bus){
    outputBus[server] = bus;
    if(synths[server] != nullptr){
        synths[server]->set("out", bus);
    }
}

void scSynthdef::setInputBus(ofxSCServer* server, scNode* node, int bus){
    inputBuses[server][node] = bus;
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i].get() == node){
            string paramName = "in";
            if(i > 0) paramName += ofToString(i+1);
            if(synths[server] != nullptr){
                synths[server]->set(paramName, bus);
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
