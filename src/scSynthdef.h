//
//  scSynthdef.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#ifndef scSynthdef_h
#define scSynthdef_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"

class scSynthdef : public ofxOceanodeNodeModel {
public:
    scSynthdef(std::string name, int _size, std::string _params, int _ins = 0, int _outs = 1, ofxSCServer *_server = ofxSCServer::local()) : synthdefName(name), size(_size), params(_params), ins(_ins), outs(_outs), ofxOceanodeNodeModel("SC " + name){
        server = _server;
        synth = nullptr;
    };
    ~scSynthdef(){
        if(synth != nullptr){
            synth->free();
            delete synth;
        }
        for(auto b : buses) b->free();
    }
    
    void setup(){
        synth = new ofxSCSynth(ofToLower(synthdefName), server);
        synth->create();
        
        for(int i = 0; i < ins; i++){
            ofParameter<std::pair<ofxSCBus*, scSynthdef*>> p;
            string paramName = "In";
            if(i > 0) paramName += ofToString(i+1);
            addParameter(p.set(paramName, std::make_pair(nullptr, nullptr)), ofxOceanodeParameterFlags_DisableOutConnection);
            listeners.push(p.newListener([this](std::pair<ofxSCBus*, scSynthdef*> &pair){
                if(pair.first != nullptr){
                    for(int i = ins-1; i >= 0; i--){
                        string paramName = "In";
                        if(i > 0) paramName += ofToString(i+1);
                        auto p = getParameter<std::pair<ofxSCBus*, scSynthdef*>>(paramName);
                        if(p->first != nullptr){
                            synth->order(3, getRecursiveIDs(false));
                            synth->set(ofToLower(paramName), p->first->index);
                        }
                    }
                    for(int i = 0; i < outs; i++){
                        busesParams[i] = busesParams[i];
                    }
                }
            }));
        }
        addParameters();
        buses.resize(outs);
        busesParams.resize(outs);
        for(int i = 0; i < outs; i++){
            buses[i] = new ofxSCBus(RATE_AUDIO, size, server);
            string paramName = "Out";
            if(i > 0) paramName += ofToString(i+1);
            synth->set(ofToLower(paramName), buses[i]->index);
            addOutputParameter(busesParams[i].set(paramName, std::make_pair(buses.back(), this)));
        }
    }
    
    void addParameters(){
        vector<std::string> splittedParams = ofSplitString(params, ", ");
        for(string &s : splittedParams){
            vector<string> ss = ofSplitString(s, ":");
            if(ss[0] == "f"){
                ofParameter<float> f;
                addParameter(f.set(ss[1], ofToFloat(ss[2]), ofToFloat(ss[3]), ofToFloat(ss[4])));
                string toSendName = ofToLower(ss[1]);
                listeners.push(f.newListener([this, toSendName](float &f_){
                    synth->set(toSendName, f_);
                }));
            }else if(ss[0] == "vf"){
                ofParameter<vector<float>> vf;
                addParameter(vf.set(ss[1], vector<float>(1, ofToFloat(ss[2])), vector<float>(1, ofToFloat(ss[3])), vector<float>(1, ofToFloat(ss[4]))));
                string toSendName = ofToLower(ss[1]);
                listeners.push(vf.newListener([this, toSendName](vector<float> &vf_){
                    if(vf_.size() == 1) synth->set(toSendName, vector<float>(size, vf_[0]));
                    else synth->set(toSendName, vf_);
                }));
            }
            else if(ss[0] == "i"){
                ofParameter<int> i;
                addParameter(i.set(ss[1], ofToInt(ss[2]), ofToInt(ss[3]), ofToInt(ss[4])));
                string toSendName = ofToLower(ss[1]);
                listeners.push(i.newListener([this, toSendName](int &i_){
                    synth->set(toSendName, i_);
                }));
            }
            else if(ss[0] == "vi"){
                ofParameter<vector<int>> vi;
                addParameter(vi.set(ss[1], vector<int>(1, ofToInt(ss[2])), vector<int>(1, ofToInt(ss[3])), vector<int>(1, ofToInt(ss[4]))));
                string toSendName = ofToLower(ss[1]);
                listeners.push(vi.newListener([this, toSendName](vector<int> &vi_){
                    if(vi_.size() == 1) synth->set(toSendName, vector<int>(size, vi_[0]));
                    else synth->set(toSendName, vi_);
                }));
            }
        }
    }
    
    vector<int> getRecursiveIDs(bool itself = true){
        vector<int> ids;
        for(int i = ins-1; i >= 0; i--){
            string paramName = "In";
            if(i > 0) paramName += ofToString(i+1);
            auto p = getParameter<std::pair<ofxSCBus*, scSynthdef*>>(paramName);
            if(p->first != nullptr){
                vector<int> newIds = p->second->getRecursiveIDs(true);
                ids.insert(ids.end(), newIds.begin(), newIds.end());
            }
        }
        
        if(itself) ids.push_back(synth->nodeID);
        
        vector<int> uniqueIds;
        for(auto &i : ids){
            if(std::find(uniqueIds.begin(), uniqueIds.end(), i) == uniqueIds.end()){
                uniqueIds.push_back(i);
            }
        }
        
        return uniqueIds;
    }
    
private:
    ofEventListeners listeners;
    
    vector<ofxSCBus*> buses;
    vector<ofParameter<std::pair<ofxSCBus*, scSynthdef*>>> busesParams;
    
    std::string synthdefName;
    int size;
    std::string params;
    int ins;
    int outs;
    
    ofxSCSynth *synth;
    ofxSCServer *server;
};

#endif /* scSynthdef_h */
