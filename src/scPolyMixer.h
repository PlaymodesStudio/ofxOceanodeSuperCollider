//
//  scPolyMixer.h
//  ofxOceanodeSupercollider
//
//  Multi-track mixer with dynamic track count and multi-instancing
//

#ifndef scPolyMixer_h
#define scPolyMixer_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include <mutex>
#include <set>
#include <map>
#include <atomic>
#include <thread>
#include <chrono>

class ofxSCSynth;
class ofxSCServer;
class ofxSCBus;

class scPolyMixer: public scNode {
public:
	scPolyMixer();
	~scPolyMixer();
	
	void setup();
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	void loadBeforeConnections(ofJson &json);
	
	// scNode interface methods
	void activate() override;
	void deactivate() override;
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void free(ofxSCServer* server);
	void freeAll();
	
	void setOutputBus(ofxSCServer* server, int index, int bus);
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	
	int getOutputBusIndex(ofxSCServer* server, int index);
	
	ofEvent<void> resendParams;
	void update(ofEventArgs &args) override;
	
	// Custom region method specific to scPolyMixer
	void addCustomRegion(ofParameter<std::function<void()>> p1, ofParameter<std::function<void()>> p2);
	
	void updateAllTracksVUTiming(float attackTime, float releaseTime);

	
private:
	std::vector<int> tracksToAddNextFrame;
	bool needsGUIRebuild = false;
	bool isLoadingPreset = false;
	
	static const bool ENABLE_VU_METERS = true;
	bool vuMetersReady = false;
	
	std::map<ofxSCServer*, std::vector<ofxSCBus*>> vuBuses;
	std::map<int, ofParameter<vector<float>>> vuParameters; // Direct parameters, not shared_ptr

	
	// Multi-instance track management
	std::map<ofxSCServer*, std::vector<ofxSCSynth*>> trackInstances;
	std::map<ofxSCServer*, std::vector<ofxSCBus*>> vuMeterBuses;
	
	// Track customization
	std::map<int, std::shared_ptr<ofParameter<string>>> trackNameParams;
	std::map<int, std::shared_ptr<ofParameter<ofColor>>> trackColorParams;
	void drawTrackSeparator(int trackIndex);
	
	
	// Core parameters
	ofParameter<int> numTracks;
	ofParameter<int> numChannels;
	ofParameter<vector<float>> masterLevel;
	ofParameter<vector<float>> gainVec;  // Renamed from levelVec - linear amp multipliers
	ofParameter<vector<float>> balanceVec;  // NEW: Balance vector for all tracks (-1 to 1)
	
	// Master output VU meter
	ofParameter<vector<float>> masterVUMeter;
	
	// Master VU Data output parameter
	shared_ptr<ofxOceanodeParameter<vector<float>>> masterVUData;
	
	// VU meter peak tracking
	std::map<int, vector<float>> trackPeakLevels;
	std::map<int, vector<float>> trackPeakDecayTimers;
	vector<float> masterPeakLevels;
	vector<float> masterPeakDecayTimers;
	std::unordered_map<int, bool> trackHasInput;

	
	// VU meter timing controls (per track)
	ofParameter<float> trackVUHeight;
	ofParameter<float> masterVUHeight;
	
	// Master VU meter timing controls
	ofParameter<float> masterVUAttack;
	ofParameter<float> masterVURelease;
	ofParameter<bool> drawVU;

	// Dynamic track parameters - using maps for proper management
	std::map<int, shared_ptr<ofxOceanodeParameter<vector<float>>>> trackLevels;
	std::map<int, shared_ptr<ofxOceanodeParameter<float>>> trackBalances; // NEW: Per-track balance
	std::map<int, shared_ptr<ofxOceanodeParameter<vector<float>>>> trackVUMeters;
	std::map<int, shared_ptr<ofxOceanodeParameter<vector<float>>>> trackVUData; // Output VU data for each track

	// Keep parameters alive
	std::map<int, shared_ptr<ofParameter<vector<float>>>> trackLevelParams;
	std::map<int, shared_ptr<ofParameter<float>>> trackBalanceParams; // NEW: Per-track balance params
	std::map<int, shared_ptr<ofParameter<bool>>> trackMuteParams;
	std::map<int, shared_ptr<ofParameter<bool>>> trackSoloParams;
	std::map<int, shared_ptr<ofParameter<vector<float>>>> trackVUMeterParams;
	
	// Track input management
	std::map<int, int> trackInputIndices; // Maps track index to scNode input index
	
	// Instance management
	int calculateNumTrackInstances() const;
	void createTrackInstances(ofxSCServer* server);
	void freeTrackInstances(ofxSCServer* server);
	void createVUMeterBuses(ofxSCServer* server);
	void freeVUMeterBuses(ofxSCServer* server);
	void recreateVUBuses(ofxSCServer* server);
	void freeVUBuses(ofxSCServer* server);
	
	// Dynamic GUI management
	void updateTrackCount();
	void addTrackToGUI(int trackIndex);
	void removeTrackFromGUI(int trackIndex);
	void removeAllTrackParameters();
	void removeAllInputParameters();
	
	// Audio parameter updates
	void updateTrackInstanceLevel(int trackIndex, const vector<float>& levels);
	void updateTrackInstanceBalance(int trackIndex, float balance); // NEW
	void updateTrackInstanceMute(int trackIndex, bool muted);
	void updateTrackInstanceSolo(int trackIndex, bool soloed);
	void updateMasterLevel(const vector<float>& levels);
	void updateSoloLogic();
	void updateTrackVUTiming(int trackIndex, float attackTime, float releaseTime);
	
	// VU meter management
	void updateVUMeters();
	void startVUMeterUpdates();
	void stopVUMeterUpdates();
	
	// Update method for continuous VU meter updates
	void update();
	
	void drawCompactTrackWidget(int trackIndex);
	void drawMasterVUWidget();
	unsigned int getVUMeterColor(float level);
	unsigned int getVUMeterColorDB(float dbLevel);
	
	// dB conversion utilities
	static float ampToDb(float amp);
	static float dbToAmp(float db);
	static float dbToVUPosition(float db, float minDb = -60.0f, float maxDb = 6.0f);
	static float vuPositionToDb(float position, float minDb = -60.0f, float maxDb = 6.0f);
	static float normalizedToDb(float normalized); // Convert 0-1 range to -60dB to +6dB
	static float dbToNormalized(float db); // Convert -60dB to +6dB to 0-1 range

	
	// Bus management
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	
	// State management
	bool isUpdatingTracks;
	std::set<int> soloedTracks;
	
	// Event listeners
	ofEventListeners listeners;
	
	// Helper methods
	string getSynthDefName() const;
	void drawTrackSeparator();
	static void drawSeparator();
	
	// Parameter synchronization
	void syncGainVecToTrackLevels();
	void syncTrackLevelsToGainVec();
	void syncBalanceVecToTrackBalances(); // NEW
	void syncTrackBalancesToBalanceVec(); // NEW
	bool isTrackConnected(ofxSCServer* server, int trackIndex) const;

	// VU meter bus management - missing from original implementation
	std::map<ofxSCServer*, std::vector<ofxSCBus*>> trackVUBuses;
	ofxSCBus* masterVUBus = nullptr;
	
	   
	void safeSetInputBus(ofxSCServer* server, scNode* node, int bus);
	void disableAllVUMeters(ofxSCServer* server);
	void enableAllVUMeters(ofxSCServer* server);
	void restoreTrackParameters(ofxSCServer* server, int trackIndex);
	void resetInputBusses(ofxSCServer* server) override;
	
	// Graph recomputation support
	void moveSynthBefore(ofxSCServer* server, int nodeID) override;
	int getLastSynthID(ofxSCServer* server) override;


};

#endif /* scPolyMixer_h */
