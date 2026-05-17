//
//  scRecbuf.h  –  RecBuf amb LED, autostop i selector de carpeta
//

#ifndef scRecbuf_h
#define scRecbuf_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "ofxOscMessage.h"

#include "ofMain.h"              // ofColor, ofSystemLoadDialog
#include <algorithm>
#include <filesystem>
namespace fs = std::filesystem;

class scRecbuf : public ofxOceanodeNodeModel{
public:
	explicit scRecbuf(std::vector<serverManager*> outputServers) :
		ofxOceanodeNodeModel("Sc RecBuf"),
		servers(std::move(outputServers)),
		recordBuf(nullptr),
		recSynth(nullptr),
		isRecording(false),
		recStartTimeMs(0) {}

	~scRecbuf() override{ cleanup(); }

	/* ─────────────────── paràmetres ─────────────────── */
	void setup() override{
		addParameter(input.set("In", nodePort()),
					 ofxOceanodeParameterFlags_DisableOutConnection);

		addParameter(serverIndex.set("Server", 0, 0,
									 MAX(0, (int)servers.size()-1)));

		addParameter(numChannels.set("N Chan", 2, 1, 40));
		addParameter(lengthMs.set("Length (ms)", 10000.0f,
								  10.0f, 3600000.0f));

		addParameter(record.set("Record", false));
		addParameter(saveTrigger.set("Save", false));

		addParameter(saveDir.set("Folder",
					ofToDataPath("recordings/", true)));

		addParameter(chooseFolder.set("Choose Folder", false));   // ← nou botó

		addParameter(bufnum.set("Bufnum", -1),
					 ofxOceanodeParameterFlags_DisableInConnection);

		addParameter(lastFile.set("Last File", ""),
					 ofxOceanodeParameterFlags_DisableInConnection);

		addParameter(recLED.set("Rec LED", ofColor(255,0,0)),
					 ofxOceanodeParameterFlags_DisableInConnection |
					 ofxOceanodeParameterFlags_ReadOnly);

		/* listeners */
		listeners.push(input.newListener([this](nodePort&){ recreateResources(); }));
		listeners.push(serverIndex.newListener([this](int&){ recreateResources(); }));
		listeners.push(numChannels.newListener([this](int&){ recreateResources(); }));
		listeners.push(lengthMs.newListener([this](float&){ recreateResources(); }));

		listeners.push(record.newListener([this](bool& r){ handleRecordToggle(r); }));
		listeners.push(saveTrigger.newListener([this](bool& t){
			if(t){ saveBufferToDisk(); saveTrigger = false; }
		}));
		listeners.push(chooseFolder.newListener([this](bool& t){
			if(t){ openFolderDialog(); chooseFolder = false; }
		}));
	}

	/* ─── update: auto-stop ─── */
	void update(ofEventArgs&) override{
		if(isRecording){
			uint64_t now = ofGetElapsedTimeMillis();
			if(now - recStartTimeMs >= static_cast<uint64_t>(lengthMs.get())){
				record = false;
				handleRecordToggle(false);
			}
		}
	}

	void draw(ofEventArgs&) override{}   // sense GUI extra

private:
	/* ─────────── selector de carpeta ─────────── */
	void openFolderDialog(){
		ofFileDialogResult res = ofSystemLoadDialog("Choose folder", true); // true = dir
		if(res.bSuccess){
			saveDir = res.getPath();
		}
	}

	/* ─────────── (re)crear ─────────── */
	void recreateResources(){
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
		cleanup();
		if(input->getNodeRef() == nullptr) return;
		if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;

		const float sr = (float)servers[serverIndex]->getSampleRate();
		int nCh  = numChannels.get();
		int nFrm = std::max(1, static_cast<int>(ceil((lengthMs.get()/1000.0f) * sr)));

		recordBuf = new ofxSCBuffer(nFrm, nCh,
									servers[serverIndex]->getServer());
		recordBuf->alloc();
		bufnum = recordBuf->index;

		std::string defName = "recbuf" + ofToString(nCh);
		recSynth = new ofxSCSynth(defName,
								  servers[serverIndex]->getServer());
        recSynth->createAndRun(1, 1, getActive()); //addToTail
		recSynth->set("in",
			input->getBusIndex(servers[serverIndex]->getServer()));
		recSynth->set("buf", bufnum.get());
		recSynth->set("record", 0);

		isRecording = false;
		recLED      = ofColor(255,0,0);
	}

	/* ─────────── record toggle ─────────── */
	void handleRecordToggle(bool rec){
		if(recSynth == nullptr) return;
		isRecording = rec;
		recSynth->set("record", rec ? 1 : 0);

		if(rec){
			recStartTimeMs = ofGetElapsedTimeMillis();
			recLED = ofColor(0,255,0);
		}else{
			recLED = ofColor(255,0,0);
		}
	}

	/* ─────────── guardar WAV ─────────── */
	void saveBufferToDisk(){
		if(recordBuf == nullptr) return;

		try{ fs::create_directories(saveDir.get()); }
		catch(const std::exception& e){
			ofLogError() << "RecBuf: cannot create dir: " << e.what();
			return;
		}

		std::string fname   = "rec_" + ofGetTimestampString("%Y%m%d_%H%M%S") + ".wav";
		std::string absPath = (fs::path(saveDir.get()) / fname).string();

		ofxOscMessage m;
		m.setAddress("/b_write");
		m.addIntArg(bufnum.get());
		m.addStringArg(absPath);
		m.addStringArg("wav");
		m.addStringArg("float");
		m.addIntArg(-1);
		m.addIntArg(0);
		m.addIntArg(0);
		servers[serverIndex]->getServer()->sendMsg(m);

		lastFile = absPath;
	}

	void activate() override { if(recSynth) recSynth->run(true); }
	void deactivate() override { if(recSynth) recSynth->run(false); }

	/* ─────────── cleanup ─────────── */
	void cleanup(){
		if(recSynth){ recSynth->free(); delete recSynth; recSynth = nullptr; }
		if(recordBuf){ recordBuf->free(); delete recordBuf; recordBuf = nullptr; }
		bufnum = -1;
		isRecording = false;
		recLED = ofColor(255,0,0);
	}

	/* ─────────── data ─────────── */
	ofEventListeners            listeners;

	ofParameter<nodePort>       input;
	ofParameter<int>            serverIndex;
	ofParameter<int>            numChannels;
	ofParameter<float>          lengthMs;
	ofParameter<bool>           record;
	ofParameter<bool>           saveTrigger;
	ofParameter<std::string>    saveDir;
	ofParameter<bool>           chooseFolder;   // nou trigger

	ofParameter<int>            bufnum;
	ofParameter<std::string>    lastFile;
	ofParameter<ofColor>        recLED;

	bool                        isRecording;
	uint64_t                    recStartTimeMs;

	ofxSCBuffer*                recordBuf;
	ofxSCSynth*                 recSynth;
	std::vector<serverManager*> servers;
};

#endif /* scRecbuf_h */
