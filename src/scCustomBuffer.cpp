//
//  scCustomBuffer.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 30/8/23.
//

#include "scCustomBuffer.h"
#include "serverManager.h"
#include "ofxSCBuffer.h"
#include "ofxSCServer.h"

scCustomBuffer::scCustomBuffer(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Custom Buffer"){
    servers = outputServers;
}

scCustomBuffer::~scCustomBuffer(){
    for(auto b : buffers){
        b->free();
    }
}

void scCustomBuffer::setup(){
    buffers.resize(1);
    buffers[0] = new ofxSCBuffer(1024, 1, servers[0]->getServer());
    buffers[0]->alloc();
    
    addParameter(numBuffers.set("Num Bufs", 1, 1, INT_MAX));
    addParameter(input.set("Input", {0}, {0}, {1}));
    addOutputParameter(buffersParam.set("Buffer", {buffers[0]->index}, {0}, {INT_MAX}));
    
    oldNumBuffers = numBuffers;
    listener = input.newListener([this](vector<float> &vf){
        if(vf.size() == 1024 * numBuffers){
            for(int n = 0; n < numBuffers; n++){
                ofxOscMessage m;
                
                m.setAddress("/b_setn");
                m.addIntArg(buffers[n]->index);
                m.addIntArg(0);
                m.addIntArg(1024);
                for(int i = 0; i < 1024; i++){
                    m.addFloatArg((vf[i + (n * 1024)] * 2) - 1); //Sample value
                }
                
                servers[0]->getServer()->sendMsg(m);
            }
        }
    });
    
    
    numBuffersListener = numBuffers.newListener([this](int &i){
        if(oldNumBuffers != numBuffers){
            bool remove = oldNumBuffers > numBuffers;
            
            if(remove){
                for(int j = oldNumBuffers-1; j >= numBuffers; j--){
                    buffers[j]->free();
                }
                buffers.resize(numBuffers);
            }else{
                buffers.resize(numBuffers);
                for(int j = oldNumBuffers; j < numBuffers; j++){
                    buffers[j] = new ofxSCBuffer(1024, 1, servers[0]->getServer());
                    buffers[j]->alloc();
                }
            }
            
            input = input;
            
            vector<int> newIndexs(numBuffers);
            for(int i = 0; i < numBuffers; i++) newIndexs[i] = buffers[i]->index;
            buffersParam = newIndexs;
        }
        oldNumBuffers = numBuffers;
    });
}
