#ifndef scFM7Drone_h
#define scFM7Drone_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include <vector>
#include <map>
#include <array>

// Struct to hold a single patch state for the drone
struct FMDronePatch {
	// Matrix
	std::vector<float> matrix;

	// Patch Defaults (scalar, stored as first-element of per-voice vectors)
	float vibFreq  = 5.0f;
	float vibAmp   = 0.0f;
	float tremFreq = 5.0f;
	float tremAmp  = 0.0f;
	float feedback = 0.0f;

	// Per-voice vectors (size n at store time, recalled as-is)
	std::vector<float> masterAmp = {1.0f};

	// Per-operator, per-voice (each array element is a vector of size n)
	std::array<std::vector<float>, 6> opAmps    = {{{1.f},{0.f},{0.f},{0.f},{0.f},{0.f}}};
	std::array<std::vector<float>, 6> opRatios  = {{{1.f},{1.f},{1.f},{1.f},{1.f},{1.f}}};
	std::array<std::vector<float>, 6> opDetunes = {{{0.f},{0.f},{0.f},{0.f},{0.f},{0.f}}};

	bool hasData = false;
};

class scFM7Drone : public scNode {
public:
	scFM7Drone();
	~scFM7Drone();

	void setup() override;

	// scNode Overrides
	void buildSynth(ofxSCServer* server) override;
	void createSynth(ofxSCServer* server) override;
	void free(ofxSCServer* server) override;
	void moveSynthBefore(ofxSCServer* server, int nodeID) override;
	int getLastSynthID(ofxSCServer* server) override;

	void setOutputBus(ofxSCServer* server, int index, int bus) override;
	void update(ofEventArgs& args) override;

	void activate() override;
	void deactivate() override;

	// Persistence
	void presetSave(ofJson &json) override;
	void presetRecallAfterSettingParameters(ofJson &json) override;

private:
	// --- GUI Drawers ---
	void drawModMatrix();
	void drawPresetSlots();

	// --- State & Resources ---
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	ofEventListeners listeners;

	// --- Parameters ---
	ofParameter<int> numChannels;

	// Performance Inputs
	ofParameter<vector<float>> pitch;

	// Per-Voice Modifiers
	ofParameter<vector<float>> modScale;
	ofParameter<vector<float>> vibFreq;
	ofParameter<vector<float>> vibAmp;
	ofParameter<vector<float>> tremFreq;
	ofParameter<vector<float>> tremAmp;

	// Global / Shared
	ofParameter<vector<float>> masterAmp;
	ofParameter<float> feedback;

	// Mod Matrix
	ofParameter<vector<float>> modMatrix;

	// Per-operator, per-voice parameters (6 ops × 3 param types)
	std::array<ofParameter<vector<float>>, 6> opAmps;
	std::array<ofParameter<vector<float>>, 6> opRatios;
	std::array<ofParameter<vector<float>>, 6> opDetunes;

	// --- Internal Preset System ---
	std::vector<FMDronePatch> presetSlots;
	ofParameter<float> morphTime;

	// Morphing State
	bool isMorphing;
	float morphStartTime;
	FMDronePatch startPatch;
	FMDronePatch targetPatch;
	int activePresetSlot;

	// --- GUI Dimensions ---
	ofParameter<float> widgetWidth;
	ofParameter<float> matrixHeight;

	// --- UI Regions ---
	ofParameter<std::function<void()>> uiMatrix;
	ofParameter<std::function<void()>> uiPresets;

	// Interaction State
	int activeMatrixCell;

	// Helpers
	string getSynthDefName() const;
	void updateMatrixParams();
	void updateAllParamsToSynth();

	// Preset Helpers
	void storeToSlot(int slot);
	void recallSlot(int slot);
	void updateMorph();

	// Disk I/O for Presets
	void savePresetToDisk(int slot);
	void loadPresetFromDisk(int slot);
	void loadAllPresetsFromDisk();
	void deletePresetFromDisk(int slot);
	string getPresetsFolderPath();
	string getPresetFilePath(int slot);

	// Param Helpers
	void sendFloatParameter(const string& name, vector<float>& values);
	// SC control name for operator index (e.g. "op_amp_1")
	static string opParamName(const string& prefix, int opIdx);
};

#endif /* scFM7Drone_h */
