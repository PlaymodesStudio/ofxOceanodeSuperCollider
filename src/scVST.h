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
		ofLogNotice("scVST") << "Starting scVST destructor";
		
		try {
			// Clear pending preset data first to avoid JSON destruction issues
			hasPendingPresetData = false;
			try {
				pendingPresetData.clear();
			} catch(...) {
				// Ignore JSON clearing errors during destruction
			}
			
			// Stop the parameter timer
			parameterTimerActive = false;
			
			// Clear all event listeners FIRST to prevent callbacks after destruction
			try {
				listeners.unsubscribeAll();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error unsubscribing listeners: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error unsubscribing listeners";
			}
			
			// Remove all dynamic parameters from GUI before freeing synths
			try {
				removeAllDynamicParameters();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error removing dynamic parameters: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error removing dynamic parameters";
			}
			
			// Free all VST instances
			try {
				freeAll();
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error freeing all VST instances: " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error freeing all VST instances";
			}
			
			// Clear all maps
			try {
				parameterInfoMap.clear();
				dynamicParameters.clear();
				dynamicFloatParameters.clear();
				dynamicStringParameters.clear();
				dynamicRemovalButtons.clear();
				outputBuses.clear();
				inputBuses.clear();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error clearing maps: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error clearing maps";
			}
			
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error in scVST destructor: " << e.what();
		} catch(...) {
			ofLogError("scVST") << "Unknown error in scVST destructor";
		}
		
		ofLogNotice("scVST") << "Finished scVST destructor";
	}
	
	void setup();
	
	void presetWillBeLoaded(){
		isPresetLoading = true;
	}
	
	void activateConnections(){
		isPresetLoading = false;
	}
	
	void presetHasLoaded(){
		isPresetLoading = false;
		restoreAllParameterValues();
	}
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	
	void activate(){}
	void deactivate(){}
	
	// Eduard's two-phase approach
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void free(ofxSCServer* server);
	void freeAll();
	
	void setOutputBus(ofxSCServer* server, int index, int bus);
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	
	int getOutputBusIndex(ofxSCServer* server, int index);
	
	ofEvent<void> resendParams;
	
protected:
	// Handle VST-specific OSC messages - using Eduard's feedbackListener pattern
	void feedbackListener(ofxOscMessage& msg);
	
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
	
	// VST OSC message handlers
	void handleVSTParam(ofxOscMessage& msg);
	void handleVSTAuto(ofxOscMessage& msg);
	void handleVSTOpen(ofxOscMessage& msg);
	
	// Parameter querying
	void queryAllVSTParameters();
	void queryAllVSTParametersSync(); // Comprehensive synchronous query
	void captureCurrentVSTState(); // Manual state capture
	void handleParameterQueryResponse(ofxOscMessage& msg);
	
	// VST program data management (more reliable than individual parameters)
	void saveCurrentVSTProgram(ofJson &json);
	void restoreVSTProgram(ofJson &json);
	
	void setVSTProgram(int programIndex);
	void queryVSTPrograms(); // Optional, for program name reference
	void handleVSTProgram(ofxOscMessage& msg); // Optional, for program names
	void sendMidiProgramChange(int channel, int program, ofxSCServer* server, ofxSCSynth* synth);
	
	// MIDI functionality (integrated from scVSTI)
	void processGates(vector<int>& gates);
	void sendMidiNoteOn(int channel, int pitch, int velocity);
	void sendMidiNoteOff(int channel, int pitch);
	
	// Parameter management UI helpers
	void removeAllDynamicParameters();
	void createParameterRemovalButtons();
	
	
	
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
	ofParameter<int> vstProgram;
	
	// MIDI parameters (integrated from scVSTI)
	ofParameter<vector<int>> gate;
	ofParameter<vector<float>> pitch;
	ofParameter<vector<float>> velocity;
	ofParameter<int> midiChannel;
	
	// Inspector parameters
	ofParameter<bool> enableMultithreading;
	ofParameter<void> removeAllParams;
	
	// Plugin discovery
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	// Parameter management
	std::map<int, VSTParameterInfo> parameterInfoMap;
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> dynamicParameters;
	std::map<int, shared_ptr<ofParameter<float>>> dynamicFloatParameters; // Keep parameters alive
	std::map<int, shared_ptr<ofParameter<string>>> dynamicStringParameters; // Keep name editors alive
	std::map<int, shared_ptr<ofParameter<void>>> dynamicRemovalButtons; // Remove parameter buttons
	
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
	
	// MIDI state tracking (from scVSTI)
	vector<int> previousGates;
	vector<int> activeNotes; // Store currently playing notes to send note offs
	
	// Bus management - now handles multiple instances per server
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
};

#endif /* scVST_h */
