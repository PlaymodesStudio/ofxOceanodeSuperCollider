#ifndef scA2k_h
#define scA2k_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scA2k : public ofxOceanodeNodeModel {
public:
	scA2k(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC A2k") {
		servers = outputServers;
		synth = nullptr;
		valueBus = nullptr;
	}

	~scA2k() {
		clearSynth();
	}

	void setup(){
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size() - 1));
		addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
		addOutputParameter(values.set("Values", {0}, {-FLT_MAX}, {FLT_MAX}));

		listeners.push(input.newListener([this](nodePort &port){
			if(port.getNodeRef() != nullptr){
				recreateSynth();
			} else {
				clearSynth();
			}
		}));

		listeners.push(serverIndex.newListener([this](int &i){
			if(input->getNodeRef() != nullptr){
				recreateSynth();
			}
			serverGraphListener.unsubscribe();
			serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){
				recreateSynth();
			});
		}));

		listeners.push(numChannels.newListener([this](int &i){
            if(i < 1 || i > MAX_NODE_CHANNELS) return;
			if(input->getNodeRef() != nullptr){
				recreateSynth();
			}
		}));
	}

	void update(ofEventArgs &args) override {
		if(synth != nullptr){
			values = valueBus->readValues;
			valueBus->requestValues();
		}
	}

private:
	void activate() override {
		if(synth) synth->run(true);
	}

	void deactivate() override {
		if(synth) synth->run(false);
	}

	void recreateSynth(){
		clearSynth();
		if(input->getNodeRef() != nullptr){
			string defName = "a2k" + ofToString(numChannels);
			synth = new ofxSCSynth(defName, servers[serverIndex]->getServer());
			synth->addToTail();
			synth->run(getActive());

			valueBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());
			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
			synth->set("value", valueBus->index);
		}
	}

	void clearSynth(){
		if(synth != nullptr){
			synth->free();
			delete synth;
			synth = nullptr;
		}
		if(valueBus != nullptr){
			valueBus->free();
			delete valueBus;
			valueBus = nullptr;
		}
	}

	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<vector<float>> values;

	ofxSCBus* valueBus;
	ofxSCSynth* synth;
	vector<serverManager*> servers;
};

#endif /* scA2k_h */
