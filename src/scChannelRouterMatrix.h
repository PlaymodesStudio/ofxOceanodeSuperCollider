//
//  scChannelRouterMatrix.h
//  ofxOceanodeSupercollider
//
//  Channel Router Matrix - Route inputs to outputs with gain control
//

#ifndef scChannelRouterMatrix_h
#define scChannelRouterMatrix_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include <map>

class ofxSCSynth;
class ofxSCServer;

class scChannelRouterMatrix : public scNode {
public:
	scChannelRouterMatrix();
	~scChannelRouterMatrix();
	
	void setup();
	
	// scNode interface methods
	void activate() override;
	void deactivate() override;
	void buildSynth(ofxSCServer* server);
	void createSynth(ofxSCServer* server);
	void free(ofxSCServer* server);
	
	void setInputBus(ofxSCServer* server, scNode* node, int bus);
	void resetInputBusses(ofxSCServer* server, int targetBus = 0);
	void setOutputBus(ofxSCServer* server, int index, int bus);
	int getOutputBusIndex(ofxSCServer* server, int index);
	
	void moveSynthBefore(ofxSCServer* server, int nodeID);
	int getLastSynthID(ofxSCServer* server);
	
	void presetSave(ofJson &json);
	void presetRecallAfterSettingParameters(ofJson &json);
	
private:
	// Synth instances per server
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	
	// Parameters
	ofParameter<int> numChannels;
	ofParameter<int> matrixMode; // 0 = Multislider, 1 = Toggle
	ofParameter<int> compensation; // 0 = 0dB, 1 = -3dB, 2 = -6dB
	ofParameter<bool> bypass;
	
	// Inspector parameters for widget size
	ofParameter<float> widgetWidth;
	ofParameter<float> widgetHeight;
	
	// Matrix state: [input][output] = gain (0.0-1.0)
	vector<vector<float>> routingMatrix;
	
	// Event listeners
	ofEventListeners listeners;
	
	// Helper methods
	string getSynthDefName() const;
	void initializeMatrix();
	void updateMatrixSize();
	void updateSynthParameters(ofxSCServer* server);
	vector<float> flattenMatrix() const;
	
	// GUI
	void drawMatrixWidget();
	void drawMultisliderMatrix();
	void drawToggleMatrix();
	static void drawSeparator();
	
	// Matrix interaction
	int hoveredCell[2]; // [row, col]
	bool isDragging;
	
	// Visual helpers - defined in .cpp to avoid ImVec2 issues
	void getCellPosition(int row, int col, const ImVec2& matrixStart, float cellWidth, float cellHeight, ImVec2& result);
};

#endif /* scChannelRouterMatrix_h */
