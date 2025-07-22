//
//  scBlockDelay.h   – “z~” Block-Delay d’una mostra (àudio)
//
#ifndef scBlockDelay_h
#define scBlockDelay_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scBlockDelay : public ofxOceanodeNodeModel{
public:
	scBlockDelay(vector<serverManager*> servers)
	: ofxOceanodeNodeModel("z~BlockDelay"), servers(servers){
		synth = nullptr;
		outBus = nullptr;
	}

	~scBlockDelay(){ cleanup(); }

	/* -------- parameters -------- */
	void setup() override{
		addParameter(in.set("In", nodePort()),
					 ofxOceanodeParameterFlags_DisableOutConnection);

		addParameter(out.set("Out", nodePort()),
					 ofxOceanodeParameterFlags_DisableInConnection);

		addParameter(numChannels.set("NChan", 1, 1, 8));

		listeners.push(in.newListener([this](nodePort&){ recreate(); }));
		listeners.push(numChannels.newListener([this](int&){ recreate(); }));
	}

	/* mark this node as “history” so the graph can ignore its
	   outgoing edge when checking cycles */
	bool isHistory() override { return true; }

private:
	void recreate(){
		cleanup();
		if(in->getNodeRef() == nullptr) return;

		int chans = numChannels.get();
		int nBuses = chans;

		outBus = new ofxSCBus(RATE_AUDIO, nBuses,
							  servers[0]->getServer());

		// SynthDef name: blockdelay<channels>
		string name = "blockdelay" + ofToString(chans);
		synth = new ofxSCSynth(name, servers[0]->getServer());
		synth->create(1, 1);               // tail of default group
		synth->set("in",  in->getBusIndex(servers[0]->getServer()));
		synth->set("out", outBus->index);

		out = nodePort(*outBus);            // exposa el port sortida
	}

	void cleanup(){
		if(synth){ synth->free(); delete synth; synth = nullptr; }
		if(outBus){ outBus->free(); delete outBus; outBus = nullptr; }
	}

	/* data */
	ofParameter<nodePort> in, out;
	ofParameter<int>      numChannels;
	ofEventListeners      listeners;

	ofxSCSynth*           synth;
	ofxSCBus*             outBus;
	vector<serverManager*> servers;
};

#endif /* scBlockDelay_h */
