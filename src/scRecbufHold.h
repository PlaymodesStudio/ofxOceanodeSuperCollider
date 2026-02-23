//
//  scRecbufHold.h  –  RecBuf with LED, folder selector and continuous recording
//

#ifndef scRecbufHold_h
#define scRecbufHold_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "ofxOscMessage.h"
#include "ofxOceanodeShared.h"

#include "ofMain.h"
#include <filesystem>
namespace fs = std::filesystem;

class scRecbufHold : public ofxOceanodeNodeModel{
public:
	explicit scRecbufHold(std::vector<serverManager*> outputServers) :
		ofxOceanodeNodeModel("Sc RecBuf"),
		servers(std::move(outputServers)),
		isRecording(false) {
		
		// Initialize per-server resources
		recordBufs.resize(servers.size(), nullptr);
		recSynths.resize(servers.size(), nullptr);
	}

	~scRecbufHold() override{ cleanup(); }

	/* ─────────────────── parameters ─────────────────── */
	void setup() override{
		description = "Records audio to disk";

		addParameter(input.set("In", nodePort()),
					 ofxOceanodeParameterFlags_DisableOutConnection);

		addParameter(numChannels.set("N Chan", 2, 1, 16));
		addParameter(lengthSec.set("Length(sec)", 20.0f,
								  0.01f, 3600.0f));

		addParameter(record.set("Record", false));

		addParameter(saveDir.set("Folder",
					ofToDataPath("recordings/", true)));

		addParameter(chooseFolder.set("ChooseFolder", false));

		addParameter(bufnum.set("Bufnum", -1),
					 ofxOceanodeParameterFlags_DisableInConnection);

		addParameter(lastFile.set("Last File", ""),
					 ofxOceanodeParameterFlags_DisableInConnection);

		addParameter(recLED.set("Rec LED", ofColor(255,0,0)),
					 ofxOceanodeParameterFlags_DisableInConnection |
					 ofxOceanodeParameterFlags_ReadOnly);

		// Create buffers during setup (following scCustomBuffer pattern)
		createBuffers();

		/* listeners */
		listeners.push(input.newListener([this](nodePort&){
			updateSynthInputs();
		}));
		
		listeners.push(numChannels.newListener([this](int&){
			recreateBuffers();
		}));
		
		listeners.push(lengthSec.newListener([this](float&){
			recreateBuffers();
		}));

		listeners.push(record.newListener([this](bool& r){
			handleRecordToggle(r);
		}));
		
		listeners.push(chooseFolder.newListener([this](bool& t){
			if(t){
				openFolderDialog();
				chooseFolder = false;
			}
		}));
	}

	/* ─────────── auto-stop when buffer is full ─────────── */
	void update(ofEventArgs&) override{
		if(isRecording){
			uint64_t elapsedMs = ofGetElapsedTimeMillis() - recStartTimeMs;
			float elapsedSec = elapsedMs / 1000.0f;
			if(elapsedSec >= lengthSec.get()){
				recStopTimeMs = ofGetElapsedTimeMillis();  // Set stop time for auto-stop
				record = false;  // This will trigger handleRecordToggle
			}
		}
	}

private:
	/* ─────────── folder dialog ─────────── */
	void openFolderDialog(){
		ofFileDialogResult res = ofSystemLoadDialog("Choose folder", true);
		if(res.bSuccess){
			saveDir = res.getPath();
		}
	}

	/* ─────────── auto-detect active server ─────────── */
	int getActiveServerIndex(){
		if(input->getNodeRef() == nullptr) return 0;
		
		// Find which server has a valid bus for this input
		for(int i = 0; i < servers.size(); i++){
			int busIndex = input->getBusIndex(servers[i]->getServer());
			if(busIndex >= 0){
				return i;
			}
		}
		
		return 0;  // Default to first server
	}

	/* ─────────── create buffers (following scCustomBuffer pattern) ─────────── */
	void createBuffers(){
		constexpr float kSR = 44100.0f;
		int nCh  = numChannels.get();
		int nFrm = static_cast<int>(ceil(lengthSec.get() * kSR));  // Convert seconds to frames

		// Create buffers for all servers (like scCustomBuffer does)
		for(int i = 0; i < servers.size(); i++){
			if(recordBufs[i] != nullptr){
				recordBufs[i]->free();
				delete recordBufs[i];
			}
			
			recordBufs[i] = new ofxSCBuffer(nFrm, nCh, servers[i]->getServer());
			recordBufs[i]->alloc();  // Immediate allocation like scCustomBuffer
			
		}

		// Update bufnum with primary server's buffer index
		bufnum = recordBufs[0]->index;

		createSynths();
	}

	/* ─────────── recreate buffers when params change ─────────── */
	void recreateBuffers(){
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
		if(isRecording){
			record = false;  // Stop recording first
			handleRecordToggle(false);
		}
		
		cleanupSynths();
		createBuffers();
		updateSynthInputs();
	}

	/* ─────────── create synths ─────────── */
	void createSynths(){
		std::string defName = "recbufhold" + ofToString(numChannels.get());
		
		for(int i = 0; i < servers.size(); i++){
			if(recSynths[i] != nullptr){
				recSynths[i]->free();
				delete recSynths[i];
			}
			
			recSynths[i] = new ofxSCSynth(defName, servers[i]->getServer());
			recSynths[i]->create(1, 1);
			recSynths[i]->run(getActive());
			recSynths[i]->set("buf", recordBufs[i]->index);
			recSynths[i]->set("record", 0);
		}
		
		updateSynthInputs();
	}

	/* ─────────── update synth inputs ─────────── */
	void updateSynthInputs(){
		if(input->getNodeRef() == nullptr) return;
		
		for(int i = 0; i < servers.size(); i++){
			if(recSynths[i] != nullptr){
				int busIndex = input->getBusIndex(servers[i]->getServer());
				recSynths[i]->set("in", busIndex);
			}
		}
	}

	/* ─────────── record toggle ─────────── */
	void handleRecordToggle(bool rec){
		if(recordBufs[0] == nullptr || recSynths[0] == nullptr) return;
		
		isRecording = rec;
		
		// Set record parameter on all servers
		for(int i = 0; i < servers.size(); i++){
			if(recSynths[i] != nullptr){
				recSynths[i]->set("record", rec ? 1 : 0);
			}
		}

		if(rec){
			recStartTimeMs = ofGetElapsedTimeMillis();  // Track start time
			recLED = ofColor(0,255,0); // Green when recording
		}else{
			recStopTimeMs = ofGetElapsedTimeMillis();   // Track stop time
			recLED = ofColor(255,0,0); // Red when stopped
			// Auto-save on stop
			saveBufferToDisk();
		}
	}

	/* ─────────── save WAV ─────────── */
	void saveBufferToDisk(){
		int activeServer = getActiveServerIndex();
		
		if(recordBufs[activeServer] == nullptr) {
			return;
		}

		try{
			fs::create_directories(saveDir.get());
		}
		catch(const std::exception& e){
			return;
		}

		// Calculate exact recording duration and frames
		float actualDurationMs = recStopTimeMs - recStartTimeMs;
		constexpr float sampleRate = 44100.0f;
		int actualFrames = static_cast<int>((actualDurationMs / 1000.0f) * sampleRate);
		
		// Ensure we don't exceed buffer size
		int maxFrames = static_cast<int>(lengthSec.get() * sampleRate);  // Convert seconds to frames
		actualFrames = std::min(actualFrames, maxFrames);
		
		// Ensure minimum of 1 frame
		actualFrames = std::max(actualFrames, 1);

		// Get current preset name
		std::string presetName = ofxOceanodeShared::getCurrentPresetName();
		if(presetName.empty()) presetName = "NoPreset";
		
		// Clean preset name for filename (remove invalid characters)
		ofStringReplace(presetName, " ", "_");
		ofStringReplace(presetName, "/", "_");
		ofStringReplace(presetName, "\\", "_");
		ofStringReplace(presetName, ":", "_");

		std::string fname = presetName + "_rec_" + ofGetTimestampString("%Y%m%d_%H%M%S") + ".wav";
		std::string absPath = (fs::path(saveDir.get()) / fname).string();

		ofxOscMessage m;
		m.setAddress("/b_write");
		m.addIntArg(recordBufs[activeServer]->index);
		m.addStringArg(absPath);
		m.addStringArg("wav");
		m.addStringArg("float");
		m.addIntArg(actualFrames);  // Write only the frames that were actually recorded
		m.addIntArg(0);             // Start from frame 0
		m.addIntArg(0);             // Write header
		servers[activeServer]->getServer()->sendMsg(m);

		lastFile = absPath;
	}

	void activate() override {
		for(auto* s : recSynths) if(s) s->run(true);
	}

	void deactivate() override {
		for(auto* s : recSynths) if(s) s->run(false);
	}

	/* ─────────── cleanup synths ─────────── */
	void cleanupSynths(){
		for(int i = 0; i < servers.size(); i++){
			if(recSynths[i] != nullptr){
				recSynths[i]->free();
				delete recSynths[i];
				recSynths[i] = nullptr;
			}
		}
	}

	/* ─────────── cleanup all ─────────── */
	void cleanup(){
		cleanupSynths();
		
		for(int i = 0; i < servers.size(); i++){
			if(recordBufs[i] != nullptr){
				recordBufs[i]->free();
				delete recordBufs[i];
				recordBufs[i] = nullptr;
			}
		}
		
		bufnum = -1;
		isRecording = false;
		recLED = ofColor(255,0,0); // Red when not ready
	}

	/* ─────────── data ─────────── */
	ofEventListeners            listeners;

	ofParameter<nodePort>       input;
	ofParameter<int>            numChannels;
	ofParameter<float>          lengthSec;
	ofParameter<bool>           record;
	ofParameter<std::string>    saveDir;
	ofParameter<bool>           chooseFolder;

	ofParameter<int>            bufnum;
	ofParameter<std::string>    lastFile;
	ofParameter<ofColor>        recLED;

	bool                        isRecording;
	uint64_t                    recStartTimeMs;  // Track recording start time
	uint64_t                    recStopTimeMs;   // Track recording stop time

	std::vector<ofxSCBuffer*>   recordBufs;
	std::vector<ofxSCSynth*>    recSynths;
	std::vector<serverManager*> servers;
};

#endif /* scRecbufHold_h */
