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

// Simple structure to hold VST parameter information
struct VSTParameterInfo {
	int index;
	std::string displayName;  // User-editable name
	float value;
	
	VSTParameterInfo() : index(-1), value(0.0f) {}
	VSTParameterInfo(int idx, const std::string& name)
		: index(idx), displayName(name), value(0.0f) {}
};

class scVST: public scNode {
public:
	scVST();
	~scVST(){
		freeAll();
	}
	
	void setup() override;
	
	void presetWillBeLoaded() override{
		isPresetLoading = true;
	}
	
	void activateConnections() override{
		isPresetLoading = false;
	}
	
	void presetHasLoaded() override{
		isPresetLoading = false;
		restoreAllParameterValues();
	}
	
	void presetSave(ofJson &json) override;
	void presetRecallAfterSettingParameters(ofJson &json) override;
	
	void activate() override{}
	void deactivate() override{}
	
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
	void addParameterToGUI(int paramIndex);
	void removeParameterFromGUI(int paramIndex);
	void updateParameterValue(int paramIndex, float value);
	void restoreAllParameterValues();
	void setVSTParameter(int paramIndex, float value);
	void applyPendingPresetData();
	void setupParameterTimer(int delayMs);
	
	// Multi-instance helpers
	int calculateNumInstances() const;
	void createVSTInstances(ofxSCServer* server);
	void freeVSTInstances(ofxSCServer* server);
	bool isMyVSTInstance(int nodeID) const;
	
	ofEventListeners listeners;
	
	// Multi-instance VST support - each server can have multiple VST instances
	std::map<ofxSCServer*, std::vector<ofxSCSynth*>> synthInstances;
	
	// Core parameters
	ofParameter<int> numChannels;
	ofParameter<int> pluginSelector;
	ofParameter<float> mixLevel;
	
	// VST management
	ofParameter<void> openEditor;
	ofParameter<void> addLastTouched;
	
	// Plugin discovery
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	// Parameter management
	std::map<int, VSTParameterInfo> parameterInfoMap;
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> dynamicParameters;
	std::map<int, shared_ptr<ofxOceanodeParameter<string>>> parameterNameEditors;
	
	// Last touched parameter tracking
	int lastTouchedIndex;
	
	// State management
	bool isPresetLoading;
	bool pluginLoaded;
	
	// Preset restoration
	ofJson pendingPresetData;
	bool hasPendingPresetData;
	
	// Timer for delayed parameter application
	uint64_t parameterTimerStart;
	int parameterTimerDelay;
	bool parameterTimerActive;
	
	// Bus management - now handles multiple instances per server
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
};

#endif /* scVST_h */
