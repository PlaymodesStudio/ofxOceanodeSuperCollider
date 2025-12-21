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
		
		// Set up parameter listeners - FOLLOW CANONICAL PATTERN FROM scSynthdef
		listeners.push(numChannels.newListener([this](int &channels){
			// Recreate synths with proper replacement - EXACTLY like scSynthdef
			for(auto& pair : synthInstances) {
				if(pair.second != nullptr) {
					ofxSCServer* server = pair.first;
					int oldNodeID = pair.second->nodeID;
					
					// Create new synth with replacement action
					ofxSCSynth *newSynth = new ofxSCSynth(getSynthDefName(), server);
					newSynth->create(4, oldNodeID); // 4 = kAddAction_replace
					
					// Delete old synth
					delete pair.second;
					pair.second = newSynth;
					
					// Recreate VU bus for new channel count
					recreateVUBus(server);
					
					// Restore all parameters after synth recreation
					if(vuBuses.count(server) > 0 && vuBuses[server] != nullptr) {
						newSynth->set("vubus", vuBuses[server]->index);
						ofLogNotice("scVUMeter") << "Restored VU bus to index " << vuBuses[server]->index;
					}
					
					newSynth->set("vuattacktime", vuAttack.get());
					newSynth->set("vureleasetime", vuRelease.get());
					ofLogNotice("scVUMeter") << "Restored VU timing parameters";
					
					// Restore input bus
					if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
						for(auto& inputPair : inputBuses[server]) {
							newSynth->set("in", inputPair.second);
							ofLogNotice("scVUMeter") << "Restored input bus to " << inputPair.second;
							break; // Only one input
						}
					}
					
					// Restore output bus
					if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
						newSynth->set("out", outputBuses[server][0]);
						ofLogNotice("scVUMeter") << "Restored output bus to " << outputBuses[server][0];
					}
				}
			}
			
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
		vuMeter.setWithoutEventNotifications(levels);
		
		// Update VU Data output parameter
		if(vuData != nullptr) {
			vuData->getParameter().set(levels);
		}
		
		pair.second->requestValues();
	}
}

string scVUMeter::getSynthDefName() const {
	return "vumeter" + ofToString(numChannels.get());
}

void scVUMeter::buildSynth(ofxSCServer* server) {
	ofLogNotice("scVUMeter") << "========== BUILD SYNTH CALLED ==========";
	ofLogNotice("scVUMeter") << "Building synth for server: " << (server != nullptr ? "valid" : "NULL");
}

void scVUMeter::createSynth(ofxSCServer* server) {
	ofLogNotice("scVUMeter") << "========== CREATE SYNTH CALLED ==========";
	
	if(server == nullptr) {
		ofLogError("scVUMeter") << "Server is NULL!";
		return;
	}
	
	try {
		ofLogNotice("scVUMeter") << "Creating synth with " << numChannels.get() << " channels";
		ofLogNotice("scVUMeter") << "SynthDef name will be: " << getSynthDefName();
		
		// Create synth instance
		if(synthInstances[server] != nullptr) {
			ofLogNotice("scVUMeter") << "Freeing existing synth instance";
			synthInstances[server]->free();
			delete synthInstances[server];
		}
		
		ofLogNotice("scVUMeter") << "Creating new ofxSCSynth...";
		synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
		
		// Create VU bus - EXACTLY like polymixer
		ofLogNotice("scVUMeter") << "Creating VU bus...";
		recreateVUBus(server);
		
		// Create the synth
		ofLogNotice("scVUMeter") << "Calling synth->create()...";
		synthInstances[server]->create();
		ofLogNotice("scVUMeter") << "Synth created with name: " << getSynthDefName();
		
		// Set VU timing parameters - EXACTLY like polymixer
		ofLogNotice("scVUMeter") << "Setting VU timing parameters...";
		synthInstances[server]->set("vuattacktime", vuAttack.get());
		synthInstances[server]->set("vureleasetime", vuRelease.get());
		ofLogNotice("scVUMeter") << "VU timing set - attack: " << vuAttack.get() << "ms, release: " << vuRelease.get() << "ms";
		
		// Set VU bus - EXACTLY like polymixer
		if(vuBuses[server] != nullptr && vuBuses[server]->index >= 0) {
			ofLogNotice("scVUMeter") << "Setting VU bus to index " << vuBuses[server]->index;
			synthInstances[server]->set("vubus", vuBuses[server]->index);
			ofLogNotice("scVUMeter") << "VU bus set successfully";
		} else {
			ofLogError("scVUMeter") << "VU bus is invalid! vuBuses[server]: "
				<< (vuBuses[server] != nullptr ? "exists" : "NULL")
				<< ", index: " << (vuBuses[server] != nullptr ? vuBuses[server]->index : -999);
		}
		
		// Set input bus if already connected
		if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
			for(auto& pair : inputBuses[server]) {
				synthInstances[server]->set("in", pair.second);
				ofLogNotice("scVUMeter") << "Set input bus to " << pair.second;
				break; // Only one input
			}
		}
		
		// Set output bus if already assigned
		if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
			synthInstances[server]->set("out", outputBuses[server][0]);
			ofLogNotice("scVUMeter") << "Set output bus to " << outputBuses[server][0];
		}
		
		ofLogNotice("scVUMeter") << "========== SYNTH CREATION COMPLETE ==========";
		
	} catch(const std::exception& e) {
		ofLogError("scVUMeter") << "========== ERROR IN CREATE SYNTH ==========";
		ofLogError("scVUMeter") << "Exception: " << e.what();
	}
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
	if(server == nullptr || node == nullptr) return;
	
	inputBuses[server][node] = bus;
	ofLogNotice("scVUMeter") << "Input bus set to " << bus;
	
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			synthInstances[server]->set("in", bus);
			ofLogNotice("scVUMeter") << "Synth input set to bus " << bus;
			
			// Restore VU bus and timing parameters after setting input
			if(vuBuses.count(server) > 0 && vuBuses[server] != nullptr) {
				synthInstances[server]->set("vubus", vuBuses[server]->index);
				ofLogNotice("scVUMeter") << "Restored VU bus to index " << vuBuses[server]->index;
			}
			synthInstances[server]->set("vuattacktime", vuAttack.get());
			synthInstances[server]->set("vureleasetime", vuRelease.get());
			ofLogNotice("scVUMeter") << "Restored VU timing parameters";
			
		} catch(const std::exception& e) {
			ofLogError("scVUMeter") << "Error setting input bus: " << e.what();
		}
	}
}

void scVUMeter::setOutputBus(ofxSCServer* server, int index, int bus) {
	if(server == nullptr) return;
	
	outputBuses[server][index] = bus;
	ofLogNotice("scVUMeter") << "Output bus set to " << bus;
	
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			synthInstances[server]->set("out", bus);
			ofLogNotice("scVUMeter") << "Synth output set to bus " << bus;
		} catch(const std::exception& e) {
			ofLogError("scVUMeter") << "Error setting output bus: " << e.what();
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
	if(server == nullptr) return;

	auto it = synthInstances.find(server);
	if(it == synthInstances.end() || it->second == nullptr) {
		return;
	}

	ofxSCSynth* synth = it->second;

	try {
		// Ensure VU bus and timing are correct
		if(vuBuses.count(server) > 0 && vuBuses[server] != nullptr) {
			synth->set("vubus", vuBuses[server]->index);
		}

		synth->set("vuattacktime", vuAttack.get());
		synth->set("vureleasetime", vuRelease.get());

		// Restore input bus (first mapped input, if any)
		if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
			int inBus = inputBuses[server].begin()->second;
			synth->set("in", inBus);
		}

		// Restore output bus (index 0, if assigned)
		if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
			synth->set("out", outputBuses[server][0]);
		}

		// Finally move the synth before the given node in the SC graph
		synth->moveBefore(nodeID);

	} catch(const std::exception& e) {
		ofLogError("scVUMeter") << "Error in moveSynthBefore(): " << e.what();
	}
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
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	const vector<float>& vuLevels = vuMeter.get();
	int numChans = vuLevels.size();
	
	const float widgetW = widgetWidth.get();
	const float totalVUHeight = widgetHeight.get();
	
	// FIXED: Add left margin for channel labels
	const float leftMargin = 20.0f;  // Space for channel numbers
	const float meterWidth = widgetW - leftMargin;
	
	// Account for 1px separators between channels
	const float totalSeparatorHeight = (numChans - 1) * 1.0f;
	const float channelH = (totalVUHeight - totalSeparatorHeight) / numChans;
	const float spacing = 2.0f;
	const float totalHeight = spacing + totalVUHeight + spacing;
	
	ImVec2 vuStart = ImVec2(cursorPos.x, cursorPos.y + spacing);
	ImVec2 vuEnd = ImVec2(vuStart.x + widgetW, vuStart.y + totalVUHeight);
	
	// Background
	drawList->AddRectFilled(vuStart, vuEnd, IM_COL32(15, 15, 15, 255));
	drawList->AddRect(vuStart, vuEnd, IM_COL32(100, 100, 100, 255), 0.0f, 0, 2.0f);
	
	// Initialize peak tracking if needed
	if(peakLevels.size() != numChans) {
		peakLevels.resize(numChans, -60.0f);
		peakDecayTimers.resize(numChans, 0.0f);
	}
	
	// Draw each channel
	float currentY = vuStart.y;
	
	for(int ch = 0; ch < numChans; ch++) {
		float ampLevel = ofClamp(vuLevels[ch], 0.0f, 2.0f);
		float dbLevel = ampToDb(ampLevel);
		
		// Meter starts after left margin
		ImVec2 channelStart = ImVec2(vuStart.x + leftMargin, currentY);
		ImVec2 channelEnd = ImVec2(vuStart.x + widgetW, currentY + channelH);
		
		// Channel background
		drawList->AddRectFilled(channelStart, channelEnd, IM_COL32(25, 25, 25, 255));
		
		// Update peak tracking
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
		
		// Draw peak line
		float peakPosition = dbToVUPosition(peakLevels[ch], -60.0f, 6.0f);
		if(peakPosition > 0.01f) {
			float peakX = channelStart.x + meterWidth * peakPosition;
			unsigned int peakColor = getVUMeterColorDB(peakLevels[ch]);
			drawList->AddLine(
				ImVec2(peakX, channelStart.y),
				ImVec2(peakX, channelEnd.y),
				peakColor,
				2.0f
			);
		}
		
		// Draw 0dB reference line
		float zeroDbPosition = dbToVUPosition(0.0f, -60.0f, 6.0f);
		if(zeroDbPosition > 0.01f && zeroDbPosition < 0.99f) {
			float zeroDbX = channelStart.x + meterWidth * zeroDbPosition;
			drawList->AddLine(
				ImVec2(zeroDbX, channelStart.y),
				ImVec2(zeroDbX, channelEnd.y),
				IM_COL32(255, 255, 255, 220),
				1.5f
			);
		}
		
		// Move to next channel
		currentY += channelH;
		if(ch < numChans - 1) {
			currentY += 1.0f;  // 1px separator
		}
	}
	
	// DRAW ALL TEXT AFTER BACKGROUNDS - prevents clipping
	currentY = vuStart.y;
	for(int ch = 0; ch < numChans; ch++) {
		float ampLevel = ofClamp(vuLevels[ch], 0.0f, 2.0f);
		float dbLevel = ampToDb(ampLevel);
		
		// Channel label - in the left margin area
		char channelLabel[8];
		if(ch == 0) sprintf(channelLabel, "L");
		else if(ch == 1) sprintf(channelLabel, "R");
		else sprintf(channelLabel, "%d", ch + 1);
		
		ImVec2 labelPos = ImVec2(vuStart.x + 4, currentY + 2);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), channelLabel);
		
		// Level value text - on the right edge
		float meterPosition = dbToVUPosition(dbLevel, -60.0f, 6.0f);
		if(meterPosition > 0.1f) {
			char levelText[8];
			sprintf(levelText, "%.1f", dbLevel);
			ImVec2 levelTextSize = ImGui::CalcTextSize(levelText);
			ImVec2 levelTextPos = ImVec2(vuEnd.x - levelTextSize.x - 4, currentY + 2);
			drawList->AddText(levelTextPos, IM_COL32(200, 200, 200, 255), levelText);
		}
		
		// Move to next channel position
		const float channelH = (totalVUHeight - (numChans - 1) * 1.0f) / numChans;
		currentY += channelH;
		if(ch < numChans - 1) {
			currentY += 1.0f;
		}
	}
	
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + totalHeight));
	ImGui::Dummy(ImVec2(widgetW, 4.0f));
}

void scVUMeter::drawSeparator() {
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(p.x, p.y),
		ImVec2(p.x + 240, p.y),
		IM_COL32(200, 200, 200, 255),
		1.0f
	);
	ImGui::Dummy(ImVec2(0, 4));
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
