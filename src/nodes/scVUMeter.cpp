//
//  scVUMeter.cpp
//  ofxOceanodeSupercollider
//
//  Simple VU meter - EXTRACTED FROM WORKING POLYMIXER
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scVUMeter.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSuperCollider.h"
#include "imgui.h"
#include "ofxOceanodeShared.h"

scVUMeter::scVUMeter() : scNode("VUMeter") {
	ofLogNotice("scVUMeter") << "========== CONSTRUCTOR CALLED ==========";
}

scVUMeter::~scVUMeter() {
	try {
		listeners.unsubscribeAll();
		
		// Free all synth instances
		for(auto& pair : synthInstances) {
			if(pair.second != nullptr) {
				pair.second->free();
				delete pair.second;
			}
		}
		synthInstances.clear();
		
		// Free all VU buses
		for(auto& pair : vuBuses) {
			if(pair.second != nullptr) {
				pair.second->free();
				delete pair.second;
			}
		}
		vuBuses.clear();
		
		inputBuses.clear();
		
	} catch(const std::exception& e) {
		ofLogError("scVUMeter") << "Error in destructor: " << e.what();
	}
}

void scVUMeter::setup() {
	ofLogNotice("scVUMeter") << "========== SETUP CALLED ==========";
	
	try {
		// Core parameters - EXACTLY like polymixer
		addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));
		
		addCustomRegion(
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
		);
		
		// VU Meter - hidden from regular GUI, EXACTLY like polymixer
		addInspectorParameter(vuMeter.set("VU", vector<float>(2, 0.0f),
										 vector<float>(2, 0.0f),
										 vector<float>(2, 1.0f)));
		
		// VU Attack/Release parameters
		addInspectorParameter(vuAttack.set("VU Attack", 10.0f, 1.0f, 100.0f));
		addInspectorParameter(vuRelease.set("VU Release", 300.0f, 10.0f, 2000.0f));
		
		// Widget size parameters
		addInspectorParameter(widgetWidth.set("Widget Width", 240.0f, 100.0f, 800.0f));
		addInspectorParameter(widgetHeight.set("VU Height", 100.0f, 20.0f, 1900.0f)); // Changed name and default
		
		// VU Data output parameter - EXACTLY like polymixer
		vector<float> defaultVUData(2, 0.0f);
		auto vuDataParam = std::make_shared<ofParameter<vector<float>>>();
		vuDataParam->set("VU Data", defaultVUData,
						vector<float>(2, 0.0f),
						vector<float>(2, 1.0f));
		vuData = addOutputParameter(*vuDataParam);
		
		// Add single input
		scNode::addInput("In");
		
		// Add passthrough output - needed for graph to determine server
		scNode::addOutput("Out");
		
		// CRITICAL: Add resendParams listener (following scSynthDef pattern)
		listeners.push(resendParams.newListener([this](){
			for(auto synthServer : synthInstances){
				if(synthServer.second != nullptr){
					// Set VU-specific parameters
					if(vuBuses.count(synthServer.first) > 0 && vuBuses[synthServer.first] != nullptr){
						synthServer.second->set("vubus", vuBuses[synthServer.first]->index);
					}
					synthServer.second->set("vuattacktime", vuAttack.get());
					synthServer.second->set("vureleasetime", vuRelease.get());
				}
			}
			for(auto synthServer : synthInstances){
				for(int i = 0; i < inputs.size(); i++){
					if(inputBuses[synthServer.first].count(inputs[i]->getNodeRef()) == 1){
						string paramName = ofToLower(inputs[i].getName());
						if(synthServer.second != nullptr){
							synthServer.second->set(paramName, inputBuses[synthServer.first][inputs[i]->getNodeRef()]);
						}
					}
				}
				for(int i = 0; i < outputs.size(); i++){
					if(outputBuses[synthServer.first].count(outputs[i]->getIndex()) == 1){
						string paramName = ofToLower(outputs[i].getName());
						if(synthServer.second != nullptr){
							synthServer.second->set(paramName, outputBuses[synthServer.first][outputs[i]->getIndex()]);
						}
					}
				}
			}
		}));
		
		// Set up parameter listeners - FOLLOW CANONICAL PATTERN FROM scSynthdef
		listeners.push(numChannels.newListener([this](int &channels){
			if(channels < 1 || channels > MAX_NODE_CHANNELS) return;
			// Recreate synths with proper replacement - EXACTLY like scSynthdef
			for(auto &synth : synthInstances){
				ofxSCSynth *newSynth = new ofxSCSynth(getSynthDefName(), synth.first);
				newSynth->createAndRun(4, synth.second->nodeID, getActive()); // 4 = kAddAction_replace
				delete synth.second;
				synth.second = newSynth;
				
				// Recreate VU bus for new channel count
				recreateVUBus(synth.first);
			}
			
			// Use event pattern to restore ALL parameters (following scSynthDef)
			resendParams.notify();
			
			// CRITICAL: Trigger graph recomputation - EXACTLY like scSynthdef
			for(auto& output : outputs) {
				output = output;
			}
			
			// Resize UI parameters
			vector<float> newVU(channels, 0.0f);
			vuMeter.set(newVU);
			
			if(vuData != nullptr) {
				vuData->getParameter().set(newVU);
			}
			
			peakLevels.resize(channels, -60.0f);
			peakDecayTimers.resize(channels, 0.0f);
			
			stickyMaxPeaks.assign(channels, -60.0f);
			if(maxPeaksOutput) maxPeaksOutput->getParameter().set(stickyMaxPeaks);
		}));
		
		listeners.push(vuAttack.newListener([this](float &attackTime){
			updateVUTiming(attackTime, -1.0f);
		}));
		
		listeners.push(vuRelease.newListener([this](float &releaseTime){
			updateVUTiming(-1.0f, releaseTime);
		}));
		
		// Initialize peak tracking
		peakLevels.resize(numChannels.get(), -60.0f);
		peakDecayTimers.resize(numChannels.get(), 0.0f);
		
		addParameter(clearPeaks.set("Clear Max"));

		// Initialize sticky peaks vector
		stickyMaxPeaks.assign(numChannels.get(), -60.0f);

		// Create the Max Peaks output
		vector<float> defaultMax(numChannels.get(), -60.0f);
		auto maxPeaksParam = std::make_shared<ofParameter<vector<float>>>();
		maxPeaksParam->set("Max Peaks", defaultMax, vector<float>(numChannels.get(), -60.0f), vector<float>(numChannels.get(), 6.0f));
		maxPeaksOutput = addOutputParameter(*maxPeaksParam);

		// Listener for the clear button
		listeners.push(clearPeaks.newListener([this](){
			stickyMaxPeaks.assign(numChannels.get(), -60.0f);
			if(maxPeaksOutput) maxPeaksOutput->getParameter().set(stickyMaxPeaks);
		}));
		
		// Add VU meter widget - EXACTLY like polymixer master VU
		addCustomRegion(
			ofParameter<std::function<void()>>().set("VU Display", [this](){
				drawVUWidget();
			}),
			ofParameter<std::function<void()>>().set("VU Display", [this](){
				drawVUWidget();
			})
		);
		
	} catch(const std::exception& e) {
		ofLogError("scVUMeter") << "Error in setup(): " << e.what();
		throw;
	}
}

void scVUMeter::update(ofEventArgs &args) {
	// EXACTLY like polymixer update logic for VU meters
	for(auto& pair : vuBuses) {
		ofxSCServer* server = pair.first;
		if(server == nullptr || pair.second == nullptr) continue;

		vector<float> levels = pair.second->readValues;

		// Sanitize: replace NaN/inf values with 0.0f to prevent downstream corruption
		for(auto& v : levels) {
			if(!std::isfinite(v)) v = 0.0f;
		}

		vuMeter.setWithoutEventNotifications(levels);

		// Update VU Data output parameter
		if(vuData != nullptr) {
			vuData->getParameter().set(levels);
		}

		pair.second->requestValues();
	}
}

void scVUMeter::activate(){
	for(auto &synth : synthInstances) synth.second->run(true);
}

void scVUMeter::deactivate(){
	for(auto &synth : synthInstances) synth.second->run(false);
}

string scVUMeter::getSynthDefName() const {
	return "vumeter" + ofToString(numChannels.get());
}

void scVUMeter::buildSynth(ofxSCServer* server) {
	// Following scSynthDef canonical pattern
	synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

void scVUMeter::createSynth(ofxSCServer* server) {
	// Following scSynthDef canonical pattern
	if(synthInstances.count(server) == 0) return;
	
	// Create VU bus before notifying params
	recreateVUBus(server);
	
	// Use event pattern to set all parameters
	resendParams.notify();
	
	synthInstances[server]->createAndRun(0, 1, getActive());
}

void scVUMeter::free(ofxSCServer* server) {
	if(server == nullptr) return;
	
	try {
		if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
			synthInstances[server]->free();
			delete synthInstances[server];
			synthInstances.erase(server);
		}
		
		if(vuBuses.count(server) > 0 && vuBuses[server] != nullptr) {
			vuBuses[server]->free();
			delete vuBuses[server];
			vuBuses.erase(server);
		}
		
		inputBuses.erase(server);
		
	} catch(const std::exception& e) {
		ofLogError("scVUMeter") << "Error in free(): " << e.what();
	}
}

void scVUMeter::setInputBus(ofxSCServer* server, scNode* node, int bus) {
	// Following scSynthDef canonical pattern
	inputBuses[server][node] = bus;
	for(int i = 0; i < inputs.size(); i++){
		if(inputs[i]->getNodeRef() == node){
			string paramName = ofToLower(inputs[i].getName());
			if(synthInstances.count(server) != 0){
				synthInstances[server]->set(paramName, bus);
			}
		}
	}
}

void scVUMeter::resetInputBusses(ofxSCServer* server, int targetBus) {
	// Following scSynthDef canonical pattern
	if(synthInstances.count(server) == 0) return;
	inputBuses[server].clear();
	for(int i = 0; i < inputs.size(); i++){
		string paramName = ofToLower(inputs[i].getName());
		synthInstances[server]->set(paramName, targetBus);
	}
	// Note: VUMeter doesn't currently have audio-rate parameters,
	// but following canonical pattern for future compatibility
	auto args = std::make_pair(server, targetBus);
	resetAudioRateBusAssignments.notify(args);
}

void scVUMeter::setOutputBus(ofxSCServer* server, int index, int bus) {
	// Following scSynthDef canonical pattern
	outputBuses[server][index] = bus;
	for(int i = 0; i < outputs.size(); i++){
		if(outputs[i]->getIndex() == index){
			string paramName = ofToLower(outputs[i].getName());
			if(synthInstances.count(server) != 0){
				synthInstances[server]->set(paramName, bus);
			}
		}
	}
}

int scVUMeter::getOutputBusIndex(ofxSCServer* server, int index) {
	if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
		return outputBuses[server][index];
	}
	return -1;
}

void scVUMeter::moveSynthBefore(ofxSCServer* server, int nodeID) {
	// Following scSynthDef canonical pattern
	if(synthInstances.count(server) == 0) return;
	resendParams.notify();
	synthInstances[server]->moveBefore(nodeID);
}

int scVUMeter::getLastSynthID(ofxSCServer* server) {
	if(server == nullptr) return -1;

	auto it = synthInstances.find(server);
	if(it != synthInstances.end() && it->second != nullptr) {
		return it->second->nodeID;
	}
	return -1;
}

void scVUMeter::recreateVUBus(ofxSCServer* server) {
	if(server == nullptr) return;
	
	// Clean up existing bus
	if(vuBuses.count(server) > 0 && vuBuses[server] != nullptr) {
		vuBuses[server]->free();
		delete vuBuses[server];
	}
	
	// EXACTLY like polymixer recreateVUBuses
	try {
		int channelCount = numChannels.get();
		ofxSCBus* newBus = new ofxSCBus(RATE_CONTROL, channelCount, server);
		
		if(newBus != nullptr && newBus->index >= 0 && newBus->index < 4096) {
			if(server->controlBusses[newBus->index] != nullptr) {
				vuBuses[server] = newBus;
				newBus->requestValues();
				ofLogNotice("scVUMeter") << "Created VU bus with index " << newBus->index
					<< " for " << channelCount << " channels";
			} else {
				delete newBus;
				vuBuses[server] = nullptr;
				ofLogError("scVUMeter") << "Server controlBusses array invalid";
			}
		} else {
			if(newBus != nullptr) delete newBus;
			vuBuses[server] = nullptr;
		}
	} catch(...) {
		vuBuses[server] = nullptr;
	}
}

void scVUMeter::updateVUTiming(float attackTime, float releaseTime) {
	// EXACTLY like polymixer updateTrackVUTiming
	for(auto& pair : synthInstances) {
		if(pair.second != nullptr) {
			try {
				if(attackTime >= 0.0f) {
					pair.second->set("vuattacktime", attackTime);
				}
				if(releaseTime >= 0.0f) {
					pair.second->set("vureleasetime", releaseTime);
				}
			} catch(const std::exception& e) {
				ofLogError("scVUMeter") << "Error setting VU timing: " << e.what();
			}
		}
	}
}

void scVUMeter::drawVUWidget() {
	float zoom = ofxOceanodeShared::getZoomLevel();

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();

	const vector<float>& vuLevels = vuMeter.get();
	int numChans = vuLevels.size();

	const float widgetW = widgetWidth.get() * zoom;
	const float totalVUHeight = widgetHeight.get() * zoom;

	const float leftMargin = 20.0f * zoom;
	const float meterWidth = widgetW - leftMargin;

	const float totalSeparatorHeight = (numChans - 1) * 1.0f * zoom;
	const float channelH = (totalVUHeight - totalSeparatorHeight) / numChans;
	const float spacing = 2.0f * zoom;
	const float totalHeight = spacing + totalVUHeight + spacing;
	
	ImVec2 vuStart = ImVec2(cursorPos.x, cursorPos.y + spacing);
	ImVec2 vuEnd = ImVec2(vuStart.x + widgetW, vuStart.y + totalVUHeight);
	
	// Background
	drawList->AddRectFilled(vuStart, vuEnd, IM_COL32(15, 15, 15, 255));
	drawList->AddRect(vuStart, vuEnd, IM_COL32(100, 100, 100, 255), 0.0f, 0, 2.0f);
	
	if(peakLevels.size() != numChans) {
		peakLevels.resize(numChans, -60.0f);
		peakDecayTimers.resize(numChans, 0.0f);
	}
	
	float currentY = vuStart.y;
	
	for(int ch = 0; ch < numChans; ch++) {
		float ampLevel = ofClamp(vuLevels[ch], 0.0f, 2.0f);
		float dbLevel = ampToDb(ampLevel);
		
		ImVec2 channelStart = ImVec2(vuStart.x + leftMargin, currentY);
		ImVec2 channelEnd = ImVec2(vuStart.x + widgetW, currentY + channelH);
		
		drawList->AddRectFilled(channelStart, channelEnd, IM_COL32(25, 25, 25, 255));
		
		float releaseMs = vuRelease.get();
		float releaseCoeff = 1.0f - expf(-1000.0f / (releaseMs * 60.0f));
		
		if(dbLevel > peakLevels[ch]) {
			peakLevels[ch] = dbLevel;
			peakDecayTimers[ch] = releaseMs * 5.0f;
		} else {
			peakDecayTimers[ch] -= 16.67f;
			if(peakDecayTimers[ch] <= 0) {
				peakLevels[ch] -= releaseCoeff * 0.5f;
				peakLevels[ch] = ofClamp(peakLevels[ch], -60.0f, 6.0f);
			}
		}
		
		// Draw level meter
		if(dbLevel > -60.0f) {
			float meterPosition = dbToVUPosition(dbLevel, -60.0f, 6.0f);
			float meterW = meterWidth * meterPosition;
			ImVec2 meterEnd = ImVec2(channelStart.x + meterW, channelEnd.y);
			unsigned int meterColor = getVUMeterColorDB(dbLevel);
			drawList->AddRectFilled(channelStart, meterEnd, meterColor);
		}
		
		if(dbLevel > stickyMaxPeaks[ch]) {
			stickyMaxPeaks[ch] = dbLevel;
			if(maxPeaksOutput) {
				maxPeaksOutput->getParameter().set(stickyMaxPeaks);
			}
		}
		
		// Draw peak line
		float peakPosition = dbToVUPosition(peakLevels[ch], -60.0f, 6.0f);
		if(peakPosition > 0.01f) {
			float peakX = channelStart.x + meterWidth * peakPosition;
			unsigned int peakColor = getVUMeterColorDB(peakLevels[ch]);
			drawList->AddLine(ImVec2(peakX, channelStart.y), ImVec2(peakX, channelEnd.y), peakColor, 2.0f);
		}
		
		// 0dB reference
		float zeroDbPosition = dbToVUPosition(0.0f, -60.0f, 6.0f);
		if(zeroDbPosition > 0.01f && zeroDbPosition < 0.99f) {
			float zeroDbX = channelStart.x + meterWidth * zeroDbPosition;
			drawList->AddLine(ImVec2(zeroDbX, channelStart.y), ImVec2(zeroDbX, channelEnd.y), IM_COL32(255, 255, 255, 220), 1.5f);
		}
		
		// Sticky Peak line
		float stickyPos = dbToVUPosition(stickyMaxPeaks[ch], -60.0f, 6.0f);
		if(stickyPos > 0.0f) {
			float stickyX = channelStart.x + meterWidth * stickyPos;
			drawList->AddLine(ImVec2(stickyX, channelStart.y), ImVec2(stickyX, channelEnd.y), IM_COL32(255, 105, 180, 255), 2.0f);
		}
		
		currentY += channelH;
		if(ch < numChans - 1) currentY += 1.0f * zoom;
	}

	// TEXT RENDERING
	currentY = vuStart.y;
	for(int ch = 0; ch < numChans; ch++) {
		// Channel Labels (L/R/Num)
		char channelLabel[8];
		if(ch == 0) sprintf(channelLabel, "L");
		else if(ch == 1) sprintf(channelLabel, "R");
		else sprintf(channelLabel, "%d", ch + 1);

		ImVec2 labelPos = ImVec2(vuStart.x + 4 * zoom, currentY + 2 * zoom);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), channelLabel);
		
		// ONLY show DB text for Sticky Peaks, positioned next to the pink line
		float stickyDb = stickyMaxPeaks[ch];
		if(stickyDb > -60.0f) {
			char levelText[8];
			sprintf(levelText, "%.1f", stickyDb);
			ImVec2 levelTextSize = ImGui::CalcTextSize(levelText);
			
			float stickyPos = dbToVUPosition(stickyDb, -60.0f, 6.0f);
			float stickyX = (vuStart.x + leftMargin) + meterWidth * stickyPos;
			
			// Position text slightly offset from the line
			// If the line is at the very end, move text to the left of the line
			float xOffset = (stickyPos > 0.8f) ? -(levelTextSize.x + 4) : 4;
			ImVec2 levelTextPos = ImVec2(stickyX + xOffset, currentY + 2);
			
			// Draw text in Lighter Pink
			drawList->AddText(levelTextPos, IM_COL32(255, 182, 193, 255), levelText);
		}
		
		const float channelH2 = (totalVUHeight - (numChans - 1) * 1.0f * zoom) / numChans;
		currentY += channelH2;
		if(ch < numChans - 1) currentY += 1.0f * zoom;
	}

	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + totalHeight));
	ImGui::Dummy(ImVec2(widgetW, 4.0f * zoom));
}

void scVUMeter::drawSeparator() {
	float zoom = ofxOceanodeShared::getZoomLevel();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(p.x, p.y),
		ImVec2(p.x + 240 * zoom, p.y),
		IM_COL32(200, 200, 200, 255),
		1.0f
	);
	ImGui::Dummy(ImVec2(0, 4 * zoom));
}

float scVUMeter::ampToDb(float amp) {
	if(amp <= 0.0f) return -60.0f;
	return 20.0f * log10f(amp);
}

float scVUMeter::dbToVUPosition(float db, float minDb, float maxDb) {
	if(db <= minDb) return 0.0f;
	if(db >= maxDb) return 1.0f;
	return (db - minDb) / (maxDb - minDb);
}

unsigned int scVUMeter::getVUMeterColorDB(float dbLevel) {
	if(dbLevel < -18.0f) {
		float greenFactor = (dbLevel + 60.0f) / 42.0f;
		greenFactor = ofClamp(greenFactor, 0.0f, 1.0f);
		int green = (int)(50 + (200 * greenFactor));
		return IM_COL32(0, green, 0, 255);
	}
	else if(dbLevel < -6.0f) {
		float yellowFactor = (dbLevel + 18.0f) / 12.0f;
		yellowFactor = ofClamp(yellowFactor, 0.0f, 1.0f);
		int red = (int)(200 * yellowFactor);
		int green = 250;
		return IM_COL32(red, green, 0, 255);
	}
	else if(dbLevel < 0.0f) {
		float orangeFactor = (dbLevel + 6.0f) / 6.0f;
		orangeFactor = ofClamp(orangeFactor, 0.0f, 1.0f);
		int red = (int)(200 + (55 * orangeFactor));
		int green = (int)(250 - (100 * orangeFactor));
		return IM_COL32(red, green, 0, 255);
	}
	else {
		float redFactor = ofClamp(dbLevel / 6.0f, 0.0f, 1.0f);
		int red = 255;
		int green = (int)(150 - (150 * redFactor));
		return IM_COL32(red, green, 0, 255);
	}
}
