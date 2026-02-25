#ifndef scGrainSamplerGui_h
#define scGrainSamplerGui_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCBuffer.h"
#include "ofxSCServer.h" // Ensure this is included
#include "imgui.h"
#include "imgui_internal.h"
#include <vector>
#include <map>
#include <deque>
#include <algorithm>
#include <cmath>

class scGrainSamplerGui : public scNode {
public:
	scGrainSamplerGui() : scNode("GrainSamplerGui") {
		color = ofColor(255, 120, 0);
		
		// Initialize Default Envelope (Trapezoid-ish)
		envPoints.push_back({0.0f, 0.0f, 0.0f}); // Start
		envPoints.push_back({0.2f, 1.0f, 0.0f}); // Attack
		envPoints.push_back({0.8f, 1.0f, 0.0f}); // Sustain
		envPoints.push_back({1.0f, 0.0f, 0.0f}); // Release
		
		// Render initial data
		renderEnvelopeData();
	}

	~scGrainSamplerGui() {
		cleanupFetcher();
		for(auto& pair : synthInstances) {
			if(pair.second) { pair.second->free(); delete pair.second; }
		}
		for(auto& pair : envBuffers) {
			if(pair.second) { pair.second->free(); delete pair.second; }
		}
		synthInstances.clear();
		envBuffers.clear();
	}

	void setup() override {
		// --- Audio Parameters ---
		addParameter(numChannels.set("N Chan", 1, 1, 64));
		addParameter(bufnum.set("Bufnum", {0}, {0}, {1000}));
		addParameter(trigger.set("Trigger", {0.0f}, {0.0f}, {1.0f}));
		addParameter(startPos.set("Pos", {0.0f}, {0.0f}, {1.0f}));
		addParameter(grainSize.set("Size (s)", {0.1f}, {0.001f}, {2.0f}));
		addParameter(speed.set("Speed", {1.0f}, {-4.0f}, {4.0f}));
		addParameter(jitter.set("Jitter", {0.0f}, {0.0f}, {1.0f}));
		addParameter(gain.set("Gain", {1.0f}, {0.0f}, {2.0f}));
		addParameter(latch.set("Latch", false));

		// --- Visual Layout ---
		addInspectorParameter(width.set("GUI Width", 300.0f, 100.0f, 1200.0f));
		addInspectorParameter(height.set("GUI Height", 240.0f, 100.0f, 800.0f));
		
		// Split UI into two regions
		uiRegion.set("MainUI", [this](){ drawGui(); });
		addCustomRegion(uiRegion, [this](){ drawGui(); });

		scNode::addOutput("Out");

		// --- Listeners ---
		listeners.push(numChannels.newListener([this](int &n){ prevTriggers.assign(n, 0.0f); }));
		
		listeners.push(bufnum.newListener([this](std::vector<int> &b){
			 if(!b.empty()) triggerWaveformFetch(b[0]);
			 sendIntParam("bufnum", b);
		}));

		listeners.push(latch.newListener([this](bool &b){
			std::vector<int> v(numChannels.get(), b ? 1 : 0);
			sendIntParam("latch", v);
		}));

		auto f = [this](const std::string& n, std::vector<float>& v){ sendFloatParam(n, v); };
		listeners.push(trigger.newListener([=](std::vector<float> &v){ f("trigger", v); }));
		listeners.push(startPos.newListener([=](std::vector<float> &v){ f("startpos", v); }));
		listeners.push(grainSize.newListener([=](std::vector<float> &v){ f("grainsize", v); }));
		listeners.push(speed.newListener([=](std::vector<float> &v){ f("speed", v); }));
		listeners.push(jitter.newListener([=](std::vector<float> &v){ f("jitter", v); }));
		listeners.push(gain.newListener([=](std::vector<float> &v){ f("levels", v); }));
	}

	void update(ofEventArgs& args) override {
		// 1. Waveform Fetching
		if(isFetching && fetchBus) {
			fetchTimer++;
			fetchBus->requestValues();
			if(fetchTimer > 5 && !fetchBus->readValues.empty()) {
				waveformData = fetchBus->readValues;
				cleanupFetcher();
			} else if(fetchTimer > 60) cleanupFetcher();
		}

		// 2. Envelope Buffer Upload (Throttled)
		if(envNeedsUpdate) {
			renderEnvelopeData();
			for(auto const& [server, buffer] : envBuffers) {
				if(buffer && server) {
					ofxOscMessage m;
					m.setAddress("/b_setn");
					m.addIntArg(buffer->index);
					m.addIntArg(0); // Start index
					m.addIntArg((int)envData.size()); // Size
					for(float val : envData) m.addFloatArg(val);
					
					// FIXED: Call sendMsg directly on the ofxSCServer object
					server->sendMsg(m);
				}
			}
			envNeedsUpdate = false;
		}

		// 3. Visual Particle System
		updateVisualGrains();
	}

	// --- scNode Lifecycle ---

	void createSynth(ofxSCServer* server) override {
		if(!server) return;
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
		
		// 1. Allocate Envelope Buffer for this server
		if(!envBuffers[server]) {
			envBuffers[server] = new ofxSCBuffer(1024, 1, server);
			envBuffers[server]->alloc();
			envNeedsUpdate = true; // Trigger upload
		}

		// 2. Create Audio Synth
		if(synthInstances[server]) { synthInstances[server]->free(); delete synthInstances[server]; }

		string defName = "GrainSamplerGui" + ofToString(numChannels.get());
		synthInstances[server] = new ofxSCSynth(defName, server);
		auto s = synthInstances[server];
		
		s->set("bufnum", bufnum.get());
		s->set("envbuf", envBuffers[server]->index); // Connect custom envelope!
		s->set("trigger", trigger.get());
		s->set("startpos", startPos.get());
		s->set("grainsize", grainSize.get());
		s->set("speed", speed.get());
		s->set("jitter", jitter.get());
		s->set("levels", gain.get());
		
		std::vector<int> latchVec(numChannels.get(), latch.get() ? 1 : 0);
		s->set("latch", latchVec);
		
		if(outputBuses.count(server) && outputBuses[server].count(0)) s->set("out", outputBuses[server][0]);
		
		s->createAndRun(0, 1, getActive());

		if(waveformData.empty() && bufnum.get().size() > 0) triggerWaveformFetch(bufnum.get()[0]);
	}

	void activate() override {
		for(auto& pair : synthInstances) if(pair.second) pair.second->run(true);
	}

	void deactivate() override {
		for(auto& pair : synthInstances) if(pair.second) pair.second->run(false);
	}

	void free(ofxSCServer* server) override {
		if(!server) return;
		if(synthInstances[server]) {
			synthInstances[server]->free(); delete synthInstances[server];
			synthInstances.erase(server);
		}
		if(envBuffers[server]) {
			envBuffers[server]->free(); delete envBuffers[server];
			envBuffers.erase(server);
		}
		outputBuses.erase(server);
	}

	void setOutputBus(ofxSCServer* server, int index, int bus) override {
		if(!server) return;
		outputBuses[server][index] = bus;
		if(synthInstances[server]) synthInstances[server]->set("out", bus);
	}

private:
	// --- Data Structures ---
	struct EnvPoint {
		float x; // Time 0..1
		float y; // Level 0..1
		float tension; // -1..1 (0 = Linear)
	};

	struct VisualGrain {
		float pos, size, level;
		int channel;
		float lifeTime, maxLife;
	};

	// --- State ---
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	std::map<ofxSCServer*, ofxSCBuffer*> envBuffers;
	
	// Envelope Data
	std::vector<EnvPoint> envPoints;
	std::vector<float> envData; // 1024 samples rendered
	bool envNeedsUpdate = false;
	int selectedPoint = -1;
	
	// Visuals
	std::deque<VisualGrain> visualGrains;
	std::vector<float> prevTriggers;
	std::vector<float> waveformData;
	ofxSCBus* fetchBus = nullptr;
	ofxSCSynth* fetchSynth = nullptr;
	bool isFetching = false;
	int fetchTimer = 0;

	// --- Parameters ---
	ofParameter<int> numChannels;
	ofParameter<std::vector<int>> bufnum;
	ofParameter<std::vector<float>> trigger, startPos, grainSize, speed, jitter, gain;
	ofParameter<bool> latch;
	ofParameter<float> width, height;
	ofParameter<std::function<void()>> uiRegion;

	// --- Logic Helpers ---

	void updateVisualGrains() {
		int n = numChannels.get();
		if(prevTriggers.size() != n) prevTriggers.resize(n, 0.0f);
		float dt = 1.0f/60.0f;
		
		for(int i=0; i<n; i++) {
			float cT = getVal(trigger.get(), i);
			if(cT > 0.5f && prevTriggers[i] <= 0.5f) {
				VisualGrain g;
				float p = getVal(startPos.get(), i);
				float j = getVal(jitter.get(), i);
				g.pos = ofClamp(p + (ofRandom(-0.5f,0.5f)*j), 0.0f, 1.0f);
				g.size = getVal(grainSize.get(), i);
				g.level = getVal(gain.get(), i);
				g.channel = i;
				g.lifeTime = g.maxLife = g.size;
				visualGrains.push_back(g);
			}
			prevTriggers[i] = cT;
		}
		for(auto it=visualGrains.begin(); it!=visualGrains.end();) {
			it->lifeTime -= dt;
			if(it->lifeTime <= 0) it=visualGrains.erase(it); else ++it;
		}
	}

	void renderEnvelopeData() {
		if(envData.size() != 1024) envData.resize(1024);
		
		std::sort(envPoints.begin(), envPoints.end(), [](const EnvPoint& a, const EnvPoint& b){
			return a.x < b.x;
		});

		for(int i=0; i<1024; i++) {
			float normX = (float)i / 1023.0f;
			
			EnvPoint pA = {0,0,0}, pB = {1,0,0};
			bool found = false;
			
			if(envPoints.empty()) { envData[i] = 0; continue; }
			if(normX <= envPoints.front().x) { envData[i] = envPoints.front().y; continue; }
			if(normX >= envPoints.back().x) { envData[i] = envPoints.back().y; continue; }

			for(size_t j=0; j<envPoints.size()-1; j++) {
				if(normX >= envPoints[j].x && normX < envPoints[j+1].x) {
					pA = envPoints[j];
					pB = envPoints[j+1];
					found = true;
					break;
				}
			}
			
			if(found) {
				float segT = (normX - pA.x) / (pB.x - pA.x);
				float tension = pA.tension;
				float exponent = pow(10.0f, -tension);
				float curvedT = pow(segT, exponent);
				envData[i] = pA.y + (pB.y - pA.y) * curvedT;
			}
		}
	}

	// --- GUI ---

	void drawGui() {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = width.get();
		float fullH = height.get();
		float waveH = fullH * 0.5f;
		float envH = fullH * 0.5f;

		// --- 1. WAVEFORM DISPLAY (Top Half) ---
		ImGui::InvisibleButton("##waveform", ImVec2(w, waveH));
		bool waveActive = ImGui::IsItemActive();
		
		dl->AddRectFilled(p, ImVec2(p.x+w, p.y+waveH), IM_COL32(30,30,30,255));
		dl->AddRect(p, ImVec2(p.x+w, p.y+waveH), IM_COL32(80,80,80,255));

		// Waveform
		if(!waveformData.empty()) {
			float midY = p.y + waveH * 0.5f;
			for(size_t i=0; i < waveformData.size()-1; i++) {
				float x1 = p.x + (i/(float)waveformData.size())*w;
				float x2 = p.x + ((i+1)/(float)waveformData.size())*w;
				float y1 = midY - (waveformData[i]*waveH*0.45f);
				float y2 = midY - (waveformData[i+1]*waveH*0.45f);
				dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(80,160,120,200));
			}
		}

		// Active Grains
		for(const auto& g : visualGrains) {
			float x = p.x + g.pos * w;
			float gw = 4.0f;
			ofColor c; c.setHsb((g.channel*30)%255, 180, 255);
			float alpha = g.level * (g.lifeTime/g.maxLife);
			ImU32 col = IM_COL32(c.r, c.g, c.b, (int)(alpha*255));
			dl->AddRectFilled(ImVec2(x-gw, p.y), ImVec2(x+gw, p.y+waveH), col);
		}
		
		// Cursors
		int n = numChannels.get();
		for(int i=0; i<n; i++) {
			float x = p.x + ofClamp(getVal(startPos.get(), i), 0.f, 1.f) * w;
			ofColor c; c.setHsb((i*30)%255, 255, 255);
			dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y+waveH), IM_COL32(c.r,c.g,c.b,200), 2.0f);
		}

		if(waveActive && ImGui::IsMouseDown(0)) {
			float val = ofClamp((ImGui::GetIO().MousePos.x - p.x)/w, 0.0f, 1.0f);
			startPos.set(std::vector<float>(1, val));
		}

		// --- 2. ENVELOPE EDITOR (Bottom Half) ---
		ImVec2 ep = ImVec2(p.x, p.y + waveH);
		ImGui::SetCursorScreenPos(ep);
		ImGui::InvisibleButton("##enveditor", ImVec2(w, envH));
		bool envActive = ImGui::IsItemActive();
		bool envHover = ImGui::IsItemHovered();
		ImVec2 mouse = ImGui::GetIO().MousePos;
		
		// BG
		dl->AddRectFilled(ep, ImVec2(ep.x+w, ep.y+envH), IM_COL32(40,40,45,255));
		dl->AddRect(ep, ImVec2(ep.x+w, ep.y+envH), IM_COL32(100,100,100,255));

		// Draw Filled Curve
		if(envData.size() == 1024) {
			 for(int i=0; i<1023; i++) {
				 float x1 = ep.x + (i/1023.0f)*w;
				 float x2 = ep.x + ((i+1)/1023.0f)*w;
				 float y1 = ep.y + envH - (envData[i]*envH);
				 float y2 = ep.y + envH - (envData[i+1]*envH);
				 dl->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, ep.y+envH), IM_COL32(255,160,50,60));
				 dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255,160,50,180));
			 }
		}

		if(!ImGui::IsMouseDown(0)) selectedPoint = -1;

		// Draw Tension Handles
		for(size_t i=0; i<envPoints.size()-1; i++) {
			EnvPoint& p1 = envPoints[i];
			EnvPoint& p2 = envPoints[i+1];
			ImVec2 scr1 = ImVec2(ep.x + p1.x*w, ep.y + envH - p1.y*envH);
			ImVec2 scr2 = ImVec2(ep.x + p2.x*w, ep.y + envH - p2.y*envH);
			ImVec2 mid = ImVec2((scr1.x+scr2.x)*0.5f, (scr1.y+scr2.y)*0.5f);
			
			float dist = sqrt(pow(mouse.x-mid.x,2) + pow(mouse.y-mid.y,2));
			if(envActive && ImGui::IsMouseDragging(0) && ImGui::GetIO().KeyShift && dist < 20) {
				 float dragY = ImGui::GetIO().MouseDelta.y * 0.01f;
				 p1.tension = ofClamp(p1.tension - dragY, -1.0f, 1.0f);
				 envNeedsUpdate = true;
			}
			dl->AddCircle(mid, 4.0f, IM_COL32(200,200,200,100));
		}

		// Draw Points
		for(size_t i=0; i<envPoints.size(); i++) {
			float px = ep.x + envPoints[i].x * w;
			float py = ep.y + envH - (envPoints[i].y * envH);
			bool hover = (pow(mouse.x-px,2) + pow(mouse.y-py,2)) < 36.0f;
			
			if(envActive && ImGui::IsMouseClicked(0) && hover) selectedPoint = i;
			
			if(selectedPoint == i) {
				if(ImGui::IsMouseDragging(0) && !ImGui::GetIO().KeyShift) {
					float nx = (mouse.x - ep.x) / w;
					float ny = 1.0f - ((mouse.y - ep.y) / envH);
					envPoints[i].y = ofClamp(ny, 0.0f, 1.0f);
					if(i > 0 && i < envPoints.size()-1) {
						envPoints[i].x = ofClamp(nx, envPoints[i-1].x+0.01f, envPoints[i+1].x-0.01f);
					}
					envNeedsUpdate = true;
				}
				dl->AddCircleFilled(ImVec2(px, py), 6.0f, IM_COL32(255,255,255,255));
			} else {
				dl->AddCircleFilled(ImVec2(px, py), 4.0f, IM_COL32(255,255,0,255));
			}
			
			if(hover && ImGui::IsMouseClicked(1) && i > 0 && i < envPoints.size()-1) {
				envPoints.erase(envPoints.begin() + i);
				envNeedsUpdate = true;
			}
		}
		
		if(envHover && ImGui::IsMouseDoubleClicked(0) && selectedPoint == -1) {
			float nx = (mouse.x - ep.x) / w;
			float ny = 1.0f - ((mouse.y - ep.y) / envH);
			envPoints.push_back({ofClamp(nx, 0.0f, 1.0f), ofClamp(ny, 0.0f, 1.0f), 0.0f});
			envNeedsUpdate = true;
		}
	}

	// --- Helper ---
	float getVal(const std::vector<float>& v, int i) { return v.empty() ? 0.0f : v[i % v.size()]; }
	
	void sendFloatParam(const std::string& name, std::vector<float>& v) {
		for(auto& pair : synthInstances) if(pair.second) pair.second->set(name, v);
	}
	void sendIntParam(const std::string& name, std::vector<int>& v) {
		for(auto& pair : synthInstances) if(pair.second) pair.second->set(name, v);
	}
	
	void triggerWaveformFetch(int bNum) {
		if(synthInstances.empty()) return;
		auto server = synthInstances.begin()->first;
		cleanupFetcher();
		int nPoints = 256;
		fetchBus = new ofxSCBus(RATE_CONTROL, nPoints, server);
		fetchSynth = new ofxSCSynth("bufferscopeSegment_" + ofToString(nPoints), server);
		fetchSynth->set("buf", bNum);
		fetchSynth->set("out", fetchBus->index);
		fetchSynth->addToHead();
		isFetching = true;
		fetchTimer = 0;
	}
	void cleanupFetcher() {
		if(fetchSynth) { fetchSynth->free(); delete fetchSynth; fetchSynth = nullptr; }
		if(fetchBus) { fetchBus->free(); delete fetchBus; fetchBus = nullptr; }
		isFetching = false;
	}
};

#endif /* scGrainSamplerGui_h */
