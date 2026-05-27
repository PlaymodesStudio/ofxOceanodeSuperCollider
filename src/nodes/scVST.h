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
#include "ofxOsc.h"
#include <array>
#include <mutex>
#include <set>
#include <map>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

class ofxSCSynth;
class ofxSCServer;

// Simple structure to hold VST parameter information
struct VSTParameterInfo {
	int index;
	std::string displayName;  // User-editable name
	std::string originalName; // Original VST parameter name
	std::string registeredName; // Actual name registered in Oceanode parameter group
	float value;
	bool isConnected;  // Track if parameter has external connections
	bool isPersistent; // Track if parameter should persist across preset loads
	
	VSTParameterInfo() : index(-1), value(0.0f), isConnected(false), isPersistent(true) {}
	VSTParameterInfo(int idx, const std::string& name)
		: index(idx), displayName(name), originalName(name), registeredName(name), value(0.0f), isConnected(false), isPersistent(true) {}
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
			// Persist the last known FXP cache before tearing the node down.
			if(fxpCacheValid && !cachedFXP.empty()) {
				saveCacheToGlobal();
			}
			
			// Clear pending preset data before the backing JSON starts destructing.
			hasPendingPresetData = false;
			try {
				pendingPresetData.clear();
			} catch(...) {
				// Ignore JSON clearing errors during destruction
			}
			
			parameterTimerActive = false;
			
			// Listeners go first so callbacks cannot fire during cleanup.
			try {
				listeners.unsubscribeAll();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error unsubscribing listeners: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error unsubscribing listeners";
			}
			
			// Remove GUI-owned parameters before freeing synth instances.
			try {
				removeAllDynamicParameters();
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error removing dynamic parameters: " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error removing dynamic parameters";
			}
			
			try {
				freeAll();
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error freeing all VST instances: " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error freeing all VST instances";
			}
			
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
	
    void activate() override;
    
    void deactivate() override;

	// Preset lifecycle methods - declared as overrides, implemented in .cpp
	void presetWillBeLoaded() override;
	void activateConnections() override;
	void presetHasLoaded() override;
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	void loadBeforeConnections(ofJson &json);
	
	// Node creation and graph placement
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void moveSynthBefore(ofxSCServer* server, int nodeID);

	void free(ofxSCServer* server);
	void freeAll();
	
	void setOutputBus(ofxSCServer* server, int index, int bus);
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;

	int getOutputBusIndex(ofxSCServer* server, int index);
	int getLastSynthID(ofxSCServer* server);

	ofEvent<void> resendParams;
	
protected:
	
private:
	enum class FXPWriteOperation {
		None,
		PresetSave,
		CacheSave,
		UserExport,
		SyncSave
	};

	enum class FXPReadOperation {
		None,
		SyncLoad
	};

	
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
	void rebuildInstanceLookupCache();
	void clearInstanceLookupCache();
	
	// VST OSC message handlers
	void handleVSTParam(ofxOscMessage& msg);
	void handleVSTAuto(ofxOscMessage& msg);
	void handleVSTOpen(ofxOscMessage& msg);
	
	
	void setVSTProgram(int programIndex);
	void sendMidiProgramChange(int channel, int program, ofxSCServer* server, ofxSCSynth* synth);
	
	// MIDI handling
	void processGates(vector<int>& gates);
	void sendMidiNoteOn(int channel, int pitch, int velocity, int instanceIndex = -1);
	void sendMidiNoteOff(int channel, int pitch, int instanceIndex = -1);
	void sendMidiToInstance(ofxSCServer* server, ofxSCSynth* synth, int channel, int status, int data1, int data2);
	void sendPitchBend(float value);
	void sendModWheel(float value);
		
	
	// Dynamic parameter lifecycle
	void removeAllDynamicParameters();
	void removeAllDynamicParametersForce(); // Force removal even during preset loading
	void clearParameterMaps(); // Clear all parameter tracking maps
	void validateParameterConsistency(); // Validate parameter state consistency
	
	// Dynamic parameter feedback and propagation
	void handleDynamicParameterChange(int paramIndex, const vector<float>& values);
	void propagateParameterToOtherInstances(int sourceNodeID, int paramIndex, float value);
	void propagateFirstInstanceToAll();
	void handleInstanceAwareParameterChange(int paramIndex, const vector<float>& values);
	bool shouldPropagateFromVSTGUI(int paramIndex, int sourceNodeID);
	void applyGUIParameterValueFromVST(int paramIndex, float value, int sourceNodeID);
	void updateParameterValueFromVST(int paramIndex, float value, int sourceNodeID);
	void queueGUIParameterUpdate(int paramIndex, float value, int sourceNodeID);
	void flushPendingGUIParameterUpdates();
	bool isFeedbackSuppressed(int paramIndex, uint64_t currentTime) const;
	void suppressFeedbackFor(int paramIndex, uint64_t untilTime);
	void clearAllFeedbackSuppression();
	void cacheParameterInfoSlot(int paramIndex);
	void clearParameterInfoSlot(int paramIndex);
	// Fast-slot helpers fall back to the canonical maps when a cache entry is stale.
	void cacheDynamicScalarParameterSlot(int paramIndex, const shared_ptr<ofxOceanodeParameter<float>>& param);
	void cacheDynamicVectorParameterSlot(int paramIndex, const shared_ptr<ofxOceanodeParameter<vector<float>>>& param);
	void clearDynamicParameterSlots(int paramIndex);
	shared_ptr<ofxOceanodeParameter<float>> resolveDynamicScalarParameterSlot(int paramIndex);
	shared_ptr<ofxOceanodeParameter<vector<float>>> resolveDynamicVectorParameterSlot(int paramIndex);
	bool isParameterPublished(int paramIndex);
	void cacheSavedParameterNameSlot(int paramIndex, const std::string& name);
	void clearSavedParameterNameSlot(int paramIndex);
	void clearAllParameterSlotCaches();
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
	bool lastFXPWriteSucceeded;
	FXPWriteOperation activeFXPWriteOperation;
	FXPReadOperation activeFXPReadOperation;
	int activeFXPWriteNodeID;
	std::string activeFXPWritePath;
	std::string activeFXPWriteNodeKey;
	uint64_t activeFXPWriteStartTime;
	static const uint64_t FXP_WRITE_TIMEOUT_MS = 10000;

	void applyFXPToInstance(int nodeID);
	void handleVSTPresetWrite(ofxOscMessage& msg);
	void handleVSTPresetRead(ofxOscMessage& msg);
	std::string createTempFXPPath();
	void cleanupTempFXPFile();
	void clearActiveFXPWriteState();
	void resetSyncFXPState(bool clearLoadingFlag);
	
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
	
	// FXP sync helpers
	void handleVSTUpdate(ofxOscMessage& msg);
	void syncFirstInstanceToAllViaFXP(int sourceNodeID);
	void applySyncFXPToAllOtherInstances();
	void queryVSTParametersAfterSync();
	std::string createTempSyncFXPPath();
	void cleanupTempSyncFXPFile();

	std::string tempSyncFXPPath;
	bool waitingForSyncFXPSave;
	std::set<int> syncFXPAppliedInstances;
	int syncSourceNodeID;
	bool syncInProgress;
	std::set<int> instancesBeingSynced;

	void checkAndConvertVectorToScalar(int paramIndex);
	void convertVectorParameterToScalar(int paramIndex, float scalarValue);
	std::map<int, bool> parameterHasVectorConnection;
	
	void saveFXPToUserChosenPath();
	ofParameter<void> saveFXPToDisk;
	
	// Cached FXP snapshots used for preset save and graph re-evaluation.
	std::vector<uint8_t> cachedFXP;
	bool fxpCacheValid;
	uint64_t fxpCacheScheduledTime;
	bool fxpCacheScheduled;
	 
	// Per-plugin cache keeps the last known good state when instances are rebuilt.
	std::map<std::string, std::vector<uint8_t>> cachedFXPs;
	std::atomic<bool> fxpCacheSaveInProgress;
	std::atomic<bool> fxpCacheSavePending;
	
	// State used to choose between preset FXP data and the live cache.
	bool oceanodePresetLoading;
	bool vstStateModifiedSincePreset;
	uint64_t lastParameterChangeTime;
	uint64_t parameterDebounceDelay;
	bool parameterCacheScheduled;
	
	// FXP recall temporarily switches parameter feedback to the immediate path.
	std::atomic<bool> isFXPLoading;
	uint64_t fxpLoadStartTime;
	static const uint64_t FXP_LOAD_TIMEOUT_MS = 10000;
	
	void scheduleImmediateFXPCache();
	void scheduleDebouncedFXPCache(int delayMs = 5000);
	void updateFXPCacheIfNeeded();
	void saveFXPToCache();
		
	void scheduleParameterDebouncedCache();
	void updateParameterDebouncedCacheIfNeeded();
	bool shouldUsePresetFXP() const;
	bool shouldUseCachedFXP() const;
	void resetVSTModificationTracking();
		
	std::string getPluginCacheKey() const { return currentPluginPath; }

	// Global cache preserves the last state for each node key across instance rebuilds.
	static std::map<std::string, std::vector<uint8_t>> globalFXPCache;
	static std::map<std::string, bool> globalFXPCacheValid;
	static std::mutex globalCacheMutex;

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
	int oldNumChannels = 0;
	
	// Multi-instance VST support - each server can have multiple VST instances
	struct InstanceSendTarget {
		ofxSCServer* server;
		ofxSCSynth* synth;
	};
	std::map<ofxSCServer*, std::vector<ofxSCSynth*>> synthInstances;
	std::unordered_set<int> ownedNodeIDs;
	std::unordered_set<int> firstInstanceNodeIDs;
	std::unordered_map<int, int> nodeIDToInstanceIndex;
	std::vector<InstanceSendTarget> activeInstanceTargets;
	
	// Core parameters
	ofParameter<int> numChannels;
	ofParameter<int> pluginSelector;
	ofParameter<float> mix;
	
	// VST management
	ofParameter<void> openEditor;
	ofParameter<void> addLastTouched;
	ofParameter<int> vstProgram;
	ofParameter<void> propagateParams;
	
	// MIDI parameters (integrated from scVSTI)
	ofParameter<vector<int>> gate;
	ofParameter<vector<float>> pitch;
	ofParameter<vector<float>> velocity;
	ofParameter<float> pitchBend;
	ofParameter<float> modWheel;
	ofParameter<int> midiChannel;
	ofParameter<vector<int>> instance;
	ofParameter<bool> singleInstance;   // one plugin instance handles all channels

	
	// Inspector parameters
	ofParameter<bool> enableMultithreading;
	ofParameter<bool> monoInstancing;
	ofParameter<void> removeAllParams;
	
	// Plugin discovery
	vector<string> availablePlugins;
	vector<string> pluginPaths;
	string currentPluginPath;
	
	// Parameter management
	std::map<int, VSTParameterInfo> parameterInfoMap;
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> dynamicParameters;           // Scalar parameters
	std::map<int, shared_ptr<ofxOceanodeParameter<vector<float>>>> dynamicVectorParameters;
	std::map<int, shared_ptr<ofParameter<float>>> dynamicFloatParameters; // Keep parameters alive
	std::map<int, shared_ptr<ofParameter<vector<float>>>> dynamicVectorFloatParameters; // Keep parameters alive
	std::map<int, shared_ptr<ofParameter<string>>> dynamicStringParameters; // Keep name editors alive
	std::map<int, shared_ptr<ofParameter<void>>> dynamicRemovalButtons; // Remove parameter buttons
	
	// Store saved parameter names during preset loading to restore after VST queries
	std::map<int, std::string> savedParameterNames;
	std::array<VSTParameterInfo*, 1024> parameterInfoSlots;
	std::array<shared_ptr<ofxOceanodeParameter<float>>, 1024> dynamicScalarParameterSlots;
	std::array<shared_ptr<ofxOceanodeParameter<vector<float>>>, 1024> dynamicVectorParameterSlots;
	std::array<std::string, 1024> savedParameterNameSlots;
	std::array<uint8_t, 1024> savedParameterNamePresent;
	
	std::array<std::atomic<uint64_t>, 1024> feedbackSuppressUntil;
	
	// Last touched parameter tracking
	int lastTouchedIndex;
	uint64_t lastTouchedTime;  // Track when parameter was last touched for better filtering
	
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
	
	// MIDI output batching
	std::atomic<bool> midiOutputDirty;
	uint64_t lastMidiUpdateTime;
	std::mutex midiUpdateMutex;
	void updateMidiOutputs();

	// Update scheduling
	uint64_t lastMaintenanceTime;
	uint64_t maintenanceIntervalMs;
	uint64_t lastParamThrottleCleanup;
	uint64_t paramThrottleCleanupInterval;

	// Hot-path parameter feedback state
	std::atomic<uint64_t> parameterUpdateGeneration[1024];
	std::atomic<bool> parameterDirty[1024];
	static const uint64_t PARAM_UPDATE_THROTTLE_MS = 16;
	
	// Batched GUI mirroring of VST feedback
	static const uint64_t BATCH_PROCESS_INTERVAL_MS = 8;
	uint64_t lastBatchProcessTime;
	std::array<std::atomic<float>, 1024> latestParameterValues;
	std::array<std::atomic<int>, 1024> latestParameterNodeIDs;
	std::array<float, 1024> pendingGUIValues;
	std::array<int, 1024> pendingGUINodeIDs;
	std::array<uint8_t, 1024> pendingGUISeen;
	std::vector<int> pendingGUIParamIndices;
	
	// Internal batch helpers
	void processPendingParameterUpdates();

};

#endif /* scVST_h */
