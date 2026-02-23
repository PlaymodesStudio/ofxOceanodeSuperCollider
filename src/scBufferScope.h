//  scBufferscope.h
//  -----------------------------------------------------------------------------
//  Bufferscope node – overview complet amb mostreig uniforme.
//  • Bufnums mono (vector<int>).  • Auto‑escala o gain.
//  • Header: posició 0‑1 per canal (vector o escalar).
//  • HeaderTh: gruix 0‑1 → % amplada (1 = tot el track).
//  • HeaderOp: opacitat 0‑1 (vector o escalar).
//  -----------------------------------------------------------------------------
#ifndef scBufferscope_h
#define scBufferscope_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"
#include "serverManager.h"

class scBufferscope : public ofxOceanodeNodeModel {
public:
	explicit scBufferscope(std::vector<serverManager*> srv) : ofxOceanodeNodeModel("Bufferscope"), servers(std::move(srv)) {}
	~scBufferscope() override {
		freeSynths(); if(waveformBus){ waveformBus->free(); delete waveformBus; }
	}

	// ============ setup ========================================================
	void setup() override {
		addParameter(showWindow.set("Show", false));
		addParameter(bufnums.set("Bufnums", {0}, {0}, {INT_MAX}));
		addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
		addParameterDropdown(samplesDropdown, "Samples", 2, {"32","64","128","256","512","1024","2048","4096"});
		addParameter(autoScale.set("AutoScale", true));
		addParameter(manualGain.set("Gain", 1.0f, 0.01f, 100.0f));
		addParameter(header.set("Header", {0.f}, {0.f}, {1.f}));
		addParameter(headerThickness.set("HeaderTh", {0.01f}, {0.f}, {1.f}));
		addParameter(headerOpacity.set("HeaderOp", {1.f}, {0.f}, {1.f}));
		addParameter(lineColor.set("Line", ofColor(0,255,0), ofColor(0), ofColor(255)));
		addParameter(backgroundColor.set("BG", ofColor(0,0,0,180), ofColor(0), ofColor(255)));

		sampSizes = {32,64,128,256,512,1024,2048,4096};
		samplesPerChannel = sampSizes[samplesDropdown.get()];

		listeners.push(bufnums.newListener([this](std::vector<int> &){ recreateSynths(); }));
		listeners.push(serverIndex.newListener([this](int &){ recreateSynths(); }));
		listeners.push(samplesDropdown.newListener([this](int &i){ samplesPerChannel=sampSizes[ofClamp(i,0,(int)sampSizes.size()-1)]; recreateSynths(); }));
	}

	// ============ update =======================================================
	void update(ofEventArgs&) override {
		if(!waveformBus) return;
		size_t need = channels()*samplesPerChannel;
		const auto &v = waveformBus->readValues;
		if(v.size()>=need) waveformData=v;
		waveformBus->requestValues();

		if(autoScale && !waveformData.empty() && ofGetFrameNum()%30==0){
			float m=0; for(float f:waveformData){ float a=fabsf(f); if(a>m) m=a; }
			currentGain = (m<1e-6f)?1.f:1.f/m;
		} else if(!autoScale) currentGain = manualGain;
	}

	// ============ draw =========================================================
	void draw(ofEventArgs&) override {
		if(!showWindow) return;
		std::string t = (canvasID=="Canvas"?"":canvasID+"/")+"Bufferscope "+ofToString(getNumIdentifier());
		if(ImGui::Begin(t.c_str(), (bool *)&showWindow.get())) drawWaveform();
		ImGui::End();
	}

private:
	// --------------------------------------------------------------------------
	int channels() const { return bufnums->size(); }

	void freeSynths(){ for(auto *s:synths){ if(s){ s->free(); delete s; } } synths.clear(); }

	void activate() override {
		for(auto* s : synths) if(s) s->run(true);
	}

	void deactivate() override {
		for(auto* s : synths) if(s) s->run(false);
	}

	void recreateSynths(){
		freeSynths(); if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus=nullptr; }
		if(bufnums->empty()) return;
		int ch=channels();
		int total=ch*samplesPerChannel;
		waveformBus=new ofxSCBus(RATE_CONTROL,total,servers[serverIndex]->getServer());
		waveformData.assign(total,0.f);
		for(int k=0;k<ch;++k){
			auto *s=new ofxSCSynth("bufferscopeSpread1_"+ofToString(samplesPerChannel),servers[serverIndex]->getServer());
			s->addToTail();
			s->run(getActive());
			s->set("buf",bufnums->at(k));
			s->set("out",waveformBus->index + k*samplesPerChannel);
			synths.push_back(s);
		}
		currentGain=1.f; waveformBus->requestValues();
	}

	void drawWaveform(){
		if(waveformData.empty()) return;
		ImDrawList* dl=ImGui::GetWindowDrawList();
		ImVec2 pos=ImGui::GetCursorScreenPos();
		ImVec2 sz=ImGui::GetContentRegionAvail();
		if(sz.x<50) sz.x=800; if(sz.y<50) sz.y=400;
		dl->AddRectFilled(pos,{pos.x+sz.x,pos.y+sz.y},ImGui::ColorConvertFloat4ToU32(ImVec4(backgroundColor->r/255.f,backgroundColor->g/255.f,backgroundColor->b/255.f,backgroundColor->a/255.f)));

		int chs=channels(); float track=sz.y/chs;
		ImU32 lCol=ImGui::ColorConvertFloat4ToU32(ImVec4(lineColor->r/255.f,lineColor->g/255.f,lineColor->b/255.f,1.f));

		const auto &hdr = header.get();
		const auto &th  = headerThickness.get();
		const auto &op  = headerOpacity.get();
		bool sH = hdr.size()==1, sT=th.size()==1, sO=op.size()==1;

		for(int ch=0; ch<chs; ++ch){
			float y0=pos.y+ch*track, mid=y0+track*0.5f;
			// waveform (sample‑major index)
			for(int s=0; s<samplesPerChannel-1; ++s){
				int i1 = ch*samplesPerChannel + s;
				int i2 = ch*samplesPerChannel + (s+1);
				float x1=pos.x + (float)s/(samplesPerChannel-1)*sz.x;
				float x2=pos.x + (float)(s+1)/(samplesPerChannel-1)*sz.x;
				float y1=mid - ofClamp(waveformData[i1]*currentGain,-1.f,1.f)*track*0.45f;
				float y2=mid - ofClamp(waveformData[i2]*currentGain,-1.f,1.f)*track*0.45f;
				dl->AddLine({x1,y1},{x2,y2},lCol,1.1f);
			}
			// header rectangle
			float h  = ofClamp(sH?hdr[0]:(ch<hdr.size()?hdr[ch]:0.f),0.f,1.f);
			float pct = ofClamp(sT?th[0] :(ch<th.size()?th[ch]:0.01f),0.f,1.f);
			float alp = ofClamp(sO?op[0] :(ch<op.size()?op[ch]:1.f),0.f,1.f);
			float xLeft  = pos.x + h * sz.x;
			float xRight = xLeft + pct * sz.x;
			ImU32 hCol = IM_COL32(255,255,255,(int)(alp*128));
			dl->AddRectFilled({xLeft,y0},{xRight,y0+track},hCol);
		}
		ImGui::Dummy(sz);
	}

	// params
	ofParameter<bool> showWindow; ofParameter<std::vector<int>> bufnums; ofParameter<int> serverIndex, samplesDropdown; ofParameter<bool> autoScale; ofParameter<float> manualGain; ofParameter<std::vector<float>> header, headerThickness, headerOpacity; ofParameter<ofColor> lineColor, backgroundColor;

	// runtime
	std::vector<float> waveformData; int samplesPerChannel=256; float currentGain=1.f; std::vector<int> sampSizes;
	// SC objs
	ofxSCBus* waveformBus=nullptr; std::vector<ofxSCSynth*> synths; std::vector<serverManager*> servers; ofEventListeners listeners;
};

#endif /* scBufferscope_h */
