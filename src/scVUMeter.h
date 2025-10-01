//
//  scVUMeter.h
//  ofxOceanodeSupercollider
//
//  Simple VU meter - EXTRACTED FROM WORKING POLYMIXER
//

#ifndef scVUMeter_h
#define scVUMeter_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include <map>

class ofxSCSynth;
class ofxSCServer;
class ofxSCBus;

class scVUMeter: public scNode {
public:
	scVUMeter();
	~scVUMeter();
	
	void setup();
	void update(ofEventArgs &args) override;
	
	// scNode interface methods
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void free(ofxSCServer* server);
	
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	void setOutputBus(ofxSCServer* server, int index, int bus);
	int getOutputBusIndex(ofxSCServer* server, int index);
	
private:
	// Synth instances per server
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, ofxSCBus*> vuBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	
	// Parameters
	ofParameter<int> numChannels;
	ofParameter<vector<float>> vuMeter;
	ofParameter<float> vuAttack;
	ofParameter<float> vuRelease;
	ofParameter<float> widgetWidth;
	ofParameter<float> widgetHeight;
	
	// VU Data output parameter
	shared_ptr<ofxOceanodeParameter<vector<float>>> vuData;
	
	// Peak tracking for GUI
	vector<float> peakLevels;
	vector<float> peakDecayTimers;
	
	// Event listeners
	ofEventListeners listeners;
	
	// Helper methods
	string getSynthDefName() const;
	void recreateVUBus(ofxSCServer* server);
	void updateVUTiming(float attackTime, float releaseTime);
	
	// GUI - EXACTLY like polymixer
	void drawVUWidget();
	static void drawSeparator();
	
	// dB conversion utilities - EXACTLY like polymixer
	static float ampToDb(float amp);
	static float dbToVUPosition(float db, float minDb = -60.0f, float maxDb = 6.0f);
	static unsigned int getVUMeterColorDB(float dbLevel);
};

#endif /* scVUMeter_h */
