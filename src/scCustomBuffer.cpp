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
    buffers.resize(servers.size());
}

scCustomBuffer::~scCustomBuffer(){
    for(auto b : buffers){
        for(auto bb : b){
            bb->free();
        }
    }
}

void scCustomBuffer::setup(){
    for(int i = 0; i < servers.size(); i++){
        buffers[i].resize(1);
        buffers[i][0] = new ofxSCBuffer(1024, 1, servers[i]->getServer());
        buffers[i][0]->alloc();
    }
    
    
    
    addParameter(numBuffers.set("Num Bufs", 1, 1, INT_MAX));
    addParameter(input.set("Input", {0}, {0}, {1}));
    addParameter(rescale.set("Rescale", false));
    addOutputParameter(buffersParam.set("Buffer", {buffers[0][0]->index}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableSavePreset);
    
    oldNumBuffers = numBuffers;
    listener = input.newListener([this](vector<float> &vf){
        if(vf.size() == 1024 * numBuffers){
            for(int n = 0; n < numBuffers; n++){
                for(int i = 0; i < servers.size(); i++){
                    ofxOscMessage m;
                    
                    m.setAddress("/b_setn");
                    m.addIntArg(buffers[i][n]->index);
                    m.addIntArg(0);
                    m.addIntArg(1024);
                    for(int j = 0; j < 1024; j++){
                        if(rescale){
                            m.addFloatArg((vf[j + (n * 1024)] * 2) - 1); //Sample value
                        }else{
                            m.addFloatArg(vf[j + (n * 1024)]);
                        }
                    }
                    
                    servers[i]->getServer()->sendMsg(m);
                }
            }
        }
    });
    
    
    numBuffersListener = numBuffers.newListener([this](int &_i){
        if(oldNumBuffers != numBuffers){
            bool remove = oldNumBuffers > numBuffers;
            
            for(int i = 0; i < servers.size(); i++){
                if(remove){
                    for(int j = oldNumBuffers-1; j >= numBuffers; j--){
                        buffers[i][j]->free();
                    }
                    buffers[i].resize(numBuffers);
                }else{
                    buffers[i].resize(numBuffers);
                    for(int j = oldNumBuffers; j < numBuffers; j++){
                        buffers[i][j] = new ofxSCBuffer(1024, 1, servers[i]->getServer());
                        buffers[i][j]->alloc();
                    }
                }
            }
            
            input = input;
            
            //We assume all servers have the same buffer indexs.
            vector<int> newIndexs(numBuffers);
            for(int i = 0; i < numBuffers; i++) newIndexs[i] = buffers[0][i]->index;
            buffersParam = newIndexs;
        }
        oldNumBuffers = numBuffers;
    });
}
