//
//  scPolyMixer.cpp
//  ofxOceanodeSupercollider
//
//  Multi-track mixer with dynamic track count and multi-instancing
//

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
	// ofLogNotice("scPolyMixer") << "Starting setup...";
	
	try {
		// Core parameters
		addParameter(numTracks.set("Num Tracks", 4, 1, 12)); // Limit to 12 tracks to prevent graphics crashes
		addParameter(numChannels.set("Num Channels", 2, 1, 16));

		addCustomRegion(
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
			ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
		);
		
		// Master level - 0-1 range (0 = -60dB, 1 = +6dB, default ~0.86 = -3dB)
		float defaultMasterLevel = ((-3.0f + 60.0f) / 66.0f); // Convert -3dB to 0-1 range
		addParameter(masterLevel.set("Master Level", vector<float>(1, defaultMasterLevel),
									vector<float>(1, 0.0f),
									vector<float>(1, 1.0f)));
		
		// Gain Vec - linear amp multipliers per track (starts with 4 tracks)
		addParameter(gainVec.set("Gain Vec", vector<float>(4, 1.0f),
								vector<float>(4, 0.0f),
								vector<float>(4, 2.0f)));
		
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
	
		
		// Set up parameter listeners
		listeners.push(numTracks.newListener([this](int &tracks){
			if(!isUpdatingTracks) {
				updateTrackCount();
			}
		}));
		
		listeners.push(numChannels.newListener([this](int &channels){
			if(!isUpdatingTracks) {
								
				// Need to recreate all instances with new SynthDef
				for(auto& serverInstances : trackInstances) {
					if(serverInstances.first != nullptr) {
						freeTrackInstances(serverInstances.first);
						createTrackInstances(serverInstances.first);
						createSynth(serverInstances.first);
					}
				}
				resendParams.notify();
			}
		}));
		
		listeners.push(masterLevel.newListener([this](vector<float> &levels){
			updateMasterLevel(levels);
		}));
		
		listeners.push(gainVec.newListener([this](vector<float> &levels){
			if(!isUpdatingTracks) {
				syncGainVecToTrackLevels();
			}
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
	// Update individual track VU meters
	vector<float> masterSum(numChannels.get(), 0.0f);
	int activeTracks = 0;
	
	for(auto& serverBuses : trackVUBuses) {
		ofxSCServer* server = serverBuses.first;
		if(server == nullptr) continue;
		
		auto& buses = serverBuses.second;
		for(int trackIndex = 0; trackIndex < buses.size(); trackIndex++) {
			if(buses[trackIndex] != nullptr && vuParameters.count(trackIndex) > 0) {
				// The ofxSCServer safety checks will prevent crashes during /c_set
				vector<float> trackLevels = buses[trackIndex]->readValues;
				vuParameters[trackIndex].setWithoutEventNotifications(trackLevels);
				
				// Update VU Data output parameter
				if(trackVUData.count(trackIndex) > 0 && trackVUData[trackIndex] != nullptr) {
					trackVUData[trackIndex]->getParameter().set(trackLevels);
				}
				
				// Sum for master VU
				bool trackIsAudible = true;
				if(trackMuteParams.count(trackIndex) > 0 && trackMuteParams[trackIndex]->get()) {
					trackIsAudible = false;
				}
				
				if(!soloedTracks.empty() && soloedTracks.count(trackIndex) == 0) {
					trackIsAudible = false;
				}
				
				if(trackIsAudible) {
					for(int ch = 0; ch < trackLevels.size() && ch < masterSum.size(); ch++) {
						masterSum[ch] += trackLevels[ch];
					}
					activeTracks++;
				}
				
				buses[trackIndex]->requestValues();
			}
		}
	}
	
	// Update master VU meter
	vector<float> masterLevels = masterLevel.get();
	float masterNormalizedLevel = (masterLevels.size() > 0) ? masterLevels[0] : ((-3.0f + 60.0f) / 66.0f);
	float masterDbLevel = (masterNormalizedLevel * 66.0f) - 60.0f;
	float masterScale = dbToAmp(masterDbLevel);
	
	for(int ch = 0; ch < masterSum.size(); ch++) {
		masterSum[ch] = ofClamp(masterSum[ch] * masterScale, 0.0f, 2.0f);
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
		// ofLogNotice("scPolyMixer") << "Updating track count to " << currentTracks;
		
		// SAFETY CHECK: Don't proceed if SuperCollider isn't ready
		// Check if we have any server instances and if they're properly initialized
		bool scReady = trackInstances.empty(); // If no instances, we're safe to proceed
		
		for(auto& serverInstances : trackInstances) {
			if(serverInstances.first != nullptr) {
				// Check if server exists and has been initialized
				// We'll assume it's ready if we have a valid server pointer
				scReady = true;
				break;
			}
		}
		
		// For now, always proceed but with extra logging
		if(!scReady) {
			// ofLogWarning("scPolyMixer") << "SuperCollider server may not be ready, proceeding with caution";
		}
		
		// INCREMENTAL APPROACH: Only change what needs to change
		
		if(trackLevels.size() != currentTracks) {
			// ofLogNotice("scPolyMixer") << "Updating tracks: " << trackLevels.size() << " -> " << currentTracks;
			
			int oldSize = trackLevels.size();
			
			if(oldSize > currentTracks) {
				// REMOVING TRACKS: Remove from the end, one by one
				// CRITICAL: Process all removals first, then clean up
				vector<int> tracksToRemove;
				for(int j = oldSize - 1; j >= currentTracks; j--) {
					tracksToRemove.push_back(j);
				}
				
				// Remove all tracks in one batch to avoid multiple GUI updates
				for(int trackIndex : tracksToRemove) {
					removeTrackFromGUI(trackIndex);
				}
				
				// Remove excess inputs one by one using the safe method
				while(inputs.size() > currentTracks) {
					int lastIndex = inputs.size() - 1;
					try {
						scNode::removeInput(lastIndex);
						// ofLogVerbose("scPolyMixer") << "Removed input " << lastIndex;
					} catch(const std::exception& e) {
						// ofLogWarning("scPolyMixer") << "Error removing input " << lastIndex << ": " << e.what();
						// If we can't remove safely, just clear the vectors
						inputs.clear();
						availableInputs.clear();
						break;
					}
				}
				
			} else {
				// ADDING TRACKS: Add new tracks to the end
				for(int trackIndex = oldSize; trackIndex < currentTracks; trackIndex++) {
					try {
						addTrackToGUI(trackIndex);
					} catch(const std::exception& e) {
						ofLogError("scPolyMixer") << "Error adding track " << trackIndex << ": " << e.what();
					}
				}
			}
		}
		
		// Update gainVec to match track count - one multiplier per track
		vector<float> currentLevels = gainVec.get();
		currentLevels.resize(currentTracks, 1.0f); // Default multiplier 1.0 for new tracks
		gainVec.setWithoutEventNotifications(currentLevels);
		
		// Update solo tracking
		std::set<int> validSoloedTracks;
		for(int trackIndex : soloedTracks) {
			if(trackIndex < currentTracks) {
				validSoloedTracks.insert(trackIndex);
			}
		}
		soloedTracks = validSoloedTracks;
		
		// ofLogNotice("scPolyMixer") << "Track count update complete - now have "
		//						   << trackLevels.size() << " tracks and " << inputs.size() << " inputs";
		
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error in updateTrackCount: " << e.what();
	} catch(...) {
		ofLogError("scPolyMixer") << "Unknown error in updateTrackCount";
	}
	
	isUpdatingTracks = false;
}

void scPolyMixer::addTrackToGUI(int trackIndex) {
	if(trackLevels.count(trackIndex) > 0) {
		// ofLogWarning("scPolyMixer") << "Track " << trackIndex << " already exists in GUI";
		return;
	}
	
	// No need to process pending removals since we now do immediate removal
	// ofLogNotice("scPolyMixer") << "Adding track " << trackIndex << " (immediate removal system active)";
	
	string trackName = "Track " + ofToString(trackIndex + 1);
	// ofLogNotice("scPolyMixer") << "Adding " << trackName << " to GUI";
	
	try {
		// Add input if we don't have enough
		if(inputs.size() <= trackIndex) {
			scNode::addInput("In " + ofToString(trackIndex + 1));
		}
		
		// Track input index mapping
		trackInputIndices[trackIndex] = trackIndex;
		
		// CRITICAL: Declare parameters outside try-catch blocks for scope access
		auto levelParam = std::make_shared<ofParameter<vector<float>>>();
		auto muteParam = std::make_shared<ofParameter<bool>>();
		auto soloParam = std::make_shared<ofParameter<bool>>();
		auto vuAttackParam = std::make_shared<ofParameter<float>>();
		auto vuReleaseParam = std::make_shared<ofParameter<float>>();
		
		try {
			// Track level parameter - 0-1 range (0 = -60dB, 1 = +6dB, default ~0.86 = -3dB)
			float defaultTrackLevel = ((-3.0f + 60.0f) / 66.0f); // Convert -3dB to 0-1 range
			vector<float> defaultLevel(1, defaultTrackLevel);
			levelParam->set("Level " + ofToString(trackIndex + 1), defaultLevel,
						   vector<float>(1, 0.0f), vector<float>(1, 1.0f));
			trackLevelParams[trackIndex] = levelParam;
			auto levelOceanodeParam = addParameter(*levelParam);
			trackLevels[trackIndex] = levelOceanodeParam;
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating level parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		try {
			// Create mute and solo parameters - HIDDEN from regular GUI (only shown in imgui widget)
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
		
		try {
			// VU Attack/Release parameters - HIDDEN inspector parameters
			vuAttackParam->set("VU Attack " + ofToString(trackIndex + 1), 10.0f, 1.0f, 100.0f); // ms
			trackVUAttack[trackIndex] = vuAttackParam;
			addInspectorParameter(*vuAttackParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating VU attack parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		try {
			vuReleaseParam->set("VU Release " + ofToString(trackIndex + 1), 300.0f, 10.0f, 2000.0f); // ms
			trackVURelease[trackIndex] = vuReleaseParam;
			addInspectorParameter(*vuReleaseParam);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating VU release parameter for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Initialize peak tracking for this track
		trackPeakLevels[trackIndex] = vector<float>(numChannels.get(), -60.0f); // Start at -60dB
		trackPeakDecayTimers[trackIndex] = vector<float>(numChannels.get(), 0.0f);
		
		// VU Meter parameter - EXACTLY like scInfo - HIDDEN from regular GUI
		string vuName = "VU " + ofToString(trackIndex + 1);
		vector<float> defaultVU(numChannels.get(), 0.0f);
		vuParameters[trackIndex].set(vuName, defaultVU,
									vector<float>(numChannels.get(), 0.0f),
									vector<float>(numChannels.get(), 1.0f));
		// Add as inspector parameter (hidden from regular GUI, only shown in imgui widget)
		addInspectorParameter(vuParameters[trackIndex]);
		
		try {
			// VU Data output parameter - normalized 0-1 VU meter data for other nodes
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
		
		// ofLogNotice("scPolyMixer") << "Created VU Data output parameter for track " << trackIndex
		//						   << " with " << numChannels.get() << " channels";
		
		// Add the compact widget as a custom region
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
		
		// Add a subtle separator after each track widget
		string separatorName = "TrackSeparator_" + ofToString(trackIndex);
		try {
			addCustomRegion(
				ofParameter<std::function<void()>>().set(separatorName, [](){
					ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddLine(
						ImVec2(p.x + 10, p.y),
						ImVec2(p.x + 230, p.y),
						IM_COL32(100, 100, 100, 100),
						1.0f
					);
					ImGui::Dummy(ImVec2(0, 3));
				}),
				ofParameter<std::function<void()>>().set(separatorName + "_Region", [](){
					ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddLine(
						ImVec2(p.x + 10, p.y),
						ImVec2(p.x + 230, p.y),
						IM_COL32(100, 100, 100, 100),
						1.0f
					);
					ImGui::Dummy(ImVec2(0, 3));
				})
			);
		} catch(const std::exception& e) {
			ofLogError("scPolyMixer") << "Error creating separator for track " << trackIndex << ": " << e.what();
			throw;
		}
		
		// Set up listeners for level, mute, solo (but NOT for VU - it's output only)
		listeners.push(levelParam->newListener([this, trackIndex](vector<float> &levels){
			if(!isUpdatingTracks && trackLevels.count(trackIndex) > 0) {
				updateTrackInstanceLevel(trackIndex, levels);
				syncTrackLevelsToGainVec();
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
		
		// Add listeners for VU attack/release parameters
		listeners.push(vuAttackParam->newListener([this, trackIndex](float &attackTime){
			if(!isUpdatingTracks) {
				updateTrackVUTiming(trackIndex, attackTime, -1.0f); // -1 means don't change release
			}
		}));
		
		listeners.push(vuReleaseParam->newListener([this, trackIndex](float &releaseTime){
			if(!isUpdatingTracks) {
				updateTrackVUTiming(trackIndex, -1.0f, releaseTime); // -1 means don't change attack
			}
		}));
		
		// ofLogNotice("scPolyMixer") << "Successfully added " << trackName;
		
	} catch(const std::exception& e) {
		ofLogError("scPolyMixer") << "Error adding track " << trackIndex << ": " << e.what();
		// Clean up on error
		trackLevels.erase(trackIndex);
		trackVUData.erase(trackIndex); // Remove VU Data output parameter
		// trackMutes and trackSolos maps removed - using direct parameters now
		trackLevelParams.erase(trackIndex);
		trackMuteParams.erase(trackIndex);
		trackSoloParams.erase(trackIndex);
		trackVUAttack.erase(trackIndex);
		trackVURelease.erase(trackIndex);
		trackPeakLevels.erase(trackIndex);
		trackPeakDecayTimers.erase(trackIndex);
		trackVUAttack.erase(trackIndex);
		trackVURelease.erase(trackIndex);
		
		// Clear peak tracking data
		trackPeakLevels.erase(trackIndex);
		trackPeakDecayTimers.erase(trackIndex);
		trackInputIndices.erase(trackIndex);
		vuParameters.erase(trackIndex);
		throw;
	}
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
	
	ofLogNotice("scPolyMixer") << "Recreating " << numTracksCount << " VU buses with EXTRA SAFETY";
	
	// Clean up existing buses
	if(trackVUBuses.count(server) > 0) {
		auto busesToDelete = trackVUBuses[server];
		trackVUBuses[server].clear();
		
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
	}
	
	trackVUBuses[server].resize(numTracksCount, nullptr);
	
	for(int i = 0; i < numTracksCount; i++) {
		try {
			int channelCount = numChannels.get();
			if(channelCount <= 0 || channelCount > 16) continue;
			
			ofxSCBus* newBus = new ofxSCBus(RATE_CONTROL, channelCount, server);
			
			if(newBus != nullptr && newBus->index >= 0 && newBus->index < 4096) {
				// EXTRA SAFETY: Verify the server's controlBusses array has this bus
				if(server->controlBusses[newBus->index] != nullptr) {
					trackVUBuses[server][i] = newBus;
					newBus->requestValues();
					ofLogNotice("scPolyMixer") << "Created VERIFIED VU bus " << i
											  << " with index " << newBus->index;
				} else {
					ofLogError("scPolyMixer") << "Server controlBusses array invalid for index "
											 << newBus->index;
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
	
	ofLogNotice("scPolyMixer") << "VU bus recreation complete with extra safety checks";
}
	
	void scPolyMixer::removeTrackFromGUI(int trackIndex) {
		// ofLogNotice("scPolyMixer") << "Removing track " << trackIndex << " from GUI";
		
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
			
			if(trackMuteParams.count(trackIndex) > 0) {
				trackMuteParams[trackIndex].reset();
				trackMuteParams.erase(trackIndex);
			}
			
			if(trackSoloParams.count(trackIndex) > 0) {
				trackSoloParams[trackIndex].reset();
				trackSoloParams.erase(trackIndex);
			}
			
			if(trackVUAttack.count(trackIndex) > 0) {
				trackVUAttack[trackIndex].reset();
				trackVUAttack.erase(trackIndex);
			}
			
			if(trackVURelease.count(trackIndex) > 0) {
				trackVURelease[trackIndex].reset();
				trackVURelease.erase(trackIndex);
			}
			
			if(trackVUData.count(trackIndex) > 0) {
				trackVUData[trackIndex].reset();
				trackVUData.erase(trackIndex);
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
				"Mute " + ofToString(trackIndex + 1),
				"Solo " + ofToString(trackIndex + 1),
				"VU Data " + ofToString(trackIndex + 1),
				"VU Attack " + ofToString(trackIndex + 1),
				"VU Release " + ofToString(trackIndex + 1),
				"VU " + ofToString(trackIndex + 1)
			};
			
			// Add custom regions
			string widgetName = "Track " + ofToString(trackIndex + 1) + " Control";
			parametersToRemoveNow.push_back(widgetName);
			parametersToRemoveNow.push_back(widgetName + "_Region");
			string separatorName = "TrackSeparator_" + ofToString(trackIndex);
			parametersToRemoveNow.push_back(separatorName);
			parametersToRemoveNow.push_back(separatorName + "_Region");
			
			// ofLogNotice("scPolyMixer") << "Force removing " << parametersToRemoveNow.size()
			//						   << " parameters for track " << trackIndex;
			
			int successfulRemovals = 0;
			for(const auto& paramName : parametersToRemoveNow) {
				bool removed = false;
				
				// Force removal from regular parameters (ignore errors)
				try {
					removeParameter(paramName);
					removed = true;
					// ofLogNotice("scPolyMixer") << "✓ Removed regular parameter: " << paramName;
				} catch(...) {
					// Ignore errors, try inspector parameters
				}
				
				// Force removal from inspector parameters (ignore errors)
				try {
					removeInspectorParameter(paramName);
					removed = true;
					// ofLogNotice("scPolyMixer") << "✓ Removed inspector parameter: " << paramName;
				} catch(...) {
					// Ignore errors
				}
				
				if(removed) {
					successfulRemovals++;
				} else {
					// ofLogWarning("scPolyMixer") << "✗ Could not remove parameter: " << paramName;
				}
			}
			
			// ofLogNotice("scPolyMixer") << "Parameter removal complete for track " << trackIndex
			//						   << " (" << successfulRemovals << "/" << parametersToRemoveNow.size() << " successful)";
			
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
		trackMuteParams.clear();
		trackSoloParams.clear();
		trackVUMeterParams.clear();
		trackVUData.clear();
		trackVUAttack.clear();
		trackVURelease.clear();
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
				removeInspectorParameter("VU " + ofToString(trackIndex + 1));
				removeParameter("VU Data " + ofToString(trackIndex + 1));
				removeInspectorParameter("VU Attack " + ofToString(trackIndex + 1));
				removeInspectorParameter("VU Release " + ofToString(trackIndex + 1));
				
				// Remove custom regions (ImGui widgets and separators)
				string widgetName = "Track " + ofToString(trackIndex + 1) + " Control";
				removeParameter(widgetName);
				string separatorName = "TrackSeparator_" + ofToString(trackIndex);
				removeParameter(separatorName);
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
					// CRITICAL FIX: Apply gainVec multiplier to track levels
					vector<float> gainVecValues = gainVec.get();
					float gainVecMultiplier = (trackIndex < gainVecValues.size()) ? gainVecValues[trackIndex] : 1.0f;
					
					vector<float> finalLevels;
					if(levels.size() == 1) {
						// Scalar mode - convert 0-1 range to dB, then to amp, then apply gainVec multiplier
						float normalizedLevel = levels[0];
						float dbLevel = (normalizedLevel * 66.0f) - 60.0f; // Convert 0-1 to -60dB to +6dB
						float ampLevel = dbToAmp(dbLevel);
						float scaledLevel = ampLevel * gainVecMultiplier;
						finalLevels = vector<float>(numChannels.get(), scaledLevel);
					} else {
						// Vector mode - convert each 0-1 to dB to amp, then multiply by gainVec
						finalLevels.resize(numChannels.get());
						for(int ch = 0; ch < numChannels.get(); ch++) {
							float channelNormalizedLevel = (ch < levels.size()) ? levels[ch] : levels[0];
							float channelDbLevel = (channelNormalizedLevel * 66.0f) - 60.0f;
							float channelAmpLevel = dbToAmp(channelDbLevel);
							finalLevels[ch] = channelAmpLevel * gainVecMultiplier;
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
							// Convert 0-1 range to dB to amplitude for SuperCollider
							if(levels.size() == 1) {
								float normalizedLevel = levels[0];
								float dbLevel = (normalizedLevel * 66.0f) - 60.0f; // Convert 0-1 to -60dB to +6dB
								float ampLevel = dbToAmp(dbLevel);
								vector<float> expandedMasterLevel(numChannels.get(), ampLevel);
								synth->set("masterLevel", expandedMasterLevel);
							} else {
								vector<float> ampLevels;
								ampLevels.resize(levels.size());
								for(int i = 0; i < levels.size(); i++) {
									float normalizedLevel = levels[i];
									float dbLevel = (normalizedLevel * 66.0f) - 60.0f;
									ampLevels[i] = dbToAmp(dbLevel);
								}
								synth->set("masterLevel", ampLevels);
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
		if(ENABLE_VU_METERS) {
			createVUMeterBuses(server);
		}
	}
	
void scPolyMixer::createSynth(ofxSCServer* server) {
	if(trackInstances.count(server) == 0) {
		ofLogWarning("scPolyMixer") << "No track instances for server";
		return;
	}
	
	ofLogNotice("scPolyMixer") << "Creating " << trackInstances[server].size()
							   << " track synths using " << getSynthDefName();
	
	// Create VU meter buses normally
	recreateVUBuses(server);
	
	// Create all track synth instances
	for(int i = 0; i < trackInstances[server].size(); i++) {
		if(trackInstances[server][i] != nullptr) {
			try {
				// Create the synth
				trackInstances[server][i]->create();
				
				// Set basic parameters (convert default normalized values to amplitude)
				float defaultNormalizedLevel = ((-3.0f + 60.0f) / 66.0f); // -3dB as 0-1 range
				float defaultDbLevel = (defaultNormalizedLevel * 66.0f) - 60.0f;
				vector<float> defaultLevel(numChannels.get(), dbToAmp(defaultDbLevel));
				trackInstances[server][i]->set("level", defaultLevel);
				trackInstances[server][i]->set("mute", 0.0f);
				
				vector<float> defaultMasterLevel(numChannels.get(), dbToAmp(defaultDbLevel));
				trackInstances[server][i]->set("masterLevel", defaultMasterLevel);
				
				// Set VU meter attack/release parameters
				float attackTime = (trackVUAttack.count(i) > 0) ? trackVUAttack[i]->get() : 10.0f;
				float releaseTime = (trackVURelease.count(i) > 0) ? trackVURelease[i]->get() : 300.0f;
				
				trackInstances[server][i]->set("vuAttackTime", attackTime);
				trackInstances[server][i]->set("vuReleaseTime", releaseTime);
				
				// Set VU bus normally - let the ofxSCServer safety checks handle invalid access
				if(trackVUBuses.count(server) > 0 &&
				   i < trackVUBuses[server].size() &&
				   trackVUBuses[server][i] != nullptr) {
					trackInstances[server][i]->set("vuBus", trackVUBuses[server][i]->index);
					ofLogNotice("scPolyMixer") << "Set VU bus for track " << i
											  << " to index " << trackVUBuses[server][i]->index;
				} else {
					trackInstances[server][i]->set("vuBus", -1);
					ofLogWarning("scPolyMixer") << "No VU bus for track " << i;
				}
				
				ofLogNotice("scPolyMixer") << "Created track " << i << " synth successfully";
				
			} catch(const std::exception& e) {
				ofLogError("scPolyMixer") << "Error creating track " << i << " synth: " << e.what();
			}
		}
	}
	
	// Send initial parameter values
	resendParams.notify();
	
	ofLogNotice("scPolyMixer") << "All track synths created";
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
	if(server == nullptr || node == nullptr) {
		ofLogError("scPolyMixer") << "Cannot set input bus: server or node is null";
		return;
	}
	
	inputBuses[server][node] = bus;
	ofLogNotice("scPolyMixer") << "Input bus set to " << bus << " for node";
	
	// Find which track this input corresponds to
	for(auto& trackIndexPair : trackInputIndices) {
		int trackIndex = trackIndexPair.first;
		int inputIndex = trackIndexPair.second;
		
		if(inputIndex < 0 || inputIndex >= availableInputs.size()) {
			continue;
		}
		
		if(availableInputs[inputIndex] != nullptr &&
		   availableInputs[inputIndex]->getNodeRef() == node) {
			
			if(trackInstances.count(server) > 0 &&
			   trackIndex >= 0 && trackIndex < trackInstances[server].size() &&
			   trackInstances[server][trackIndex] != nullptr) {
				
				try {
					// SIMPLE: Just set the input bus - let ofxSCServer safety checks handle the rest
					trackInstances[server][trackIndex]->set("in", bus);
					ofLogNotice("scPolyMixer") << "Track " << trackIndex << " input set to bus " << bus;
					
				} catch(const std::exception& e) {
					ofLogError("scPolyMixer") << "Error setting input bus for track " << trackIndex << ": " << e.what();
				}
			}
			break;
		}
	}
}

void scPolyMixer::restoreTrackParameters(ofxSCServer* server, int trackIndex) {
	if(server == nullptr || trackInstances.count(server) == 0 ||
	   trackIndex < 0 || trackIndex >= trackInstances[server].size() ||
	   trackInstances[server][trackIndex] == nullptr) {
		return;
	}
	
	auto synth = trackInstances[server][trackIndex];
	
	try {
		// Restore track level (convert normalized to amplitude)
		if(trackLevels.count(trackIndex) > 0) {
			vector<float> levels = trackLevels[trackIndex]->getParameter().get();
			vector<float> ampLevels;
			
			if(levels.size() == 1) {
				// Scalar mode - convert to dB then amp
				float normalizedLevel = levels[0];
				float dbLevel = (normalizedLevel * 66.0f) - 60.0f;
				float ampLevel = dbToAmp(dbLevel);
				ampLevels = vector<float>(numChannels.get(), ampLevel);
			} else {
				// Vector mode
				ampLevels.resize(levels.size());
				for(int i = 0; i < levels.size(); i++) {
					float normalizedLevel = levels[i];
					float dbLevel = (normalizedLevel * 66.0f) - 60.0f;
					ampLevels[i] = dbToAmp(dbLevel);
				}
			}
			
			// Apply gainVec multiplier
			vector<float> gainVecValues = gainVec.get();
			float gainVecMultiplier = (trackIndex < gainVecValues.size()) ? gainVecValues[trackIndex] : 1.0f;
			
			for(auto& level : ampLevels) {
				level *= gainVecMultiplier;
			}
			
			synth->set("level", ampLevels);
		}
		
		// Restore mute state
		if(trackMuteParams.count(trackIndex) > 0) {
			bool muted = trackMuteParams[trackIndex]->get();
			synth->set("mute", muted ? 1.0f : 0.0f);
		}
		
		// Restore master level
		vector<float> masterLevels = masterLevel.get();
		if(masterLevels.size() == 1) {
			float normalizedLevel = masterLevels[0];
			float dbLevel = (normalizedLevel * 66.0f) - 60.0f;
			float ampLevel = dbToAmp(dbLevel);
			vector<float> expandedMasterLevel(numChannels.get(), ampLevel);
			synth->set("masterLevel", expandedMasterLevel);
		} else {
			vector<float> ampLevels;
			ampLevels.resize(masterLevels.size());
			for(int i = 0; i < masterLevels.size(); i++) {
				float normalizedLevel = masterLevels[i];
				float dbLevel = (normalizedLevel * 66.0f) - 60.0f;
				ampLevels[i] = dbToAmp(dbLevel);
			}
			synth->set("masterLevel", ampLevels);
		}
		
		// Restore VU timing parameters
		if(trackVUAttack.count(trackIndex) > 0) {
			synth->set("vuAttackTime", trackVUAttack[trackIndex]->get());
		}
		if(trackVURelease.count(trackIndex) > 0) {
			synth->set("vuReleaseTime", trackVURelease[trackIndex]->get());
		}
		
		// Restore VU bus (CRITICAL: Only set if valid)
		if(trackVUBuses.count(server) > 0 &&
		   trackIndex < trackVUBuses[server].size() &&
		   trackVUBuses[server][trackIndex] != nullptr &&
		   trackVUBuses[server][trackIndex]->index >= 0) {
			
			synth->set("vuBus", trackVUBuses[server][trackIndex]->index);
			ofLogNotice("scPolyMixer") << "Restored VU bus " << trackVUBuses[server][trackIndex]->index
									  << " for track " << trackIndex;
		} else {
			synth->set("vuBus", -1); // Disable VU if no valid bus
			ofLogWarning("scPolyMixer") << "No valid VU bus for track " << trackIndex << ", disabled";
		}
		
		// Restore output bus
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
			if(trackSoloParams.count(i) > 0) {
				json["TrackInfo"][i]["Solo"] = trackSoloParams[i]->get();
			}
		}
	}
	
	void scPolyMixer::loadBeforeConnections(ofJson &json) {
		// MINIMAL CONNECTION RECOVERY FIX: Load numTracks first to ensure structure exists
		deserializeParameter(json, numTracks);
		deserializeParameter(json, numChannels);
	}
	
	void scPolyMixer::presetRecallAfterSettingParameters(ofJson &json) {
		// Restore track-specific data
		for(int i = 0; i < numTracks && i < json["TrackInfo"].size(); i++) {
			try {
				if(json["TrackInfo"][i].contains("Mute") && trackMuteParams.count(i) > 0) {
					trackMuteParams[i]->set(json["TrackInfo"][i]["Mute"]);
				}
				if(json["TrackInfo"][i].contains("Solo") && trackSoloParams.count(i) > 0) {
					trackSoloParams[i]->set(json["TrackInfo"][i]["Solo"]);
				}
			} catch(ofJson::exception& e) {
				ofLog() << "scPolyMixer preset recall error: " << e.what();
			}
		}
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
	// Safety checks
	if (trackIndex < 0 || trackIndex >= numTracks.get()) return;
	if (vuParameters.count(trackIndex) == 0) return;
	if (trackMuteParams.count(trackIndex) == 0 || trackSoloParams.count(trackIndex) == 0) return;
	
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	// Get VU meter data
	const vector<float>& vuLevels = vuParameters[trackIndex].get();
	int numChans = vuLevels.size();
	
	// Widget dimensions - compact design with vertical channel stacking
	const float widgetWidth = 240.0f;    // Full node width
	const float channelHeight = 8.0f;     // Height per channel
	const float vuHeight = numChans * channelHeight; // Total VU height based on channels
	const float buttonHeight = 16.0f;     // Small button height
	const float buttonWidth = 115.0f;     // Half width minus spacing
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
		float ampLevel = ofClamp(vuLevels[ch], 0.0f, 2.0f); // Allow up to +6dB
		float dbLevel = ampToDb(ampLevel);
		
		// Calculate position for this channel
		float channelY = vuStart.y + (ch * channelHeight);
		ImVec2 channelStart = ImVec2(vuStart.x + 1, channelY + 1);
		ImVec2 channelEnd = ImVec2(vuStart.x + widgetWidth - 1, channelY + channelHeight - 1);
		
		// Draw channel background (slightly lighter than main background)
		drawList->AddRectFilled(channelStart, channelEnd, IM_COL32(30, 30, 30, 255));
		
		// Peak line tracking (separate from VU meter bars)
		// NOTE: VU meter bars now use SuperCollider's LagUD.kr() for attack/release
		// These calculations are only for the visual peak indicator lines
		float attackMs = (trackVUAttack.count(trackIndex) > 0) ? trackVUAttack[trackIndex]->get() : 10.0f;
		float releaseMs = (trackVURelease.count(trackIndex) > 0) ? trackVURelease[trackIndex]->get() : 300.0f;
		
		// Simple peak line tracking (60fps assumed) - visual indicators only
		float attackCoeff = 1.0f - expf(-1000.0f / (attackMs * 60.0f));
		float releaseCoeff = 1.0f - expf(-1000.0f / (releaseMs * 60.0f));
		
		if (trackPeakLevels[trackIndex].size() <= ch) {
			trackPeakLevels[trackIndex].resize(ch + 1, -60.0f);
			trackPeakDecayTimers[trackIndex].resize(ch + 1, 0.0f);
		}
		
		if (dbLevel > trackPeakLevels[trackIndex][ch]) {
			trackPeakLevels[trackIndex][ch] = dbLevel;
			trackPeakDecayTimers[trackIndex][ch] = 1000.0f; // Hold peak for 1 second
		} else {
			trackPeakDecayTimers[trackIndex][ch] -= 16.67f; // ~60fps
			if (trackPeakDecayTimers[trackIndex][ch] <= 0) {
				trackPeakLevels[trackIndex][ch] -= releaseCoeff * 0.5f; // Slow decay
				trackPeakLevels[trackIndex][ch] = ofClamp(trackPeakLevels[trackIndex][ch], -60.0f, 6.0f);
			}
		}
		
		// Draw the actual level meter for this channel (dB-based)
		if (dbLevel > -60.0f) {
			float meterPosition = dbToVUPosition(dbLevel, -60.0f, 6.0f);
			float meterWidth = (widgetWidth - 2) * meterPosition;
			ImVec2 meterEnd = ImVec2(channelStart.x + meterWidth, channelEnd.y);
			
			// Professional VU color gradient based on dB
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
		
		// Draw 0dB reference line (permanent visual indicator)
		float zeroDbPosition = dbToVUPosition(0.0f, -60.0f, 6.0f);
		if (zeroDbPosition > 0.01f && zeroDbPosition < 0.99f) {
			float zeroDbX = channelStart.x + (widgetWidth - 2) * zeroDbPosition;
			drawList->AddLine(
				ImVec2(zeroDbX, channelStart.y),
				ImVec2(zeroDbX, channelEnd.y),
				IM_COL32(255, 255, 255, 180), // White line for 0dB reference
				1.0f
			);
		}
		
		// Channel separator lines (between channels)
		if (ch < numChans - 1) {
			float sepY = channelY + channelHeight;
			drawList->AddLine(
				ImVec2(vuStart.x, sepY),
				ImVec2(vuEnd.x, sepY),
				IM_COL32(60, 60, 60, 128),
				1.0f
			);
		}
		
		// Optional: Add channel number labels for multichannel (if more than 2 channels)
		if (numChans > 2) {
			char channelLabel[4];
			sprintf(channelLabel, "%d", ch + 1);
			ImVec2 labelPos = ImVec2(vuStart.x + 2, channelY + 1);
			drawList->AddText(labelPos, IM_COL32(150, 150, 150, 200), channelLabel);
		}
	}
	
	// === MUTE & SOLO BUTTONS ===
	ImVec2 buttonsStart = ImVec2(cursorPos.x, cursorPos.y + vuHeight + spacing);
	
	// Move ImGui cursor to button position
	ImGui::SetCursorScreenPos(buttonsStart);
	
	// Get button states from direct parameters
	bool isMuted = trackMuteParams[trackIndex]->get();
	bool isSoloed = trackSoloParams[trackIndex]->get();
	
	// Mute button
	ImGui::PushID(trackIndex * 1000 + 1); // Unique ID
	
	// Mute button colors
	ImVec4 muteColor = isMuted ?
		ImVec4(0.8f, 0.2f, 0.2f, 1.0f) :  // Red when muted
		ImVec4(0.3f, 0.3f, 0.3f, 1.0f);   // Dark gray when not muted
	ImVec4 muteHover = isMuted ?
		ImVec4(0.9f, 0.3f, 0.3f, 1.0f) :  // Lighter red on hover
		ImVec4(0.4f, 0.4f, 0.4f, 1.0f);   // Lighter gray on hover
	ImVec4 muteActive = isMuted ?
		ImVec4(0.7f, 0.1f, 0.1f, 1.0f) :  // Darker red when pressed
		ImVec4(0.6f, 0.6f, 0.6f, 1.0f);   // Lighter gray when pressed
		
	ImGui::PushStyleColor(ImGuiCol_Button, muteColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muteHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, muteActive);
	
	if (ImGui::Button("MUTE", ImVec2(buttonWidth, buttonHeight))) {
		trackMuteParams[trackIndex]->set(!isMuted);
	}
	
	ImGui::PopStyleColor(3);
	ImGui::PopID();
	
	// Solo button (side by side)
	ImGui::SameLine(0, spacing);
	ImGui::PushID(trackIndex * 1000 + 2); // Unique ID
	
	// Solo button colors
	ImVec4 soloColor = isSoloed ?
		ImVec4(0.8f, 0.8f, 0.2f, 1.0f) :  // Yellow when soloed
		ImVec4(0.3f, 0.3f, 0.3f, 1.0f);   // Dark gray when not soloed
	ImVec4 soloHover = isSoloed ?
		ImVec4(0.9f, 0.9f, 0.3f, 1.0f) :  // Lighter yellow on hover
		ImVec4(0.4f, 0.4f, 0.4f, 1.0f);   // Lighter gray on hover
	ImVec4 soloActive = isSoloed ?
		ImVec4(0.7f, 0.7f, 0.1f, 1.0f) :  // Darker yellow when pressed
		ImVec4(0.6f, 0.6f, 0.6f, 1.0f);   // Lighter gray when pressed
		
	ImGui::PushStyleColor(ImGuiCol_Button, soloColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, soloHover);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, soloActive);
	
	if (ImGui::Button("SOLO", ImVec2(buttonWidth, buttonHeight))) {
		trackSoloParams[trackIndex]->set(!isSoloed);
	}
	
	ImGui::PopStyleColor(3);
	ImGui::PopID();
	
	// Add some spacing after the widget
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + totalHeight));
	ImGui::Dummy(ImVec2(widgetWidth, 2.0f)); // Small spacing for next widget
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
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	// Get master VU meter data
	const vector<float>& masterVULevels = masterVUMeter.get();
	int numChans = masterVULevels.size();
	
	// Master VU widget dimensions - bigger than track widgets
	const float widgetWidth = 240.0f;     // Full node width
	const float channelHeight = 20.0f;    // Bigger height per channel for master
	const float vuHeight = numChans * channelHeight; // Total VU height
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
		ImVec2 channelStart = ImVec2(vuStart.x + 2, channelY + 2);
		ImVec2 channelEnd = ImVec2(vuStart.x + widgetWidth - 2, channelY + channelHeight - 2);
		
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
			
			// Add slight glow effect for master VU
			if (meterPosition > 0.1f) {
				// Draw a slightly larger, more transparent version behind for glow
				ImVec2 glowStart = ImVec2(channelStart.x, channelStart.y - 1);
				ImVec2 glowEnd = ImVec2(meterEnd.x, channelEnd.y + 1);
				ImU32 glowColor = (meterColor & 0x00FFFFFF) | 0x40000000; // Same color, 25% alpha
				drawList->AddRectFilled(glowStart, glowEnd, glowColor);
			}
			
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
		
		// Channel separator lines (between channels)
		if (ch < numChans - 1) {
			float sepY = channelY + channelHeight;
			drawList->AddLine(
				ImVec2(vuStart.x, sepY),
				ImVec2(vuEnd.x, sepY),
				IM_COL32(80, 80, 80, 128),
				1.0f
			);
		}
		
		// Channel labels (L/R for stereo, or numbers for multichannel)
		const char* channelLabels[] = {"L", "R", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};
		if (ch < 16) {
			ImVec2 labelPos = ImVec2(vuStart.x + 4, channelY + 3);
			drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), channelLabels[ch]);
		}
		
		// Level value text on the right (for levels > 10%)
		if (meterPosition > 0.1f) {
			char levelText[8];
			sprintf(levelText, "%.1f", dbLevel);
			ImVec2 levelTextSize = ImGui::CalcTextSize(levelText);
			ImVec2 levelTextPos = ImVec2(vuEnd.x - levelTextSize.x - 4, channelY + 3);
			drawList->AddText(levelTextPos, IM_COL32(200, 200, 200, 255), levelText);
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
