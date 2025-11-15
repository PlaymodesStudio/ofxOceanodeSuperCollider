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
		string nodeKey = "";
		try {
			nodeKey = getParameterGroup().getName();
		} catch(...) {
			nodeKey = "unknown";
		}
		
		ofLogNotice("scVST") << "Starting scVST destructor for node '" << nodeKey << "'";
		
		try {
			// FIRST: Save current cache to global storage before cleanup
			if(fxpCacheValid && !cachedFXP.empty()) {
				saveCacheToGlobal();
			}
			
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

			try {
				midiCCParameters.clear();
				dynamicMidiCCParameters.clear();
				dynamicMidiCCFloatParameters.clear();
				dynamicMidiCCNameParameters.clear();
				dynamicMidiCCRemovalButtons.clear();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error clearing MIDI CC maps: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error clearing MIDI CC maps";
			}
			
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error in scVST destructor: " << e.what();
		} catch(...) {
			ofLogError("scVST") << "Unknown error in scVST destructor";
		}
		
		ofLogNotice("scVST") << "Finished scVST destructor for node '" << nodeKey << "'";
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
	void moveSynthBefore(ofxSCServer* server, int nodeID);  // NEW

	void free(ofxSCServer* server);
	void freeAll();
	
	void setOutputBus(ofxSCServer* server, int index, int bus);
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	
	int getOutputBusIndex(ofxSCServer* server, int index);
	int getLastSynthID(ofxSCServer* server);                // NEW

	ofEvent<void> resendParams;
	
protected:
	
private:
	
	struct MidiCCParameter {
		int ccNumber;
		float value;
		std::string displayName;
		bool enabled;
		
		MidiCCParameter() : ccNumber(0), value(0.0f), displayName(""), enabled(true) {}
		MidiCCParameter(int cc, const std::string& name)
			: ccNumber(cc), value(0.0f), displayName(name), enabled(true) {}
	};
	
	
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
	
	void saveFXPToUserChosenPath();
	ofParameter<void> saveFXPToDisk;
	
	// Periodic FXP caching for state preservation during graph recomputation
	 std::vector<uint8_t> cachedFXP;
	 bool fxpCacheValid;
	 uint64_t fxpCacheScheduledTime;
	 bool fxpCacheScheduled;
	 
	// Per-plugin FXP caching for BOTH preset saving AND graph recomputation
	std::map<std::string, std::vector<uint8_t>> cachedFXPs;        // pluginPath -> FXP data
	
	// NEW: State tracking for smart FXP source selection
	bool oceanodePresetLoading;           // Track if Oceanode preset is currently loading
	bool vstStateModifiedSincePreset;     // Track if VST has been modified since last preset load
	uint64_t lastParameterChangeTime;     // Timestamp of last parameter change for debouncing
	uint64_t parameterDebounceDelay;      // Configurable debounce delay (default 1000ms)
	bool parameterCacheScheduled;         // Track if parameter-triggered cache is scheduled
	
		// Methods for FXP management
		void scheduleImmediateFXPCache();
		void scheduleDebouncedFXPCache(int delayMs = 5000);
		void updateFXPCacheIfNeeded();
		void saveFXPToCache();
		
		// NEW: Enhanced caching methods
		void scheduleParameterDebouncedCache();
		void updateParameterDebouncedCacheIfNeeded();
		bool shouldUsePresetFXP() const;
		bool shouldUseCachedFXP() const;
		void resetVSTModificationTracking();
		
		// Helper methods
		std::string getPluginCacheKey() const { return currentPluginPath; }

	// Add these static members for global FXP caching
	static std::map<std::string, std::vector<uint8_t>> globalFXPCache;  // nodeName -> FXP data
	static std::map<std::string, bool> globalFXPCacheValid;            // nodeName -> validity
	static std::mutex globalCacheMutex;                                // Thread safety

	// Add these helper methods
	std::string getNodeCacheKey();
	void loadCacheFromGlobal();
	void saveCacheToGlobal();
	
	// Transport control parameters
	ofParameter<bool> transportPlay;
	ofParameter<float> transportPosition;
	ofParameter<void> transportReset;
	ofParameter<float> tempo;
	ofParameter<int> timeSignatureNum;
	ofParameter<int> timeSignatureDenom;
	ofParameter<void> queryTransportPos;  // Button to manually query position

	// Transport state management
	bool transportFeedbackSuppressed;
	uint64_t transportFeedbackClearTime;
	float lastKnownPosition;
	bool isTransportQuerying;

	// Transport control methods
	void setTransportPlay(bool playing);
	void setTransportPosition(float position);
	void resetTransport();
	void setTempo(float bpm);
	void setTimeSignature(int num, int denom);
	void queryTransportPosition();
	void handleTransportPosition(ofxOscMessage& msg);
	void sendTransportCommandToAllInstances(const std::string& command, const std::vector<float>& args);



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
	ofParameter<bool> singleInstance;   // one plugin instance handles all channels

	
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
	
	//MIDI CC
	std::map<int, MidiCCParameter> midiCCParameters; // CC number -> parameter info
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> dynamicMidiCCParameters; // CC -> GUI parameter
	std::map<int, shared_ptr<ofParameter<float>>> dynamicMidiCCFloatParameters; // Keep parameters alive
	std::map<int, shared_ptr<ofParameter<string>>> dynamicMidiCCNameParameters; // CC name editors
	std::map<int, shared_ptr<ofParameter<void>>> dynamicMidiCCRemovalButtons; // CC removal buttons
	
	// MIDI CC control parameters
	ofParameter<void> addMidiCC;
	ofParameter<int> midiCCToAdd;
	ofParameter<void> removeAllMidiCC;
	
	// MIDI CC methods (add to private section)
	void addMidiCCParameter(int ccNumber);
	void removeMidiCCParameter(int ccNumber);
	void removeAllMidiCCParameters();
	void sendMidiCC(int ccNumber, float value);
	void addMidiCCNameEditor(int ccNumber, const string& paramName);
	void addMidiCCRemovalButton(int ccNumber, const string& paramName);
	
	void propagateFirstInstanceViaFXP();
	std::string createTempPropagateFXPPath();
	
	// MIDI Output parameters
	ofParameter<vector<float>> noteOut;    // 128 elements: note velocity (0=off, >0=velocity)
	ofParameter<vector<float>> ccOut;      // 128 elements: CC values (0.0-1.0)

	// MIDI output 
	vector<float> currentNoteStates;       // Track current note velocities
	vector<float> currentCCStates;         // Track current CC values
	void handleVSTMidi(ofxOscMessage& msg);
	
	// MIDI output batching for performance
	bool midiOutputDirty;
	uint64_t lastMidiUpdateTime;
	std::mutex midiUpdateMutex;
	void updateMidiOutputs();

};

#endif /* scVST_h */
