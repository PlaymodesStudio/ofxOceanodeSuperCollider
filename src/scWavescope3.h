#ifndef scWavescope3_h
#define scWavescope3_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "imgui.h"

class scWavescope3 : public ofxOceanodeNodeModel {
public:
	scWavescope3(vector<serverManager*> outputServers)
		: ofxOceanodeNodeModel("Wavescope3"), servers(outputServers) {
		synth = nullptr;
		displayBufferSize = 64;
	}
	
	~scWavescope3() {
		cleanup();
	}
	
	void setup() {
		addParameter(showWindow.set("Show", false));
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
		addParameter(numChannels.set("Channels", 1, 1, 16));
		addParameter(timeWindow.set("Time Window", 0.01, 0.001, 5.0));
		
		addParameter(lineColor.set("Line Color", ofColor(0, 255, 0), ofColor(0), ofColor(255)));
		addParameter(backgroundColor.set("Background", ofColor(0, 0, 0, 180), ofColor(0), ofColor(255)));
		addParameter(gridVisible.set("Grid", true));
		addParameter(freeze.set("Freeze", false));
		
		listeners.push(input.newListener([this](nodePort &port){
			if(port.getNodeRef() != nullptr) {
				recreateSynth();
			} else {
				cleanup();
			}
		}));
		
		listeners.push(serverIndex.newListener([this](int &i){
			if(input->getNodeRef() != nullptr) {
				recreateSynth();
			}
			serverGraphListener.unsubscribe();
			serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){
				recreateSynth();
			});
		}));
		
		listeners.push(numChannels.newListener([this](int &i){
			if(input->getNodeRef() != nullptr) {
				recreateSynth();
			}
		}));
		
		// NEW: Time window listener
		listeners.push(timeWindow.newListener([this](float &time){
			if(synth != nullptr) {
				synth->set("timeWindow", time);
			}
		}));
	}
	
	void update(ofEventArgs &) override {
		if(synth != nullptr && !controlBuses.empty()) {
			int numChans = numChannels.get();
			displayBuffer.resize(numChans * displayBufferSize, 0.0f);
			
			if(!freeze.get()) {
				// Detect allocation order: ascending or descending?
				bool allocatedAscending = (controlBuses.size() > 1 &&
										  controlBuses[1]->index > controlBuses[0]->index);
				
				if(allocatedAscending) {
					// Normal case: buses allocated 0,1,2,3...
					for(int i = 0; i < controlBuses.size() && i < displayBuffer.size(); i++) {
						if(controlBuses[i] != nullptr) {
							displayBuffer[i] = controlBuses[i]->readValues[0];
						}
					}
				} else {
					// Reverse case: buses allocated 383,382,381...
					// SuperCollider expects lowest index first, so reverse the mapping
					for(int i = 0; i < controlBuses.size() && i < displayBuffer.size(); i++) {
						if(controlBuses[i] != nullptr) {
							int reverseIndex = controlBuses.size() - 1 - i;
							displayBuffer[i] = controlBuses[reverseIndex]->readValues[0];
						}
					}
				}
			}
			
			for(auto bus : controlBuses) {
				if(bus != nullptr) bus->requestValues();
			}
		}
	}
	
	void draw(ofEventArgs &) override {
		if(!showWindow) return;
		
		string modCanvasID = canvasID == "Canvas" ? "" : (canvasID + "/");
		string title = modCanvasID + "Wavescope3 " + ofToString(getNumIdentifier());
		
		if(ImGui::Begin(title.c_str(), (bool *)&showWindow.get())) {
			drawWaveform();
		}
		ImGui::End();
	}
	
private:
	ofParameter<float> timeWindow;

	void drawWaveform() {
		if(displayBuffer.empty()) return;
		
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
		
		for(int ch = 0; ch < numChans; ch++) {
			float trackY = canvasPos.y + ch * trackHeight;
			float trackCenterY = trackY + trackHeight * 0.5f;
			
			if(gridVisible) {
				drawList->AddLine(ImVec2(canvasPos.x, trackCenterY),
					ImVec2(canvasPos.x + canvasSize.x, trackCenterY),
					IM_COL32(120, 120, 120, 150));
				
				if(ch < numChans - 1) {
					float separatorY = trackY + trackHeight;
					drawList->AddLine(ImVec2(canvasPos.x, separatorY),
						ImVec2(canvasPos.x + canvasSize.x, separatorY),
						IM_COL32(80, 80, 80, 200));
				}
			}
			
			ImU32 lineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
				lineColor->r/255.f, lineColor->g/255.f, lineColor->b/255.f,
				freeze.get() ? 0.8f : 1.0f));
			
			int samplesPerChannel = displayBufferSize;
			int channelOffset = ch * samplesPerChannel;
			
			for(int x = 0; x < canvasSize.x - 1; x++) {
				float progress1 = (float)x / (canvasSize.x - 1);
				float progress2 = (float)(x + 1) / (canvasSize.x - 1);
				
				int idx1 = channelOffset + (int)(progress1 * (samplesPerChannel - 1));
				int idx2 = channelOffset + (int)(progress2 * (samplesPerChannel - 1));
				
				if(idx1 < displayBuffer.size() && idx2 < displayBuffer.size()) {
					float s1 = ofClamp(displayBuffer[idx1], -1.f, 1.f);
					float s2 = ofClamp(displayBuffer[idx2], -1.f, 1.f);
					
					float y1 = trackCenterY - s1 * trackHeight * 0.45f;
					float y2 = trackCenterY - s2 * trackHeight * 0.45f;
					
					drawList->AddLine(ImVec2(canvasPos.x + x, y1),
						ImVec2(canvasPos.x + x + 1, y2), lineCol, 1.5f);
				}
			}
		}
		
		if(freeze.get()) {
			ImVec2 textPos = ImVec2(canvasPos.x + canvasSize.x - 80, canvasPos.y + 10);
			drawList->AddText(textPos, IM_COL32(255, 200, 100, 255), "FROZEN");
		}
		
		ImGui::Dummy(canvasSize);
	}
	
	void recreateSynth() {
		cleanup();
		
		if(input->getNodeRef() == nullptr) return;
		
		int numChans = numChannels.get();
		int totalBuses = numChans * displayBufferSize;
		
		controlBuses.resize(totalBuses);
		for(int i = 0; i < totalBuses; i++) {
			controlBuses[i] = new ofxSCBus(RATE_CONTROL, 1, servers[serverIndex]->getServer());
		}
		
		int lowestBusIndex = controlBuses[0]->index;
		int highestBusIndex = controlBuses[0]->index;
		for(auto bus : controlBuses) {
			if(bus->index < lowestBusIndex) {
				lowestBusIndex = bus->index;
			}
			if(bus->index > highestBusIndex) {
				highestBusIndex = bus->index;
			}
		}
		
		bool allocatedAscending = (controlBuses.size() > 1 &&
								  controlBuses[1]->index > controlBuses[0]->index);
		
		ofLogNotice("scWavescope3") << "Created " << numChans << " channels, "
			<< totalBuses << " buses";
		ofLogNotice("scWavescope3") << "Bus range: " << lowestBusIndex << " to " << highestBusIndex;
		ofLogNotice("scWavescope3") << "Allocation order: " << (allocatedAscending ? "ascending" : "descending");
		
		synth = new ofxSCSynth("wavescope3_" + ofToString(numChans),
			servers[serverIndex]->getServer());
		synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
		synth->set("out", lowestBusIndex);
		synth->set("timeWindow", timeWindow.get()); // NEW: Set initial value
		synth->addToTail();
		
		displayBuffer.resize(totalBuses, 0.0f);
	}
	
	void cleanup() {
		if(synth) {
			synth->free();
			delete synth;
			synth = nullptr;
		}
		for(auto bus : controlBuses) {
			if(bus) {
				bus->free();
				delete bus;
			}
		}
		controlBuses.clear();
	}
	
	ofEventListeners listeners;
	ofEventListener serverGraphListener;
	
	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<bool> showWindow;
	ofParameter<bool> freeze;
	ofParameter<ofColor> lineColor;
	ofParameter<ofColor> backgroundColor;
	ofParameter<bool> gridVisible;
	
	vector<ofxSCBus*> controlBuses;
	ofxSCSynth* synth;
	
	int displayBufferSize;
	vector<float> displayBuffer;
	vector<serverManager*> servers;
};

#endif /* scWavescope3_h */
