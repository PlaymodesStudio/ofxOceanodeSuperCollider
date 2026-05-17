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
#include <cmath>

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
	description = "Converts float vector data into SuperCollider buffer data. Includes temporal lag filtering to reduce artifacts from 60fps control rate updates. Buffer size is fixed at 1024 samples per buffer.";
	
	for(int i = 0; i < servers.size(); i++){
		buffers[i].resize(1);
		buffers[i][0] = new ofxSCBuffer(1024, 1, servers[i]->getServer());
		buffers[i][0]->alloc();
	}
	
	
	
	addParameter(numBuffers.set("Num Bufs", 1, 1, INT_MAX));
	addParameter(input.set("Input", {0}, {0}, {1}));
	addParameter(rescale.set("Rescale", false));
	addOutputParameter(buffersParam.set("Buffer", {buffers[0][0]->index}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableSavePreset);
	addParameter(lagTime.set("Lag Time", 1.0f/30.0f, 0.001f, 1.0f));
	
	// Pre-calculate initial lag coefficient: 1 - e^(-dt/tau)
	float dt = 1.0f / frameRate;
	lagCoeff = 1.0f - exp(-dt / lagTime.get());
	
	listener = input.newListener([this](vector<float> &vf){
		if(vf.size() == 1024 * numBuffers){
			
			// Initialize lagged values on first run
			if(laggedValues.size() != vf.size()) {
				laggedValues = vf;  // Initialize to current values
			}
			
			// Update lag coefficient if lag time changed
			float dt = 1.0f / frameRate;
			lagCoeff = 1.0f - exp(-dt / lagTime.get());
			
			// Apply lag to each value
			for(size_t i = 0; i < vf.size(); i++) {
				laggedValues[i] += lagCoeff * (vf[i] - laggedValues[i]);
			}
			
			// Send lagged values instead of raw values
			for(int n = 0; n < numBuffers; n++){
				for(int i = 0; i < servers.size(); i++){
					ofxOscMessage m;
					
					m.setAddress("/b_setn");
					m.addIntArg(buffers[i][n]->index);
					m.addIntArg(0);
					m.addIntArg(1024);
					for(int j = 0; j < 1024; j++){
						float laggedValue = laggedValues[j + (n * 1024)];
						if(rescale){
							m.addFloatArg((laggedValue * 2) - 1); //Sample value
						}else{
							m.addFloatArg(laggedValue);
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
			
			// Clear lagged values when buffer count changes
			laggedValues.clear();
			
			//We assume all servers have the same buffer indexs.
			vector<int> newIndexs(numBuffers);
			for(int i = 0; i < numBuffers; i++) newIndexs[i] = buffers[0][i]->index;
			buffersParam = newIndexs;
		}
		oldNumBuffers = numBuffers;
	});
}
