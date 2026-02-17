#ifndef scFM7_h
#define scFM7_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include <vector>
#include <map>

// Struct to hold a single patch state
struct FMPatch {
	// Envelope Data
	std::vector<float> egLevels;
	std::vector<float> egTimes;
	
	// Operator Data
	std::vector<float> opAmps;
	std::vector<float> opRatios;
	std::vector<float> opDetunes;
	
	// Matrix
	std::vector<float> matrix;
	
	// Patch Defaults (Single floats applied to vectors on recall)
	float vibFreq = 5.0f;
	float vibAmp = 0.0f;
	float tremFreq = 5.0f;
	float tremAmp = 0.0f;
	
	float masterAmp = 0.5f;
	float feedback = 0.0f;
	float duration = 2.0f;
	
	bool hasData = false;
};

class scFM7 : public scNode {
public:
	scFM7();
	~scFM7();

	void setup() override;
	
	// scNode Overrides
	void buildSynth(ofxSCServer* server) override;
	void createSynth(ofxSCServer* server) override;
	void free(ofxSCServer* server) override;
	// Graph ordering support
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
	void drawEnvelopeEditor();
	void drawPresetSlots();

	// --- State & Resources ---
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	ofEventListeners listeners;

	// --- Parameters ---
	ofParameter<int> numChannels;

	// Performance Inputs (Vectors)
	ofParameter<vector<float>> pitch;
	ofParameter<vector<int>> gate;
	ofParameter<vector<float>> levels; // Velocity
	
	// New Per-Voice Modifiers
	ofParameter<vector<float>> modScale;
	
	ofParameter<vector<float>> vibFreq;
	ofParameter<vector<float>> vibAmp;
	
	ofParameter<vector<float>> tremFreq;
	ofParameter<vector<float>> tremAmp;

	// Global / Shared
	ofParameter<float> masterAmp;
	ofParameter<float> feedback;
	
	// Mod Matrix (Now a parameter for Input/Output)
	ofParameter<vector<float>> modMatrix;

	// Operators
	ofParameter<vector<float>> opAmps;
	ofParameter<vector<float>> opRatios;
	ofParameter<vector<float>> opDetunes;
	
	// Envelope Data
	ofParameter<int> selectedOp;
	ofParameter<float> totalDuration;
	
	// Internal Backing Params
	ofParameter<vector<float>> currentEgLevels;
	ofParameter<vector<float>> currentEgTimes;
	
	// --- Internal Preset System ---
	std::vector<FMPatch> presetSlots;
	ofParameter<float> morphTime;
	
	// Morphing State
	bool isMorphing;
	float morphStartTime;
	FMPatch startPatch;
	FMPatch targetPatch;
	int activePresetSlot;
	
	// --- GUI Dimensions ---
	ofParameter<float> widgetWidth;
	ofParameter<float> matrixHeight;
	ofParameter<float> envelopeHeight;
	
	// --- UI Regions ---
	ofParameter<std::function<void()>> uiMatrix;
	ofParameter<std::function<void()>> uiEnvelope;
	ofParameter<std::function<void()>> uiPresets;

	// Internal State
	vector<float> allEgLevels;
	vector<float> allEgTimes;
	vector<float> derivedRates;
	
	// Interaction State
	int activeMatrixCell;
	int draggingPoint;
	
	// Helpers
	string getSynthDefName() const;
	void updateEnvelopeParams();
	void updateMatrixParams();
	void updateAllParamsToSynth();
	void refreshEnvelopeGUI();
	
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
	void sendIntParameter(const string& name, vector<int>& values);
};

#endif /* scFM7_h */
