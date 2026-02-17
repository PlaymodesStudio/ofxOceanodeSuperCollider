//
//  scPolyMixer.cpp
//  ofxOceanodeSupercollider
//
//  Multi-track mixer with dynamic track count and multi-instancing
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scPolyMixer.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSuperCollider.h"
#include "imgui.h"

scPolyMixer::scPolyMixer() : scNode("PolyMixerTrack") {
	isUpdatingTracks = false;
}

scPolyMixer::~scPolyMixer() {
	
	try {
		// Stop VU meter updates
		stopVUMeterUpdates();
		
		// Clear event listeners first
		listeners.unsubscribeAll();
		
		// Remove all dynamic track parameters
		removeAllTrackParameters();
		
		// Free all track instances
		freeAll();
		
		// Clear all maps
		outputBuses.clear();
		inputBuses.clear();
		soloedTracks.clear();
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in destructor: " << e.what();
	} catch(...) {
		ofLogError("scPolyMixer") << "Unknown error in destructor";
	}
	
}

void scPolyMixer::setup() {
	ofLogNotice("scPolyMixer") << "=== POLYMIXER SETUP - NEW CODE VERSION ===";
	
	try {
		// Core parameters
		addParameter(numTracks.set("Num Tracks", 4, 1, 12)); // Limit to 12 tracks to prevent graphics crashes
		addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));

		addCustomRegion(
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
		);
		
		addParameter(masterLevel.set("Master Level", vector<float>(1, 1.0f),  // Default 1.0
									vector<float>(1, 0.0f),
									vector<float>(1, 2.0f)));
		
		// Gain Vec - linear amp multipliers per track (starts with 4 tracks)
		addParameter(gainVec.set("Gain Vec", vector<float>(4, 1.0f),
								vector<float>(4, 0.0f),
								vector<float>(4, 2.0f)));
		
		addParameter(balanceVec.set("Balance Vec", vector<float>(4, 0.0f),
								   vector<float>(4, -1.0f),
								   vector<float>(4, 1.0f)));
		
		// Visual separator
		addCustomRegion(
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
		);
		
		// Master VU Meter - hidden from regular GUI, shown in custom widget
		addInspectorParameter(masterVUMeter.set("Master VU", vector<float>(2, 0.0f),
											   vector<float>(2, 0.0f),
											   vector<float>(2, 1.0f)));
		
		// Master VU Attack/Release parameters - inspector parameters
		addInspectorParameter(masterVUAttack.set("Master VU Attack", 10.0f, 1.0f, 100.0f)); // ms
		addInspectorParameter(masterVURelease.set("Master VU Release", 300.0f, 10.0f, 2000.0f)); // ms

		// VU height parameters
		addInspectorParameter(trackVUHeight.set("Track VU Height", 40.0f, 10.0f, 200.0f)); // px
		addInspectorParameter(masterVUHeight.set("Master VU Height", 60.0f, 10.0f, 200.0f)); // px

		addInspectorParameter(drawVU.set("Draw VU", true));
		
		listeners.push(numTracks.newListener([this](int &tracks){
				if(!isUpdatingTracks) {
					updateTrackCount();

					// Safe Replacement Logic
					for(auto& serverInstances : trackInstances) {
						ofxSCServer* server = serverInstances.first;
						if(!server) continue;
						
						recreateVUBuses(server);
						
						auto& instances = serverInstances.second;
						int numTrackInstances = calculateNumTrackInstances();
						
						// Resize container logic (same as before) ...
						if(instances.size() != numTrackInstances) {
							 for(int i = numTrackInstances; i < instances.size(); i++) {
								if(instances[i]) { instances[i]->free(); delete instances[i]; }
							 }
							 instances.resize(numTrackInstances, nullptr);
						}

						// ATOMIC REPLACEMENT LOOP
						for(int i = 0; i < instances.size(); i++) {
							ofxSCSynth* oldSynth = instances[i];
							ofxSCSynth* newSynth = new ofxSCSynth(getSynthDefName(), server);
							
							// 1. PRE-CONFIGURE NEW SYNTH (The key fix)
							// We must set critical params BEFORE creating
							newSynth->set("in", -1); // Start disconnected/silent
							
							// Determine output bus
							int outBus = -1;
							if(outputBuses[server].count(0)) outBus = outputBuses[server][0];
							newSynth->set("out", outBus);

							// Set VU Bus
							if(trackVUBuses.count(server) && i < trackVUBuses[server].size() && trackVUBuses[server][i]) {
								 newSynth->set("vuBus", (float)trackVUBuses[server][i]->index);
							} else {
								 newSynth->set("vuBus", -1.0f);
							}
							
							// 2. EXECUTE REPLACEMENT
							if(oldSynth != nullptr) {
								// Action 4 (Replace) takes the target node ID
								newSynth->create(4, oldSynth->nodeID);
								delete oldSynth;
							} else {
								newSynth->create();
							}
							
							instances[i] = newSynth;
						}
					}
					resendParams.notify();
				}
			}));

		listeners.push(numChannels.newListener([this](int &channels){
            if(channels < 1 || channels > MAX_NODE_CHANNELS) return;
					if(!isUpdatingTracks) {
						// Store old value
						static int oldNumChannels = channels;
						
						// Recreate synths using REPLACEMENT action
						std::vector<ofxSCServer*> servers;
						for(auto& serverInstances : trackInstances) {
							if(serverInstances.first != nullptr) {
								servers.push_back(serverInstances.first);
							}
						}
						
						for(auto server : servers) {
							// Recreate VU buses with new channel count FIRST
							recreateVUBuses(server);
							
							// Recreate each track instance with replacement
							if(trackInstances.count(server) > 0) {
								auto& instances = trackInstances[server];
								
								// Recreate all instances with new channel count using REPLACEMENT
								for(int i = 0; i < instances.size(); i++) {
									if(instances[i] != nullptr) {
										ofxSCSynth* oldSynth = instances[i];
										
										// Create new synth object
										ofxSCSynth* newSynth = new ofxSCSynth(getSynthDefName(), server);
										
										// --- ATOMIC ARGUMENT CONFIGURATION START ---
										// Pre-set arguments so they are sent bundled with the creation message
										// This prevents the audio glitch/explosion because the synth never exists in a default state
										
										// 1. Set Input Bus (Default to -1 for safety)
										float inputBus = -1.0f;
										if (inputBuses[server].size() > 0) {
											for(auto& pair : inputBuses[server]) {
												scNode* node = pair.first;
												int bus = pair.second;
												// Check if this input matches the current track index (i)
												if (trackInputIndices.count(i) &&
													availableInputs[trackInputIndices[i]] != nullptr &&
													availableInputs[trackInputIndices[i]]->getNodeRef() == node) {
													inputBus = (float)bus;
													break;
												}
											}
										}
										newSynth->set("in", inputBus);
										
										// 2. Set Output Bus
										int outBus = -1;
										if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
											outBus = outputBuses[server][0];
										}
										newSynth->set("out", (float)outBus);
										
										// 3. Set VU Bus
										float vuBusIndex = -1.0f;
										if(trackVUBuses.count(server) > 0 &&
										   i < trackVUBuses[server].size() &&
										   trackVUBuses[server][i] != nullptr) {
											vuBusIndex = (float)trackVUBuses[server][i]->index;
										}
										newSynth->set("vuBus", vuBusIndex);
										
										// 4. Set Initial State Defaults (Safe values)
										newSynth->set("mute", 0.0f);
										newSynth->set("balance", 0.0f);
										
										vector<float> defaultLevel(channels, 0.5f);
										newSynth->set("level", defaultLevel);
										
										vector<float> defaultMasterLevel(channels, 1.0f);
										newSynth->set("masterLevel", defaultMasterLevel);
										
										newSynth->set("vuAttackTime", masterVUAttack.get());
										newSynth->set("vuReleaseTime", masterVURelease.get());
										
										// --- ATOMIC ARGUMENT CONFIGURATION END ---
										
										// Create new synth with replacement action (AddAction 4)
										// All the above .set() calls are sent in this single OSC bundle
										newSynth->create(4, oldSynth->nodeID);
										
										// Delete old synth object
										delete oldSynth;
										instances[i] = newSynth;
									}
								}
							}
						}
						
						// Restore ALL parameters using resendParams (Updates GUI params to Synths)
						resendParams.notify();
						
						// Update peak tracking arrays for new channel count
						for(auto& peakPair : trackPeakLevels) {
							peakPair.second.resize(channels, -60.0f);
						}
						for(auto& timerPair : trackPeakDecayTimers) {
							timerPair.second.resize(channels, 0.0f);
						}
						masterPeakLevels.resize(channels, -60.0f);
						masterPeakDecayTimers.resize(channels, 0.0f);
						
						// Update VU parameter sizes
						for(auto& vuParam : vuParameters) {
							vector<float> newVU(channels, 0.0f);
							vuParam.second.set(newVU);
						}
						
						// Update master VU
						vector<float> newMasterVU(channels, 0.0f);
						masterVUMeter.set(newMasterVU);
						
						if(masterVUData != nullptr) {
							masterVUData->getParameter().set(newMasterVU);
						}
						
						oldNumChannels = channels;
					}
				}));
		
		listeners.push(resendParams.newListener([this](){
			ofLogNotice("scPolyMixer") << "=== RESEND PARAMS TRIGGERED ===";
			
			// Restore parameters for all tracks across all servers
			for(auto& serverInstances : trackInstances) {
				ofxSCServer* server = serverInstances.first;
				if(server == nullptr) continue;
				
				auto& instances = serverInstances.second;
				for(int i = 0; i < instances.size(); i++) {
					if(instances[i] != nullptr) {
						restoreTrackParameters(server, i);  // This now includes solo logic
					}
				}
				
				// Restore input buses
				for(auto& inputPair : inputBuses[server]) {
					scNode* node = inputPair.first;
					int bus = inputPair.second;
					
					for(auto& trackIndexPair : trackInputIndices) {
						int trackIndex = trackIndexPair.first;
						int inputIndex = trackIndexPair.second;
						
						if(inputIndex >= 0 && inputIndex < availableInputs.size() &&
						   availableInputs[inputIndex] != nullptr &&
						   availableInputs[inputIndex]->getNodeRef() == node) {
							
							if(trackIndex < instances.size() && instances[trackIndex] != nullptr) {
								instances[trackIndex]->set("in", bus);
								ofLogNotice("scPolyMixer") << "Restored input bus " << bus << " for track " << trackIndex;
							}
							break;
						}
					}
				}
				
				// Restore output bus for all tracks
				if(outputBuses[server].count(0) > 0) {
					int outBus = outputBuses[server][0];
					for(auto synth : instances) {
						if(synth != nullptr) {
							synth->set("out", outBus);
						}
					}
					ofLogNotice("scPolyMixer") << "Restored output bus " << outBus << " for all tracks";
				}
			}
			
			ofLogNotice("scPolyMixer") << "=== RESEND PARAMS COMPLETE (solo logic applied) ===";
		}));

		
		listeners.push(masterLevel.newListener([this](vector<float> &levels){
			updateMasterLevel(levels);
		}));
		
		listeners.push(gainVec.newListener([this](vector<float> &levels){
			if(!isUpdatingTracks) {
				syncGainVecToTrackLevels();
			}
		}));
		
		listeners.push(balanceVec.newListener([this](vector<float> &balances){
			if(!isUpdatingTracks) {
				syncBalanceVecToTrackBalances();
			}
		}));
		
		listeners.push(masterVUAttack.newListener([this](float &v){
			updateAllTracksVUTiming(v, -1.0f);
		}));

		listeners.push(masterVURelease.newListener([this](float &v){
			updateAllTracksVUTiming(-1.0f, v);
		}));

		
		
		// Add single output
		scNode::addOutput("Out");
		
		// Master VU Data output parameter - normalized 0-1 VU meter data for other nodes
		vector<float> defaultMasterVUData(2, 0.0f); // Default to stereo
		auto masterVUDataParam = std::make_shared<ofParameter<vector<float>>>();
		masterVUDataParam->set("VU Data", defaultMasterVUData,
							  vector<float>(2, 0.0f),
							  vector<float>(2, 1.0f));
		masterVUData = addOutputParameter(*masterVUDataParam);
		
		
		// Add master VU meter widget at the end
		addCustomRegion(
			ofParameter<std::function<void()>>().set("Master Output", [this](){
				drawMasterVUWidget();
			}),
			ofParameter<std::function<void()>>().set("Master Output VU", [this](){
				drawMasterVUWidget();
			})
		);
		
		addCustomRegion(
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
		);
		
		// Initialize with default track count - DO THIS LAST
		updateTrackCount();
		
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in setup(): " << e.what();
		throw; // Re-throw to prevent partial initialization
	} catch(...) {
		ofLogError("scPolyMixer") << "Unknown error in setup()";
		throw;
	}
}

void scPolyMixer::update(ofEventArgs &args) {
	// 1. Process deferred track additions
	// This handles adding new tracks to the GUI when numTracks changes
	if(needsGUIRebuild && !tracksToAddNextFrame.empty() && !isLoadingPreset) {
		isUpdatingTracks = true;
		
		ofLogNotice("scPolyMixer") << "Processing deferred track additions: "
								   << tracksToAddNextFrame.size() << " tracks";
		
		for(int trackIndex : tracksToAddNextFrame) {
			try {
				// Initialize tracking data if needed
				if(trackPeakLevels.count(trackIndex) == 0) {
					trackPeakLevels[trackIndex] = vector<float>(numChannels.get(), -60.0f);
				}
				if(trackPeakDecayTimers.count(trackIndex) == 0) {
					trackPeakDecayTimers[trackIndex] = vector<float>(numChannels.get(), 0.0f);
				}
				
				// Initialize VU parameter
				string vuName = "VU " + ofToString(trackIndex + 1);
				vector<float> defaultVU(numChannels.get(), 0.0f);
				
				// Ensure map entry exists before setting
				if(vuParameters.count(trackIndex) == 0) {
					 // Should have been created in updateTrackCount, but safe fallback
					 // Note: We can't easily add to map here if not pre-allocated,
					 // but updateTrackCount handles allocation.
				}
				
				if(vuParameters.count(trackIndex) > 0) {
					vuParameters[trackIndex].set(vuName, defaultVU,
												vector<float>(numChannels.get(), 0.0f),
												vector<float>(numChannels.get(), 1.0f));
				}
				
				addTrackToGUI(trackIndex);
				
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error adding deferred track " << trackIndex << ": " << e.what();
			}
		}
		
		tracksToAddNextFrame.clear();
		needsGUIRebuild = false;
		isUpdatingTracks = false;
		
		ofLogNotice("scPolyMixer") << "Deferred track addition complete";
	}
	
	// 2. Update individual track VU meters
	// We sum these up to create the master VU
	vector<float> masterSum(numChannels.get(), 0.0f);
	int activeTracks = 0;
	
	for(auto& serverBuses : trackVUBuses) {
		ofxSCServer* server = serverBuses.first;
		if(server == nullptr) continue;
		
		auto& buses = serverBuses.second;
		for(int trackIndex = 0; trackIndex < buses.size(); trackIndex++) {
			// Safety checks
			if(buses[trackIndex] == nullptr) continue;
			
			// OPTIMIZATION: Use cached connection state instead of graph traversal
			// This prevents the "superlong" delay when loading presets
			bool hasInput = false;
			if(trackHasInput.count(trackIndex) > 0) {
				hasInput = trackHasInput[trackIndex];
			}
			
			// Only update parameter if it exists
			if(vuParameters.count(trackIndex) > 0) {
				vector<float> trackLevels;
				
				if(hasInput) {
					// Track has input - read actual VU data from SuperCollider
					trackLevels = buses[trackIndex]->readValues;
					
					// Fallback if data isn't ready yet
					if(trackLevels.empty()) {
						trackLevels.assign(numChannels.get(), 0.0f);
					}
				} else {
					// Track has NO input - zero out VU meter immediately
					trackLevels.assign(numChannels.get(), 0.0f);
				}
				
				// Update GUI parameter without triggering events (prevents feedback loops)
				vuParameters[trackIndex].setWithoutEventNotifications(trackLevels);
				
				// Update node output for external visualization
				if(trackVUData.count(trackIndex) > 0 && trackVUData[trackIndex] != nullptr) {
					trackVUData[trackIndex]->getParameter().set(trackLevels);
				}
				
				// Sum for master VU (only if track is audible AND has input)
				bool trackIsAudible = hasInput;
				
				// Check mute
				if(trackMuteParams.count(trackIndex) > 0 && trackMuteParams[trackIndex]->get()) {
					trackIsAudible = false;
				}
				
				// Check solo logic
				if(!soloedTracks.empty() && soloedTracks.count(trackIndex) == 0) {
					trackIsAudible = false;
				}
				
				// Add to master sum
				if(trackIsAudible) {
					for(int ch = 0; ch < trackLevels.size() && ch < masterSum.size(); ch++) {
						masterSum[ch] += trackLevels[ch];
					}
					activeTracks++;
				}
			}
			
			// CRITICAL: Always request next values to keep bus active
			// Doing this even for disconnected tracks keeps the bus "warm" on the SC side
			buses[trackIndex]->requestValues();
		}
	}
	
	// 3. Update master VU meter
	vector<float> masterLevels = masterLevel.get();

	for(int ch = 0; ch < masterSum.size(); ch++) {
		// Apply master fader level to the visual sum
		float masterLinearLevel = (ch < masterLevels.size()) ? masterLevels[ch] :
								  (masterLevels.size() > 0 ? masterLevels[0] : 1.0f);
		
		masterSum[ch] = ofClamp(masterSum[ch] * masterLinearLevel, 0.0f, 2.0f);
	}
	
	masterVUMeter.setWithoutEventNotifications(masterSum);
	
	if(masterVUData != nullptr) {
		masterVUData->getParameter().set(masterSum);
	}
}



int scPolyMixer::calculateNumTrackInstances() const {
	return numTracks.get();
}

string scPolyMixer::getSynthDefName() const {
	return "polyMixerTrack" + ofToString(numChannels.get());
}

void scPolyMixer::updateTrackCount() {
	if(isUpdatingTracks) {
		ofLogWarning("scPolyMixer") << "Already updating tracks, skipping";
		return;
	}
	
	isUpdatingTracks = true;
	
	try {
		int currentTracks = numTracks.get();
		
		if(trackLevels.size() != currentTracks) {
			int oldSize = trackLevels.size();
			
			if(oldSize > currentTracks) {
				// REMOVING TRACKS
				vector<int> tracksToRemove;
				for(int j = oldSize - 1; j >= currentTracks; j--) {
					tracksToRemove.push_back(j);
				}
				
				for(int trackIndex : tracksToRemove) {
					removeTrackFromGUI(trackIndex);
				}
				
				while(inputs.size() > currentTracks) {
					int lastIndex = inputs.size() - 1;
					try {
						scNode::removeInput(lastIndex);
					} catch(const std::exception& e) {
						inputs.clear();
						availableInputs.clear();
						break;
					}
				}
				
			} else {
				// ADDING TRACKS
				// If loading preset, add immediately; otherwise defer to next frame
				if(isLoadingPreset) {
					ofLogNotice("scPolyMixer") << "Preset loading: adding tracks immediately";
					for(int trackIndex = oldSize; trackIndex < currentTracks; trackIndex++) {
						try {
							// Pre-allocate tracking data
							trackPeakLevels[trackIndex] = vector<float>(numChannels.get(), -60.0f);
							trackPeakDecayTimers[trackIndex] = vector<float>(numChannels.get(), 0.0f);
							
							string vuName = "VU " + ofToString(trackIndex + 1);
							vector<float> defaultVU(numChannels.get(), 0.0f);
							vuParameters[trackIndex].set(vuName, defaultVU,
														vector<float>(numChannels.get(), 0.0f),
														vector<float>(numChannels.get(), 1.0f));
							
							addTrackToGUI(trackIndex);
						} catch(const std::exception& e) {
							ofLogError("scPolyMixer") << "Error adding track " << trackIndex << ": " << e.what();
						}
					}
				} else {
					// Normal operation: defer to next frame
					tracksToAddNextFrame.clear();
					for(int trackIndex = oldSize; trackIndex < currentTracks; trackIndex++) {
						tracksToAddNextFrame.push_back(trackIndex);
					}
					needsGUIRebuild = true;
					ofLogNotice("scPolyMixer") << "Scheduled " << tracksToAddNextFrame.size()
											  << " tracks for addition next frame";
				}
			}
		}
		
		// Update gainVec to match track count
		vector<float> currentLevels = gainVec.get();
		currentLevels.resize(currentTracks, 1.0f);
		gainVec.setWithoutEventNotifications(currentLevels);
		
		// Update balanceVec to match track count
		vector<float> currentBalances = balanceVec.get();
		currentBalances.resize(currentTracks, 0.0f);
		balanceVec.setWithoutEventNotifications(currentBalances);
		
		// Update solo tracking
		std::set<int> validSoloedTracks;
		for(int trackIndex : soloedTracks) {
			if(trackIndex < currentTracks) {
				validSoloedTracks.insert(trackIndex);
			}
		}
		soloedTracks = validSoloedTracks;
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in updateTrackCount: " << e.what();
	} catch(...) {
		ofLogError("scPolyMixer") << "Unknown error in updateTrackCount";
	}
	
	isUpdatingTracks = false;
}

void scPolyMixer::drawTrackSeparator(int trackIndex) {
	// Safety checks
	if(trackIndex < 0 || trackIndex >= numTracks.get()) {
		ImGui::Dummy(ImVec2(240.0f, 20.0f));
		return;
	}
	
	if(trackNameParams.count(trackIndex) == 0 || trackColorParams.count(trackIndex) == 0) {
		ImGui::Dummy(ImVec2(240.0f, 20.0f));
		return;
	}
	
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	const float barWidth = 240.0f;
	const float barHeight = 20.0f;
	
	// Get track name and color
	string trackName = trackNameParams[trackIndex]->get();
	ofColor trackColor = trackColorParams[trackIndex]->get();
	
	// Draw simple colored bar
	ImVec2 barStart = cursorPos;
	ImVec2 barEnd = ImVec2(barStart.x + barWidth, barStart.y + barHeight);
	
	// Use EXACT color from inspector (no darkening or modification)
	ImU32 bgColor = IM_COL32(
		trackColor.r,
		trackColor.g,
		trackColor.b,
		255
	);
	drawList->AddRectFilled(barStart, barEnd, bgColor);
	
	// Draw track name text (CENTERED)
	ImVec2 textSize = ImGui::CalcTextSize(trackName.c_str());
	ImVec2 textPos = ImVec2(
		barStart.x + (barWidth - textSize.x) * 0.5f,  // Center horizontally
		barStart.y + (barHeight - textSize.y) * 0.5f  // Center vertically
	);
	
	// Simple white text, no shadow
	drawList->AddText(
		textPos,
		IM_COL32(255, 255, 255, 255),
		trackName.c_str()
	);
	
	// Move cursor
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + barHeight));
	ImGui::Dummy(ImVec2(barWidth, 0.0f));
}

void scPolyMixer::addTrackToGUI(int trackIndex) {
	if(trackLevels.count(trackIndex) > 0) {
		return;
	}
	
	string trackName = "Track " + ofToString(trackIndex + 1);
	
	try {
		// CRITICAL: Declare parameters outside try-catch blocks for scope access
		auto levelParam = std::make_shared<ofParameter<vector<float>>>();
		auto balanceParam = std::make_shared<ofParameter<float>>();

		auto muteParam = std::make_shared<ofParameter<bool>>();
		auto soloParam = std::make_shared<ofParameter<bool>>();
		
		// Add track name parameter FIRST (inspector only, not visible in main GUI)
		try {
			auto nameParam = std::make_shared<ofParameter<string>>();
			nameParam->set("Name " + ofToString(trackIndex + 1),
						   "Track " + ofToString(trackIndex + 1));
			trackNameParams[trackIndex] = nameParam;
			addInspectorParameter(*nameParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating name parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Add track color parameter (inspector only)
		try {
			auto colorParam = std::make_shared<ofParameter<ofColor>>();
			// Generate a default color based on track index
			ofColor defaultColor;
			defaultColor.setHsb((trackIndex * 30) % 360, 100, 150);
			colorParam->set("Color " + ofToString(trackIndex + 1), defaultColor);
			trackColorParams[trackIndex] = colorParam;
			addInspectorParameter(*colorParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating color parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Add thick separator line BEFORE track name bar
		string topSeparatorName = "TrackTopSeparator_" + ofToString(trackIndex);
		try {
			addCustomRegion(
				ofParameter<std::function<void()>>().set(topSeparatorName, [](){
					drawSeparator();
				}),
				ofParameter<std::function<void()>>().set(topSeparatorName + "_Region", [](){
					drawSeparator();
				})
			);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating top separator for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Add separator with track name and color - this is the visual header
		string separatorName = "TrackSeparator_" + ofToString(trackIndex);
		try {
			addCustomRegion(
				ofParameter<std::function<void()>>().set(separatorName, [this, trackIndex](){
					drawTrackSeparator(trackIndex);
				}),
				ofParameter<std::function<void()>>().set(separatorName + "_Region", [this, trackIndex](){
					drawTrackSeparator(trackIndex);
				})
			);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating separator for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// NOW add input - this comes AFTER the track name bar
		if(inputs.size() <= trackIndex) {
			scNode::addInput("In " + ofToString(trackIndex + 1));
		}
		
		// Track input index mapping
		trackInputIndices[trackIndex] = trackIndex;
		
		try {
			vector<float> defaultLevel(1, 0.5f);
			levelParam->set("Level " + ofToString(trackIndex + 1), defaultLevel,
						   vector<float>(1, 0.0f), vector<float>(1, 2.0f));
			trackLevelParams[trackIndex] = levelParam;
			auto levelOceanodeParam = addParameter(*levelParam);
			trackLevels[trackIndex] = levelOceanodeParam;
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating level parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// NEW: Add balance parameter
		try {
			balanceParam->set("Balance " + ofToString(trackIndex + 1), 0.0f, -1.0f, 1.0f);
			trackBalanceParams[trackIndex] = balanceParam;
			auto balanceOceanodeParam = addParameter(*balanceParam);
			trackBalances[trackIndex] = balanceOceanodeParam;
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating balance parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		try {
			muteParam->set("Mute " + ofToString(trackIndex + 1), false);
			trackMuteParams[trackIndex] = muteParam;
			addInspectorParameter(*muteParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating mute parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		try {
			soloParam->set("Solo " + ofToString(trackIndex + 1), false);
			trackSoloParams[trackIndex] = soloParam;
			addInspectorParameter(*soloParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating solo parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Peak tracking was already pre-allocated in updateTrackCount()
		// Just verify it exists
		if(trackPeakLevels.count(trackIndex) == 0) {
			trackPeakLevels[trackIndex] = vector<float>(numChannels.get(), -60.0f);
			trackPeakDecayTimers[trackIndex] = vector<float>(numChannels.get(), 0.0f);
		}
		
		// VU parameter was already pre-allocated in updateTrackCount()
		// Just add it to the inspector
		if(vuParameters.count(trackIndex) > 0) {
			addInspectorParameter(vuParameters[trackIndex]);
		} else {
			// Fallback: create it now if somehow not pre-allocated
			string vuName = "VU " + ofToString(trackIndex + 1);
			vector<float> defaultVU(numChannels.get(), 0.0f);
			vuParameters[trackIndex].set(vuName, defaultVU,
										vector<float>(numChannels.get(), 0.0f),
										vector<float>(numChannels.get(), 1.0f));
			addInspectorParameter(vuParameters[trackIndex]);
		}
		
		try {
			string vuDataName = "VU Data " + ofToString(trackIndex + 1);
			vector<float> defaultVUData(numChannels.get(), 0.0f);
			auto vuDataParam = std::make_shared<ofParameter<vector<float>>>();
			vuDataParam->set(vuDataName, defaultVUData,
							vector<float>(numChannels.get(), 0.0f),
							vector<float>(numChannels.get(), 1.0f));
			auto vuDataOceanodeParam = addOutputParameter(*vuDataParam);
			trackVUData[trackIndex] = vuDataOceanodeParam;
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating VU data parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Then add the track widget (VU meter and buttons)
		string widgetName = "Track " + ofToString(trackIndex + 1) + " Control";
		try {
			addCustomRegion(
				ofParameter<std::function<void()>>().set(widgetName, [this, trackIndex](){
					drawCompactTrackWidget(trackIndex);
				}),
				ofParameter<std::function<void()>>().set(widgetName + "_Region", [this, trackIndex](){
					drawCompactTrackWidget(trackIndex);
				})
			);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating track widget for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		listeners.push(levelParam->newListener([this, trackIndex](vector<float> &levels){
			if(!isUpdatingTracks && trackLevels.count(trackIndex) > 0) {
				updateTrackInstanceLevel(trackIndex, levels);
				syncTrackLevelsToGainVec();
			}
		}));
		
		listeners.push(balanceParam->newListener([this, trackIndex](float &balance){
			if(!isUpdatingTracks && trackBalances.count(trackIndex) > 0) {
				updateTrackInstanceBalance(trackIndex, balance);
				syncTrackBalancesToBalanceVec();
			}
		}));
		
		listeners.push(muteParam->newListener([this, trackIndex](bool &muted){
			if(!isUpdatingTracks && trackMuteParams.count(trackIndex) > 0) {
				updateTrackInstanceMute(trackIndex, muted);
			}
		}));
		
		listeners.push(soloParam->newListener([this, trackIndex](bool &soloed){
			if(!isUpdatingTracks && trackSoloParams.count(trackIndex) > 0) {
				updateTrackInstanceSolo(trackIndex, soloed);
				updateSoloLogic();
			}
		}));
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error adding track " << trackIndex << ": " << e.what();
		trackLevels.erase(trackIndex);
		trackVUData.erase(trackIndex);
		trackLevelParams.erase(trackIndex);
		trackMuteParams.erase(trackIndex);
		trackSoloParams.erase(trackIndex);
		trackNameParams.erase(trackIndex);
		trackColorParams.erase(trackIndex);
		trackPeakLevels.erase(trackIndex);
		trackPeakDecayTimers.erase(trackIndex);
		trackInputIndices.erase(trackIndex);
		vuParameters.erase(trackIndex);
		throw;
	}
}

void scPolyMixer::updateTrackInstanceBalance(int trackIndex, float balance) {
	if(trackInstances.empty()) return;
	
	ofLogVerbose("scPolyMixer") << "Updating track " << trackIndex << " balance to " << balance;
	
	for(auto& serverInstances : trackInstances) {
		if(serverInstances.first != nullptr &&
		   trackIndex < serverInstances.second.size() &&
		   serverInstances.second[trackIndex] != nullptr) {
			
			try {
				serverInstances.second[trackIndex]->set("balance", balance);
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error setting balance for track " << trackIndex
				<< ": " << e.what();
			}
		}
	}
}

void scPolyMixer::syncBalanceVecToTrackBalances() {
	vector<float> balanceVecValues = balanceVec.get();
	
	for(int i = 0; i < numTracks.get(); i++) {
		if(trackBalances.count(i) > 0 && i < balanceVecValues.size()) {
			// CRITICAL: Update the GUI parameter first (without triggering listener)
			trackBalances[i]->getParameter().setWithoutEventNotifications(balanceVecValues[i]);
			
			// Then update the SuperCollider instance
			updateTrackInstanceBalance(i, balanceVecValues[i]);
		}
	}
}

void scPolyMixer::syncTrackBalancesToBalanceVec() {
	// balanceVec is independent, not synced back
}
	
int scPolyMixer::getLastSynthID(ofxSCServer* server) {
	if (!server) return -1;
	if (trackInstances.count(server) == 0) return -1;

	int lastID = -1;

	for (auto *synth : trackInstances[server]) {
		if (synth && synth->nodeID > 0)
			lastID = synth->nodeID;
	}

	return lastID;
}

void scPolyMixer::moveSynthBefore(ofxSCServer* server, int nodeID)
{
	if (!server) return;
	if (trackInstances.count(server) == 0) return;

	// Re-send parameters (this is consistent with other nodes)
	resendParams.notify();

	auto &instances = trackInstances[server];

	//we iterate reversedly to address the bus allocation order
	for (int i = instances.size()-1 ; i >= 0; i--) {
		ofxSCSynth *synth = instances[i];
		if (!synth) continue;

		try {
			// --- Restore parameters per track (now includes solo logic) ---
			restoreTrackParameters(server, i);

			// Restore output bus
			if (outputBuses[server].count(0) > 0)
				synth->set("out", outputBuses[server][0]);

			// Restore input bus (if any)
			for (auto &pair : inputBuses[server]) {
				scNode *node = pair.first;
				int bus = pair.second;

				// match track by input index
				if (trackInputIndices.count(i) &&
					availableInputs[trackInputIndices[i]] != nullptr &&
					availableInputs[trackInputIndices[i]]->getNodeRef() == node)
				{
					synth->set("in", bus);
					break;
				}
			}

			// Finally, move synth in the graph
			synth->moveBefore(nodeID);

		}
		catch (const std::exception &e) {
			ofLogError("scPolyMixer") << "Error in moveSynthBefore for track "
									  << i << ": " << e.what();
		}
	}
	
	ofLogNotice("scPolyMixer") << "moveSynthBefore complete (solo logic applied via restoreTrackParameters)";
}


void scPolyMixer::recreateVUBuses(ofxSCServer* server) {
	if(server == nullptr) {
		ofLogError("scPolyMixer") << "Cannot recreate VU buses: server is null";
		return;
	}
	
	int numTracksCount = numTracks.get();
	if(numTracksCount <= 0 || numTracksCount > 32) {
		ofLogError("scPolyMixer") << "Invalid track count for VU bus creation: " << numTracksCount;
		return;
	}
	
	ofLogNotice("scPolyMixer") << "Recreating " << numTracksCount << " VU buses";
	
	// Clean up existing buses
	if(trackVUBuses.count(server) > 0) {
		for(auto bus : trackVUBuses[server]) {
			if(bus != nullptr) {
				bus->free();
				delete bus;
			}
		}
		trackVUBuses[server].clear();
	}
	
	//is this really needed???
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	
	trackVUBuses[server].resize(numTracksCount, nullptr);
	
	for(int i = 0; i < numTracksCount; i++) {
		try {
			int channelCount = numChannels.get();
			// FIXED: Use MAX_NODE_CHANNELS instead of hardcoded 16
			if(channelCount <= 0 || channelCount > MAX_NODE_CHANNELS) continue;
			
			ofxSCBus* newBus = new ofxSCBus(RATE_CONTROL, channelCount, server);
			
			if(newBus != nullptr && newBus->index >= 0 && newBus->index < 4096) {
				if(server->controlBusses[newBus->index] != nullptr) {
					trackVUBuses[server][i] = newBus;
					newBus->requestValues();
					ofLogNotice("scPolyMixer") << "Created VU bus " << i << " with index " << newBus->index;
				} else {
					delete newBus;
					trackVUBuses[server][i] = nullptr;
				}
			} else {
				if(newBus != nullptr) delete newBus;
				trackVUBuses[server][i] = nullptr;
			}
		} catch(...) {
			trackVUBuses[server][i] = nullptr;
		}
	}
	
	ofLogNotice("scPolyMixer") << "VU bus recreation complete";
}
	
void scPolyMixer::removeTrackFromGUI(int trackIndex) {
	try {
		// Step 1: Clear all shared_ptr references to prevent dangling pointers
		if(trackLevels.count(trackIndex) > 0) {
			trackLevels[trackIndex].reset();
			trackLevels.erase(trackIndex);
		}
		
		if(trackLevelParams.count(trackIndex) > 0) {
			trackLevelParams[trackIndex].reset();
			trackLevelParams.erase(trackIndex);
		}
		
		if(trackBalances.count(trackIndex) > 0) {
			trackBalances[trackIndex].reset();
			trackBalances.erase(trackIndex);
		}

		if(trackBalanceParams.count(trackIndex) > 0) {
			trackBalanceParams[trackIndex].reset();
			trackBalanceParams.erase(trackIndex);
		}
		
		if(trackMuteParams.count(trackIndex) > 0) {
			trackMuteParams[trackIndex].reset();
			trackMuteParams.erase(trackIndex);
		}
		
		if(trackSoloParams.count(trackIndex) > 0) {
			trackSoloParams[trackIndex].reset();
			trackSoloParams.erase(trackIndex);
		}
		
		if(trackVUData.count(trackIndex) > 0) {
			trackVUData[trackIndex].reset();
			trackVUData.erase(trackIndex);
		}
		
		if(trackNameParams.count(trackIndex) > 0) {
			trackNameParams[trackIndex].reset();
			trackNameParams.erase(trackIndex);
		}
		
		if(trackColorParams.count(trackIndex) > 0) {
			trackColorParams[trackIndex].reset();
			trackColorParams.erase(trackIndex);
		}
		
		// Clear VU meters and other tracking data
		if(ENABLE_VU_METERS) {
			trackVUMeters.erase(trackIndex);
			trackVUMeterParams.erase(trackIndex);
		}
		
		trackInputIndices.erase(trackIndex);
		soloedTracks.erase(trackIndex);
		trackPeakLevels.erase(trackIndex);
		trackPeakDecayTimers.erase(trackIndex);
		vuParameters.erase(trackIndex);
		
		// Step 2: FORCE PARAMETER REMOVAL - Remove from both groups without checking existence
		vector<string> parametersToRemoveNow = {
			"Level " + ofToString(trackIndex + 1),
			"Balance " + ofToString(trackIndex + 1),
			"Mute " + ofToString(trackIndex + 1),
			"Solo " + ofToString(trackIndex + 1),
			"Name " + ofToString(trackIndex + 1),
			"Color " + ofToString(trackIndex + 1),
			"VU Data " + ofToString(trackIndex + 1),
			"VU " + ofToString(trackIndex + 1)
		};
		
		// Add custom regions
		string widgetName = "Track " + ofToString(trackIndex + 1) + " Control";
		parametersToRemoveNow.push_back(widgetName);
		parametersToRemoveNow.push_back(widgetName + "_Region");
		string separatorName = "TrackSeparator_" + ofToString(trackIndex);
		parametersToRemoveNow.push_back(separatorName);
		parametersToRemoveNow.push_back(separatorName + "_Region");
		string topSeparatorName = "TrackTopSeparator_" + ofToString(trackIndex);
		parametersToRemoveNow.push_back(topSeparatorName);
		parametersToRemoveNow.push_back(topSeparatorName + "_Region");
		
		int successfulRemovals = 0;
		for(const auto& paramName : parametersToRemoveNow) {
			bool removed = false;
			
			// Force removal from regular parameters (ignore errors)
			try {
				removeParameter(paramName);
				removed = true;
			} catch(...) {
				// Ignore errors, try inspector parameters
			}
			
			// Force removal from inspector parameters (ignore errors)
			try {
				removeInspectorParameter(paramName);
				removed = true;
			} catch(...) {
				// Ignore errors
			}
			
			if(removed) {
				successfulRemovals++;
			}
		}
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error removing track " << trackIndex << ": " << e.what();
	}
}

void scPolyMixer::removeAllTrackParameters() {
	ofLogNotice("scPolyMixer") << "Removing all track parameters";
	
	try {
		// CRITICAL: Clear maps first to prevent access during removal
		auto levelsCopy = trackLevels;
		
		// Clear maps immediately
		trackLevels.clear();
		trackVUMeters.clear();
		trackLevelParams.clear();
		trackBalances.clear(); // NEW
		trackBalanceParams.clear(); // NEW
		trackMuteParams.clear();
		trackSoloParams.clear();
		trackVUMeterParams.clear();
		trackVUData.clear();
		trackNameParams.clear();
		trackColorParams.clear();
		trackInputIndices.clear();
		soloedTracks.clear();
		trackPeakLevels.clear();
		trackPeakDecayTimers.clear();
		vuParameters.clear();
		
		// Now safely remove parameters from GUI
		for(auto& pair : levelsCopy) {
			int trackIndex = pair.first;
			try {
				removeParameter("Level " + ofToString(trackIndex + 1));
				removeInspectorParameter("Mute " + ofToString(trackIndex + 1));
				removeInspectorParameter("Solo " + ofToString(trackIndex + 1));
				removeInspectorParameter("Name " + ofToString(trackIndex + 1));
				removeInspectorParameter("Color " + ofToString(trackIndex + 1));
				removeInspectorParameter("VU " + ofToString(trackIndex + 1));
				removeParameter("VU Data " + ofToString(trackIndex + 1));
				
				// Remove custom regions (ImGui widgets and separators)
				string widgetName = "Track " + ofToString(trackIndex + 1) + " Control";
				removeParameter(widgetName);
				string separatorName = "TrackSeparator_" + ofToString(trackIndex);
				removeParameter(separatorName);
				string topSeparatorName = "TrackTopSeparator_" + ofToString(trackIndex);
				removeParameter(topSeparatorName);
			} catch(const std::exception& e) {
				ofLogWarning("scPolyMixer") << "Error removing parameters for track " << trackIndex << ": " << e.what();
			}
		}
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in removeAllTrackParameters: " << e.what();
	}
	
	ofLogNotice("scPolyMixer") << "Finished removing all track parameters";
}

void scPolyMixer::removeAllInputParameters() {
	ofLogNotice("scPolyMixer") << "Removing all input parameters - DIRECT APPROACH";
	
	try {
		// DIRECT APPROACH: Remove input parameters immediately
		for(int i = 0; i < inputs.size(); i++) {
			try {
				removeParameter("In " + ofToString(i + 1));
				ofLogVerbose("scPolyMixer") << "Removed input parameter: In " << (i + 1);
			} catch(const std::exception& e) {
				ofLogWarning("scPolyMixer") << "Could not remove input parameter In " << (i + 1) << ": " << e.what();
			}
		}
		
		// Clear internal vectors
		inputs.clear();
		availableInputs.clear();
		trackInputIndices.clear();
		
		ofLogNotice("scPolyMixer") << "Completed direct input parameter removal";
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in removeAllInputParameters: " << e.what();
	}
}
	
	// Audio parameter update implementations
void scPolyMixer::updateTrackInstanceLevel(int trackIndex, const vector<float>& levels) {
	if(trackInstances.empty()) return;
	
	ofLogVerbose("scPolyMixer") << "Updating track " << trackIndex << " levels (" << levels.size() << " channels)";
	
	// Update all instances of this track across all servers
	for(auto& serverInstances : trackInstances) {
		if(serverInstances.first != nullptr &&
		   trackIndex < serverInstances.second.size() &&
		   serverInstances.second[trackIndex] != nullptr) {
			
			try {
				// Apply gainVec multiplier to track levels (now linear)
				vector<float> gainVecValues = gainVec.get();
				float gainVecMultiplier = (trackIndex < gainVecValues.size()) ? gainVecValues[trackIndex] : 1.0f;
				
				vector<float> finalLevels;
				if(levels.size() == 1) {
					// Scalar mode - linear gain, just multiply by gainVec
					float linearLevel = levels[0];
					float scaledLevel = linearLevel * gainVecMultiplier;
					finalLevels = vector<float>(numChannels.get(), scaledLevel);
				} else {
					// Vector mode - linear gain per channel
					finalLevels.resize(numChannels.get());
					for(int ch = 0; ch < numChannels.get(); ch++) {
						float channelLinearLevel = (ch < levels.size()) ? levels[ch] : levels[0];
						finalLevels[ch] = channelLinearLevel * gainVecMultiplier;
					}
				}
				serverInstances.second[trackIndex]->set("level", finalLevels);
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error setting levels for track " << trackIndex
				<< ": " << e.what();
			}
		}
	}
}
	
	void scPolyMixer::updateTrackInstanceMute(int trackIndex, bool muted) {
		if(trackInstances.empty()) return;
		
		ofLogVerbose("scPolyMixer") << "Updating track " << trackIndex << " mute to " << muted;
		
		// Update all instances of this track across all servers
		for(auto& serverInstances : trackInstances) {
			if(serverInstances.first != nullptr &&
			   trackIndex < serverInstances.second.size() &&
			   serverInstances.second[trackIndex] != nullptr) {
				
				try {
					// Convert bool to float (0.0 = unmuted, 1.0 = muted)
					float muteValue = muted ? 1.0f : 0.0f;
					serverInstances.second[trackIndex]->set("mute", muteValue);
				} catch(const std::exception& e) {
					ofLogError("scPolyMixer") << "Error setting mute for track " << trackIndex
					<< ": " << e.what();
				}
			}
		}
	}
	
	void scPolyMixer::updateTrackInstanceSolo(int trackIndex, bool soloed) {
		// Update solo tracking
		if(soloed) {
			soloedTracks.insert(trackIndex);
		} else {
			soloedTracks.erase(trackIndex);
		}
		
		ofLogVerbose("scPolyMixer") << "Track " << trackIndex << " solo: " << soloed
		<< " (total soloed: " << soloedTracks.size() << ")";
	}
	
void scPolyMixer::updateMasterLevel(const vector<float>& levels) {
	if(trackInstances.empty()) return;
	
	ofLogVerbose("scPolyMixer") << "Updating master levels (" << levels.size() << " channels)";
	
	// Update master level on all track instances across all servers
	for(auto& serverInstances : trackInstances) {
		if(serverInstances.first != nullptr) {
			for(auto synth : serverInstances.second) {
				if(synth != nullptr) {
					try {
						// Linear gain - direct pass-through
						if(levels.size() == 1) {
							float linearLevel = levels[0];
							vector<float> expandedMasterLevel(numChannels.get(), linearLevel);
							synth->set("masterLevel", expandedMasterLevel);
						} else {
							synth->set("masterLevel", levels);
						}
					} catch(const std::exception& e) {
						ofLogError("scPolyMixer") << "Error setting master levels: " << e.what();
					}
				}
			}
		}
	}
}
	
	void scPolyMixer::updateSoloLogic() {
		if(trackInstances.empty()) return;
		
		int totalTracks = numTracks.get();
		bool anySoloed = !soloedTracks.empty();
		
		ofLogVerbose("scPolyMixer") << "Updating solo logic - " << soloedTracks.size()
		<< " tracks soloed out of " << totalTracks;
		
		// Solo logic:
		// - If no tracks are soloed, all tracks should be unmuted (unless individually muted)
		// - If any tracks are soloed, non-soloed tracks should be muted via the solo system
		
		for(int trackIndex = 0; trackIndex < totalTracks; trackIndex++) {
			bool shouldBeSoloMuted = false;
			
			if(anySoloed) {
				// If any track is soloed, mute this track unless it's also soloed
				shouldBeSoloMuted = (soloedTracks.count(trackIndex) == 0);
			}
			
			// Apply solo muting to SuperCollider instances
			// Note: Individual track mute is handled separately in updateTrackInstanceMute()
			for(auto& serverInstances : trackInstances) {
				if(serverInstances.first != nullptr &&
				   trackIndex < serverInstances.second.size() &&
				   serverInstances.second[trackIndex] != nullptr) {
					
					try {
						// Get individual mute state
						bool individuallyMuted = false;
						if(trackMuteParams.count(trackIndex) > 0) {
							individuallyMuted = trackMuteParams[trackIndex]->get();
						}
						
						// Final mute state is individual mute OR solo mute
						bool finalMute = individuallyMuted || shouldBeSoloMuted;
						float muteValue = finalMute ? 1.0f : 0.0f;
						
						serverInstances.second[trackIndex]->set("mute", muteValue);
						
					} catch(const std::exception& e) {
						ofLogError("scPolyMixer") << "Error updating solo logic for track "
						<< trackIndex << ": " << e.what();
					}
				}
			}
		}
	}
	
	void scPolyMixer::updateTrackVUTiming(int trackIndex, float attackTime, float releaseTime) {
		if(trackInstances.empty()) return;
		
		ofLogVerbose("scPolyMixer") << "Updating VU timing for track " << trackIndex
									<< " (attack: " << attackTime << "ms, release: " << releaseTime << "ms)";
		
		// Update all instances of this track across all servers
		for(auto& serverInstances : trackInstances) {
			if(serverInstances.first != nullptr &&
			   trackIndex < serverInstances.second.size() &&
			   serverInstances.second[trackIndex] != nullptr) {
				
				try {
					// Only update the parameter that changed (use -1 to indicate no change)
					if(attackTime >= 0.0f) {
						ofLogNotice("scPolyMixer") << "Updating vuAttackTime for track " << trackIndex << " to " << attackTime << "ms";
						serverInstances.second[trackIndex]->set("vuAttackTime", attackTime);
					}
					if(releaseTime >= 0.0f) {
						ofLogNotice("scPolyMixer") << "Updating vuReleaseTime for track " << trackIndex << " to " << releaseTime << "ms";
						serverInstances.second[trackIndex]->set("vuReleaseTime", releaseTime);
					}
				} catch(const std::exception& e) {
					ofLogError("scPolyMixer") << "Error setting VU timing for track " << trackIndex
											  << ": " << e.what();
				}
			}
		}
	}

void scPolyMixer::updateAllTracksVUTiming(float attackTime, float releaseTime) {
	for(auto& serverInstances : trackInstances) {
		for(auto* synth : serverInstances.second) {
			if(!synth) continue;
			if(attackTime >= 0.0f) {
				synth->set("vuAttackTime", attackTime);
			}
			if(releaseTime >= 0.0f) {
				synth->set("vuReleaseTime", releaseTime);
			}
		}
	}
}

	
	void scPolyMixer::syncGainVecToTrackLevels() {
		// gainVec changed - update all track instances with new multipliers
		for(int i = 0; i < numTracks.get(); i++) {
			if(trackLevels.count(i) > 0) {
				// Re-apply track levels with new gainVec multipliers
				updateTrackInstanceLevel(i, trackLevels[i]->getParameter().get());
			}
		}
	}
	
	void scPolyMixer::syncTrackLevelsToGainVec() {
		// Track levels changed - no need to update gainVec
		// gainVec is independent multiplier, not a copy of track levels
	}
	
	// SuperCollider integration implementations
	void scPolyMixer::buildSynth(ofxSCServer* server) {
		ofLogNotice("scPolyMixer") << "Building synth for server";
		
		// Create track instances
		createTrackInstances(server);
		
		// ONLY create VU meter buses if enabled
		/*
		if(ENABLE_VU_METERS) {
			createVUMeterBuses(server);
		}
		 */
	}
	
void scPolyMixer::createSynth(ofxSCServer* server) {
	if(trackInstances.count(server) == 0) return;
	
	// Create VU meter buses normally
	recreateVUBuses(server);
	
	int outBus = -1;
	if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
		outBus = outputBuses[server][0];
	}

	// Create all track synth instances
	for(int i = 0; i < trackInstances[server].size(); i++) {
		if(trackInstances[server][i] != nullptr) {
			try {
				// 1. CONFIGURATION PHASE (Before Create)
				// ofxSCSynth stores these in a temporary map
				
				// Determine safe input bus
				float inputBus = -1.0f; // Default to disconnected
				if (inputBuses[server].size() > 0) {
					for(auto& pair : inputBuses[server]) {
						scNode* node = pair.first;
						int bus = pair.second;
						if (trackInputIndices.count(i) &&
							availableInputs[trackInputIndices[i]] != nullptr &&
							availableInputs[trackInputIndices[i]]->getNodeRef() == node) {
							inputBus = (float)bus;
							break;
						}
					}
				}

				// Determine VU bus
				float vuBusIndex = -1.0f;
				if(trackVUBuses.count(server) > 0 &&
				   i < trackVUBuses[server].size() &&
				   trackVUBuses[server][i] != nullptr) {
					vuBusIndex = (float)trackVUBuses[server][i]->index;
				}

				// 2. SET ATOMIC ARGUMENTS
				// These will be bundled into the /s_new OSC message
				trackInstances[server][i]->set("in", inputBus);
				trackInstances[server][i]->set("out", (float)outBus);
				trackInstances[server][i]->set("vuBus", vuBusIndex);
				
				// CRITICAL SAFETY: Explicitly initialize audio params
				trackInstances[server][i]->set("mute", 0.0f); // Or 1.0f if you prefer starting muted
				trackInstances[server][i]->set("balance", 0.0f);
				
				// Set initial levels
				vector<float> defaultLevel(numChannels.get(), 0.5f);
				trackInstances[server][i]->set("level", defaultLevel);
				
				vector<float> defaultMasterLevel(numChannels.get(), 1.0f);
				trackInstances[server][i]->set("masterLevel", defaultMasterLevel);
				
				trackInstances[server][i]->set("vuAttackTime", masterVUAttack.get());
				trackInstances[server][i]->set("vuReleaseTime", masterVURelease.get());

				// 3. EXECUTE CREATION
				// This sends the /s_new message WITH all the above arguments
				trackInstances[server][i]->create();
				
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error creating track " << i << ": " << e.what();
			}
		}
	}
	
	resendParams.notify();
}
	
void scPolyMixer::free(ofxSCServer* server) {
	if(!server) {
		ofLogWarning("scPolyMixer") << "Cannot free: server is null";
		return;
	}
	
	ofLogNotice("scPolyMixer") << "Freeing track instances for server";
	
	try {
		freeTrackInstances(server);
		freeVUBuses(server);
		
		// Clean up master VU bus if this is the last server
		if(masterVUBus != nullptr) {
			try {
				masterVUBus->free();
				delete masterVUBus;
				masterVUBus = nullptr;
				ofLogNotice("scPolyMixer") << "Freed master VU bus";
			} catch(...) {
				ofLogWarning("scPolyMixer") << "Error freeing master VU bus";
				masterVUBus = nullptr;
			}
		}
		
		trackInstances.erase(server);
		trackVUBuses.erase(server);
		outputBuses.erase(server);
		inputBuses.erase(server);
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in free(): " << e.what();
	}
}
	
	void scPolyMixer::freeAll() {
		ofLogNotice("scPolyMixer") << "Freeing all track instances from all servers";
		
		// Stop VU meter updates
		stopVUMeterUpdates();
		
		// Create a copy of server keys to avoid iterator invalidation
		std::vector<ofxSCServer*> servers;
		for(auto& serverInstances : trackInstances) {
			if(serverInstances.first != nullptr) {
				servers.push_back(serverInstances.first);
			}
		}
		
		// Free instances from each server safely
		for(auto server : servers) {
			try {
				free(server);
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error freeing instances from server: " << e.what();
			}
		}
		
		// Clear all maps
		trackInstances.clear();
		vuMeterBuses.clear();
		outputBuses.clear();
		inputBuses.clear();
	}
	
	void scPolyMixer::setOutputBus(ofxSCServer* server, int index, int bus) {
		if(server == nullptr) {
			ofLogError("scPolyMixer") << "Cannot set output bus: server is null";
			return;
		}
		
		outputBuses[server][index] = bus;
		ofLogNotice("scPolyMixer") << "Setting output bus " << index << " to " << bus;
		
		if(trackInstances.count(server) > 0) {
			// Set output bus for all track instances - they all output to the same bus (automatic summing)
			for(int i = 0; i < trackInstances[server].size(); i++) {
				if(trackInstances[server][i] != nullptr) {
					try {
						trackInstances[server][i]->set("out", bus);
						ofLogVerbose("scPolyMixer") << "Track " << i << " output set to bus " << bus;
					} catch(const std::exception& e) {
						ofLogError("scPolyMixer") << "Error setting output bus for track " << i << ": " << e.what();
					} catch(...) {
						ofLogError("scPolyMixer") << "Unknown error setting output bus for track " << i;
					}
				}
			}
		}
	}
	
void scPolyMixer::setInputBus(ofxSCServer* server, scNode* node, int bus) {
	if(server == nullptr) {
		ofLogError("scPolyMixer") << "Cannot set input bus: server is null";
		return;
	}
	
	ofLogNotice("scPolyMixer") << "setInputBus: node=" << (node ? "valid" : "null") << ", bus=" << bus;
	
	// Store in the map (even if bus is -1, though serverManager shouldn't call us with -1)
	if(bus >= 0 && node != nullptr) {
		inputBuses[server][node] = bus;
	}
	
	// Find which track this input corresponds to
	for(auto& trackIndexPair : trackInputIndices) {
		int trackIndex = trackIndexPair.first;
		int inputIndex = trackIndexPair.second;
		
		if(inputIndex < 0 || inputIndex >= (int)availableInputs.size()) {
			continue;
		}
		
		if(availableInputs[inputIndex] != nullptr &&
		   availableInputs[inputIndex]->getNodeRef() == node) {
			
			if(trackInstances.count(server) > 0 &&
			   trackIndex < (int)trackInstances[server].size() &&
			   trackInstances[server][trackIndex] != nullptr) {
				
				try {
					trackInstances[server][trackIndex]->set("in", bus);
					ofLogNotice("scPolyMixer") << "Track " << trackIndex << " input set to bus " << bus;
					
					// CRITICAL: Update connection tracking
					trackHasInput[trackIndex] = (bus >= 0);
					
					// If disconnecting (bus < 0 means no connection in the future, but won't happen here)
					// We need to detect when the node becomes nullptr
					// Actually, check if this is a valid connection
					bool isConnected = (bus >= 0 && node != nullptr);
					trackHasInput[trackIndex] = isConnected;
					
					ofLogNotice("scPolyMixer") << "Track " << trackIndex << " connection status: "
											  << (isConnected ? "CONNECTED" : "DISCONNECTED");
					
				} catch(const std::exception& e) {
					ofLogError("scPolyMixer") << "Error setting input for track " << trackIndex << ": " << e.what();
				}
			}
			break;
		}
	}
}

void scPolyMixer::resetInputBusses(ofxSCServer* server) {
	ofLogNotice("scPolyMixer") << "=== RESET INPUT BUSSES (called by serverManager) ===";
	
	// Clear the input buses map
	inputBuses[server].clear();
	
	// Set ALL track inputs to -1 (silence/disconnected)
	if(trackInstances.count(server) > 0) {
		for(int i = 0; i < trackInstances[server].size(); i++) {
			if(trackInstances[server][i] != nullptr) {
				try {
					trackInstances[server][i]->set("in", -1);
					
					// CRITICAL: Immediately zero out the VU meter for disconnected tracks
					if(vuParameters.count(i) > 0) {
						vector<float> zeroVU(numChannels.get(), 0.0f);
						vuParameters[i].setWithoutEventNotifications(zeroVU);
						
						if(trackVUData.count(i) > 0 && trackVUData[i] != nullptr) {
							trackVUData[i]->getParameter().set(zeroVU);
						}
					}
					
				} catch(const std::exception& e) {
					ofLogError("scPolyMixer") << "Error resetting input for track " << i << ": " << e.what();
				}
			}
		}
	}
	
	ofLogNotice("scPolyMixer") << "All tracks reset to -1 (disconnected) with zeroed VU meters";
}


void scPolyMixer::restoreTrackParameters(ofxSCServer* server, int trackIndex) {
	if(server == nullptr || trackInstances.count(server) == 0 ||
	   trackIndex < 0 || trackIndex >= trackInstances[server].size() ||
	   trackInstances[server][trackIndex] == nullptr) {
		return;
	}
	
	auto synth = trackInstances[server][trackIndex];
	
	try {
		// --- LEVEL RESTORATION (FIXED) ---
		if(trackLevels.count(trackIndex) > 0) {
			// Get raw values from GUI (0.0 to 2.0 usually)
			vector<float> levels = trackLevels[trackIndex]->getParameter().get();
			
			// Get Gain Vector multiplier
			vector<float> gainVecValues = gainVec.get();
			float gainVecMultiplier = (trackIndex < gainVecValues.size()) ? gainVecValues[trackIndex] : 1.0f;
			
			// Prepare final amplitude levels for SC
			vector<float> finalLevels;
			
			// LOGIC MUST MATCH updateTrackInstanceLevel EXACTLY:
			// 1. Levels are LINEAR amplitude, not normalized/dB (based on updateTrackInstanceLevel)
			// 2. Apply Gain Vector multiplier
			
			if(levels.size() == 1) {
				// Scalar case: Broadcast single value to all channels
				float linearLevel = levels[0];
				float scaledLevel = linearLevel * gainVecMultiplier;
				finalLevels = vector<float>(numChannels.get(), scaledLevel);
			} else {
				// Vector case: Per-channel mapping
				finalLevels.resize(numChannels.get());
				for(int ch = 0; ch < numChannels.get(); ch++) {
					float channelLinearLevel = (ch < levels.size()) ? levels[ch] : levels[0];
					finalLevels[ch] = channelLinearLevel * gainVecMultiplier;
				}
			}
			
			synth->set("level", finalLevels);
		}
		
		// --- BALANCE RESTORATION ---
		if(trackBalances.count(trackIndex) > 0) {
			float balance = trackBalances[trackIndex]->getParameter().get();
			synth->set("balance", balance);
		}
		
		// --- MUTE/SOLO RESTORATION ---
		bool individuallyMuted = false;
		if(trackMuteParams.count(trackIndex) > 0) {
			individuallyMuted = trackMuteParams[trackIndex]->get();
		}
		
		bool anySoloed = !soloedTracks.empty();
		bool shouldBeSoloMuted = false;
		
		if(anySoloed) {
			shouldBeSoloMuted = (soloedTracks.count(trackIndex) == 0);
		}
		
		bool finalMute = individuallyMuted || shouldBeSoloMuted;
		synth->set("mute", finalMute ? 1.0f : 0.0f);
		
		// --- MASTER LEVEL RESTORATION ---
		vector<float> masterLevels = masterLevel.get();
		// Master level logic in updateMasterLevel is purely LINEAR (direct pass-through)
		// We must replicate that here.
		if(masterLevels.size() == 1) {
			float linearLevel = masterLevels[0];
			vector<float> expandedMasterLevel(numChannels.get(), linearLevel);
			synth->set("masterLevel", expandedMasterLevel);
		} else {
			// Ensure size matches numChannels
			vector<float> expandedMasterLevel = masterLevels;
			if(expandedMasterLevel.size() != numChannels.get()) {
				expandedMasterLevel.resize(numChannels.get());
				// Fill remaining with last known value or default
				for(int i = masterLevels.size(); i < numChannels.get(); i++) {
					expandedMasterLevel[i] = (masterLevels.empty()) ? 1.0f : masterLevels.back();
				}
			}
			synth->set("masterLevel", expandedMasterLevel);
		}
		
		// --- VU PARAMETERS ---
		synth->set("vuAttackTime", masterVUAttack.get());
		synth->set("vuReleaseTime", masterVURelease.get());

		// --- BUS RESTORATION ---
		if(trackVUBuses.count(server) > 0 &&
		   trackIndex < trackVUBuses[server].size() &&
		   trackVUBuses[server][trackIndex] != nullptr &&
		   trackVUBuses[server][trackIndex]->index >= 0) {
			synth->set("vuBus", trackVUBuses[server][trackIndex]->index);
		} else {
			synth->set("vuBus", -1);
		}
		
		if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
			synth->set("out", outputBuses[server][0]);
		}
		
		ofLogNotice("scPolyMixer") << "Restored all parameters for track " << trackIndex;
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error restoring parameters for track " << trackIndex
								 << ": " << e.what();
	}
}

	
	int scPolyMixer::getOutputBusIndex(ofxSCServer* server, int index) {
		if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
			return outputBuses[server][index];
		}
		return -1;
	}
	
	// Instance management implementations
	void scPolyMixer::createTrackInstances(ofxSCServer* server) {
		int numTrackInstances = calculateNumTrackInstances();
		string synthDefName = getSynthDefName();
		
		ofLogNotice("scPolyMixer") << "Creating " << numTrackInstances
		<< " track instances using " << synthDefName;
		
		// Clear existing instances
		freeTrackInstances(server);
		
		// Create new instances
		trackInstances[server].resize(numTrackInstances);
		for(int i = 0; i < numTrackInstances; i++) {
			try {
				trackInstances[server][i] = new ofxSCSynth(synthDefName, server);
				ofLogVerbose("scPolyMixer") << "Created track instance " << i;
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error creating track instance " << i << ": " << e.what();
				trackInstances[server][i] = nullptr;
			}
		}
	}
	
	void scPolyMixer::freeTrackInstances(ofxSCServer* server) {
		if(!server || trackInstances.count(server) == 0) {
			return;
		}
		
		ofLogNotice("scPolyMixer") << "Freeing " << trackInstances[server].size() << " track instances";
		
		// Create a copy to avoid iterator invalidation
		auto instancesCopy = trackInstances[server];
		
		// Clear the original vector immediately
		trackInstances[server].clear();
		
		// Now safely delete each instance
		for(auto synth : instancesCopy) {
			if(synth != nullptr) {
				try {
					synth->free();
					delete synth;
				} catch(const std::exception& e) {
					ofLogWarning("scPolyMixer") << "Error freeing track instance: " << e.what();
				} catch(...) {
					ofLogWarning("scPolyMixer") << "Unknown error freeing track instance";
				}
			}
		}
		
		//is this really needed??
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	}
	
	void scPolyMixer::createVUMeterBuses(ofxSCServer* server) {
		if(server == nullptr) {
			ofLogError("scPolyMixer") << "Cannot create VU meter buses: server is null";
			return;
		}
		
		int numTrackInstances = calculateNumTrackInstances();
		int channelsPerTrack = numChannels.get();
		
		ofLogNotice("scPolyMixer") << "=== CREATING VU METER BUSES ===";
		ofLogNotice("scPolyMixer") << "Tracks: " << numTrackInstances
		<< ", Channels per track: " << channelsPerTrack;
		
		// Clear existing buses safely
		freeVUMeterBuses(server);
		
		// Validate parameters
		if(numTrackInstances <= 0 || numTrackInstances > 32) {
			ofLogError("scPolyMixer") << "Invalid track count: " << numTrackInstances;
			return;
		}
		
		if(channelsPerTrack <= 0 || channelsPerTrack > 16) {
			ofLogError("scPolyMixer") << "Invalid channel count: " << channelsPerTrack;
			return;
		}
		
		// Initialize the vector properly
		vuMeterBuses[server] = std::vector<ofxSCBus*>(numTrackInstances, nullptr);
		
		int successfulBuses = 0;
		for(int i = 0; i < numTrackInstances; i++) {
			try {
				ofLogNotice("scPolyMixer") << "Creating VU meter bus " << i
				<< " with " << channelsPerTrack << " channels...";
				
				// Create bus with error checking
				auto newBus = new ofxSCBus(RATE_CONTROL, channelsPerTrack, server);
				
				if(newBus == nullptr) {
					ofLogError("scPolyMixer") << "Failed to allocate VU meter bus " << i;
					continue;
				}
				
				ofLogNotice("scPolyMixer") << "Bus " << i << " allocated, checking index...";
				
				if(newBus->index < 0) {
					ofLogError("scPolyMixer") << "VU meter bus " << i
					<< " has invalid index: " << newBus->index;
					delete newBus;
					continue;
				}
				
				// Bus created successfully
				vuMeterBuses[server][i] = newBus;
				successfulBuses++;
				
				ofLogNotice("scPolyMixer") << "✅ VU meter bus " << i
				<< " created successfully (control bus index: "
				<< newBus->index << ")";
				
				// CRITICAL: Initialize the bus immediately
				newBus->requestValues();
				
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Exception creating VU meter bus " << i
				<< ": " << e.what();
				vuMeterBuses[server][i] = nullptr;
			}
		}
		
		ofLogNotice("scPolyMixer") << "=== VU METER BUS CREATION COMPLETE ===";
		ofLogNotice("scPolyMixer") << "Success rate: " << successfulBuses
		<< "/" << numTrackInstances << " buses";
		
		if(successfulBuses == 0) {
			ofLogError("scPolyMixer") << "❌ Failed to create any VU meter buses!";
			vuMeterBuses[server].clear();
			vuMetersReady = false;
		} else if(successfulBuses == numTrackInstances) {
			ofLogNotice("scPolyMixer") << "✅ All VU meter buses created successfully";
			vuMetersReady = true;
		} else {
			ofLogWarning("scPolyMixer") << "⚠️ Partial VU meter bus creation";
			vuMetersReady = false; // Don't enable if not all buses are ready
		}
	}
	
	void scPolyMixer::freeVUMeterBuses(ofxSCServer* server) {
		if(!server || vuMeterBuses.count(server) == 0) {
			return;
		}
		
		ofLogNotice("scPolyMixer") << "Freeing " << vuMeterBuses[server].size()
		<< " VU meter buses safely";
		
		// Create a copy to avoid iterator invalidation
		auto busesCopy = vuMeterBuses[server];
		
		// Clear the original vector immediately
		vuMeterBuses[server].clear();
		
		// Now safely delete each bus with additional error handling
		for(int i = 0; i < busesCopy.size(); i++) {
			if(busesCopy[i] != nullptr) {
				try {
					delete busesCopy[i];
					ofLogVerbose("scPolyMixer") << "Freed VU meter bus " << i;
				} catch(const std::exception& e) {
					ofLogWarning("scPolyMixer") << "Error freeing VU meter bus " << i
					<< ": " << e.what();
				} catch(...) {
					ofLogWarning("scPolyMixer") << "Unknown error freeing VU meter bus " << i;
				}
			}
		}
	}
	
	void scPolyMixer::updateVUMeters() {
		// Multiple safety layers
		if(!ENABLE_VU_METERS) return;
		if(!vuMetersReady) return;
		if(vuMeterBuses.empty()) return;
		if(trackVUMeters.empty()) return;
		
		static int updateCounter = 0;
		updateCounter++;
		
		// Log occasionally for debugging
		bool shouldLog = (updateCounter % 300 == 0); // Every ~5 seconds at 60fps
		
		if(shouldLog) {
			ofLogNotice("scPolyMixer") << "VU meter update #" << updateCounter
			<< " (servers: " << vuMeterBuses.size() << ")";
		}
		
		for(auto& serverBuses : vuMeterBuses) {
			ofxSCServer* server = serverBuses.first;
			if(server == nullptr) continue;
			
			try {
				auto& buses = serverBuses.second;
				
				for(int trackIndex = 0; trackIndex < buses.size(); trackIndex++) {
					// Ultra-defensive checking
					if(buses[trackIndex] == nullptr) continue;
					if(buses[trackIndex]->index < 0) continue;
					if(trackVUMeters.count(trackIndex) == 0) continue;
					
					auto vuMeterParam = trackVUMeters[trackIndex];
					if(vuMeterParam == nullptr) continue;
					
					try {
						// Get VU meter data
						vector<float> vuLevels = buses[trackIndex]->readValues;
						
						// Extensive data validation
						int expectedChannels = numChannels.get();
						bool dataValid = true;
						
						if(vuLevels.empty()) {
							vuLevels.resize(expectedChannels, 0.0f);
							dataValid = false;
						} else if(vuLevels.size() != expectedChannels) {
							vuLevels.resize(expectedChannels, 0.0f);
							dataValid = false;
						}
						
						// Validate each value
						for(auto& level : vuLevels) {
							if(!std::isfinite(level)) {
								level = 0.0f;
								dataValid = false;
							} else {
								level = ofClamp(level, 0.0f, 1.0f);
							}
						}
						
						// Only update parameter if we have reasonable data
						if(dataValid || shouldLog) {
							// Use setWithoutEventNotifications to prevent recursion
							vuMeterParam->getParameter().setWithoutEventNotifications(vuLevels);
							
							if(shouldLog && trackIndex == 0) {
								ofLogNotice("scPolyMixer") << "Track 0 VU: "
								<< (vuLevels.empty() ? 0.0f : vuLevels[0]);
							}
						}
						
						// Always request next values
						buses[trackIndex]->requestValues();
						
					} catch(const std::exception& e) {
						if(shouldLog) {
							ofLogError("scPolyMixer") << "Error updating VU meter for track "
							<< trackIndex << ": " << e.what();
						}
					}
				}
				
			} catch(const std::exception& e) {
				if(shouldLog) {
					ofLogError("scPolyMixer") << "Error in server VU meter update: " << e.what();
				}
			}
		}
	}
	
	void scPolyMixer::startVUMeterUpdates() {
		// VU meter updates will be called from the main update loop
		// This could be enhanced with a timer-based system if needed
		ofLogNotice("scPolyMixer") << "VU meter updates started";
	}
	
	void scPolyMixer::stopVUMeterUpdates() {
		// VU meter updates will be stopped automatically when listeners are cleared
		ofLogNotice("scPolyMixer") << "VU meter updates stopped";
	}

	
void scPolyMixer::presetSave(ofJson &json) {
	// Save track-specific data
	for(int i = 0; i < numTracks; i++) {
		if(trackMuteParams.count(i) > 0) {
			json["TrackInfo"][i]["Mute"] = trackMuteParams[i]->get();
		}
		if(trackBalanceParams.count(i) > 0) {
			json["TrackInfo"][i]["Balance"] = trackBalanceParams[i]->get();
		}
		if(trackSoloParams.count(i) > 0) {
			json["TrackInfo"][i]["Solo"] = trackSoloParams[i]->get();
		}
		if(trackNameParams.count(i) > 0) {
			json["TrackInfo"][i]["Name"] = trackNameParams[i]->get();
		}
		if(trackColorParams.count(i) > 0) {
			ofColor color = trackColorParams[i]->get();
			json["TrackInfo"][i]["Color"]["r"] = color.r;
			json["TrackInfo"][i]["Color"]["g"] = color.g;
			json["TrackInfo"][i]["Color"]["b"] = color.b;
		}
	}
}
	
void scPolyMixer::loadBeforeConnections(ofJson &json) {
	ofLogNotice("scPolyMixer") << "=== LOAD BEFORE CONNECTIONS ===";
	
	// Set flag to bypass deferred loading
	isLoadingPreset = true;
	
	// Load numTracks and numChannels first
	deserializeParameter(json, numTracks);
	deserializeParameter(json, numChannels);
	
	// CRITICAL: Force immediate track creation if needed (bypass deferred system)
	if(!tracksToAddNextFrame.empty()) {
		ofLogNotice("scPolyMixer") << "Forcing immediate track creation for preset loading: "
								   << tracksToAddNextFrame.size() << " tracks";
		
		for(int trackIndex : tracksToAddNextFrame) {
			try {
				// Pre-allocate tracking data
				trackPeakLevels[trackIndex] = vector<float>(numChannels.get(), -60.0f);
				trackPeakDecayTimers[trackIndex] = vector<float>(numChannels.get(), 0.0f);
				
				string vuName = "VU " + ofToString(trackIndex + 1);
				vector<float> defaultVU(numChannels.get(), 0.0f);
				vuParameters[trackIndex].set(vuName, defaultVU,
											vector<float>(numChannels.get(), 0.0f),
											vector<float>(numChannels.get(), 1.0f));
				
				// Add to GUI immediately
				addTrackToGUI(trackIndex);
				
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error adding track during preset load " << trackIndex << ": " << e.what();
			}
		}
		
		tracksToAddNextFrame.clear();
		needsGUIRebuild = false;
	}
	
	isLoadingPreset = false;
	
	ofLogNotice("scPolyMixer") << "Tracks ready for connection restoration";
}
	
void scPolyMixer::presetRecallAfterSettingParameters(ofJson &json) {
	ofLogNotice("scPolyMixer") << "=== PRESET RECALL AFTER SETTING PARAMETERS ===";
	
	// Restore track-specific data
	for(int i = 0; i < numTracks && i < json["TrackInfo"].size(); i++) {
		try {
			if(json["TrackInfo"][i].contains("Mute") && trackMuteParams.count(i) > 0) {
				trackMuteParams[i]->set(json["TrackInfo"][i]["Mute"]);
			}
			if(json["TrackInfo"][i].contains("Solo") && trackSoloParams.count(i) > 0) {
				bool soloState = json["TrackInfo"][i]["Solo"];
				trackSoloParams[i]->set(soloState);
				
				// CRITICAL: Update soloedTracks set directly
				if(soloState) {
					soloedTracks.insert(i);
					ofLogNotice("scPolyMixer") << "Restored solo state for track " << i;
				}
			}
			if(json["TrackInfo"][i].contains("Name") && trackNameParams.count(i) > 0) {
				trackNameParams[i]->set(json["TrackInfo"][i]["Name"]);
			}
			if(json["TrackInfo"][i].contains("Color") && trackColorParams.count(i) > 0) {
				ofColor color;
				color.r = json["TrackInfo"][i]["Color"]["r"];
				color.g = json["TrackInfo"][i]["Color"]["g"];
				color.b = json["TrackInfo"][i]["Color"]["b"];
				trackColorParams[i]->set(color);
			}
			if(json["TrackInfo"][i].contains("Balance") && trackBalanceParams.count(i) > 0) {
				trackBalanceParams[i]->set(json["TrackInfo"][i]["Balance"]);
			}
		} catch(ofJson::exception& e) {
			ofLog() << "scPolyMixer preset recall error: " << e.what();
		}
	}
	
	// CRITICAL: Apply solo logic after all solo states are restored
	updateSoloLogic();
	
	ofLogNotice("scPolyMixer") << "Solo states restored: " << soloedTracks.size() << " tracks soloed";
}
	
	void scPolyMixer::drawSeparator() {
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddLine(
											ImVec2(p.x, p.y),
											ImVec2(p.x + 240, p.y),
											IM_COL32(200, 200, 200, 255),
											1.0f
											);
		ImGui::Dummy(ImVec2(0, 4));
	}
	
	void scPolyMixer::addCustomRegion(ofParameter<std::function<void()>> p1, ofParameter<std::function<void()>> p2) {
		// Only add the first parameter to avoid duplication - the second is likely a duplicate/region variant
		// Use the base class method from ofxOceanodeNodeModel
		ofxOceanodeNodeModel::addCustomRegion(p1, p1.get());
	}
	
void scPolyMixer::freeVUBuses(ofxSCServer* server) {
	// RADICAL CRASH FIX: Use isUpdatingTracks for consistent protection
	if(server == nullptr) {
		ofLogWarning("scPolyMixer") << "Cannot free VU buses: server is null";
		return;
	}
	
	if(trackVUBuses.count(server) > 0) {
		ofLogNotice("scPolyMixer") << "Freeing " << trackVUBuses[server].size() << " VU buses";
		
		// CRITICAL: Disable VU updates during cleanup
		isUpdatingTracks = true;
		
		try {
			// ATOMIC: Clear immediately, then delete
			auto busesToDelete = trackVUBuses[server];
			trackVUBuses[server].clear();
			
			// Delete buses
			for(auto bus : busesToDelete) {
				if(bus != nullptr) {
					try {
						bus->free();
						delete bus;
					} catch(...) {
						// Ignore deletion errors
					}
				}
			}
			
		} catch(...) {
			// Ignore cleanup errors
		}
		
		// CRITICAL: Re-enable VU updates
		isUpdatingTracks = false;
	}
}

void scPolyMixer::drawCompactTrackWidget(int trackIndex) {
	// CRITICAL: Don't render if we're in the middle of updating tracks
	if(isUpdatingTracks) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	// Safety checks - return early if data not ready
	if (trackIndex < 0 || trackIndex >= numTracks.get()) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	if (vuParameters.count(trackIndex) == 0) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	if (trackMuteParams.count(trackIndex) == 0 || trackSoloParams.count(trackIndex) == 0) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	// CRITICAL: Check if peak tracking arrays exist and have correct size
	if(trackPeakLevels.count(trackIndex) == 0 ||
	   trackPeakLevels[trackIndex].size() != numChannels.get()) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	if(trackPeakDecayTimers.count(trackIndex) == 0 ||
	   trackPeakDecayTimers[trackIndex].size() != numChannels.get()) {
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	// NEW: Check if VU drawing is enabled
	if (!drawVU.get()) {
		// Only draw mute/solo buttons without VU meter
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();
		const float buttonHeight = 16.0f;
		const float buttonWidth = 115.0f;
		const float spacing = 5.0f;
		
		ImGui::SetCursorScreenPos(cursorPos);
		
		bool isMuted = trackMuteParams[trackIndex]->get();
		bool isSoloed = trackSoloParams[trackIndex]->get();
		
		// Mute button
		ImGui::PushID(trackIndex * 1000 + 1);
		ImVec4 muteColor = isMuted ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
		ImVec4 muteHover = isMuted ? ImVec4(0.9f, 0.3f, 0.3f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
		ImVec4 muteActive = isMuted ? ImVec4(0.7f, 0.1f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, muteColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muteHover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, muteActive);
		if (ImGui::Button("MUTE", ImVec2(buttonWidth, buttonHeight))) {
			trackMuteParams[trackIndex]->set(!isMuted);
		}
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		
		// Solo button
		ImGui::SameLine(0, spacing);
		ImGui::PushID(trackIndex * 1000 + 2);
		ImVec4 soloColor = isSoloed ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
		ImVec4 soloHover = isSoloed ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
		ImVec4 soloActive = isSoloed ? ImVec4(0.7f, 0.7f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, soloColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, soloHover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, soloActive);
		if (ImGui::Button("SOLO", ImVec2(buttonWidth, buttonHeight))) {
			trackSoloParams[trackIndex]->set(!isSoloed);
		}
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		
		ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + buttonHeight + spacing));
		ImGui::Dummy(ImVec2(240.0f, 2.0f));
		return;
	}
	
	// === FULL WIDGET WITH VU METER ===
	
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	// Get VU meter data
	const vector<float>& vuLevels = vuParameters[trackIndex].get();
	int numChans = vuLevels.size();
	
	// Widget dimensions - compact design with vertical channel stacking
	const float widgetWidth = 240.0f;
	const float totalVUHeight = trackVUHeight.get();
	const float channelHeight = totalVUHeight / numChans;
	const float vuHeight = totalVUHeight;
	const float buttonHeight = 16.0f;
	const float buttonWidth = 115.0f;
	const float spacing = 5.0f;
	const float totalHeight = vuHeight + spacing + buttonHeight + spacing;
	
	// === MULTICHANNEL VU METER (VERTICAL STACK) ===
	ImVec2 vuStart = cursorPos;
	ImVec2 vuEnd = ImVec2(vuStart.x + widgetWidth, vuStart.y + vuHeight);
	
	// Background for entire VU meter area
	drawList->AddRectFilled(vuStart, vuEnd, IM_COL32(20, 20, 20, 255));
	drawList->AddRect(vuStart, vuEnd, IM_COL32(80, 80, 80, 255), 0.0f, 0, 1.0f);
	
	// Draw each channel as a horizontal bar stacked vertically
	for (int ch = 0; ch < numChans; ch++) {
		float ampLevel = ofClamp(vuLevels[ch], 0.0f, 2.0f);
		float dbLevel = ampToDb(ampLevel);
		
		float channelY = vuStart.y + (ch * channelHeight);
		ImVec2 channelStart = ImVec2(vuStart.x, channelY);
		ImVec2 channelEnd   = ImVec2(vuStart.x + widgetWidth, channelY + channelHeight);
		
		drawList->AddRectFilled(channelStart, channelEnd, IM_COL32(30, 30, 30, 255));
		
		// Peak line tracking
		float attackMs = masterVUAttack.get();
		float releaseMs = masterVURelease.get();

		
		float attackCoeff = 1.0f - expf(-1000.0f / (attackMs * 60.0f));
		float releaseCoeff = 1.0f - expf(-1000.0f / (releaseMs * 60.0f));
		
		// CRITICAL: Extra bounds check before accessing array
		if (ch < trackPeakLevels[trackIndex].size() &&
			ch < trackPeakDecayTimers[trackIndex].size()) {
			
			if (dbLevel > trackPeakLevels[trackIndex][ch]) {
				trackPeakLevels[trackIndex][ch] = dbLevel;
				trackPeakDecayTimers[trackIndex][ch] = 1000.0f;
			} else {
				trackPeakDecayTimers[trackIndex][ch] -= 16.67f;
				if (trackPeakDecayTimers[trackIndex][ch] <= 0) {
					trackPeakLevels[trackIndex][ch] -= releaseCoeff * 0.5f;
					trackPeakLevels[trackIndex][ch] = ofClamp(trackPeakLevels[trackIndex][ch], -60.0f, 6.0f);
				}
			}
			
			// Draw the actual level meter
			if (dbLevel > -60.0f) {
				float meterPosition = dbToVUPosition(dbLevel, -60.0f, 6.0f);
				float meterWidth = (widgetWidth - 2) * meterPosition;
				ImVec2 meterEnd = ImVec2(channelStart.x + meterWidth, channelEnd.y);
				
				ImU32 meterColor = getVUMeterColorDB(dbLevel);
				drawList->AddRectFilled(channelStart, meterEnd, meterColor);
			}
			
			// Draw peak line
			float peakPosition = dbToVUPosition(trackPeakLevels[trackIndex][ch], -60.0f, 6.0f);
			if (peakPosition > 0.01f) {
				float peakX = channelStart.x + (widgetWidth - 2) * peakPosition;
				ImU32 peakColor = getVUMeterColorDB(trackPeakLevels[trackIndex][ch]);
				drawList->AddLine(
					ImVec2(peakX, channelStart.y),
					ImVec2(peakX, channelEnd.y),
					peakColor,
					2.0f
				);
			}
			
			// Draw 0dB reference line
			float zeroDbPosition = dbToVUPosition(0.0f, -60.0f, 6.0f);
			if (zeroDbPosition > 0.01f && zeroDbPosition < 0.99f) {
				float zeroDbX = channelStart.x + (widgetWidth - 2) * zeroDbPosition;
				drawList->AddLine(
					ImVec2(zeroDbX, channelStart.y),
					ImVec2(zeroDbX, channelEnd.y),
					IM_COL32(255, 255, 255, 180),
					1.0f
				);
			}
		}
	}
	
	// === MUTE & SOLO BUTTONS ===
	ImVec2 buttonsStart = ImVec2(cursorPos.x, cursorPos.y + vuHeight + spacing);
	ImGui::SetCursorScreenPos(buttonsStart);
	
	bool isMuted = trackMuteParams[trackIndex]->get();
	bool isSoloed = trackSoloParams[trackIndex]->get();
	
	// Mute button
	ImGui::PushID(trackIndex * 1000 + 1);
	ImVec4 muteColor = isMuted ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
	ImVec4 muteHover = isMuted ? ImVec4(0.9f, 0.3f, 0.3f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
	ImVec4 muteActive = isMuted ? ImVec4(0.7f, 0.1f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Button, muteColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muteHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, muteActive);
	if (ImGui::Button("MUTE", ImVec2(buttonWidth, buttonHeight))) {
		trackMuteParams[trackIndex]->set(!isMuted);
	}
	ImGui::PopStyleColor(3);
	ImGui::PopID();
	
	// Solo button
	ImGui::SameLine(0, spacing);
	ImGui::PushID(trackIndex * 1000 + 2);
	ImVec4 soloColor = isSoloed ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
	ImVec4 soloHover = isSoloed ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
	ImVec4 soloActive = isSoloed ? ImVec4(0.7f, 0.7f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Button, soloColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, soloHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, soloActive);
	if (ImGui::Button("SOLO", ImVec2(buttonWidth, buttonHeight))) {
		trackSoloParams[trackIndex]->set(!isSoloed);
	}
	ImGui::PopStyleColor(3);
	ImGui::PopID();
	
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + totalHeight));
	ImGui::Dummy(ImVec2(widgetWidth, 2.0f));
}

ImU32 scPolyMixer::getVUMeterColor(float level) {
	// Professional VU meter color scheme:
	// 0.0 - 0.7: Green (safe zone)
	// 0.7 - 0.85: Yellow (caution zone)
	// 0.85 - 1.0: Red (danger zone)
	
	if (level < 0.7f) {
		// Green zone: interpolate from dark green to bright green
		float greenFactor = level / 0.7f;
		int green = (int)(50 + (200 * greenFactor)); // 50 to 250
		return IM_COL32(0, green, 0, 255);
	}
	else if (level < 0.85f) {
		// Yellow zone: interpolate from green to yellow
		float yellowFactor = (level - 0.7f) / 0.15f; // 0 to 1
		int red = (int)(200 * yellowFactor);   // 0 to 200
		int green = 250;                       // Keep green high
		return IM_COL32(red, green, 0, 255);
	}
	else {
		// Red zone: interpolate from yellow to red
		float redFactor = (level - 0.85f) / 0.15f; // 0 to 1
		int red = (int)(200 + (55 * redFactor));   // 200 to 255
		int green = (int)(250 - (200 * redFactor)); // 250 to 50
		return IM_COL32(red, green, 0, 255);
	}
}

void scPolyMixer::drawMasterVUWidget() {
	// NEW: Check if VU drawing is enabled
	if (!drawVU.get()) {
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(cursorPos);
		ImGui::Dummy(ImVec2(240.0f, 4.0f));
		return;
	}
	
	// === FULL MASTER VU WIDGET ===
	
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	// Get master VU meter data
	const vector<float>& masterVULevels = masterVUMeter.get();
	int numChans = masterVULevels.size();
	
	// Master VU widget dimensions - bigger than track widgets
	// Master VU widget dimensions - bigger than track widgets
	const float widgetWidth = 240.0f;     // Full node width
	const float totalVUHeight = masterVUHeight.get();   // FIXED total height for all channels (bigger for master)
	const float channelHeight = (totalVUHeight / numChans); // Divide by number of channels
	const float vuHeight = totalVUHeight; // Total VU height is constant
	const float spacing = 2.0f;           // More spacing for master
	const float totalHeight = spacing + vuHeight + spacing;
	
	// === MASTER VU METER (VERTICAL STACK) ===
	ImVec2 vuStart = ImVec2(cursorPos.x, cursorPos.y + spacing);
	ImVec2 vuEnd = ImVec2(vuStart.x + widgetWidth, vuStart.y + vuHeight);
	
	// Background for entire VU meter area
	drawList->AddRectFilled(vuStart, vuEnd, IM_COL32(15, 15, 15, 255));
	drawList->AddRect(vuStart, vuEnd, IM_COL32(100, 100, 100, 255), 0.0f, 0, 2.0f);
	
	// Initialize master peak tracking if needed
	if (masterPeakLevels.size() != numChans) {
		masterPeakLevels.resize(numChans, -60.0f);
		masterPeakDecayTimers.resize(numChans, 0.0f);
	}
	
	// Draw each channel as a horizontal bar stacked vertically
	for (int ch = 0; ch < numChans; ch++) {
		float ampLevel = ofClamp(masterVULevels[ch], 0.0f, 2.0f); // Allow up to +6dB
		float dbLevel = ampToDb(ampLevel);
		
		// Calculate position for this channel
		float channelY = vuStart.y + (ch * channelHeight);
		ImVec2 channelStart = ImVec2(vuStart.x, channelY);
		ImVec2 channelEnd   = ImVec2(vuStart.x + widgetWidth, channelY + channelHeight);
		
		// Draw channel background (darker than track VUs)
		drawList->AddRectFilled(channelStart, channelEnd, IM_COL32(25, 25, 25, 255));
		
		// Update master peak tracking using configurable attack/release timing
		// Master VU meter data comes from summed track levels (already processed by SuperCollider)
		float masterAttackMs = masterVUAttack.get();
		float masterReleaseMs = masterVURelease.get();
		
		// Calculate coefficients based on master VU timing controls
		float masterAttackCoeff = 1.0f - expf(-1000.0f / (masterAttackMs * 60.0f));
		float masterReleaseCoeff = 1.0f - expf(-1000.0f / (masterReleaseMs * 60.0f));
		
		if (dbLevel > masterPeakLevels[ch]) {
			masterPeakLevels[ch] = dbLevel;
			masterPeakDecayTimers[ch] = masterReleaseMs * 5.0f; // Hold peak for 5x release time
		} else {
			masterPeakDecayTimers[ch] -= 16.67f; // ~60fps
			if (masterPeakDecayTimers[ch] <= 0) {
				masterPeakLevels[ch] -= masterReleaseCoeff * 0.5f; // Use configurable release
				masterPeakLevels[ch] = ofClamp(masterPeakLevels[ch], -60.0f, 6.0f);
			}
		}
		
		// Draw the actual level meter for this channel (dB-based)
		float meterPosition = 0.0f;
		if (dbLevel > -60.0f) {
			meterPosition = dbToVUPosition(dbLevel, -60.0f, 6.0f);
			float meterWidth = (widgetWidth - 4) * meterPosition;
			ImVec2 meterEnd = ImVec2(channelStart.x + meterWidth, channelEnd.y);
			
			// Master VU uses more intense colors based on dB
			ImU32 meterColor = getVUMeterColorDB(dbLevel);
			
			drawList->AddRectFilled(channelStart, meterEnd, meterColor);
		}
		
		// Draw master peak line (thicker than track peaks)
		float peakPosition = dbToVUPosition(masterPeakLevels[ch], -60.0f, 6.0f);
		if (peakPosition > 0.01f) {
			float peakX = channelStart.x + (widgetWidth - 4) * peakPosition;
			ImU32 peakColor = getVUMeterColorDB(masterPeakLevels[ch]);
			drawList->AddLine(
				ImVec2(peakX, channelStart.y),
				ImVec2(peakX, channelEnd.y),
				peakColor,
				3.0f // Thicker line for master
			);
		}
		
		// Draw 0dB reference line (permanent visual indicator) - more prominent for master
		float zeroDbPosition = dbToVUPosition(0.0f, -60.0f, 6.0f);
		if (zeroDbPosition > 0.01f && zeroDbPosition < 0.99f) {
			float zeroDbX = channelStart.x + (widgetWidth - 4) * zeroDbPosition;
			drawList->AddLine(
				ImVec2(zeroDbX, channelStart.y),
				ImVec2(zeroDbX, channelEnd.y),
				IM_COL32(255, 255, 255, 220), // Brighter white line for master 0dB reference
				1.5f // Slightly thicker for master
			);
		}
	}
	
	// Add some spacing after the master widget
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + totalHeight));
	ImGui::Dummy(ImVec2(widgetWidth, 4.0f)); // Spacing for next widget
}

// dB conversion utility functions
float scPolyMixer::ampToDb(float amp) {
	if (amp <= 0.0f) return -60.0f; // Minimum dB level
	return 20.0f * log10f(amp);
}

float scPolyMixer::dbToAmp(float db) {
	if (db <= -60.0f) return 0.0f;
	return powf(10.0f, db / 20.0f);
}

float scPolyMixer::dbToVUPosition(float db, float minDb, float maxDb) {
	if (db <= minDb) return 0.0f;
	if (db >= maxDb) return 1.0f;
	return (db - minDb) / (maxDb - minDb);
}

float scPolyMixer::vuPositionToDb(float position, float minDb, float maxDb) {
	return minDb + position * (maxDb - minDb);
}

float scPolyMixer::normalizedToDb(float normalized) {
	// Convert 0-1 range to -60dB to +6dB
	return (normalized * 66.0f) - 60.0f;
}

float scPolyMixer::dbToNormalized(float db) {
	// Convert -60dB to +6dB to 0-1 range
	return (db + 60.0f) / 66.0f;
}

ImU32 scPolyMixer::getVUMeterColorDB(float dbLevel) {
	// Professional VU meter color scheme based on dB levels:
	// -60 to -18dB: Green (safe zone)
	// -18 to -6dB: Yellow (caution zone)
	// -6 to 0dB: Orange (warning zone)
	// 0dB+: Red (danger zone)
	
	if (dbLevel < -18.0f) {
		// Green zone: interpolate from dark green to bright green
		float greenFactor = (dbLevel + 60.0f) / 42.0f; // -60 to -18 = 42dB range
		greenFactor = ofClamp(greenFactor, 0.0f, 1.0f);
		int green = (int)(50 + (200 * greenFactor)); // 50 to 250
		return IM_COL32(0, green, 0, 255);
	}
	else if (dbLevel < -6.0f) {
		// Yellow zone: interpolate from green to yellow
		float yellowFactor = (dbLevel + 18.0f) / 12.0f; // -18 to -6 = 12dB range
		yellowFactor = ofClamp(yellowFactor, 0.0f, 1.0f);
		int red = (int)(200 * yellowFactor);   // 0 to 200
		int green = 250;                       // Keep green high
		return IM_COL32(red, green, 0, 255);
	}
	else if (dbLevel < 0.0f) {
		// Orange zone: interpolate from yellow to orange
		float orangeFactor = (dbLevel + 6.0f) / 6.0f; // -6 to 0 = 6dB range
		orangeFactor = ofClamp(orangeFactor, 0.0f, 1.0f);
		int red = (int)(200 + (55 * orangeFactor));   // 200 to 255
		int green = (int)(250 - (100 * orangeFactor)); // 250 to 150
		return IM_COL32(red, green, 0, 255);
	}
	else {
		// Red zone: above 0dB
		float redFactor = ofClamp(dbLevel / 6.0f, 0.0f, 1.0f); // 0 to +6dB
		int red = 255;
		int green = (int)(150 - (150 * redFactor)); // 150 to 0
		return IM_COL32(red, green, 0, 255);
	}
}
