//
//  scOldSynthdef.cpp
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#include "scOldSynthdef.h"
#include "ofxSCSynth.h"

scOldSynthdef::scOldSynthdef(oldSynthdefDesc _description) : description(_description), synthdefName(_description.name), scNode(_description.name){
    
}

void scOldSynthdef::setup(){
    for(int i = 0; i < description.numInputs; i++){
        string paramName = "In";
        if(i > 0) paramName += ofToString(i+1);
        scNode::addInput(paramName);
    }
    
    numChannels = description.numChannels;
    
    buffers.resize(description.numBuffers);
    for(int i = 0; i < buffers.size(); i++){
        string paramName = "Bufnum";
        if(i > 0) paramName += ofToString(i+1);
        addParameter(buffers[i].set(paramName, {0}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(buffers[i].newListener([this, i, paramName](vector<int> &buffs){
            for(auto synthServer : synths){
                if(buffs.size() != 0){
                    if(buffs.size() == 1) synthServer.second->setMultiple(ofToLower(paramName), buffs[0], numChannels);
                    else synthServer.second->set(ofToLower(paramName), buffs);
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
                    if(vi_.size() == 1) synthServer.second->setMultiple(toSendName, vi_[0], numChannels);
                    else synthServer.second->set(toSendName, vi_);
                }
            }));
            listeners.push(resendParams.newListener([this, vi, toSendName]{
                for(auto synthServer : synths){
                    if(vi->size() == 1) synthServer.second->setMultiple(toSendName, vi->at(0), numChannels);
                    else synthServer.second->set(toSendName, vi);
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
                    if(vf_.size() == 1) synthServer.second->setMultiple(toSendName, vf_[0], numChannels);
                    else synthServer.second->set(toSendName, vf_);
                }
            }));
            listeners.push(resendParams.newListener([this, vf, toSendName]{
                for(auto synthServer : synths){
                    if(vf->size() == 1) synthServer.second->setMultiple(toSendName, vf->at(0), numChannels);
                    else synthServer.second->set(toSendName, vf);
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
                    synthServer.second->set(toSendName, i_);
                }
            }));
            listeners.push(resendParams.newListener([this, i, toSendName]{
                for(auto synthServer : synths){
                    synthServer.second->set(toSendName, i);
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
                    synthServer.second->set(toSendName, f_);
                }
            }));
            listeners.push(resendParams.newListener([this, f, toSendName]{
                for(auto synthServer : synths){
                    synthServer.second->set(toSendName, f);
                }
            }));
        }
    }
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            if(synthServer.second != nullptr){
                synthServer.second->set("inChannels", numChannels);
            }
        }
    }));
    
    listeners.push(resendParams.newListener([this](){
        for(auto synthServer : synths){
            for(int b = 0; b < buffers.size(); b++){
                if(buffers[b]->size() == 1) synthServer.second->set(ofToLower(buffers[b].getName()), buffers[b]->at(0));
                else synthServer.second->set(ofToLower(buffers[b].getName()), buffers[b]);
            }
        }
    }));
    
    scNode::addOutput("Out");
}


void scOldSynthdef::createSynth(ofxSCServer* server){
    synths[server] = new ofxSCSynth(ofToLower(synthdefName), server);
    synths[server]->create();
    resendParams.notify();
}

void scOldSynthdef::free(ofxSCServer* server){
    if(synths.count(server) == 1){
        synths[server]->free();
        delete synths[server];
        synths.erase(server);
    }
}

void scOldSynthdef::freeAll(){
    for(auto &synth : synths) synth.second->free();
    synths.clear();
}

void scOldSynthdef::setOutputBus(ofxSCServer* server, int index, int bus){
    outputBus[server] = bus;
    if(synths[server] != nullptr){
        synths[server]->set("out", bus);
    }
}

void scOldSynthdef::setInputBus(ofxSCServer* server, scNode* node, int bus){
    inputBuses[server][node] = bus;
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i]->getNodeRef() == node){
            string paramName = "in";
            if(i > 0) paramName += ofToString(i+1);
            if(synths[server] != nullptr){
                synths[server]->set(paramName, bus);
            }
        }
    }
}

int scOldSynthdef::getOutputBusIndex(ofxSCServer* server, int index){
    return 0; //Only one output in this type
}

oldSynthdefDesc scOldSynthdef::readAndCreateSynthdef(string file){
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
    oldSynthdefDesc currentDescription;
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
