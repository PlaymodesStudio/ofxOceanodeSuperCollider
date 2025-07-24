#ifndef scConvolution_h
#define scConvolution_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "ofxOscMessage.h"
#include "ofMain.h"
#include <filesystem>
namespace fs = std::filesystem;

class scConvolution : public ofxOceanodeNodeModel {
public:
	explicit scConvolution(std::vector<serverManager*> outputServers) :
		ofxOceanodeNodeModel("SC Convolution"),
		servers(std::move(outputServers)),
		impulseBuffer(nullptr),
		irSpectrumBuffer(nullptr),
		synth(nullptr) {}

	~scConvolution() override { cleanup(); }

	void setup() override {
		addParameter(input.set("In", nodePort()),
					 ofxOceanodeParameterFlags_DisableOutConnection);
		
		addParameter(serverIndex.set("Server", 0, 0,
									 MAX(0, (int)servers.size()-1)));
		
		addParameter(numChannels.set("N Chan", 1, 1, 100));
		addParameter(impulseFile.set("Impulse File", ""));
		addParameter(partitionSize.set("Partition Size", 1024, 256, 2048));
		addParameter(mix.set("Mix", 0.0f, 0.0f, 1.0f));
		addParameter(levels.set("Levels", 0.5f, 0.0f, 1.0f));
		
		addParameter(chooseFile.set("Choose File", false));
		
		addOutputParameter(output.set("Out", nodePort()));
		addParameter(bufnum.set("Bufnum", -1),
					 ofxOceanodeParameterFlags_DisableInConnection);
		addParameter(irBufnum.set("IR Bufnum", -1),
					 ofxOceanodeParameterFlags_DisableInConnection);

		setupListeners();
	}

private:
	void setupListeners() {
		listeners.push(input.newListener([this](nodePort&){ recreateResources(); }));
		listeners.push(serverIndex.newListener([this](int&){ recreateResources(); }));
		listeners.push(numChannels.newListener([this](int&){ recreateResources(); }));
		listeners.push(partitionSize.newListener([this](int&){
			if(impulseBuffer != nullptr){ prepareConvolutionBuffer(); }
		}));
		
		listeners.push(impulseFile.newListener([this](std::string& file){
			if(file != ""){ loadImpulseResponse(file); }
		}));
		
		listeners.push(chooseFile.newListener([this](bool& t){
			if(t){ openFileDialog(); chooseFile = false; }
		}));
		
		listeners.push(mix.newListener([this](float& m){
			if(synth != nullptr){ synth->set("mix", m); }
		}));
		
		listeners.push(levels.newListener([this](float& l){
			if(synth != nullptr){ synth->set("levels", l); }
		}));
	}

	void openFileDialog() {
		ofFileDialogResult res = ofSystemLoadDialog("Choose impulse response");
		if(res.bSuccess){
			impulseFile = res.getPath();
		}
	}

	void loadImpulseResponse(const std::string& filepath) {
		clearBuffers();
		
		// Load the impulse response file
		impulseBuffer = new ofxSCBuffer(1, 1, servers[serverIndex]->getServer());
		impulseBuffer->read(filepath);
		bufnum = impulseBuffer->index;
		
		prepareConvolutionBuffer();
	}

	void prepareConvolutionBuffer() {
		if(impulseBuffer == nullptr) return;
		
		// Calculate buffer size needed for PartConv (simplified)
		int numPartitions = (impulseBuffer->frames + partitionSize - 1) / partitionSize;
		int bufsize = numPartitions * partitionSize * 2; // Complex FFT data
		
		// Create spectrum buffer
		if(irSpectrumBuffer != nullptr){
			irSpectrumBuffer->free();
			delete irSpectrumBuffer;
		}
		
		irSpectrumBuffer = new ofxSCBuffer(bufsize, 1, servers[serverIndex]->getServer());
		irSpectrumBuffer->alloc();
		irBufnum = irSpectrumBuffer->index;
		
		// Prepare the convolution buffer via OSC message
		preparePartConvBuffer();
		
		recreateResources();
	}

	void preparePartConvBuffer() {
		// Send OSC command to prepare the buffer
		ofxOscMessage m;
		m.setAddress("/b_gen");
		m.addIntArg(irSpectrumBuffer->index);
		m.addStringArg("PreparePartConv");
		m.addIntArg(impulseBuffer->index);
		m.addIntArg(partitionSize);
		
		servers[serverIndex]->getServer()->sendMsg(m);
	}

	void recreateResources() {
		clearSynth();
		if(input->getNodeRef() == nullptr || irSpectrumBuffer == nullptr) return;

		std::string defName = "convolution" + ofToString(numChannels);
		synth = new ofxSCSynth(defName, servers[serverIndex]->getServer());
		synth->create(1, 1);
		
		synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
		synth->set("out", output->getBusIndex(servers[serverIndex]->getServer()));
		synth->set("irspectrum", irBufnum.get());
		synth->set("partsize", partitionSize);
		synth->set("mix", mix);
		synth->set("levels", levels);
	}

	void clearSynth() {
		if(synth != nullptr){
			synth->free();
			delete synth;
			synth = nullptr;
		}
	}

	void clearBuffers() {
		if(impulseBuffer != nullptr){
			impulseBuffer->free();
			delete impulseBuffer;
			impulseBuffer = nullptr;
		}
		if(irSpectrumBuffer != nullptr){
			irSpectrumBuffer->free();
			delete irSpectrumBuffer;
			irSpectrumBuffer = nullptr;
		}
		bufnum = -1;
		irBufnum = -1;
	}

	void cleanup() {
		clearSynth();
		clearBuffers();
	}

	// Member variables
	ofEventListeners listeners;
	
	ofParameter<nodePort> input;
	ofParameter<nodePort> output;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<std::string> impulseFile;
	ofParameter<int> partitionSize;
	ofParameter<float> mix;
	ofParameter<float> levels;
	ofParameter<bool> chooseFile;
	
	ofParameter<int> bufnum;
	ofParameter<int> irBufnum;

	ofxSCBuffer* impulseBuffer;
	ofxSCBuffer* irSpectrumBuffer;
	ofxSCSynth* synth;
	std::vector<serverManager*> servers;
};

#endif /* scConvolution_h */
