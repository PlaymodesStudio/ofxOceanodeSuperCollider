//
//  scCustomBuffer.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 30/8/23.
//

#ifndef scCustomBuffer_h
#define scCustomBuffer_h

#include "ofxOceanodeNodeModel.h"

class ofxSCBuffer;
class serverManager;

class scCustomBuffer : public ofxOceanodeNodeModel {
public:
	scCustomBuffer(vector<serverManager*> outputServers);
	~scCustomBuffer();
	
	void setup();
	
private:
	ofEventListener listener;
	ofEventListener numBuffersListener;
	
	ofParameter<int> numBuffers;
	ofParameter<vector<float>> input;
	ofParameter<bool> rescale;
	ofParameter<float> lagTime;
	ofParameter<vector<int>> buffersParam;
	
	vector<serverManager*> servers;
	vector<vector<ofxSCBuffer*>> buffers;
	
	int oldNumBuffers;
	
	// Lag implementation variables
	vector<float> laggedValues;
	float frameRate = 60.0f;        // Oceanode's frame rate
	float lagCoeff;                 // Pre-calculated lag coefficient
};

#endif /* scCustomBuffer_h */
