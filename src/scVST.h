//
//  scVST.h
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//

#ifndef scVST_h
#define scVST_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class ofxSCSynth;
class ofxSCServer;

class scVST: public scNode {
public:
	scVST();
	~scVST(){
		freeAll();
	}
	
	void setup() override;
	
	void presetWillBeLoaded() override{
		//        isPresetLoading = true;
	}
	
	void activateConnections() override{
		//        isPresetLoading = false;
	}
	
	void presetHasLoaded() override{
		//        isPresetLoading = false;
	}
	
	void activate() override{
		//        synth->run(true);
	}
	
	void deactivate() override{
		//        synth->run(false);
	}
	
	void createSynth(ofxSCServer* server) override;
	void free(ofxSCServer* server) override;
	void freeAll();
	
	
	void setOutputBus(ofxSCServer* server, int index, int bus) override;
	void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
	
	int getOutputBusIndex(ofxSCServer* server, int index) override;
	
	ofEvent<void> resendParams;
	
private:
	void searchForVSTPlugins();
	void loadSelectedPlugin();
	
	ofEventListeners listeners;
	
	std::map<ofxSCServer*, ofxSCSynth*> synths;
	
	ofParameter<int> numChannels;
	ofParameter<int> pluginSelector;
	
	int oldNumChannels;
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
};

#endif /* scVST_h */
