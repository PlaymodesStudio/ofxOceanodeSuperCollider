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
    ofParameter<vector<int>> buffersParam;
    
    vector<serverManager*> servers;
    vector<ofxSCBuffer*> buffers;
    
    int oldNumBuffers;
};

#endif /* scCustomBuffer_h */
