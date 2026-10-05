#ifndef scPitchTracker_h
#define scPitchTracker_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "scAnalyzerBinding.h"

class scPitchTracker : public ofxOceanodeNodeModel {
public:
	scPitchTracker(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC PitchTracker") {
		servers = outputServers;
		synth = nullptr;
		freqBus = nullptr;
		confBus = nullptr;
	}

	~scPitchTracker() {
		clearSynth();
	}

	void setup(){
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size() - 1));
		addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
		addOutputParameter(frequencies.set("Frequencies", {0}, {0}, {22000}));
		addOutputParameter(confidences.set("Confidence", {0}, {0}, {1}));

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
				rebindOrRecreateSynth();
			});
		}));

		listeners.push(numChannels.newListener([this](int &i){
			if(input->getNodeRef() != nullptr){
				recreateSynth();
			}
		}));
	}

	void update(ofEventArgs &args) override {
		if(synth != nullptr){
			frequencies = freqBus->readValues;
			confidences = confBus->readValues;
			freqBus->requestValues();
			confBus->requestValues();
		}
	}

	

	void activate() override {
		if(synth) synth->run(true);
	}

	void deactivate() override {
		if(synth) synth->run(false);
	}

private:
	void recreateSynth(){
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
		clearSynth();
		if(input->getNodeRef() != nullptr){
			string defName = "pitchTracker" + ofToString(numChannels);
			synth = new ofxSCSynth(defName, servers[serverIndex]->getServer());
            synth->createAndRun(1, 1, getActive()); //addToTail
			freqBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());
			confBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());

			const int inBus = input->getBusIndex(servers[serverIndex]->getServer());
			synth->set("in", inBus);
			binding.bind(servers[serverIndex], inBus);
			synth->set("freq", freqBus->index);
			synth->set("hasFreq", confBus->index);
		}
	}

	// After a graph rebuild: the synth keeps running and only follows its
	// input to the new bus (see scAnalyzerBinding.h).
	void rebindOrRecreateSynth(){
		if(synth == nullptr || input->getNodeRef() == nullptr || !binding.canRebind(servers[serverIndex])){
			recreateSynth();
			return;
		}
		const int inBus = input->getBusIndex(servers[serverIndex]->getServer());
		if(inBus != binding.inBus()){
			synth->set("in", inBus);
			binding.setInBus(inBus);
		}
	}

	void clearSynth(){
		binding.clear();
		if(synth != nullptr){
			synth->free();
			delete synth;
			synth = nullptr;
		}
		if(freqBus != nullptr){
			freqBus->free();
			delete freqBus;
			freqBus = nullptr;
		}
		if(confBus != nullptr){
			confBus->free();
			delete confBus;
			confBus = nullptr;
		}
	}

	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;

	ofParameter<vector<float>> frequencies;
	ofParameter<vector<float>> confidences;

	ofxSCBus* freqBus;
	ofxSCBus* confBus;
	ofxSCSynth* synth;
	scAnalyzerBinding binding;
	vector<serverManager*> servers;
};

#endif /* scPitchTracker_h */

