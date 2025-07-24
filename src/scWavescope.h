#ifndef scWavescope_h
#define scWavescope_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scWavescope : public ofxOceanodeNodeModel {
public:
	scWavescope(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("Wavescope"), servers(outputServers){
		synth = nullptr;
		waveformBus = nullptr;
		samplesPerChannel = 256;
		waveformData.resize(samplesPerChannel, 0.0f);
		frozenWaveformData.resize(samplesPerChannel, 0.0f);
		wasFrozen = false;
	}
	
	~scWavescope(){
		if(synth != nullptr){ synth->free(); delete synth; }
		if(waveformBus != nullptr){ waveformBus->free(); delete waveformBus; }
	}
	
	void setup(){
		addParameter(showWindow.set("Show", false));
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
		addParameter(numChannels.set("N Chan", 1, 1, 40));
		addParameterDropdown(samplesDropdown, "Samples", 2, {"32", "64", "128", "256", "512"});
		addParameter(timeWindow.set("Time Window", 1.0f, 0.001f, 10.0f));
		addParameter(freeze.set("Freeze", false));
		
		// Visual parameters
		addParameter(lineColor.set("Line Color", ofColor(0, 255, 0), ofColor(0), ofColor(255)));
		addParameter(backgroundColor.set("Background", ofColor(0, 0, 0, 180), ofColor(0), ofColor(255)));
		addParameter(gridVisible.set("Show Grid", true));
		addParameter(header.set("Header", {0.f}, {0.f}, {1.f}));
		addParameter(headerThickness.set("HeaderTh", {0.01f}, {0.f}, {1.f}));
		addParameter(headerOpacity.set("HeaderOp", {1.f}, {0.f}, {1.f}));
		
		listeners.push(input.newListener([this](nodePort &port){
			if(port.getNodeRef() != nullptr) recreateSynth();
			else { if(synth){ synth->free(); delete synth; synth = nullptr; }
				   if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; } }
		}));

		listeners.push(serverIndex.newListener([this](int &i){
			if(input->getNodeRef() != nullptr) recreateSynth();
			serverGraphListener.unsubscribe();
			serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){ recreateSynth(); });
		}));

		listeners.push(numChannels.newListener([this](int &i){
			if(input->getNodeRef() != nullptr) recreateSynth();
			updateMaxChannels();
		}));

		listeners.push(samplesDropdown.newListener([this](int &i){
			vector<int> sampleSizes = {32, 64, 128, 256, 512};
			samplesPerChannel = sampleSizes[ofClamp(i, 0, 4)];
			updateMaxChannels();
			if(input->getNodeRef() != nullptr) recreateSynth();
		}));

		listeners.push(timeWindow.newListener([this](float &f){
			if(synth != nullptr) synth->set("timewindow", f);
		}));

		vector<int> sampleSizes = {32, 64, 128, 256, 512};
		samplesPerChannel = sampleSizes[samplesDropdown.get()];
		updateMaxChannels();
	}
	
	void updateMaxChannels(){
		int maxChannels = max(1, 1280 / samplesPerChannel);
		numChannels.setMax(maxChannels);
		if(numChannels.get() > maxChannels) numChannels.set(maxChannels);
	}
	
	void update(ofEventArgs &) override{
		if(!synth || !waveformBus) return;
		if(waveformBus->index < 0) return;

		auto newData = waveformBus->readValues;
		int expectedSize = numChannels.get() * samplesPerChannel;
		
		if(newData.size() != expectedSize){
			newData.resize(expectedSize, 0.0f);
		}

		if(!freeze.get()){
			waveformData = newData;
			wasFrozen = false;
		}else{
			if(!wasFrozen) frozenWaveformData = waveformData;
			wasFrozen = true;
			waveformData = newData;
		}
		waveformBus->requestValues();
	}
	
	void draw(ofEventArgs &) override{
		if(!showWindow) return;
		std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "Wavescope " + ofToString(getNumIdentifier());
		if(ImGui::Begin(title.c_str(), (bool *)&showWindow.get())){
			drawWaveform();
		}
		ImGui::End();
	}
	
	void drawWaveform(){
		vector<float>& dataToDisplay = freeze.get() ? frozenWaveformData : waveformData;
		if(dataToDisplay.empty()) return;

		auto drawList = ImGui::GetWindowDrawList();
		auto canvasPos = ImGui::GetCursorScreenPos();
		auto canvasSize = ImGui::GetContentRegionAvail();
		if(canvasSize.x < 50) canvasSize.x = 800;
		if(canvasSize.y < 50) canvasSize.y = 400;

		int numChans = numChannels.get();
		float trackHeight = canvasSize.y / max(numChans, 1);
		ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
			backgroundColor->r/255.f, backgroundColor->g/255.f,
			backgroundColor->b/255.f, backgroundColor->a/255.f));
		drawList->AddRectFilled(canvasPos,
			ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), bgColor);

		for(int ch = 0; ch < numChans; ch++){
			float trackY = canvasPos.y + ch * trackHeight;
			float trackCenterY = trackY + trackHeight * 0.5f;

			if(gridVisible){
				ImU32 gridColor = IM_COL32(60, 60, 60, 100);
				ImU32 centerLineColor = IM_COL32(120, 120, 120, 150);
				if(ch < numChans - 1){
					float separatorY = trackY + trackHeight;
					drawList->AddLine(ImVec2(canvasPos.x, separatorY),
						ImVec2(canvasPos.x + canvasSize.x, separatorY), IM_COL32(80, 80, 80, 200));
				}
				drawList->AddLine(ImVec2(canvasPos.x, trackCenterY),
					ImVec2(canvasPos.x + canvasSize.x, trackCenterY), centerLineColor);
				for(int i = 1; i < 10; i++){
					float x = canvasPos.x + (canvasSize.x * i / 10);
					drawList->AddLine(ImVec2(x, trackY),
						ImVec2(x, trackY + trackHeight), gridColor);
				}
			}

			ImU32 lineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
				lineColor->r/255.f, lineColor->g/255.f, lineColor->b/255.f, freeze.get() ? 0.8f : 1.0f));
			
			int channelStartIdx = ch * samplesPerChannel;
			for(int i = 0; i < canvasSize.x - 1; i++){
				float sampleProgress = (float)i / (canvasSize.x - 1);
				int sampleIdx1 = channelStartIdx + (int)(sampleProgress * (samplesPerChannel - 1));
				int sampleIdx2 = channelStartIdx + (int)(((float)(i + 1) / (canvasSize.x - 1)) * (samplesPerChannel - 1));
				sampleIdx1 = ofClamp(sampleIdx1, channelStartIdx, channelStartIdx + samplesPerChannel - 1);
				sampleIdx2 = ofClamp(sampleIdx2, channelStartIdx, channelStartIdx + samplesPerChannel - 1);
				if(sampleIdx1 < dataToDisplay.size() && sampleIdx2 < dataToDisplay.size()){
					float s1 = ofClamp(dataToDisplay[sampleIdx1], -1.f, 1.f);
					float s2 = ofClamp(dataToDisplay[sampleIdx2], -1.f, 1.f);
					float y1 = trackCenterY - s1 * trackHeight * 0.45f;
					float y2 = trackCenterY - s2 * trackHeight * 0.45f;
					drawList->AddLine(ImVec2(canvasPos.x + i, y1), ImVec2(canvasPos.x + i + 1, y2), lineCol, 1.5f);
				}
			}

			// HEADER DRAW
			const auto& hdrVec = header.get();
			const auto& thVec = headerThickness.get();
			const auto& opVec = headerOpacity.get();

			float hdr = ofClamp((hdrVec.size() == 1 ? hdrVec[0] : (ch < hdrVec.size() ? hdrVec[ch] : 0.f)), 0.f, 1.f);
			float th = ofClamp((thVec.size() == 1 ? thVec[0] : (ch < thVec.size() ? thVec[ch] : 0.01f)), 0.f, 1.f);
			float op = ofClamp((opVec.size() == 1 ? opVec[0] : (ch < opVec.size() ? opVec[ch] : 1.f)), 0.f, 1.f);

			float xLeft = canvasPos.x + (1.0f - hdr) * canvasSize.x;
			float xRight = xLeft + th * canvasSize.x;


			ImU32 headerCol = IM_COL32(255, 255, 255, int(op * 128));
			drawList->AddRectFilled(ImVec2(xLeft, trackY), ImVec2(xRight, trackY + trackHeight), headerCol);

		}

		if(freeze.get()){
			ImVec2 textPos = ImVec2(canvasPos.x + canvasSize.x - 80, canvasPos.y + 10);
			drawList->AddText(textPos, IM_COL32(255, 200, 100, 255), "FROZEN");
		}
		ImGui::Dummy(canvasSize);
	}
	
	void recreateSynth(){
		int numChans = numChannels;
		if(synth){ synth->free(); delete synth; synth = nullptr; }
		if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; ofSleepMillis(10); }

		if(input->getNodeRef() == nullptr) return;

		int totalBusSize = samplesPerChannel * numChans;
		if(totalBusSize > 1280){
			numChannels.set(1280 / samplesPerChannel);
			return;
		}

		try {
			waveformBus = new ofxSCBus(RATE_CONTROL, totalBusSize, servers[serverIndex]->getServer());
			if(waveformBus->index < 0){ delete waveformBus; waveformBus = nullptr; return; }
			synth = new ofxSCSynth("wavescope" + ofToString(numChans) + "_" + ofToString(samplesPerChannel), servers[serverIndex]->getServer());
			synth->addToTail();
			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
			synth->set("out", waveformBus->index);
			synth->set("timewindow", timeWindow);

			int expectedDataSize = numChans * samplesPerChannel;
			waveformData.resize(expectedDataSize, 0.0f);
			frozenWaveformData.resize(expectedDataSize, 0.0f);

		} catch (const std::exception &e) {
			if(synth){ synth->free(); delete synth; synth = nullptr; }
			if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; }
		}
	}

private:
	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<int> samplesDropdown;
	ofParameter<bool> showWindow;

	ofParameter<float> timeWindow;
	ofParameter<bool> freeze;

	ofParameter<ofColor> lineColor;
	ofParameter<ofColor> backgroundColor;
	ofParameter<bool> gridVisible;

	ofParameter<vector<float>> header;
	ofParameter<vector<float>> headerThickness;
	ofParameter<vector<float>> headerOpacity;

	vector<float> waveformData;
	vector<float> frozenWaveformData;
	int samplesPerChannel;
	bool wasFrozen;

	ofxSCBus* waveformBus = nullptr;
	ofxSCSynth* synth = nullptr;
	vector<serverManager*> servers;
};

#endif /* scWavescope_h */
