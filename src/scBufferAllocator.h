#ifndef scBufferAllocator_h
#define scBufferAllocator_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "serverManager.h"
#include "ofMain.h"

class scBufferAllocator : public ofxOceanodeNodeModel {
public:
	scBufferAllocator(std::vector<serverManager*> outputServers) :
		ofxOceanodeNodeModel("SC Buffer Allocator"),
		servers(std::move(outputServers)),
		isRecording(false),
		recordStartTimeMs(0) {}

	~scBufferAllocator() override {
		cleanup();
	}

	void setup() override {
		addParameter(input.set("In", nodePort()),
					 ofxOceanodeParameterFlags_DisableOutConnection);

		addParameter(serverIndex.set("Server", 0, 0,
						 MAX(0, (int)servers.size() - 1)));

		addParameter(numChannels.set("N Chan", 2, 1, 32));
		addParameter(lengthMs.set("Length (ms)", 10000.f, 10.f, 3600000.f));
		addParameter(circular.set("Circular", false));

		addParameter(record.set("Record", false));
		addOutputParameter(bufferIndices.set("Bufnums", { -1 }, { -1 }, { 10000 }),
						   ofxOceanodeParameterFlags_DisableSavePreset);

		addParameter(allocLED.set("Alloc LED", ofColor(255, 0, 0)),
					 ofxOceanodeParameterFlags_DisableInConnection |
					 ofxOceanodeParameterFlags_ReadOnly);

		addOutputParameter(done.set("Done"), ofxOceanodeParameterFlags_DisableSavePreset);

		listeners.push(input.newListener([this](nodePort&) { recreateResources(); }));
		listeners.push(serverIndex.newListener([this](int&) { recreateResources(); }));
		listeners.push(numChannels.newListener([this](int&) { recreateResources(); }));
		listeners.push(lengthMs.newListener([this](float&) { recreateResources(); }));
		listeners.push(circular.newListener([this](bool&) { recreateResources(); }));

		listeners.push(record.newListener([this](bool& r){
			handleRecordToggle(r);
		}));
	}

	void update(ofEventArgs&) override {
		if(isRecording && !circular){
			uint64_t now = ofGetElapsedTimeMillis();
			if(now - recordStartTimeMs >= static_cast<uint64_t>(lengthMs.get())){
				record = false; // triggers handleRecordToggle(false)
			}
		}
	}

private:
	void recreateResources() {
		cleanup();
		if(input->getNodeRef() == nullptr) return;

		const float sr = 44100.0f;
		int ch = numChannels.get();
		int frames = static_cast<int>((lengthMs.get() / 1000.f) * sr);

		vector<int> indices(ch, -1);
		buffers.clear();
		synths.clear();

		for(int i = 0; i < ch; ++i){
			auto* buf = new ofxSCBuffer(frames, 1,
				servers[serverIndex]->getServer());
			buf->alloc();
			buffers.push_back(buf);
			indices[i] = buf->index;

			auto* synth = new ofxSCSynth("bufalloc", servers[serverIndex]->getServer());
			synth->create(1, 1);

			// 🔒 Ensure synth is silent until manually triggered
			synth->set("record", 0);  // must be set first to avoid early recording
			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()) + i);
			synth->set("buf", buf->index);
			synth->set("loop", circular.get() ? 1 : 0);

			synths.push_back(synth);
		}

		bufferIndices = indices;
		allocLED = ofColor(255, 0, 0);  // red = idle
	}

	void handleRecordToggle(bool r) {
		if(synths.empty()) return;

		for(auto* synth : synths){
			if(synth) synth->set("record", r ? 1 : 0);
		}

		if(r){
			isRecording = true;
			recordStartTimeMs = ofGetElapsedTimeMillis();
			allocLED = ofColor(0, 255, 0); // green = recording
		}else{
			isRecording = false;
			allocLED = ofColor(255, 0, 0); // red = idle

			if(!circular){
				done.trigger(); // one-shot signal when recording completes
			}
		}
	}

	void cleanup() {
		for(auto* s : synths){
			if(s){ s->set("record", 0); s->free(); delete s; }
		}
		for(auto* b : buffers){
			if(b){ b->free(); delete b; }
		}
		synths.clear();
		buffers.clear();
		bufferIndices = { -1 };
		isRecording = false;
		allocLED = ofColor(255, 0, 0);
	}

	// Data
	std::vector<serverManager*> servers;
	std::vector<ofxSCBuffer*> buffers;
	std::vector<ofxSCSynth*> synths;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<float> lengthMs;
	ofParameter<bool> circular;
	ofParameter<bool> record;
	ofParameter<vector<int>> bufferIndices;
	ofParameter<ofColor> allocLED;
	ofParameter<void> done;

	ofEventListeners listeners;

	bool isRecording;
	uint64_t recordStartTimeMs;
};

#endif /* scBufferAllocator_h */
