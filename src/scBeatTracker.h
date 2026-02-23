#ifndef scBeatTracker_h
#define scBeatTracker_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scBeatTracker : public ofxOceanodeNodeModel {
public:
	scBeatTracker(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC BeatTracker") {
		servers = outputServers;
		synth = nullptr;
		quarterBus = nullptr;
		eighthBus = nullptr;
		sixteenthBus = nullptr;
		tempoBus = nullptr;
	}

	~scBeatTracker() {
		clearSynth();
	}

	void setup(){
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size() - 1));
		addParameter(numChannels.set("N Chan", 1, 1, 100));

		addOutputParameter(quarter.set("Quarter", {0}, {0}, {1}));
		addOutputParameter(eighth.set("Eighth", {0}, {0}, {1}));
		addOutputParameter(sixteenth.set("Sixteenth", {0}, {0}, {1}));
		addOutputParameter(tempo.set("Tempo", {0}, {0}, {300}));

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
			const auto& rawQuarter = quarterBus->readValues;
			const auto& rawEighth = eighthBus->readValues;
			const auto& rawSixteenth = sixteenthBus->readValues;
			const auto& newTempo = tempoBus->readValues;

			vector<int> intQuarter, intEighth, intSixteenth;
			intQuarter.reserve(rawQuarter.size());
			intEighth.reserve(rawEighth.size());
			intSixteenth.reserve(rawSixteenth.size());

			for(size_t i = 0; i < rawQuarter.size(); ++i){
				intQuarter.push_back(rawQuarter[i] > 0.1f ? 1 : 0);
				intEighth.push_back(rawEighth[i] > 0.1f ? 1 : 0);
				intSixteenth.push_back(rawSixteenth[i] > 0.1f ? 1 : 0);
			}

			if(intQuarter != quarter.get()) quarter = intQuarter;
			if(intEighth != eighth.get()) eighth = intEighth;
			if(intSixteenth != sixteenth.get()) sixteenth = intSixteenth;
			if(newTempo != tempo.get()) tempo = newTempo;

			quarterBus->requestValues();
			eighthBus->requestValues();
			sixteenthBus->requestValues();
			tempoBus->requestValues();
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
		clearSynth();
		if(input->getNodeRef() != nullptr){
			string defName = "beatTracker" + ofToString(numChannels);
			synth = new ofxSCSynth(defName, servers[serverIndex]->getServer());
			synth->addToTail();
			synth->run(getActive());

			quarterBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());
			eighthBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());
			sixteenthBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());
			tempoBus = new ofxSCBus(RATE_CONTROL, numChannels, servers[serverIndex]->getServer());

			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
			synth->set("quarter", quarterBus->index);
			synth->set("eighth", eighthBus->index);
			synth->set("sixteenth", sixteenthBus->index);
			synth->set("tempo", tempoBus->index);
		}
	}

	void clearSynth(){
		if(synth != nullptr){
			synth->free();
			delete synth;
			synth = nullptr;
		}
		if(quarterBus != nullptr){
			quarterBus->free();
			delete quarterBus;
			quarterBus = nullptr;
		}
		if(eighthBus != nullptr){
			eighthBus->free();
			delete eighthBus;
			eighthBus = nullptr;
		}
		if(sixteenthBus != nullptr){
			sixteenthBus->free();
			delete sixteenthBus;
			sixteenthBus = nullptr;
		}
		if(tempoBus != nullptr){
			tempoBus->free();
			delete tempoBus;
			tempoBus = nullptr;
		}
	}

	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;

	ofParameter<vector<int>> quarter;
	ofParameter<vector<int>> eighth;
	ofParameter<vector<int>> sixteenth;
	ofParameter<vector<float>> tempo;

	ofxSCBus* quarterBus;
	ofxSCBus* eighthBus;
	ofxSCBus* sixteenthBus;
	ofxSCBus* tempoBus;
	ofxSCSynth* synth;
	vector<serverManager*> servers;
};

#endif /* scBeatTracker_h */
