//
//  scVSTI.h
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//

#ifndef scVSTI_h
#define scVSTI_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class ofxSCSynth;
class ofxSCServer;

class scVSTI: public scNode {
public:
	scVSTI();
	~scVSTI(){
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
	void processGates(vector<int>& gates);
	void sendMidiNoteOn(int channel, int pitch, int velocity);
	void sendMidiNoteOff(int channel, int pitch);
	
	ofEventListeners listeners;
	
	std::map<ofxSCServer*, ofxSCSynth*> synths;
	
	ofParameter<int> numChannels;
	ofParameter<int> pluginSelector;
	ofParameter<vector<int>> gate;
	ofParameter<vector<float>> pitch;
	ofParameter<vector<float>> velocity;
	ofParameter<int> midiChannel;
	
	int oldNumChannels;
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	// Store previous gate states to detect rising/falling edges
	vector<int> previousGates;
	
	// Store currently playing notes to send note offs
	vector<int> activeNotes;
	
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
};

#endif /* scVSTI_h */
