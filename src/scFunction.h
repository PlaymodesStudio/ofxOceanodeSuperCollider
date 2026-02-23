#ifndef scFunction_h
#define scFunction_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "imgui.h"

// Structure to hold curve point data
struct CurvePoint {
	glm::vec2 point;
	bool firstCreated;
	
	CurvePoint() : point(0, 0), firstCreated(true) {}
	CurvePoint(float x, float y) : point(x, y), firstCreated(true) {}
};

// Structure to hold segment data between points
struct CurveSegment {
	float tensionExponent;  // steepness parameter (k)
	float inflectionX;      // inflection point (p)
	
	CurveSegment() : tensionExponent(1.0f), inflectionX(0.5f) {}
};

class scFunction: public scNode {
public:
	scFunction();
	~scFunction();
	
	void setup();
	void draw(ofEventArgs &args);
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	
	// scNode interface methods
	void activate() override;
	void deactivate() override;
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void free(ofxSCServer* server);
	void freeAll();
	
	void setOutputBus(ofxSCServer* server, int index, int bus);
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	void resetInputBusses(ofxSCServer* server, int targetBus = 0);
	int getOutputBusIndex(ofxSCServer* server, int index);
	
private:
	static const int MAX_POINTS = 8;
	static constexpr float MIN_TENSION = 0.01f;
	static constexpr float MAX_TENSION = 10.0f;
	
	// Core parameters
	ofParameter<int> numChannels;
	
	// GUI parameters
	ofParameter<bool> showWindow;
	
	// SuperCollider parameters
	ofParameter<vector<float>> pointsX;
	ofParameter<vector<float>> pointsY;
	ofParameter<vector<float>> tensions;
	ofParameter<vector<float>> inflections;
	
	// Synth management
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	
	// Curve data
	vector<CurvePoint> points;
	vector<CurveSegment> segments;
	
	// Basic visual state
	int hoveredPointIndex;
	bool tensionDragActive;
	
	// Helper methods
	void initializeDefaultCurve();
	void sendCurveDataToSuperCollider();
	
	// Synth management
		std::map<ofxSCServer*, std::shared_ptr<ofxSCSynth>> synthPtrs; // Keep alive
	
	ofEventListeners listeners;
};

#endif
