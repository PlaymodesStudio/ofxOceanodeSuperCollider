#ifndef scDoubleBuffer_h
#define scDoubleBuffer_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"

class serverManager;

class scDoubleBuffer : public ofxOceanodeNodeModel {
public:
	scDoubleBuffer(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Double Buffer"){
		servers = outputServers;
		buffersSetA.resize(servers.size());
		buffersSetB.resize(servers.size());
		currentBufferSize = 1024; // Default start size
		oldNumBuffers = 1;
	}
	
	~scDoubleBuffer(){
		clearBuffers();
	}

	void setup(){
		description = "Smart double-buffered wavetable uploader. Automatically detects buffer size from input vector length. Latched for click-free updates.";

		// --- Parameters ---
		addParameter(numBuffers.set("Num Bufs", 1, 1, INT_MAX));
		addParameter(input.set("Input", {0}, {0}, {1})); // Vector input
		addParameter(rescale.set("Rescale", false));
		addParameter(xfadeTime.set("XFade Time", 0.01f, 0.001f, 0.1f));
		
		// --- Outputs ---
		addOutputParameter(buffersA.set("Buffer A", {0}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableSavePreset);
		addOutputParameter(buffersB.set("Buffer B", {0}, {0}, {INT_MAX}), ofxOceanodeParameterFlags_DisableSavePreset);
		addOutputParameter(bufSelect.set("Buf Select", {0.0f}, {0.0f}, {1.0f}), ofxOceanodeParameterFlags_DisableSavePreset);

		// Initial allocation
		reallocateBuffers(1024, 1);

		// --- DATA LISTENER with AUTO-DETECT ---
		listener = input.newListener([this](vector<float> &vf){
			int totalSize = vf.size();
			int nBuffers = numBuffers.get();
			
			// Safety: Avoid divide by zero
			if(nBuffers < 1 || totalSize == 0) return;

			// 1. Check for consistency
			// The vector size must be perfectly divisible by numBuffers
			if(totalSize % nBuffers != 0){
				// Optional: Log warning if vector size doesn't match channel count
				return;
			}

			int detectedSize = totalSize / nBuffers;

			// 2. AUTO-REALLOCATE if size mismatch
			// If the incoming vector implies a different buffer size than what we have,
			// we must rebuild everything immediately.
			if(detectedSize != currentBufferSize || nBuffers != oldNumBuffers){
				reallocateBuffers(detectedSize, nBuffers);
			}

			// 3. Upload Data (Standard Logic)
			for(int n = 0; n < nBuffers; n++){
				bool targetIsB = !writeToB[n];
				auto& targetBuffers = targetIsB ? buffersSetB : buffersSetA;
				
				for(int i = 0; i < servers.size(); i++){
					ofxOscMessage m;
					m.setAddress("/b_setn");
					m.addIntArg(targetBuffers[i][n]->index);
					m.addIntArg(0);
					m.addIntArg(detectedSize);
					
					for(int j = 0; j < detectedSize; j++){
						float value = vf[j + (n * detectedSize)];
						if(rescale) m.addFloatArg((value * 2.0f) - 1.0f);
						else m.addFloatArg(value);
					}
					servers[i]->getServer()->sendMsg(m);
				}
				
				// Queue the switch
				switchTimers[n] = 1;
			}
		});

		// We only need to listen to numBuffers to trigger a potential clear/resize
		// implicitly handled by the next input frame, but we can do it explicitly too.
		numBuffersListener = numBuffers.newListener([this](int &i){
			// We don't allocate here anymore. We wait for the input vector
			// to arrive so we know the correct size (Input Size / i).
		});
	}

	void update(ofEventArgs &e) override {
		bool changed = false;
		int nBuffers = numBuffers.get();
		
		// Safety check
		if(switchTimers.size() != nBuffers) return;

		for(int i = 0; i < nBuffers; i++){
			if(switchTimers[i] > 0){
				switchTimers[i]--;
				if(switchTimers[i] == 0){
					writeToB[i] = !writeToB[i];
					changed = true;
					switchTimers[i] = -1;
				}
			}
		}
		if(changed) updateBufferSelect();
	}

private:
	void clearBuffers(){
		for(int s = 0; s < servers.size(); s++){
			for(auto b : buffersSetA[s]) if(b) { b->free(); delete b; }
			for(auto b : buffersSetB[s]) if(b) { b->free(); delete b; }
			buffersSetA[s].clear();
			buffersSetB[s].clear();
		}
	}

	void reallocateBuffers(int newSize, int newNum){
		clearBuffers();
		
		for(int s = 0; s < servers.size(); s++){
			buffersSetA[s].resize(newNum);
			buffersSetB[s].resize(newNum);
			
			for(int j = 0; j < newNum; j++){
				// Allocate new buffers with the DETECTED size
				buffersSetA[s][j] = new ofxSCBuffer(newSize, 1, servers[s]->getServer());
				buffersSetA[s][j]->alloc();
				
				buffersSetB[s][j] = new ofxSCBuffer(newSize, 1, servers[s]->getServer());
				buffersSetB[s][j]->alloc();
			}
		}
		
		// Reset Logic
		writeToB.assign(newNum, false);
		switchTimers.assign(newNum, -1);
		
		// Update State Variables
		currentBufferSize = newSize;
		oldNumBuffers = newNum;

		// Update Output Indices
		vector<int> idxA(newNum), idxB(newNum);
		for(int i = 0; i < newNum; i++){
			if(buffersSetA[0][i] && buffersSetB[0][i]){
				idxA[i] = buffersSetA[0][i]->index;
				idxB[i] = buffersSetB[0][i]->index;
			}
		}
		buffersA = idxA;
		buffersB = idxB;
		updateBufferSelect();
	}

	void updateBufferSelect(){
		int num = numBuffers.get();
		if(writeToB.size() != num) return;
		
		vector<float> newSelect(num);
		for(int i = 0; i < num; i++){
			newSelect[i] = writeToB[i] ? 1.0f : 0.0f;
		}
		bufSelect = newSelect;
	}

	ofEventListener listener;
	ofEventListener numBuffersListener;

	ofParameter<int> numBuffers;
	ofParameter<vector<float>> input;
	ofParameter<bool> rescale;
	ofParameter<float> xfadeTime;
	
	ofParameter<vector<int>> buffersA;
	ofParameter<vector<int>> buffersB;
	ofParameter<vector<float>> bufSelect;

	vector<serverManager*> servers;
	vector<vector<ofxSCBuffer*>> buffersSetA;
	vector<vector<ofxSCBuffer*>> buffersSetB;

	vector<bool> writeToB;
	vector<int> switchTimers;

	int currentBufferSize; // Now tracked internally
	int oldNumBuffers;
};

#endif /* scDoubleBuffer_h */
