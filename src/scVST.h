//
//  scVST.h
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//  Extended by Santi Vilanova on 25/7/25.
//

#ifndef scVST_h
#define scVST_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include <mutex>
#include <set>
#include <map>

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
	
	// Preset lifecycle methods - declared as overrides, implemented in .cpp
	void presetWillBeLoaded() override;
	void activateConnections() override;
	void presetHasLoaded() override;
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	void loadBeforeConnections(ofJson &json);
	
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
	
private:
	
	
	void searchForVSTPlugins();
	void loadSelectedPlugin();
	void addParameterToGUI(int paramIndex);
	void removeParameterFromGUI(int paramIndex);
	void updateParameterValue(int paramIndex, float value);
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
	
	
	void setVSTProgram(int programIndex);
	void sendMidiProgramChange(int channel, int program, ofxSCServer* server, ofxSCSynth* synth);
	
	// MIDI functionality (integrated from scVSTI)
	void processGates(vector<int>& gates);
	void sendMidiNoteOn(int channel, int pitch, int velocity, int instanceIndex = -1);  // NEW: Instance routing
	void sendMidiNoteOff(int channel, int pitch, int instanceIndex = -1);              // NEW: Instance routing
	void sendMidiToInstance(ofxSCServer* server, ofxSCSynth* synth, int channel, int status, int data1, int data2);
		
	
	// Parameter management UI helpers
	void removeAllDynamicParameters();
	
	// NEW: Dynamic parameter vector handling
	void handleDynamicParameterChange(int paramIndex, const vector<float>& values);
	void propagateParameterToOtherInstances(int sourceNodeID, int paramIndex, float value);
	void propagateFirstInstanceToAll();
	void handleInstanceAwareParameterChange(int paramIndex, const vector<float>& values);
	bool shouldPropagateFromVSTGUI(int paramIndex, int sourceNodeID);
	void updateParameterValueFromVST(int paramIndex, float value, int sourceNodeID);
	int getInstanceIndexFromNodeID(int nodeID);
		
	// Restore methods
	void queryVSTParametersAfterUpdate(int nodeID);
	
	// VST readiness tracking
	std::set<int> readyInstances;
	bool waitForAllInstancesReady(int timeoutMs = 10000);
	bool areAllInstancesReady();
	
	// FXP preset management
	std::vector<uint8_t> savedFXPData;  // Store FXP data as binary
	bool hasSavedFXPData;
	std::set<int> fxpAppliedInstances;   // Track which instances have received FXP data
	std::string tempFXPPath;             // Temporary file path for FXP operations
	bool waitingForFXPSave;              // Flag to track if we're waiting for FXP save completion
	bool waitingForFXPLoad;              // Flag to track if we're waiting for FXP load completion

	void applyFXPToInstance(int nodeID);
	void handleVSTPresetWrite(ofxOscMessage& msg);
	void handleVSTPresetRead(ofxOscMessage& msg);
	std::string createTempFXPPath();
	void cleanupTempFXPFile();
	
	// Base64 encoding/decoding helpers
	std::string base64Encode(const std::vector<uint8_t>& data);
	std::vector<uint8_t> base64Decode(const std::string& encoded);
	
	void createGUIParameterWithValues(int paramIndex, const string& paramName, const vector<float>& values);
	void addParameterNameEditor(int paramIndex, const string& paramName);
	void addParameterRemovalButton(int paramIndex, const string& paramName);
	void syncGUIParametersToVST(ofJson &json);
	void setVSTParameterDirectToAll(int paramIndex, float value);
	void setVSTParameterVectorDirectToAll(int paramIndex, const vector<float>& values);
	void activateParameterBindings();
	
	// Add these method declarations to scVST.h (in the private section)
	void handleVSTUpdate(ofxOscMessage& msg);
	void syncFirstInstanceToAllViaFXP(int sourceNodeID);
	void applySyncFXPToAllOtherInstances();
	void queryVSTParametersAfterSync();
	std::string createTempSyncFXPPath();
	void cleanupTempSyncFXPFile();

	// Add these member variables to scVST.h (private section)
	std::string tempSyncFXPPath;
	bool waitingForSyncFXPSave;
	std::set<int> syncFXPAppliedInstances;
	int syncSourceNodeID; // Track which instance we're syncing from
	bool syncInProgress; // CRITICAL: Prevent feedback loops
	std::set<int> instancesBeingSynced; // Track which instances are receiving FXP

	void checkAndConvertVectorToScalar(int paramIndex);
	void convertVectorParameterToScalar(int paramIndex, float scalarValue);
	std::map<int, bool> parameterHasVectorConnection;



	static void drawSeparator();

	
	ofEventListeners listeners;
	
	// Multi-instance VST support - each server can have multiple VST instances
	std::map<ofxSCServer*, std::vector<ofxSCSynth*>> synthInstances;
	
	// Core parameters
	ofParameter<int> numChannels;
	ofParameter<int> pluginSelector;
	
	// VST management
	ofParameter<void> openEditor;
	ofParameter<void> addLastTouched;
	ofParameter<int> vstProgram;
	ofParameter<void> propagateParams;
	
	// MIDI parameters (integrated from scVSTI)
	ofParameter<vector<int>> gate;
	ofParameter<vector<float>> pitch;
	ofParameter<vector<float>> velocity;
	ofParameter<int> midiChannel;
	ofParameter<vector<int>> instance;  // NEW: Instance routing parameter
	
	// Inspector parameters
	ofParameter<bool> enableMultithreading;
	ofParameter<bool> monoInstancing;   // NEW: Mono/Stereo instancing mode
	ofParameter<void> removeAllParams;
	
	// Plugin discovery
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	// Parameter management
	std::map<int, VSTParameterInfo> parameterInfoMap;
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> dynamicParameters;           // Scalar parameters
	std::map<int, shared_ptr<ofxOceanodeParameter<vector<float>>>> dynamicVectorParameters; // NEW: Vector parameters
	std::map<int, shared_ptr<ofParameter<float>>> dynamicFloatParameters; // Keep parameters alive
	std::map<int, shared_ptr<ofParameter<vector<float>>>> dynamicVectorFloatParameters; // NEW: Keep vector parameters alive
	std::map<int, shared_ptr<ofParameter<string>>> dynamicStringParameters; // Keep name editors alive
	std::map<int, shared_ptr<ofParameter<void>>> dynamicRemovalButtons; // Remove parameter buttons
	
	std::set<int> suppressingFeedback; // Track parameters currently being set to prevent feedback
	std::mutex feedbackMutex;          // Thread safety for feedback prevention
	std::map<int, uint64_t> feedbackClearTimes;
	
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
