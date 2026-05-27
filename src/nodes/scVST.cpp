//
//  scVST.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//  Extended by Santi Vilanova on 25/7/25.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scVST.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include "ofxOceanodeShared.h"
#include <algorithm>
#include <set>
#include <unordered_set>
#include <vector>
#include <fstream>

std::map<std::string, std::vector<uint8_t>> scVST::globalFXPCache;
std::map<std::string, bool> scVST::globalFXPCacheValid;
std::mutex scVST::globalCacheMutex;

scVST::scVST() : scNode("VST") {
	isPresetLoading = false;
	pluginLoaded = false;
	hasPendingPresetData = false;
	parameterTimerActive = false;
	lastTouchedIndex = -1;
	lastTouchedTime = 0;
	waitingForSyncFXPSave = false;
	syncSourceNodeID = -1;
	syncInProgress = false;
	
	waitingForFXPSave = false;
	waitingForFXPLoad = false;
	lastFXPWriteSucceeded = false;
	activeFXPWriteOperation = FXPWriteOperation::None;
	activeFXPReadOperation = FXPReadOperation::None;
	activeFXPWriteNodeID = -1;
	activeFXPWriteStartTime = 0;
	
	oceanodePresetLoading = false;
	vstStateModifiedSincePreset = false;
	lastParameterChangeTime = 0;
	parameterDebounceDelay = 1000;
	parameterCacheScheduled = false;
	clearAllParameterSlotCaches();
	fxpCacheSaveInProgress.store(false, std::memory_order_relaxed);
	fxpCacheSavePending.store(false, std::memory_order_relaxed);
	
	isFXPLoading.store(false, std::memory_order_relaxed);
	fxpLoadStartTime = 0;
	
	midiOutputDirty.store(false, std::memory_order_relaxed);
	lastMidiUpdateTime = 0;
	
	lastMaintenanceTime = 0;
	maintenanceIntervalMs = 50;
	lastParamThrottleCleanup = 0;
	paramThrottleCleanupInterval = 5000;
	
	for(int i = 0; i < 1024; i++) {
		parameterUpdateGeneration[i].store(0, std::memory_order_relaxed);
		parameterDirty[i].store(false, std::memory_order_relaxed);
		feedbackSuppressUntil[i].store(0, std::memory_order_relaxed);
		latestParameterValues[i].store(0.0f, std::memory_order_relaxed);
		latestParameterNodeIDs[i].store(-1, std::memory_order_relaxed);
	}
	
	lastBatchProcessTime = 0;
	pendingGUISeen.fill(0);
	pendingGUIParamIndices.reserve(128);
}

void scVST::setup(){
	
	// Initialize FXP-related variables
	hasSavedFXPData = false;
	waitingForFXPSave = false;
	waitingForFXPLoad = false;
	lastFXPWriteSucceeded = false;
	activeFXPWriteOperation = FXPWriteOperation::None;
	activeFXPReadOperation = FXPReadOperation::None;
	activeFXPWriteNodeID = -1;
	activeFXPWritePath.clear();
	activeFXPWriteNodeKey.clear();
	activeFXPWriteStartTime = 0;
	clearAllParameterSlotCaches();
	for(int i = 0; i < 1024; i++) {
		parameterDirty[i].store(false, std::memory_order_relaxed);
		latestParameterValues[i].store(0.0f, std::memory_order_relaxed);
		latestParameterNodeIDs[i].store(-1, std::memory_order_relaxed);
	}
	pendingGUISeen.fill(0);
	pendingGUIParamIndices.clear();
	clearAllFeedbackSuppression();
	
	// Inputs and outputs need to exist before preset loading can trigger upstream updates.
	scNode::addInput("In");
	scNode::addOutput("Out");
	
	addParameter(numChannels.set("N Chan", 2, 1, MAX_NODE_CHANNELS));
	addParameter(mix.set("Mix", 1.0f, 0.0f, 1.0f));
	
	// Thick separator after VST controls
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	searchForVSTPlugins();
	
	if (!availablePlugins.empty()) {
		addParameterDropdown(pluginSelector, "Plugin", 0, availablePlugins,
							 ofxOceanodeParameterFlags_DisableSavePreset);
		currentPluginPath = pluginPaths[0]; // Set default to first plugin
	} else {
		availablePlugins.push_back("No VST plugins found");
		pluginPaths.push_back("");
		addParameterDropdown(pluginSelector, "Plugin", 0, availablePlugins,
							 ofxOceanodeParameterFlags_DisableSavePreset);
	}
	
	
	// VST control parameters
	addParameter(openEditor.set("Editor"));
	addParameter(addLastTouched.set("Add Last"));
	addParameter(propagateParams.set("Propagate"));
	
	
	// Thick separator after basic parameters
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	// MIDI parameters (integrated from scVSTI)
	// VST Program parameter - NOT SAVED IN PRESETS
	auto programParam = addParameter(vstProgram.set("Program", 0, 0, 127),
									 ofxOceanodeParameterFlags_DisableSavePreset |
									 ofxOceanodeParameterFlags_DisableSaveProject);
	
	
	addParameter(instance.set("Instance", {0}, {0}, {64}));
	addParameter(midiChannel.set("MIDI Chan", 1, 1, 16));
	
	addParameter(pitch.set("Pitch", {60}, {0}, {127}));
	addParameter(velocity.set("Velocity", {0.5}, {0}, {1}));
	addParameter(gate.set("Gate", {0}, {0}, {1}));
	addParameter(pitchBend.set("PitchBend", 0.5, 0, 1));
	addParameter(modWheel.set("ModWheel", 0, 0, 1));
	
	
	
	// Thick separator after MIDI parameters
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	addParameter(transportPlay.set("Play", false));
	addParameter(transportPosition.set("Position", 0.0f, 0.0f, 1000.0f));
	addParameter(transportReset.set("Reset"));
	addParameter(tempo.set("BPM", 120.0f, 60.0f, 200.0f));
	addInspectorParameter(timeSignatureNum.set("TimeSig Num", 4, 1, 16));
	addInspectorParameter(timeSignatureDenom.set("TimeSig Denom", 4, 1, 16));
	addInspectorParameter(queryTransportPos.set("QueryPosition"));
	
	// Initialize transport state
	transportFeedbackSuppressed = false;
	transportFeedbackClearTime = 0;
	lastKnownPosition = 0.0f;
	isTransportQuerying = false;
	
	// MIDI Output section
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	// Initialize MIDI output vectors (128 elements each)
	vector<float> initialNoteVector(128, 0.0f);  // All notes off initially
	vector<float> initialCCVector(128, 0.0f);    // All CCs at 0 initially
	
	addOutputParameter(noteOut.set("Note Out", initialNoteVector,
								   vector<float>(128, 0.0f),
								   vector<float>(128, 1.0f)));
	addOutputParameter(ccOut.set("CC Out", initialCCVector,
								 vector<float>(128, 0.0f),
								 vector<float>(128, 1.0f)));
	
	// Initialize state tracking
	currentNoteStates = initialNoteVector;
	currentCCStates = initialCCVector;
	
	// MIDI CC Parameters section
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	// MIDI CC control parameters
	addInspectorParameter(addMidiCC.set("Add MIDI CC"));
	addInspectorParameter(midiCCToAdd.set("CC Number", 1, 1, 127));
	addInspectorParameter(removeAllMidiCC.set("Remove All CC"));
	
	// MIDI CC listeners
	listeners.push(addMidiCC.newListener([this]{
		if(!isPresetLoading) {
			addMidiCCParameter(midiCCToAdd.get());
		}
	}));
	
	listeners.push(removeAllMidiCC.newListener([this]{
		removeAllMidiCCParameters();
	}));
	
	
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	// Inspector parameters
	addInspectorParameter(enableMultithreading.set("Multithreading", true));
	addInspectorParameter(singleInstance.set("Single Instance", true));
	addInspectorParameter(monoInstancing.set("Mono Instancing", false));
	addInspectorParameter(removeAllParams.set("Remove All Params"));
	addInspectorParameter(saveFXPToDisk.set("Save FXP to Disk"));
	
	listeners.push(singleInstance.newListener([this](bool &b){
		for (auto &serverInstances : synthInstances) {
			if (serverInstances.first) {
				freeVSTInstances(serverInstances.first);
				createVSTInstances(serverInstances.first);
				createSynth(serverInstances.first);
			}
		}
		resendParams.notify();
	}));
	
	
	
	// Transport parameter listeners
	listeners.push(transportPlay.newListener([this](bool &playing){
		if(!transportFeedbackSuppressed && !isPresetLoading) {
			setTransportPlay(playing);
		}
	}));
	
	listeners.push(transportPosition.newListener([this](float &position){
		if(!transportFeedbackSuppressed && !isPresetLoading) {
			setTransportPosition(position);
		}
	}));
	
	listeners.push(transportReset.newListener([this]{
		if(!isPresetLoading) {
			resetTransport();
		}
	}));
	
	listeners.push(tempo.newListener([this](float &bpm){
		if(!isPresetLoading) {
			setTempo(bpm);
		}
	}));
	
	listeners.push(timeSignatureNum.newListener([this](int &num){
		if(!isPresetLoading) {
			setTimeSignature(num, timeSignatureDenom.get());
		}
	}));
	
	listeners.push(timeSignatureDenom.newListener([this](int &denom){
		if(!isPresetLoading) {
			setTimeSignature(timeSignatureNum.get(), denom);
		}
	}));
	
	listeners.push(queryTransportPos.newListener([this]{
		queryTransportPosition();
	}));
	
	listeners.push(saveFXPToDisk.newListener([this]{
		saveFXPToUserChosenPath();
	}));
	
	listeners.push(vstProgram.newListener([this](int &programIndex){
		try {
			if(this == nullptr) {
				ofLogError("scVST") << "VST program listener called on null object";
				return;
			}
			
			if(!isPresetLoading) {  // Only respond to user changes, not preset loading
				if(synthInstances.empty()) {
					ofLogWarning("scVST") << "Cannot set VST program: no instances available";
					return;
				}
				
				setVSTProgram(programIndex);
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error in VST program listener: " << e.what();
		} catch(...) {
			ofLogError("scVST") << "Unknown error in VST program listener";
		}
	}));
	
	listeners.push(numChannels.newListener([this](int &i){
		if(oldNumChannels != i) {
			resendParams.notify();
		}
		oldNumChannels = i;
	}));
	
	listeners.push(mix.newListener([this](float &m){
		// Send mix parameter to all VST instances
		for(auto& serverInstances : synthInstances){
			for(auto synth : serverInstances.second){
				if(synth != nullptr){
					synth->set("mix", m);
				}
			}
		}
	}));
	
	listeners.push(monoInstancing.newListener([this](bool &mono){
		/*
		 ofLogNotice("scVST") << "Instancing mode changed to " << (mono ? "mono" : "stereo")
		 << ", will need " << calculateNumInstances() << " instances";
		 */
		// Need to recreate instances with new channel configuration
		for(auto& serverInstances : synthInstances) {
			if(serverInstances.first != nullptr) {
				// Free existing instances
				freeVSTInstances(serverInstances.first);
				
				// Create new instances with correct configuration
				createVSTInstances(serverInstances.first);
				createSynth(serverInstances.first);
			}
		}
		
		resendParams.notify();
	}));
	
	listeners.push(openEditor.newListener([this]{
		// Only open editor for the first instance of each server
		for(auto& serverInstances : synthInstances){
			if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr){
				ofxOscMessage m;
				m.setAddress("/u_cmd");
				m.addIntArg(serverInstances.second[0]->nodeID);
				m.addIntArg(2);
				m.addStringArg("/vis");
				m.addIntArg(1);
				serverInstances.first->sendMsg(m);
				
				//ofLogNotice("scVST") << "Opening editor for first instance on server";
			}
		}
	}));
	
	listeners.push(addLastTouched.newListener([this]{
		if(lastTouchedIndex >= 0) {
			// Check if parameter already exists in GUI
			if(dynamicParameters.count(lastTouchedIndex) > 0 ||
			   dynamicVectorParameters.count(lastTouchedIndex) > 0) {
				ofLogWarning("scVST") << "Parameter " << lastTouchedIndex << " already exists in GUI";
			} else {
				// Check how recently this parameter was touched (to avoid stale touches)
				uint64_t timeSinceTouch = ofGetElapsedTimeMillis() - lastTouchedTime;
				if (timeSinceTouch > 30000) { // 30 seconds timeout
					ofLogWarning("scVST") << "Last touched parameter " << lastTouchedIndex
					<< " was touched " << (timeSinceTouch/1000)
					<< " seconds ago - might be stale";
				}
				
				ofLogNotice("scVST") << "Adding last touched parameter: " << lastTouchedIndex;
				addParameterToGUI(lastTouchedIndex);
			}
		} else {
			ofLogWarning("scVST") << "No parameter has been touched in the VST GUI";
			ofLogWarning("scVST") << "Touch/move a parameter in the VST editor first, then click 'Add Last'";
			ofLogWarning("scVST") << "Note: Already published parameters are ignored to prevent automation conflicts";
		}
	}));
	
	listeners.push(propagateParams.newListener([this]{
		ofLogNotice("scVST") << "=== PROPAGATING FIRST INSTANCE VIA FXP (BUTTON) ===";
		propagateFirstInstanceViaFXP();
	}));
	
	listeners.push(removeAllParams.newListener([this]{
		removeAllDynamicParameters();
	}));
	
	// FIXED: Plugin selector listener with preset loading check
	listeners.push(pluginSelector.newListener([this](int &selection){
		ofLogNotice("scVST") << "🔍 Plugin selector changed to " << selection
		<< " (isPresetLoading = " << (isPresetLoading ? "TRUE" : "FALSE") << ")"
		<< " - plugin path will be: " << (selection >= 0 && selection < pluginPaths.size() ? pluginPaths[selection] : "INVALID");
		
		// CRITICAL: Don't do ANYTHING during preset loading!
		// currentPluginPath is already set correctly from the preset JSON
		if(isPresetLoading) {
			ofLogNotice("scVST") << "🔒 Preset loading in progress - ignoring selector change (currentPluginPath already set from preset)";
			return; // Don't update currentPluginPath, don't load the plugin
		}
		
		// Normal operation - load the plugin immediately
		if (selection >= 0 && selection < pluginPaths.size()) {
			currentPluginPath = pluginPaths[selection];
			ofLogNotice("scVST") << "🔄 Loading selected plugin: " << currentPluginPath;
			loadSelectedPlugin();
		}
	}));
	
	// MIDI gate listener
	listeners.push(gate.newListener([this](vector<int> &gates){
		processGates(gates);
	}));
	
	listeners.push(pitchBend.newListener([this](float &val){
		if(!isPresetLoading) {
			sendPitchBend(val);
		}
	}));
	
	listeners.push(modWheel.newListener([this](float &val){
		if(!isPresetLoading) {
			sendModWheel(val);
		}
	}));
	
	listeners.push(resendParams.newListener([this](){
		for(auto& serverInstances : synthInstances){
			for(auto synth : serverInstances.second){
				if(synth != nullptr){
					synth->set("inChannels", numChannels);
					synth->set("mix", mix);
				}
			}
		}
	}));
	
	addCustomRegion(
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); }),
					ofParameter<std::function<void()>>().set("", [](){ drawSeparator(); })
					);
	
	listeners.push(ofEvents().update.newListener([this](ofEventArgs&) {
		uint64_t currentTime = ofGetElapsedTimeMillis();
		
		if(currentTime - lastBatchProcessTime >= BATCH_PROCESS_INTERVAL_MS) {
			processPendingParameterUpdates();
			flushPendingGUIParameterUpdates();
			lastBatchProcessTime = currentTime;
		}
		
		if(midiOutputDirty.load(std::memory_order_acquire)) {
			if(currentTime - lastMidiUpdateTime >= 16) {
				updateMidiOutputs();
			}
		}
		
		if(currentTime - lastMaintenanceTime < maintenanceIntervalMs) {
			return;
		}
		lastMaintenanceTime = currentTime;
		
		if(transportFeedbackSuppressed && currentTime >= transportFeedbackClearTime) {
			transportFeedbackSuppressed = false;
		}
		
		// If the plugin has not reported ready yet, keep extending the delayed
		// preset application window instead of abandoning the pending data.
		if(parameterTimerActive) {
			if(currentTime - parameterTimerStart >= parameterTimerDelay) {
				
				if(areAllInstancesReady()) {
					parameterTimerActive = false;
					ofLogNotice("scVST") << "Timer timeout - VST ready, applying preset data now";
					if(hasPendingPresetData) {
						applyPendingPresetData();
					}
				} else {
					ofLogWarning("scVST") << "Timer timeout but VST not ready - extending wait by 2s";
					parameterTimerStart = currentTime;
					parameterTimerDelay = 2000;
				}
			}
		}
		
		if(isFXPLoading.load(std::memory_order_acquire)) {
			if(currentTime - fxpLoadStartTime > FXP_LOAD_TIMEOUT_MS) {
				ofLogWarning("scVST") << "FXP loading timeout - clearing flag";
				isFXPLoading.store(false, std::memory_order_release);
				if(fxpCacheSavePending.exchange(false, std::memory_order_acq_rel)) {
					scheduleImmediateFXPCache();
				}
			}
		}

		if(activeFXPWriteOperation != FXPWriteOperation::None &&
		   currentTime - activeFXPWriteStartTime > FXP_WRITE_TIMEOUT_MS) {
			ofLogWarning("scVST") << "FXP write timeout - clearing pending operation";

			switch(activeFXPWriteOperation) {
				case FXPWriteOperation::PresetSave:
					waitingForFXPSave = false;
					cleanupTempFXPFile();
					break;
				case FXPWriteOperation::CacheSave: {
					fxpCacheSaveInProgress.store(false, std::memory_order_release);
					try {
						if(!activeFXPWritePath.empty()) ofFile::removeFile(activeFXPWritePath);
					} catch(...) {}
					bool scheduleFollowUpSave = fxpCacheSavePending.exchange(false, std::memory_order_acq_rel);
					if(scheduleFollowUpSave) scheduleImmediateFXPCache();
					break;
				}
				case FXPWriteOperation::UserExport:
					ofLogWarning("scVST") << "User FXP export timed out";
					break;
				case FXPWriteOperation::SyncSave:
					resetSyncFXPState(true);
					break;
				case FXPWriteOperation::None:
					break;
			}

			clearActiveFXPWriteState();
		}
		
		updateFXPCacheIfNeeded();
		updateParameterDebouncedCacheIfNeeded();
		
		if(currentTime - lastParamThrottleCleanup > paramThrottleCleanupInterval) {
			lastParamThrottleCleanup = currentTime;
			for(int i = 0; i < 1024; i++) {
				if(parameterDirty[i].load(std::memory_order_acquire)) {
					uint64_t lastUpdate = parameterUpdateGeneration[i].load(std::memory_order_acquire);
					if(currentTime - lastUpdate > 1000) {
						parameterDirty[i].store(false, std::memory_order_release);
					}
				}
			}
		}
	}));
	
	loadCacheFromGlobal();
	
}

void scVST::activate(){
    for(auto &synths : synthInstances){
        for(auto &synth : synths.second){
            synth->run(true);
        }
    }
}

void scVST::deactivate(){
    for(auto &synths : synthInstances){
        for(auto &synth : synths.second){
            synth->run(false);
        }
    }
}

int scVST::calculateNumInstances() const {
	if (singleInstance.get()) {
		return 1;                      // one multichannel instance
	}
	if (monoInstancing.get()) {
		return numChannels.get();      // 1ch per instance
	} else {
		return (numChannels.get() + 1) / 2; // 2ch per instance
	}
}



void scVST::searchForVSTPlugins() {
	availablePlugins.clear();
	pluginPaths.clear();
	
	// Common VST paths for macOS
	vector<string> vstSearchPaths = {
		"/Library/Audio/Plug-Ins/VST/",
		"/Library/Audio/Plug-Ins/VST3/",
		ofFilePath::getUserHomeDir() + "/Library/Audio/Plug-Ins/VST/",
		ofFilePath::getUserHomeDir() + "/Library/Audio/Plug-Ins/VST3/"
	};
	
	// Add common Windows paths if needed
#ifdef TARGET_WIN32
	vstSearchPaths.push_back("C:/Program Files/VSTPlugins/");
	vstSearchPaths.push_back("C:/Program Files/Common Files/VST2/");
	vstSearchPaths.push_back("C:/Program Files/Common Files/VST3/");
	vstSearchPaths.push_back("C:/Program Files/Steinberg/VSTPlugins/");
#endif
	
	// Add common Linux paths if needed
#ifdef TARGET_LINUX
	vstSearchPaths.push_back(ofFilePath::getUserHomeDir() + "/.vst/");
	vstSearchPaths.push_back(ofFilePath::getUserHomeDir() + "/.vst3/");
	vstSearchPaths.push_back("/usr/lib/vst/");
	vstSearchPaths.push_back("/usr/lib/vst3/");
	vstSearchPaths.push_back("/usr/local/lib/vst/");
	vstSearchPaths.push_back("/usr/local/lib/vst3/");
#endif
	
	// Search for VST plugins recursively
	for (const string& path : vstSearchPaths) {
		ofLogNotice("scVST") << "Searching VST path: " << path;
		
		// Use a stack-based approach for recursive directory traversal
		std::vector<string> dirsToProcess;
		dirsToProcess.push_back(path);
		
		while (!dirsToProcess.empty()) {
			string currentPath = dirsToProcess.back();
			dirsToProcess.pop_back();
			
			ofDirectory dir(currentPath);
			if (dir.exists()) {
				try {
					dir.listDir();
					ofLogVerbose("scVST") << "Processing directory: " << currentPath << " (" << dir.size() << " items)";
					
					for (int i = 0; i < dir.size(); i++) {
						try {
							string filename = dir.getName(i);
							string filepath = dir.getPath(i);
							
							ofLogVerbose("scVST") << "Processing item: " << filename << " (isDirectory: " << dir.getFile(i).isDirectory() << ")";
							
							// Check for VST extensions first (VST3 plugins are actually directories with .vst3 extension)
							string extension = ofToLower(ofFilePath::getFileExt(filename));
							if (extension == "vst" ||
								extension == "vst3" ||
								filename.find(".vst") != string::npos) {
								
								string pluginName = ofFilePath::getBaseName(filename);
								
								// Create display name with folder context for subfolders
								string displayName = pluginName;
								string parentFolder = ofFilePath::getBaseName(ofFilePath::getEnclosingDirectory(filepath));
								if (!parentFolder.empty() && parentFolder != ofFilePath::getBaseName(path)) {
									displayName = parentFolder + "/" + pluginName;
								}
								
								availablePlugins.push_back(displayName);
								pluginPaths.push_back(filepath);
								ofLogNotice("scVST") << "Found VST plugin: " << displayName << " at " << filepath;
							}
							// Only recurse into directories that are NOT VST plugins
							else if (dir.getFile(i).isDirectory()) {
								// Add subdirectory to processing stack
								dirsToProcess.push_back(filepath + "/");
								ofLogVerbose("scVST") << "Added subdir to stack: " << filepath;
							}
						} catch (const std::exception& e) {
							// Skip individual files/dirs that cause problems
							ofLogVerbose("scVST") << "Error processing item in " << currentPath << ": " << e.what();
						}
					}
				} catch (const std::exception& e) {
					ofLogWarning("scVST") << "Error accessing directory " << currentPath << ": " << e.what();
					// Skip this directory and continue with others
				}
			} else {
				ofLogVerbose("scVST") << "Directory does not exist: " << currentPath;
			}
		}
		
		ofLogNotice("scVST") << "Finished searching path: " << path << " (found " << availablePlugins.size() << " plugins so far)";
	}
	
	// Sort plugins alphabetically
	if (!availablePlugins.empty()) {
		// Create pairs of (plugin name, plugin path) for sorting
		vector<pair<string, string>> pluginPairs;
		for (int i = 0; i < availablePlugins.size(); i++) {
			pluginPairs.push_back(make_pair(availablePlugins[i], pluginPaths[i]));
		}
		
		// Sort by plugin name (case-insensitive)
		sort(pluginPairs.begin(), pluginPairs.end(),
			 [](const pair<string, string>& a, const pair<string, string>& b) {
			return ofToLower(a.first) < ofToLower(b.first);
		});
		
		// Rebuild the sorted vectors
		availablePlugins.clear();
		pluginPaths.clear();
		for (const auto& pair : pluginPairs) {
			availablePlugins.push_back(pair.first);
			pluginPaths.push_back(pair.second);
		}
	}
	
	// If no plugins found, add a default message (existing code)
	if (availablePlugins.empty()) {
		availablePlugins.push_back("No VST plugins found");
		pluginPaths.push_back("");
	}}

void scVST::processPendingParameterUpdates() {
	for(int paramIndex = 0; paramIndex < 1024; ++paramIndex) {
		if(!parameterDirty[paramIndex].exchange(false, std::memory_order_acq_rel)) {
			continue;
		}

		float value = latestParameterValues[paramIndex].load(std::memory_order_acquire);
		int nodeID = latestParameterNodeIDs[paramIndex].load(std::memory_order_acquire);
		if(nodeID < 0) {
			continue;
		}
		
		// Update parameter info
		auto infoSlot = parameterInfoSlots[paramIndex];
		if(infoSlot == nullptr) {
			VSTParameterInfo info;
			info.index = paramIndex;
			info.displayName = savedParameterNamePresent[paramIndex] ?
			savedParameterNameSlots[paramIndex] : "Param" + ofToString(paramIndex);
			info.value = value;
			parameterInfoMap[paramIndex] = info;
			cacheParameterInfoSlot(paramIndex);
			infoSlot = parameterInfoSlots[paramIndex];
		} else {
			infoSlot->value = value;
		}
		
		// GUI writes are flushed once per update tick to keep OSC feedback cheap.
		queueGUIParameterUpdate(paramIndex, value, nodeID);
		
		// Handle propagation if needed
		if(shouldPropagateFromVSTGUI(paramIndex, nodeID)) {
			propagateParameterToOtherInstances(nodeID, paramIndex, value);
		}
	}
}

void scVST::queueGUIParameterUpdate(int paramIndex, float value, int sourceNodeID) {
	if(!pendingGUISeen[paramIndex]) {
		pendingGUISeen[paramIndex] = 1;
		pendingGUIParamIndices.push_back(paramIndex);
	}

	pendingGUIValues[paramIndex] = value;
	pendingGUINodeIDs[paramIndex] = sourceNodeID;
}

void scVST::flushPendingGUIParameterUpdates() {
	if(pendingGUIParamIndices.empty()) return;

	for(int paramIndex : pendingGUIParamIndices) {
		applyGUIParameterValueFromVST(paramIndex, pendingGUIValues[paramIndex], pendingGUINodeIDs[paramIndex]);
		pendingGUISeen[paramIndex] = 0;
	}

	pendingGUIParamIndices.clear();
}

bool scVST::isFeedbackSuppressed(int paramIndex, uint64_t currentTime) const {
	return paramIndex >= 0 &&
		   paramIndex < 1024 &&
		   currentTime < feedbackSuppressUntil[paramIndex].load(std::memory_order_acquire);
}

void scVST::suppressFeedbackFor(int paramIndex, uint64_t untilTime) {
	if(paramIndex >= 0 && paramIndex < 1024) {
		feedbackSuppressUntil[paramIndex].store(untilTime, std::memory_order_release);
	}
}

void scVST::clearAllFeedbackSuppression() {
	for(auto& suppressUntil : feedbackSuppressUntil) {
		suppressUntil.store(0, std::memory_order_relaxed);
	}
}

void scVST::cacheParameterInfoSlot(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	auto it = parameterInfoMap.find(paramIndex);
	parameterInfoSlots[paramIndex] = it != parameterInfoMap.end() ? &it->second : nullptr;
}

void scVST::clearParameterInfoSlot(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	parameterInfoSlots[paramIndex] = nullptr;
}

void scVST::cacheDynamicScalarParameterSlot(int paramIndex, const shared_ptr<ofxOceanodeParameter<float>>& param) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	dynamicScalarParameterSlots[paramIndex] = param;
}

void scVST::cacheDynamicVectorParameterSlot(int paramIndex, const shared_ptr<ofxOceanodeParameter<vector<float>>>& param) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	dynamicVectorParameterSlots[paramIndex] = param;
}

void scVST::clearDynamicParameterSlots(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	dynamicScalarParameterSlots[paramIndex].reset();
	dynamicVectorParameterSlots[paramIndex].reset();
}

shared_ptr<ofxOceanodeParameter<float>> scVST::resolveDynamicScalarParameterSlot(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return nullptr;

	auto param = dynamicScalarParameterSlots[paramIndex];
	if(param) return param;

	auto it = dynamicParameters.find(paramIndex);
	if(it == dynamicParameters.end()) return nullptr;

	cacheDynamicScalarParameterSlot(paramIndex, it->second);
	return it->second;
}

shared_ptr<ofxOceanodeParameter<vector<float>>> scVST::resolveDynamicVectorParameterSlot(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return nullptr;

	auto param = dynamicVectorParameterSlots[paramIndex];
	if(param) return param;

	auto it = dynamicVectorParameters.find(paramIndex);
	if(it == dynamicVectorParameters.end()) return nullptr;

	cacheDynamicVectorParameterSlot(paramIndex, it->second);
	return it->second;
}

bool scVST::isParameterPublished(int paramIndex) {
	return resolveDynamicScalarParameterSlot(paramIndex) != nullptr ||
		   resolveDynamicVectorParameterSlot(paramIndex) != nullptr;
}

void scVST::cacheSavedParameterNameSlot(int paramIndex, const std::string& name) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	savedParameterNameSlots[paramIndex] = name;
	savedParameterNamePresent[paramIndex] = 1;
}

void scVST::clearSavedParameterNameSlot(int paramIndex) {
	if(paramIndex < 0 || paramIndex >= 1024) return;
	savedParameterNameSlots[paramIndex].clear();
	savedParameterNamePresent[paramIndex] = 0;
}

void scVST::clearAllParameterSlotCaches() {
	parameterInfoSlots.fill(nullptr);
	for(auto& scalarSlot : dynamicScalarParameterSlots) scalarSlot.reset();
	for(auto& vectorSlot : dynamicVectorParameterSlots) vectorSlot.reset();
	for(auto& savedName : savedParameterNameSlots) savedName.clear();
	savedParameterNamePresent.fill(0);
}

void scVST::loadSelectedPlugin() {
	if (currentPluginPath.empty()) {
		ofLogWarning("scVST") << "No plugin path set, skipping load";
		return;
	}
	
	// Clear existing parameter mappings
	parameterInfoMap.clear();
	for(int i = 0; i < 1024; ++i) {
		clearParameterInfoSlot(i);
		clearDynamicParameterSlots(i);
	}
	
	if (!isPresetLoading) {
		ofLogNotice("scVST") << "Removing existing GUI parameters before loading new plugin";
		removeAllDynamicParameters();
	} else {
		ofLogNotice("scVST") << "Preserving GUI parameters during preset loading";
	}
	
	// Clear readiness tracking - the handleVSTOpen will manage synchronization
	readyInstances.clear();
	fxpAppliedInstances.clear();
	
	int totalInstances = 0;
	
	// OPTIMIZED PARALLEL APPROACH with event-driven synchronization
	// Phase 1: Send all close commands in parallel
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				totalInstances++;
				
				// Close current plugin if any - NO DELAYS
				ofxOscMessage closeMsg;
				closeMsg.setAddress("/u_cmd");
				closeMsg.addIntArg(synth->nodeID);
				closeMsg.addIntArg(2);
				closeMsg.addStringArg("/close");
				serverInstances.first->sendMsg(closeMsg);
			}
		}
	}
	
	// Phase 2: Minimal processing for close commands
	for(int i = 0; i < 3; i++) {
		for(auto& serverInstances : synthInstances) {
			serverInstances.first->process();
		}
		ofSleepMillis(5); // Very brief delay
	}
	
	// Phase 3: Send all open commands in parallel
	ofLogNotice("scVST") << "🚀 Loading plugin on all " << totalInstances << " instances (parallel): " << currentPluginPath;
	
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				// Open new plugin - NO DELAYS
				ofxOscMessage openMsg;
				openMsg.setAddress("/u_cmd");
				openMsg.addIntArg(synth->nodeID);
				openMsg.addIntArg(2);
				openMsg.addStringArg("/open");
				openMsg.addStringArg(currentPluginPath);
				openMsg.addIntArg(1); // Request GUI editor
				openMsg.addIntArg(enableMultithreading.get() ? 1 : 0);
				openMsg.addIntArg(0); // Normal mode
				serverInstances.first->sendMsg(openMsg);
			}
		}
	}
	
	// Phase 4: Minimal processing to kickstart opens
	for(int i = 0; i < 2; i++) {
		for(auto& serverInstances : synthInstances) {
			serverInstances.first->process();
		}
		ofSleepMillis(5);
	}
	
	// The handleVSTOpen callback will handle synchronization and FXP loading
	// when all instances report ready via /vst_open messages
	ofLogNotice("scVST") << "Plugin load commands sent - waiting for /vst_open confirmations...";
	
	fxpCacheValid = false;
	cachedFXP.clear();
	fxpCacheScheduled = false;
	pluginLoaded = true;
}

bool scVST::areAllInstancesReady() {
	int expectedInstances = 0;
	for(auto& serverInstances : synthInstances) {
		expectedInstances += serverInstances.second.size();
	}
	return readyInstances.size() >= expectedInstances;
}

void scVST::handleVSTParam(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 4) {
		int nodeID = msg.getArgAsInt32(0);
		int paramIndex = (int)msg.getArgAsFloat(2);
		float value = msg.getArgAsFloat(3);
		
		if (!isMyVSTInstance(nodeID)) return;
		if (paramIndex < 0 || paramIndex >= 1024) return;
		
		bool useFXPLoading = isFXPLoading.load(std::memory_order_acquire);
		uint64_t currentTime = ofGetElapsedTimeMillis();
		
		if(useFXPLoading) {
			// During FXP recall we stay on the immediate path so state restoration
			// is not delayed by the normal steady-state batcher.
			bool isAlreadyPublished = isParameterPublished(paramIndex);
			
			if (!isAlreadyPublished) {
				lastTouchedIndex = paramIndex;
				lastTouchedTime = currentTime;
				ofLogVerbose("scVST") << "VST Parameter " << paramIndex << " = " << value
				<< " from node " << nodeID << " (updating lastTouchedIndex for addLast)";
			}
			
			auto infoSlot = parameterInfoSlots[paramIndex];
			if(infoSlot == nullptr) {
				VSTParameterInfo info;
				info.index = paramIndex;
				
				if(savedParameterNamePresent[paramIndex]) {
					info.displayName = savedParameterNameSlots[paramIndex];
				} else {
					info.displayName = "Param" + ofToString(paramIndex);
				}
				
				info.value = value;
				parameterInfoMap[paramIndex] = info;
				cacheParameterInfoSlot(paramIndex);
			} else {
				infoSlot->value = value;
			}
			
			updateParameterValueFromVST(paramIndex, value, nodeID);
			
			if(shouldPropagateFromVSTGUI(paramIndex, nodeID)) {
				propagateParameterToOtherInstances(nodeID, paramIndex, value);
			}
		} else {
			bool isAlreadyPublished = isParameterPublished(paramIndex);

			if(isAlreadyPublished) {
				// Published parameters need snappy GUI mirroring from the plugin editor,
				// so we always keep the latest value for the next flush.
				latestParameterValues[paramIndex].store(value, std::memory_order_release);
				latestParameterNodeIDs[paramIndex].store(nodeID, std::memory_order_release);
				parameterDirty[paramIndex].store(true, std::memory_order_release);
				parameterUpdateGeneration[paramIndex].store(currentTime, std::memory_order_release);
			} else {
				uint64_t lastUpdate = parameterUpdateGeneration[paramIndex].load(std::memory_order_acquire);
				
				if (currentTime - lastUpdate < PARAM_UPDATE_THROTTLE_MS) {
					return;
				}
				
				uint64_t expected = lastUpdate;
				if (!parameterUpdateGeneration[paramIndex].compare_exchange_weak(
																				 expected, currentTime, std::memory_order_release, std::memory_order_relaxed)) {
																					 return;
																				 }
				
				latestParameterValues[paramIndex].store(value, std::memory_order_release);
				latestParameterNodeIDs[paramIndex].store(nodeID, std::memory_order_release);
				parameterDirty[paramIndex].store(true, std::memory_order_release);
			}
			
			if (!isAlreadyPublished) {
				lastTouchedIndex = paramIndex;
				lastTouchedTime = currentTime;
				ofLogVerbose("scVST") << "VST Parameter " << paramIndex << " = " << value
				<< " from node " << nodeID << " (updating lastTouchedIndex for addLast)";
			}
		}
	}
}

void scVST::handleVSTAuto(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 4) {
		int nodeID = msg.getArgAsInt32(0);
		int paramIndex = (int)msg.getArgAsFloat(2);
		float value = msg.getArgAsFloat(3);
		
		if (!isMyVSTInstance(nodeID)) return;
		if (paramIndex < 0 || paramIndex >= 1024) return;
		
		bool useFXPLoading = isFXPLoading.load(std::memory_order_acquire);
		uint64_t currentTime = ofGetElapsedTimeMillis();
		
		if(useFXPLoading) {
			updateParameterValueFromVST(paramIndex, value, nodeID);
		} else {
			bool isAlreadyPublished = isParameterPublished(paramIndex);

			if(isAlreadyPublished) {
				latestParameterValues[paramIndex].store(value, std::memory_order_release);
				latestParameterNodeIDs[paramIndex].store(nodeID, std::memory_order_release);
				parameterDirty[paramIndex].store(true, std::memory_order_release);
				parameterUpdateGeneration[paramIndex].store(currentTime, std::memory_order_release);
			} else {
				uint64_t lastUpdate = parameterUpdateGeneration[paramIndex].load(std::memory_order_acquire);
				
				if (currentTime - lastUpdate < PARAM_UPDATE_THROTTLE_MS) {
					return;
				}
				
				uint64_t expected = lastUpdate;
				if (!parameterUpdateGeneration[paramIndex].compare_exchange_weak(
																				 expected, currentTime, std::memory_order_release, std::memory_order_relaxed)) {
																					 return;
																				 }
				
				latestParameterValues[paramIndex].store(value, std::memory_order_release);
				latestParameterNodeIDs[paramIndex].store(nodeID, std::memory_order_release);
				parameterDirty[paramIndex].store(true, std::memory_order_release);
			}
		}
		
		bool isAlreadyPublished = isParameterPublished(paramIndex);
		
		if (!isAlreadyPublished) {
			lastTouchedIndex = paramIndex;
			lastTouchedTime = currentTime;
		}
	}
}

void scVST::applyGUIParameterValueFromVST(int paramIndex, float value, int sourceNodeID) {
	auto vectorParam = resolveDynamicVectorParameterSlot(paramIndex);
	if(vectorParam) {
		auto& param = vectorParam->getParameter();
		const auto& currentValues = param.get();

		if(currentValues.size() == 1) {
			if(currentValues[0] != value) {
				static thread_local vector<float> scalarVec(1);
				scalarVec[0] = value;
				param.setWithoutEventNotifications(scalarVec);
			}
		} else {
			int instanceIndex = getInstanceIndexFromNodeID(sourceNodeID);
			if(instanceIndex >= 0 && instanceIndex < static_cast<int>(currentValues.size())) {
				if(currentValues[instanceIndex] != value) {
					static thread_local vector<float> vectorScratch;
					vectorScratch.assign(currentValues.begin(), currentValues.end());
					vectorScratch[instanceIndex] = value;
					param.setWithoutEventNotifications(vectorScratch);
				}
			}
		}
	}

	auto scalarParamSlot = resolveDynamicScalarParameterSlot(paramIndex);
	if(scalarParamSlot) {
		auto& scalarParam = scalarParamSlot->getParameter();
		if(scalarParam.get() != value) {
			scalarParam.setWithoutEventNotifications(value);
		}
	}
}

void scVST::updateParameterValueFromVST(int paramIndex, float value, int sourceNodeID) {
	applyGUIParameterValueFromVST(paramIndex, value, sourceNodeID);

	if(paramIndex >= 0 && paramIndex < 1024 && parameterInfoSlots[paramIndex] != nullptr) {
		parameterInfoSlots[paramIndex]->value = value;
	}
}

int scVST::getInstanceIndexFromNodeID(int nodeID) {
	auto it = nodeIDToInstanceIndex.find(nodeID);
	return it != nodeIDToInstanceIndex.end() ? it->second : -1;
}

void scVST::propagateParameterToOtherInstances(int sourceNodeID, int paramIndex, float value) {
	// Check if we should propagate (redundant check for safety)
	if(!shouldPropagateFromVSTGUI(paramIndex, sourceNodeID)) {
		return;
	}
	suppressFeedbackFor(paramIndex, ofGetElapsedTimeMillis() + 50);
	/*
	 ofLogNotice("scVST") << "Propagating parameter " << paramIndex << " = " << value
	 << " from node " << sourceNodeID << " to other instances (GUI-initiated)";
	 */
	// Apply to all OTHER instances (not the source)
	static thread_local ofxOscMessage setMsg;
	for(const auto& target : activeInstanceTargets) {
		if(target.synth->nodeID != sourceNodeID) { // Skip source instance
			try {
				setMsg.clear();
				setMsg.setAddress("/u_cmd");
				setMsg.addIntArg(target.synth->nodeID);
				setMsg.addIntArg(2);
				setMsg.addStringArg("/set");
				setMsg.addIntArg(paramIndex);
				setMsg.addFloatArg(value);
				target.server->sendMsg(setMsg);
				
				//ofLogVerbose("scVST") << "Propagated to instance " << target.synth->nodeID;
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error propagating parameter to synth " << target.synth->nodeID << ": " << e.what();
			}
		}
	}
	
}

bool scVST::shouldPropagateFromVSTGUI(int paramIndex, int sourceNodeID) {
	if(isPresetLoading || hasPendingPresetData) {
		return false;
	}

	if(isFeedbackSuppressed(paramIndex, ofGetElapsedTimeMillis())) {
		return false;
	}
	
	if(firstInstanceNodeIDs.count(sourceNodeID) == 0) {
		return false;
	}
	
	auto vectorParam = resolveDynamicVectorParameterSlot(paramIndex);
	if(vectorParam) {
		auto values = vectorParam->getParameter().get();
		
		if(values.size() > 1) {
			return false;
		}
		
		return true;
	}
	
	return true;
}



void scVST::handleInstanceAwareParameterChange(int paramIndex, const vector<float>& values) {
	// Mark the parameter as locally-driven so echoed VST feedback is ignored,
	// but do not drop fresh GUI/LFO changes while suppression is active.
	suppressFeedbackFor(paramIndex, ofGetElapsedTimeMillis() + 100);
	/*
	 ofLogNotice("scVST") << "Instance-aware parameter change: param " << paramIndex
	 << " with " << values.size() << " values (USER-initiated)";
	 */
	// Apply parameter values to specific instances
	static thread_local ofxOscMessage setMsg;
	for(size_t instanceIndex = 0; instanceIndex < activeInstanceTargets.size(); ++instanceIndex) {
		const auto& target = activeInstanceTargets[instanceIndex];
		try {
			float value;
			
			if(values.size() == 1) {
				// Scalar value - broadcast to all instances
				value = values[0];
				/*
				 ofLogVerbose("scVST") << "Broadcasting scalar value " << value
				 << " to instance " << instanceIndex << " (node " << target.synth->nodeID << ")";
				 */
			}
			else if(instanceIndex < values.size()) {
				// Vector value - use specific value for this instance
				value = values[instanceIndex];
				/*
				 ofLogVerbose("scVST") << "Setting instance " << instanceIndex
				 << " (node " << target.synth->nodeID << ") to value " << value;
				 */
			}
			else {
				// Vector is shorter than number of instances - use last value
				value = values.back();
				/*
				 ofLogVerbose("scVST") << "Using last value " << value
				 << " for instance " << instanceIndex << " (node " << target.synth->nodeID << ")";
				 */
			}
			
			setMsg.clear();
			setMsg.setAddress("/u_cmd");
			setMsg.addIntArg(target.synth->nodeID);
			setMsg.addIntArg(2);
			setMsg.addStringArg("/set");
			setMsg.addIntArg(paramIndex);
			setMsg.addFloatArg(value);
			target.server->sendMsg(setMsg);
			
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error setting parameter on synth " << target.synth->nodeID << ": " << e.what();
		}
	}
	
}

void scVST::handleVSTOpen(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 5) {
		int nodeID = msg.getArgAsInt32(0);
		bool success = msg.getArgAsFloat(2) > 0.5f;
		bool hasEditor = msg.getArgAsFloat(3) > 0.5f;
		float latency = msg.getArgAsFloat(4);
		
		// Verify this is our VST instance
		if (!isMyVSTInstance(nodeID)) return;
		
		if(success) {
			readyInstances.insert(nodeID);
			
			// Enhanced progress tracking for parallel loading
			int totalExpectedInstances = 0;
			for(auto& serverInstances : synthInstances) {
				totalExpectedInstances += serverInstances.second.size();
			}
			
				ofLogVerbose("scVST") << "VST instance " << nodeID << " loaded successfully ("
									 << readyInstances.size() << "/" << totalExpectedInstances << " ready)";
			
			// When ALL instances are ready, apply FXP to all at once
			if(areAllInstancesReady()) {
					ofLogVerbose("scVST") << "All " << totalExpectedInstances
									 << " VST instances ready; applying state restoration";
				
				// CRITICAL: Stop the timer - event-driven approach takes over
				parameterTimerActive = false;
				
				// NEW SMART SOURCE SELECTION LOGIC
				std::string nodeKey = getNodeCacheKey();
				
				// Priority 1: During Oceanode preset loading - ALWAYS use preset FXP data
				if(oceanodePresetLoading && hasSavedFXPData) {
						ofLogVerbose("scVST") << "Restoring VST state from Oceanode preset FXP for node '"
										 << nodeKey << "'";
					for(auto& serverInstances : synthInstances) {
						for(auto synth : serverInstances.second) {
							if(synth != nullptr && fxpAppliedInstances.count(synth->nodeID) == 0) {
								applyFXPToInstance(synth->nodeID);
							}
						}
					}
				}
				// Priority 2: VST has been modified since preset load - use cached FXP data
				else if(shouldUseCachedFXP()) {
						ofLogVerbose("scVST") << "Restoring VST state from cache for node '"
										 << nodeKey << "' (" << cachedFXP.size() << " bytes)";
					
					string tempPath = createTempFXPPath();
					try {
						std::ofstream file(tempPath, std::ios::binary);
						if(file.is_open()) {
							file.write(reinterpret_cast<const char*>(cachedFXP.data()), cachedFXP.size());
							file.close();
							
							for(auto& serverInstances : synthInstances) {
								for(auto synth : serverInstances.second) {
									if(synth != nullptr) {
										ofxOscMessage readMsg;
										readMsg.setAddress("/u_cmd");
										readMsg.addIntArg(synth->nodeID);
										readMsg.addIntArg(2);
										readMsg.addStringArg("/program_read");
										readMsg.addStringArg(tempPath);
										readMsg.addIntArg(1);
										serverInstances.first->sendMsg(readMsg);
									}
								}
							}
							
								ofLogVerbose("scVST") << "VST state restoration commands sent to all instances";
							
						}
					} catch(const std::exception& e) {
						ofLogError("scVST") << "Error applying cached FXP for node '" << nodeKey << "': " << e.what();
						fxpCacheValid = false;
					}
				}
				else if(shouldUsePresetFXP()) {
					if(hasSavedFXPData) {
						string tempPath = createTempFXPPath();
						std::ofstream file(tempPath, std::ios::binary);
						file.write(reinterpret_cast<const char*>(savedFXPData.data()), savedFXPData.size());
						file.close();
						
						for(auto& serverInstances : synthInstances) {
							for(auto synth : serverInstances.second) {
								if(synth != nullptr) {
									ofxOscMessage readMsg;
									readMsg.setAddress("/u_cmd");
									readMsg.addIntArg(synth->nodeID);
									readMsg.addIntArg(2);
									readMsg.addStringArg("/program_read");
									readMsg.addStringArg(tempPath);
									readMsg.addIntArg(1);
									serverInstances.first->sendMsg(readMsg);
								}
							}
						}
					}
				}
				else {
						ofLogVerbose("scVST") << "No FXP to apply; using default VST state";
				}
				
				// Hold the immediate feedback path briefly so recall-driven parameter
				// messages land before steady-state batching resumes.
				if(hasSavedFXPData || fxpCacheValid) {
					ofSleepMillis(500);
						ofLogVerbose("scVST") << "FXP load complete; disabling blocking locks";
				}
				isFXPLoading.store(false, std::memory_order_release);
				if(fxpCacheSavePending.exchange(false, std::memory_order_acq_rel)) {
					scheduleImmediateFXPCache();
				}
				
				// GUI-side preset values are applied after the plugin state has settled.
				if(hasPendingPresetData) {
						ofLogVerbose("scVST") << "Applying pending GUI parameters after VST ready event";
					ofSleepMillis(200);
					applyPendingPresetData();
				}
			}
		} else {
			ofLogError("scVST") << "VST Plugin failed to open on node " << nodeID;
		}
	}
}

void scVST::applyFXPToInstance(int nodeID) {
	if(!hasSavedFXPData) {
		ofLogWarning("scVST") << "No FXP data to apply to instance " << nodeID;
		return;
	}
	
	// Find the server for this nodeID
	ofxSCServer* targetServer = nullptr;
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr && synth->nodeID == nodeID) {
				targetServer = serverInstances.first;
				break;
			}
		}
		if(targetServer) break;
	}
	
	if(!targetServer) {
		ofLogError("scVST") << "Could not find server for node " << nodeID;
		return;
	}
	
	//ofLogNotice("scVST") << "Applying FXP preset to instance " << nodeID
	//					<< " (" << savedFXPData.size() << " bytes)";
	
	try {
		// Create temporary file with FXP data
		tempFXPPath = createTempFXPPath();
		
		std::ofstream fxpFile(tempFXPPath, std::ios::binary);
		if(fxpFile.is_open()) {
			fxpFile.write(reinterpret_cast<const char*>(savedFXPData.data()), savedFXPData.size());
			fxpFile.close();
			
			// Send async program_read command to VST (no waiting)
			ofxOscMessage readMsg;
			readMsg.setAddress("/u_cmd");
			readMsg.addIntArg(nodeID);
			readMsg.addIntArg(2);
			readMsg.addStringArg("/program_read");
			readMsg.addStringArg(tempFXPPath);
			readMsg.addIntArg(1); // async = true
			targetServer->sendMsg(readMsg);
			
			//ofLogNotice("scVST") << "Sent async FXP load to instance " << nodeID;
			
		} else {
			ofLogError("scVST") << "Could not create temporary FXP file: " << tempFXPPath;
		}
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error applying FXP to instance " << nodeID << ": " << e.what();
	}
}

void scVST::addParameterToGUI(int paramIndex) {
	// Safety check
	if(paramIndex < 0) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}
	
	// CRITICAL FIX: First check if parameter already exists in ANY form
	// This prevents duplication when loading presets
	if((paramIndex >= 0 && paramIndex < 1024 &&
		(dynamicScalarParameterSlots[paramIndex] || dynamicVectorParameterSlots[paramIndex])) ||
	   dynamicParameters.count(paramIndex) > 0 || dynamicVectorParameters.count(paramIndex) > 0) {
		// Parameter already exists, just update its value AND send to VST
		auto infoSlot = (paramIndex >= 0 && paramIndex < 1024) ? parameterInfoSlots[paramIndex] : nullptr;
		if(infoSlot != nullptr) {
			if(paramIndex >= 0 && paramIndex < 1024 && dynamicScalarParameterSlots[paramIndex]) {
				float value = infoSlot->value;
				dynamicScalarParameterSlots[paramIndex]->getParameter().setWithoutEventNotifications(value);
				// Send to VST
				setVSTParameter(paramIndex, value);
			}
			if(paramIndex >= 0 && paramIndex < 1024 && dynamicVectorParameterSlots[paramIndex]) {
				float value = infoSlot->value;
				dynamicVectorParameterSlots[paramIndex]->getParameter().setWithoutEventNotifications({value});
				// Send to VST
				setVSTParameter(paramIndex, value);
			}
		}
		ofLogVerbose("scVST::addParameterToGUI") << "Parameter " << paramIndex << " already exists, updated value only";
		return;
	}
	
	// CRITICAL FIX: Also check if a parameter with this name already exists in the parameter group
	// This catches cases where the parameter exists but isn't in our tracking maps
	auto infoSlot = (paramIndex >= 0 && paramIndex < 1024) ? parameterInfoSlots[paramIndex] : nullptr;
	if(infoSlot != nullptr) {
		string paramName = infoSlot->displayName;
		
		// Check for any variant of this parameter name in the group
		for(int i = 0; i < getParameterGroup().size(); i++) {
			string existingName = getParameterGroup().get(i).getName();
			// Check if this is the same parameter (with or without suffix)
			if(existingName == paramName || existingName.find(paramName + "_") == 0) {
				ofLogWarning("scVST::addParameterToGUI") << "Parameter '" << paramName
				<< "' (index " << paramIndex << ") already exists in group as '"
				<< existingName << "' - skipping creation";
				
				// Try to update the value if we can find it
				try {
					float value = infoSlot->value;
					// Try to find and update the existing parameter
					for(auto& param : dynamicVectorFloatParameters) {
						if(param.second && param.second->getName() == existingName) {
							param.second->setWithoutEventNotifications({value});
							setVSTParameter(paramIndex, value);
							break;
						}
					}
					for(auto& param : dynamicFloatParameters) {
						if(param.second && param.second->getName() == existingName) {
							param.second->setWithoutEventNotifications(value);
							setVSTParameter(paramIndex, value);
							break;
						}
					}
				} catch(...) {}
				
				return;
			}
		}
	}
	
	// Create parameter info if it doesn't exist
	if(infoSlot == nullptr) {
		VSTParameterInfo info;
		info.index = paramIndex;
		info.displayName = "Param" + ofToString(paramIndex);
		info.value = 0.0f;
		parameterInfoMap[paramIndex] = info;
		cacheParameterInfoSlot(paramIndex);
		//ofLogNotice("scVST") << "Created parameter info for index " << paramIndex;
	}
	
	// Create new parameter - now as VECTOR parameter to support both scalar and vector values
	infoSlot = (paramIndex >= 0 && paramIndex < 1024) ? parameterInfoSlots[paramIndex] : nullptr;
	string paramName = infoSlot != nullptr ? infoSlot->displayName : "Param" + ofToString(paramIndex);
	
	// Ensure parameter name is unique
	string uniqueParamName = paramName;
	int nameCounter = 1;
	while(getParameterGroup().contains(uniqueParamName)) {
		uniqueParamName = paramName + "_" + ofToString(nameCounter);
		nameCounter++;
	}
	
	// Create VECTOR parameter instead of scalar
	auto newParam = std::make_shared<ofParameter<vector<float>>>();
	newParam->set(uniqueParamName, {infoSlot != nullptr ? infoSlot->value : 0.0f}, {0.0f}, {1.0f});
	
	// Store the parameter to keep it alive BEFORE adding to GUI
	dynamicVectorFloatParameters[paramIndex] = newParam;
	
	// CRITICAL: Store the actual registered name
	parameterInfoMap[paramIndex].registeredName = uniqueParamName;
	ofLogVerbose("scVST") << "Registered parameter " << paramIndex
	<< " with name '" << uniqueParamName
	<< "' (display: " << parameterInfoMap[paramIndex].displayName << ")";
	cacheParameterInfoSlot(paramIndex);
	
	try {
		auto oceanodeParam = addParameter(*newParam);
		dynamicVectorParameters[paramIndex] = oceanodeParam;
		cacheDynamicVectorParameterSlot(paramIndex, oceanodeParam);
		
		//ofLogNotice("scVST") << "Successfully added vector parameter " << uniqueParamName << " to GUI";
		
		// Listen on the registered Oceanode parameter so restored connections
		// keep driving the VST after preset reload.
		listeners.push(oceanodeParam->getParameter().newListener([this, paramIndex](vector<float> &values) -> void {
			try {
				if(this == nullptr) return;
				
				// Always update parameter info regardless of state
				if(parameterInfoMap.count(paramIndex) > 0 && !values.empty()) {
					parameterInfoMap[paramIndex].value = values[0];
				}
				
				// Check if this parameter should be converted from vector to scalar
				checkAndConvertVectorToScalar(paramIndex);
				
				// Only send to VST if conditions are met
				if(!isPresetLoading &&
				   !synthInstances.empty() &&
				   areAllInstancesReady() &&
				   parameterInfoMap.count(paramIndex) > 0) {
					
					/*
					 ofLogNotice("scVST") << "🎛️ GUI parameter " << paramIndex
					 << " changed by user - sending to VST ("
					 << values.size() << " values)";
					 */
					handleDynamicParameterChange(paramIndex, values);
					
				} else {
					/*
					 if(isPresetLoading) {
					 ofLogVerbose("scVST") << "⏳ GUI parameter " << paramIndex
					 << " changed during preset loading - not sending to VST yet";
					 } else if(synthInstances.empty()) {
					 ofLogVerbose("scVST") << "❌ GUI parameter " << paramIndex
					 << " changed but no VST instances available";
					 } else if(!areAllInstancesReady()) {
					 ofLogVerbose("scVST") << "⏳ GUI parameter " << paramIndex
					 << " changed but VST instances not ready yet";
					 }
					 */
				}
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in parameter listener for index " << paramIndex << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in parameter listener for index " << paramIndex;
			}
		}));
		
		// IMPORTANT: Send the initial value to the VST after creating the GUI parameter
		float initialValue = parameterInfoSlots[paramIndex] != nullptr ? parameterInfoSlots[paramIndex]->value : 0.0f;
		if(initialValue != 0.0f || !isPresetLoading) { // Always send during preset loading
			/*
			 ofLogNotice("scVST") << "Sending initial value " << initialValue
			 << " for parameter " << paramIndex << " to VST";
			 */
			setVSTParameter(paramIndex, initialValue);
		}
		
		// ADD NAME EDITOR TO INSPECTOR
		string nameEditorName = uniqueParamName + "_Name";
		string uniqueNameEditorName = nameEditorName;
		int nameEditorCounter = 1;
		while(getInspectorParameterGroup().contains(uniqueNameEditorName)) {
			uniqueNameEditorName = nameEditorName + "_" + ofToString(nameEditorCounter);
			nameEditorCounter++;
		}
		
		auto nameEditor = std::make_shared<ofParameter<string>>();
		nameEditor->set(uniqueNameEditorName, parameterInfoMap[paramIndex].displayName);
		
		// Store the name editor to keep it alive
		dynamicStringParameters[paramIndex] = nameEditor;
		
		// Add to inspector
		addInspectorParameter(*nameEditor);
		
		// Set up name change listener
		listeners.push(nameEditor->newListener([this, paramIndex](string &newName) -> void {
			if(this == nullptr) return; // Safety check
			
			if(!newName.empty() && parameterInfoMap.count(paramIndex) > 0) {
				string oldName = parameterInfoMap[paramIndex].displayName;
				parameterInfoMap[paramIndex].displayName = newName;
				cacheParameterInfoSlot(paramIndex);
				
				//ofLogNotice("scVST") << "Renamed parameter " << paramIndex << " from '" << oldName << "' to '" << newName << "'";
				
				// Update the actual parameter name in the GUI
				if(dynamicVectorParameters.count(paramIndex) > 0) {
					// We need to remove and re-add the parameter with the new name
					// This is tricky because we need to preserve the value and connections
					
					// Get current value
					vector<float> currentValue = dynamicVectorParameters[paramIndex]->getParameter().get();
					
					// Remove the old parameter
					try {
						removeParameter(oldName);
					} catch(...) {
						// If removal fails, try with various suffixes that might have been added
						for(int suffix = 1; suffix <= 10; suffix++) {
							try {
								removeParameter(oldName + "_" + ofToString(suffix));
								break;
							} catch(...) {
								continue;
							}
						}
					}
					
					// Create new parameter with the new name
					string newUniqueName = newName;
					int counter = 1;
					while(getParameterGroup().contains(newUniqueName)) {
						newUniqueName = newName + "_" + ofToString(counter);
						counter++;
					}
					
					auto newParam = std::make_shared<ofParameter<vector<float>>>();
					newParam->set(newUniqueName, currentValue, {0.0f}, {1.0f});
					
					// Store the new parameter
					dynamicVectorFloatParameters[paramIndex] = newParam;
					
					try {
						auto oceanodeParam = addParameter(*newParam);
						dynamicVectorParameters[paramIndex] = oceanodeParam;
						cacheDynamicVectorParameterSlot(paramIndex, oceanodeParam);
						
						// Re-setup the parameter listener on the registered parameter.
						listeners.push(oceanodeParam->getParameter().newListener([this, paramIndex](vector<float> &values) -> void {
							if(this == nullptr) return;
							if(!isPresetLoading && parameterInfoMap.count(paramIndex) > 0) {
								handleDynamicParameterChange(paramIndex, values);
								if(!values.empty()) {
									parameterInfoMap[paramIndex].value = values[0];
								}
							}
						}));
						
						//ofLogNotice("scVST") << "Successfully renamed parameter to " << newUniqueName;
						
					} catch(const std::exception& e) {
						ofLogError("scVST") << "Error re-adding parameter with new name: " << e.what();
					}
				}
			}
		}));
		
		// ADD INDIVIDUAL REMOVAL BUTTON TO INSPECTOR
		string removeButtonName = "Remove " + uniqueParamName;
		string uniqueRemoveButtonName = removeButtonName;
		int removeNameCounter = 1;
		while(getInspectorParameterGroup().contains(uniqueRemoveButtonName)) {
			uniqueRemoveButtonName = removeButtonName + "_" + ofToString(removeNameCounter);
			removeNameCounter++;
		}
		
		auto removeButton = std::make_shared<ofParameter<void>>();
		removeButton->set(uniqueRemoveButtonName);
		
		// Store the button to keep it alive
		dynamicRemovalButtons[paramIndex] = removeButton;
		
		// Add to inspector
		addInspectorParameter(*removeButton);
		
		// Set up removal listener
		listeners.push(removeButton->newListener([this, paramIndex](){
			//ofLogNotice("scVST") << "Removing parameter " << paramIndex << " via individual button";
			removeParameterFromGUI(paramIndex);
		}));
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error adding parameter to GUI: " << e.what();
		// Clean up on error
		dynamicVectorFloatParameters.erase(paramIndex);
		dynamicVectorParameters.erase(paramIndex);
	}
}

void scVST::removeParameterFromGUI(int paramIndex) {
	ofLogNotice("scVST::removeParameterFromGUI") << "Removing parameter " << paramIndex << " from GUI";
	
	// CRITICAL FIX: Force removal even during preset loading
	// Store the current preset loading state and temporarily disable it
	bool wasPresetLoading = isPresetLoading;
	bool wasOceanodePresetLoading = oceanodePresetLoading;
	isPresetLoading = false;
	oceanodePresetLoading = false;
	
	// Get parameter info if available
	string paramName = "Param" + ofToString(paramIndex);
	string registeredName = "";
	string originalName = "";
	string actualParameterNameInGroup = "";  // The actual name found in the parameter group
	
	if(parameterInfoMap.count(paramIndex) > 0) {
		// Use the actual registered name if available
		if(!parameterInfoMap[paramIndex].registeredName.empty()) {
			registeredName = parameterInfoMap[paramIndex].registeredName;
			ofLogNotice("scVST") << "Using registered name '" << registeredName
			<< "' for removing parameter " << paramIndex;
		}
		paramName = parameterInfoMap[paramIndex].displayName;
		originalName = parameterInfoMap[paramIndex].originalName;
		ofLogNotice("scVST") << "Parameter " << paramIndex << " info: displayName='" << paramName
		<< "', originalName='" << originalName
		<< "', registeredName='" << registeredName << "'";
	}
	
	// DEBUG: List all parameters in the group to see what's actually there
	ofLogNotice("scVST") << "Current parameters in group:";
	for(int i = 0; i < getParameterGroup().size(); i++) {
		ofLogNotice("scVST") << "  [" << i << "] " << getParameterGroup().get(i).getName();
	}
	
	// FIX: Check if parameter exists in our tracking maps
	bool parameterTracked = (dynamicParameters.count(paramIndex) > 0 ||
							 dynamicVectorParameters.count(paramIndex) > 0);
	
	// CRITICAL FIX: Search for VST parameters by their index pattern
	// VST parameters are typically at the end of the parameter group after the fixed parameters
	// We need to find parameters that could be VST parameter 861
	bool parameterFoundInGroup = false;
	
	// First, try to find by VST parameter index pattern
	// VST parameters typically start after the fixed parameters
	// Look for any parameter that could be index 861
	for(int i = 0; i < getParameterGroup().size(); i++) {
		string currentName = getParameterGroup().get(i).getName();
		
		// Skip empty names and fixed parameters
		if(currentName.empty()) continue;
		
		// Check if this could be a VST parameter
		// VST parameters are typically after the fixed ones (after index ~28)
		if(i >= 28) {  // Approximate start of VST parameters
			// This could be our parameter if:
			// 1. It matches any of our known names
			// 2. It's at a position that could correspond to parameter 861
			// 3. It's not one of the fixed parameter names
			
			// List of fixed parameter names to exclude
			vector<string> fixedParams = {
				"In", "Out", "N Chan", "Mix", "Plugin", "Editor", "Add Last", "Propagate",
				"Program", "Instance", "MIDI Chan", "Pitch", "Velocity", "Gate", "PitchBend",
				"ModWheel", "Play", "Position", "Reset", "BPM", "Note Out", "CC Out"
			};
			
			bool isFixedParam = false;
			for(const string& fixed : fixedParams) {
				if(currentName == fixed) {
					isFixedParam = true;
					break;
				}
			}
			
			if(!isFixedParam) {
				// This is likely a VST parameter
				// For parameter 861, it's likely one of the first VST parameters added
				ofLogNotice("scVST") << "Found potential VST parameter at index " << i << ": " << currentName;
				
				// Check if this matches our parameter by various criteria
				bool matches = false;
				
				// Check if it matches the original VST name
				if(!originalName.empty()) {
					string lowerCurrent = currentName;
					string lowerOriginal = originalName;
					std::transform(lowerCurrent.begin(), lowerCurrent.end(), lowerCurrent.begin(), ::tolower);
					std::transform(lowerOriginal.begin(), lowerOriginal.end(), lowerOriginal.begin(), ::tolower);
					if(lowerCurrent == lowerOriginal || lowerCurrent.find(lowerOriginal) != string::npos) {
						matches = true;
						ofLogNotice("scVST") << "Matched by original name: " << originalName;
					}
				}
				
				// Check common VST parameter names for index 861
				// Index 861 often corresponds to filter cutoff in many VSTs
				vector<string> commonNames = {"CutOff", "cutoff", "Cutoff", "Filter", "filter", "Freq", "freq"};
				if(!matches) {
					for(const string& common : commonNames) {
						if(currentName == common || currentName.find(common) != string::npos) {
							matches = true;
							ofLogNotice("scVST") << "Matched by common name: " << common;
							break;
						}
					}
				}
				
				// If we found a match, use this parameter
				if(matches) {
					actualParameterNameInGroup = currentName;
					parameterFoundInGroup = true;
					ofLogNotice("scVST") << "✓ Identified parameter to remove: " << actualParameterNameInGroup;
					break;
				}
			}
		}
	}
	
	// If not found by VST index pattern, try other methods
	if(!parameterFoundInGroup) {
		// Build list of names to try
		vector<string> namesToTry;
		
		// Priority 1: Registered name
		if(!registeredName.empty()) {
			namesToTry.push_back(registeredName);
		}
		
		// Priority 2: Original VST name
		if(!originalName.empty()) {
			namesToTry.push_back(originalName);
		}
		
		// Priority 3: Display name (if it's not the generic Param name)
		if(!paramName.empty() && paramName != "Param" + ofToString(paramIndex)) {
			namesToTry.push_back(paramName);
		}
		
		// Priority 4: Generic name
		namesToTry.push_back("Param" + ofToString(paramIndex));
		
		// Search through all parameters
		for(int i = 0; i < getParameterGroup().size(); i++) {
			string currentName = getParameterGroup().get(i).getName();
			
			// Check against known names
			for(const string& tryName : namesToTry) {
				if(currentName == tryName ||
				   currentName.find(tryName + "_") == 0) {
					actualParameterNameInGroup = currentName;
					parameterFoundInGroup = true;
					ofLogNotice("scVST") << "Found parameter by name search: " << actualParameterNameInGroup;
					break;
				}
			}
			
			if(parameterFoundInGroup) break;
		}
	}
	
	// Remove the parameter from the group if found
	if(parameterFoundInGroup && !actualParameterNameInGroup.empty()) {
		try {
			ofLogNotice("scVST") << "Removing parameter from group: " << actualParameterNameInGroup;
			removeParameter(actualParameterNameInGroup);
			
			// Verify removal
			if(!getParameterGroup().contains(actualParameterNameInGroup)) {
				ofLogNotice("scVST") << "✅ Successfully removed parameter from group: " << actualParameterNameInGroup;
			} else {
				ofLogError("scVST") << "❌ Failed to remove parameter from group: " << actualParameterNameInGroup;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Exception removing parameter: " << e.what();
		}
	} else if(!parameterTracked) {
		ofLogWarning("scVST") << "Parameter " << paramIndex << " not found in group or tracking maps";
	}
	
	// Clean up tracking maps (always do this, even if parameter wasn't in group)
	if(dynamicParameters.count(paramIndex) > 0) {
		dynamicParameters.erase(paramIndex);
		clearDynamicParameterSlots(paramIndex);
		ofLogNotice("scVST") << "Removed from dynamicParameters";
	}
	
	if(dynamicVectorParameters.count(paramIndex) > 0) {
		dynamicVectorParameters.erase(paramIndex);
		clearDynamicParameterSlots(paramIndex);
		ofLogNotice("scVST") << "Removed from dynamicVectorParameters";
	}
	
	// Remove stored float parameters
	dynamicFloatParameters.erase(paramIndex);
	dynamicVectorFloatParameters.erase(paramIndex);
	
	// Remove inspector parameters using the actual parameter name found
	string nameForInspector = !actualParameterNameInGroup.empty() ? actualParameterNameInGroup : paramName;
	
	// Remove name editor from inspector
	if(dynamicStringParameters.count(paramIndex) > 0) {
		vector<string> nameEditorVariations = {
			nameForInspector + "_Name",
			paramName + "_Name",
			"Param" + ofToString(paramIndex) + "_Name"
		};
		
		bool removedNameEditor = false;
		for(const string& editorName : nameEditorVariations) {
			if(getInspectorParameterGroup().contains(editorName)) {
				try {
					removeInspectorParameter(editorName);
					ofLogNotice("scVST") << "Removed name editor: " << editorName;
					removedNameEditor = true;
					break;
				} catch(...) {}
			}
		}
		
		// If not found with exact names, search for it
		if(!removedNameEditor) {
			for(int i = getInspectorParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getInspectorParameterGroup().get(i).getName();
				if(currentName.find("_Name") != string::npos &&
				   (currentName.find(nameForInspector) == 0 ||
					currentName.find(paramName) == 0 ||
					currentName.find("Param" + ofToString(paramIndex)) == 0)) {
					try {
						removeInspectorParameter(currentName);
						ofLogNotice("scVST") << "Removed name editor by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		
		dynamicStringParameters.erase(paramIndex);
	}
	
	// Remove removal button from inspector
	if(dynamicRemovalButtons.count(paramIndex) > 0) {
		vector<string> removalButtonVariations = {
			"Remove " + nameForInspector,
			"Remove " + paramName,
			"Remove Param" + ofToString(paramIndex)
		};
		
		bool removedButton = false;
		for(const string& buttonName : removalButtonVariations) {
			if(getInspectorParameterGroup().contains(buttonName)) {
				try {
					removeInspectorParameter(buttonName);
					ofLogNotice("scVST") << "Removed removal button: " << buttonName;
					removedButton = true;
					break;
				} catch(...) {}
			}
		}
		
		// If not found with exact names, search for it
		if(!removedButton) {
			for(int i = getInspectorParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getInspectorParameterGroup().get(i).getName();
				if(currentName.find("Remove ") == 0 &&
				   (currentName.find(nameForInspector) != string::npos ||
					currentName.find(paramName) != string::npos ||
					currentName.find("Param" + ofToString(paramIndex)) != string::npos)) {
					try {
						removeInspectorParameter(currentName);
						ofLogNotice("scVST") << "Removed removal button by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		
		dynamicRemovalButtons.erase(paramIndex);
	}
	
	// Remove from parameter info map
	parameterInfoMap.erase(paramIndex);
	clearParameterInfoSlot(paramIndex);
	
	// CRITICAL FIX: Restore the preset loading flags
	isPresetLoading = wasPresetLoading;
	oceanodePresetLoading = wasOceanodePresetLoading;
	
	// FINAL VERIFICATION: Check if the parameter was actually removed
	if(parameterFoundInGroup && !actualParameterNameInGroup.empty()) {
		if(getParameterGroup().contains(actualParameterNameInGroup)) {
			ofLogError("scVST") << "❌ FAILED to remove parameter " << paramIndex
			<< " - still exists as '" << actualParameterNameInGroup << "' in parameter group!";
		} else {
			ofLogNotice("scVST") << "✅ Successfully removed parameter " << paramIndex << " from GUI";
		}
	} else {
		ofLogNotice("scVST") << "Parameter " << paramIndex << " removal completed (was not in group)";
	}
	
	// CRITICAL FIX: Search for all possible parameter names in the GUI
	vector<string> possibleParamNames;
	// Priority 1: Use the registered name if we have it
	if(!registeredName.empty()) {
		possibleParamNames.push_back(registeredName);
	}
	// Priority 2: Try the display name
	possibleParamNames.push_back(paramName);
	
	// Add numbered suffixes that might have been added
	for(int suffix = 1; suffix <= 20; suffix++) {
		possibleParamNames.push_back(paramName + "_" + ofToString(suffix));
	}
	
	// Also search by the generic "Param" + index name
	string genericName = "Param" + ofToString(paramIndex);
	if(genericName != paramName) {
		possibleParamNames.push_back(genericName);
		for(int suffix = 1; suffix <= 20; suffix++) {
			possibleParamNames.push_back(genericName + "_" + ofToString(suffix));
		}
	}
	
	// Remove main parameter (scalar)
	if(dynamicParameters.count(paramIndex) > 0) {
		bool removed = false;
		for(const string& possibleName : possibleParamNames) {
			try {
				if(getParameterGroup().contains(possibleName)) {
					ofLogNotice("scVST") << "Attempting to remove scalar parameter: " << possibleName;
					removeParameter(possibleName);
					// Verify it was actually removed
					if(!getParameterGroup().contains(possibleName)) {
						ofLogNotice("scVST") << "✅ Successfully removed scalar parameter: " << possibleName;
						removed = true;
						break;
					} else {
						ofLogError("scVST") << "❌ Failed to remove scalar parameter: " << possibleName << " - still exists!";
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Exception removing parameter " << possibleName << ": " << e.what();
			}
		}
		
		if(!removed) {
			// Last resort: iterate through all parameters to find one that matches
			for(int i = getParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getParameterGroup().get(i).getName();
				if(currentName.find(paramName) == 0 || currentName.find(genericName) == 0) {
					try {
						removeParameter(currentName);
						ofLogNotice("scVST") << "Removed scalar parameter by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		dynamicParameters.erase(paramIndex);
		clearDynamicParameterSlots(paramIndex);
	}
	
	// Remove main parameter (vector)
	if(dynamicVectorParameters.count(paramIndex) > 0) {
		bool removed = false;
		for(const string& possibleName : possibleParamNames) {
			try {
				if(getParameterGroup().contains(possibleName)) {
					ofLogNotice("scVST") << "Attempting to remove vector parameter: " << possibleName;
					removeParameter(possibleName);
					// Verify it was actually removed
					if(!getParameterGroup().contains(possibleName)) {
						ofLogNotice("scVST") << "✅ Successfully removed vector parameter: " << possibleName;
						removed = true;
						break;
					} else {
						ofLogError("scVST") << "❌ Failed to remove vector parameter: " << possibleName << " - still exists!";
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Exception removing parameter " << possibleName << ": " << e.what();
			}
		}
		
		if(!removed) {
			// Last resort: iterate through all parameters to find one that matches
			for(int i = getParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getParameterGroup().get(i).getName();
				if(currentName.find(paramName) == 0 || currentName.find(genericName) == 0) {
					try {
						removeParameter(currentName);
						ofLogNotice("scVST") << "Removed vector parameter by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		dynamicVectorParameters.erase(paramIndex);
		clearDynamicParameterSlots(paramIndex);
	}
	
	// Remove stored float parameter
	dynamicFloatParameters.erase(paramIndex);
	
	// Remove stored vector float parameter
	dynamicVectorFloatParameters.erase(paramIndex);
	
	// Remove name editor from inspector
	if(dynamicStringParameters.count(paramIndex) > 0) {
		// Build list of possible name editor names
		vector<string> possibleNameEditors;
		for(const string& baseName : possibleParamNames) {
			possibleNameEditors.push_back(baseName + "_Name");
			for(int suffix = 1; suffix <= 10; suffix++) {
				possibleNameEditors.push_back(baseName + "_Name_" + ofToString(suffix));
			}
		}
		
		bool removed = false;
		for(const string& possibleName : possibleNameEditors) {
			try {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed name editor: " << possibleName;
					removed = true;
					break;
				}
			} catch(...) {}
		}
		
		if(!removed) {
			// Search for any name editor that might be related
			for(int i = getInspectorParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getInspectorParameterGroup().get(i).getName();
				if((currentName.find(paramName + "_Name") == 0) ||
				   (currentName.find(genericName + "_Name") == 0)) {
					try {
						removeInspectorParameter(currentName);
						ofLogNotice("scVST") << "Removed name editor by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		dynamicStringParameters.erase(paramIndex);
	}
	
	// Remove removal button from inspector
	if(dynamicRemovalButtons.count(paramIndex) > 0) {
		// Build list of possible removal button names
		vector<string> possibleRemovalButtons;
		for(const string& baseName : possibleParamNames) {
			possibleRemovalButtons.push_back("Remove " + baseName);
			for(int suffix = 1; suffix <= 10; suffix++) {
				possibleRemovalButtons.push_back("Remove " + baseName + "_" + ofToString(suffix));
			}
		}
		
		bool removed = false;
		for(const string& possibleName : possibleRemovalButtons) {
			try {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed removal button: " << possibleName;
					removed = true;
					break;
				}
			} catch(...) {}
		}
		
		if(!removed) {
			// Search for any removal button that might be related
			for(int i = getInspectorParameterGroup().size() - 1; i >= 0; i--) {
				string currentName = getInspectorParameterGroup().get(i).getName();
				if((currentName.find("Remove " + paramName) == 0) ||
				   (currentName.find("Remove " + genericName) == 0)) {
					try {
						removeInspectorParameter(currentName);
						ofLogNotice("scVST") << "Removed removal button by search: " << currentName;
						break;
					} catch(...) {}
				}
			}
		}
		dynamicRemovalButtons.erase(paramIndex);
	}
	
	// Remove from parameter info map
	parameterInfoMap.erase(paramIndex);
	clearParameterInfoSlot(paramIndex);
	
	// CRITICAL FIX: Restore the preset loading flags
	isPresetLoading = wasPresetLoading;
	oceanodePresetLoading = wasOceanodePresetLoading;
	
	// FINAL VERIFICATION: Check if the parameter was actually removed
	bool stillExists = false;
	string foundName = "";
	
	// Check if any variant of the parameter still exists in the group
	for(int i = 0; i < getParameterGroup().size(); i++) {
		string currentName = getParameterGroup().get(i).getName();
		// Check against all possible names
		if(!registeredName.empty() && currentName == registeredName) {
			stillExists = true;
			foundName = currentName;
			break;
		}
		if(currentName == paramName ||
		   currentName.find(paramName + "_") == 0 ||
		   currentName == "Param" + ofToString(paramIndex) ||
		   currentName.find("Param" + ofToString(paramIndex) + "_") == 0) {
			stillExists = true;
			foundName = currentName;
			break;
		}
	}
	
	if(stillExists) {
		ofLogError("scVST") << "❌ FAILED to remove parameter " << paramIndex
		<< " - still exists as '" << foundName << "' in parameter group!";
		ofLogError("scVST") << "   Tried to remove: registeredName='" << registeredName
		<< "', displayName='" << paramName << "'";
	} else {
		ofLogNotice("scVST") << "✅ Successfully removed parameter " << paramIndex << " from GUI";
	}
}

void scVST::removeAllDynamicParameters() {
	ofLogNotice("scVST::removeAllDynamicParameters") << "Starting removal of all dynamic parameters";
	ofLogNotice("scVST::removeAllDynamicParameters") << "Current counts - scalar: " << dynamicParameters.size()
	<< ", vector: " << dynamicVectorParameters.size()
	<< ", paramInfo: " << parameterInfoMap.size();
	
	removeAllMidiCCParameters();
	
	try {
		// Create a copy of the keys to avoid iterator invalidation
		vector<int> paramIndices;
		
		// Collect scalar parameter indices
		for(auto& param : dynamicParameters) {
			paramIndices.push_back(param.first);
		}
		
		// Collect vector parameter indices
		for(auto& param : dynamicVectorParameters) {
			if(std::find(paramIndices.begin(), paramIndices.end(), param.first) == paramIndices.end()) {
				paramIndices.push_back(param.first);
			}
		}
		
		// Remove each parameter individually using the safe removal function
		for(int paramIndex : paramIndices) {
			try {
				removeParameterFromGUI(paramIndex);
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error removing parameter " << paramIndex << ": " << e.what();
				// Continue with other parameters
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error removing parameter " << paramIndex;
				// Continue with other parameters
			}
		}
		
		// Force clear all maps as a safety measure
		try {
			dynamicParameters.clear();
			dynamicVectorParameters.clear();
			dynamicFloatParameters.clear();
			dynamicVectorFloatParameters.clear();
			dynamicStringParameters.clear();
			dynamicRemovalButtons.clear();
			for(int i = 0; i < 1024; ++i) {
				clearDynamicParameterSlots(i);
			}
		} catch(...) {
			ofLogError("scVST::removeAllDynamicParameters") << "Error clearing parameter maps";
		}
		
		ofLogNotice("scVST::removeAllDynamicParameters") << "Finished removing all dynamic parameters";
		ofLogNotice("scVST::removeAllDynamicParameters") << "Final counts - scalar: " << dynamicParameters.size()
		<< ", vector: " << dynamicVectorParameters.size()
		<< ", paramInfo: " << parameterInfoMap.size();
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in removeAllDynamicParameters: " << e.what();
	} catch(...) {
		ofLogError("scVST") << "Unknown error in removeAllDynamicParameters";
	}
}

void scVST::removeAllDynamicParametersForce() {
	ofLogNotice("scVST::removeAllDynamicParametersForce") << "Force removing all dynamic parameters (ignoring preset loading state)";
	ofLogNotice("scVST::removeAllDynamicParametersForce") << "Initial counts - scalar: " << dynamicParameters.size()
	<< ", vector: " << dynamicVectorParameters.size()
	<< ", paramInfo: " << parameterInfoMap.size();
	
	// Temporarily disable preset loading flag to allow removal
	bool wasPresetLoading = isPresetLoading;
	bool wasOceanodePresetLoading = oceanodePresetLoading;
	isPresetLoading = false;
	oceanodePresetLoading = false;
	
	removeAllMidiCCParameters();
	
	try {
		// Create a copy of all parameter indices
		vector<int> allParamIndices;
		
		// Collect ALL parameter indices from all maps
		for(auto& param : dynamicParameters) {
			allParamIndices.push_back(param.first);
		}
		for(auto& param : dynamicVectorParameters) {
			if(std::find(allParamIndices.begin(), allParamIndices.end(), param.first) == allParamIndices.end()) {
				allParamIndices.push_back(param.first);
			}
		}
		for(auto& param : parameterInfoMap) {
			if(std::find(allParamIndices.begin(), allParamIndices.end(), param.first) == allParamIndices.end()) {
				allParamIndices.push_back(param.first);
			}
		}
		
		// Remove each parameter forcefully
		for(int paramIndex : allParamIndices) {
			try {
				removeParameterFromGUI(paramIndex);
			} catch(...) {
				// Continue even if individual removal fails
			}
		}
		
		// Force clear all maps
		clearParameterMaps();
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in removeAllDynamicParametersForce: " << e.what();
	}
	
	// Restore preset loading flags
	isPresetLoading = wasPresetLoading;
	oceanodePresetLoading = wasOceanodePresetLoading;
	
	ofLogNotice("scVST::removeAllDynamicParametersForce") << "Force removal complete";
	ofLogNotice("scVST::removeAllDynamicParametersForce") << "Final counts - scalar: " << dynamicParameters.size()
	<< ", vector: " << dynamicVectorParameters.size()
	<< ", paramInfo: " << parameterInfoMap.size();
}

void scVST::clearParameterMaps() {
	ofLogNotice("scVST") << "Clearing all parameter tracking maps";
	
	try {
		// Clear all parameter-related maps
		parameterInfoMap.clear();
		dynamicParameters.clear();
		dynamicVectorParameters.clear();
		dynamicFloatParameters.clear();
		dynamicVectorFloatParameters.clear();
		dynamicStringParameters.clear();
		dynamicRemovalButtons.clear();
		savedParameterNames.clear();
		clearAllParameterSlotCaches();
		
		// Clear feedback tracking
		clearAllFeedbackSuppression();
		
		// Clear parameter throttling
		// Old throttle map cleanup no longer needed with atomic approach
		
		ofLogNotice("scVST") << "All parameter maps cleared";
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error clearing parameter maps: " << e.what();
	}
}

void scVST::validateParameterConsistency() {
	ofLogNotice("scVST") << "Validating parameter consistency";
	
	// Check for orphaned parameters in GUI
	int orphanedCount = 0;
	for(int i = getParameterGroup().size() - 1; i >= 0; i--) {
		string paramName = getParameterGroup().get(i).getName();
		
		// Check if this is a dynamic parameter
		bool found = false;
		for(auto& param : dynamicVectorParameters) {
			if(dynamicVectorFloatParameters.count(param.first) > 0) {
				auto& floatParam = dynamicVectorFloatParameters[param.first];
				if(floatParam && floatParam->getName() == paramName) {
					found = true;
					break;
				}
			}
		}
		
		if(!found) {
			for(auto& param : dynamicParameters) {
				if(dynamicFloatParameters.count(param.first) > 0) {
					auto& floatParam = dynamicFloatParameters[param.first];
					if(floatParam && floatParam->getName() == paramName) {
						found = true;
						break;
					}
				}
			}
		}
		
		// Remove orphaned parameters
		if(!found && (paramName.find("Param") == 0 || paramName.find("_") != string::npos)) {
			try {
				removeParameter(paramName);
				orphanedCount++;
				ofLogWarning("scVST") << "Removed orphaned parameter: " << paramName;
			} catch(...) {}
		}
	}
	
	if(orphanedCount > 0) {
		ofLogNotice("scVST") << "Removed " << orphanedCount << " orphaned parameters";
	}
	
	// Validate parameter info map consistency
	vector<int> invalidIndices;
	for(auto& info : parameterInfoMap) {
		if(dynamicVectorParameters.count(info.first) == 0 &&
		   dynamicParameters.count(info.first) == 0) {
			invalidIndices.push_back(info.first);
		}
	}
	
	for(int idx : invalidIndices) {
		parameterInfoMap.erase(idx);
		clearParameterInfoSlot(idx);
		ofLogWarning("scVST") << "Removed invalid parameter info for index " << idx;
	}
	
	ofLogNotice("scVST") << "Parameter consistency validation complete";
}

void scVST::updateParameterValue(int paramIndex, float value) {
	// Update scalar parameter if it exists (without triggering events)
	auto scalarParam = (paramIndex >= 0 && paramIndex < 1024) ? dynamicScalarParameterSlots[paramIndex] : nullptr;
	if(scalarParam) {
		scalarParam->getParameter().setWithoutEventNotifications(value);
	}
	
	// Update vector parameter if it exists (set as single-element vector, without triggering events)
	auto vectorParam = (paramIndex >= 0 && paramIndex < 1024) ? dynamicVectorParameterSlots[paramIndex] : nullptr;
	if(vectorParam) {
		vectorParam->getParameter().setWithoutEventNotifications({value});
	}
	
	if(paramIndex >= 0 && paramIndex < 1024 && parameterInfoSlots[paramIndex] != nullptr) {
		parameterInfoSlots[paramIndex]->value = value;
	}
}

void scVST::setVSTParameter(int paramIndex, float value) {
	// Safety checks
	if(paramIndex < 0 || paramIndex >= 1024) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}
	
	if(activeInstanceTargets.empty()) {
		ofLogWarning("scVST") << "No VST instances available to set parameter";
		return;
	}

	suppressFeedbackFor(paramIndex, ofGetElapsedTimeMillis() + 50);
	
	// Mark parameter as recently updated
	parameterUpdateGeneration[paramIndex].store(ofGetElapsedTimeMillis(), std::memory_order_release);
	
	// Apply parameter change to ALL instances
	static thread_local ofxOscMessage setMsg;
	for(const auto& target : activeInstanceTargets) {
		try {
			setMsg.clear();
			setMsg.setAddress("/u_cmd");
			setMsg.addIntArg(target.synth->nodeID);
			setMsg.addIntArg(2);
			setMsg.addStringArg("/set");
			setMsg.addIntArg(paramIndex);
			setMsg.addFloatArg(value);
			target.server->sendMsg(setMsg);
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error setting parameter on synth " << target.synth->nodeID << ": " << e.what();
		}
	}
	
}

bool scVST::isMyVSTInstance(int nodeID) const {
	return ownedNodeIDs.count(nodeID) > 0;
}

void scVST::processGates(vector<int> &gates){
	// OPTIMIZATION: Cache parameter vectors to avoid repeated .get() calls
	const auto& currentPitch = pitch.get();
	const auto& currentVelocity = velocity.get();
	const auto& currentInstance = instance.get();

	// OPTIMIZATION: Cache sizes for bounds checking
	const size_t pitchSize = currentPitch.size();
	const size_t velocitySize = currentVelocity.size();
	const size_t instanceSize = currentInstance.size();
	const size_t gatesSize = gates.size();
	const int channelNum = midiChannel.get();

	// The source of truth for polyphony is the pitch vector size
	size_t numVoices = pitchSize;
	if (numVoices == 0) numVoices = 1; // Safety fallback

	// Resize our internal tracking vectors to match the polyphony count (Pitch Size)
	// We do NOT resize the 'gates' vector itself to preserve the GUI slider.
	if (previousGates.size() != numVoices) {
		previousGates.resize(numVoices, 0);
		activeNotes.resize(numVoices, -1);
	}

	// OPTIMIZATION: Pre-compute scalar/broadcast values
	const bool gatesBroadcast = (gatesSize == 1);
	const bool velocityBroadcast = (velocitySize == 1);
	const bool instanceBroadcast = (instanceSize == 1);
	const float scalarVelocity = velocitySize > 0 ? ofClamp(currentVelocity[0], 0.0f, 1.0f) : 0.5f;
	const int scalarInstance = instanceSize > 0 ? ofClamp(currentInstance[0], 0, 64) : 0;

	// Process each voice based on the Pitch vector size
	for (size_t i = 0; i < numVoices; i++) {

		// OPTIMIZATION: Determine the effective gate value for this voice
		int currentGate = 0;
		if (gatesBroadcast) {
			currentGate = gates[0];
		} else if (i < gatesSize) {
			currentGate = gates[i];
		}

		const int previousGate = previousGates[i];

		// Rising edge - gate went from 0 to 1
		if (currentGate == 1 && previousGate == 0) {
			// OPTIMIZATION: Get pitch for this index with minimal branching
			int noteNumber = 60;
			if (i < pitchSize) {
				noteNumber = ofClamp(currentPitch[i], 0, 127);
			} else if (pitchSize > 0) {
				noteNumber = ofClamp(currentPitch[0], 0, 127);
			}

			// OPTIMIZATION: Get velocity using pre-computed values
			float vel = velocityBroadcast ? scalarVelocity :
						(i < velocitySize ? ofClamp(currentVelocity[i], 0.0f, 1.0f) : scalarVelocity);

			// OPTIMIZATION: Get instance using pre-computed values
			int targetInstance = instanceBroadcast ? scalarInstance :
								(i < instanceSize ? ofClamp(currentInstance[i], 0, 64) : scalarInstance);

			int midiVelocity = static_cast<int>(vel * 127.0f);

			sendMidiNoteOn(channelNum, noteNumber, midiVelocity, targetInstance);
			activeNotes[i] = noteNumber;
		}

		// Falling edge - gate went from 1 to 0
		else if (currentGate == 0 && previousGate == 1) {
			if (activeNotes[i] >= 0) {
				// OPTIMIZATION: Get instance using pre-computed values
				int targetInstance = instanceBroadcast ? scalarInstance :
									(i < instanceSize ? ofClamp(currentInstance[i], 0, 64) : scalarInstance);

				sendMidiNoteOff(channelNum, activeNotes[i], targetInstance);
				activeNotes[i] = -1;
			}
		}

		// Update state for next frame
		previousGates[i] = currentGate;
	}
}

void scVST::sendMidiNoteOn(int channel, int pitch, int velocity, int instanceIndex) {
	// OPTIMIZATION: Early validation
	if(synthInstances.empty()) return;

	if(instanceIndex == 0) {
		// OPTIMIZATION: Route to all instances - cache iterator end
		for(auto& serverInstances : synthInstances){
			ofxSCServer* server = serverInstances.first;
			const auto& synths = serverInstances.second;
			for(auto synth : synths){
				if(synth != nullptr){
					sendMidiToInstance(server, synth, channel, 0x90, pitch, velocity);
				}
			}
		}
	} else if(instanceIndex > 0) {
		// OPTIMIZATION: Route to specific instance with early exit
		int currentInstance = 1;
		for(auto& serverInstances : synthInstances){
			ofxSCServer* server = serverInstances.first;
			const auto& synths = serverInstances.second;
			for(auto synth : synths){
				if(synth != nullptr){
					if(currentInstance == instanceIndex) {
						sendMidiToInstance(server, synth, channel, 0x90, pitch, velocity);
						return;
					}
					currentInstance++;
				}
			}
		}
		// Only log warning in verbose mode to reduce overhead
		ofLogVerbose("scVST") << "Instance " << instanceIndex << " not found for MIDI note on";
	}
}

void scVST::sendMidiNoteOff(int channel, int pitch, int instanceIndex) {
	// OPTIMIZATION: Early validation
	if(synthInstances.empty()) return;

	if(instanceIndex == 0) {
		// OPTIMIZATION: Route to all instances - cache iterator end
		for(auto& serverInstances : synthInstances){
			ofxSCServer* server = serverInstances.first;
			const auto& synths = serverInstances.second;
			for(auto synth : synths){
				if(synth != nullptr){
					sendMidiToInstance(server, synth, channel, 0x80, pitch, 0x40);
				}
			}
		}
	} else if(instanceIndex > 0) {
		// OPTIMIZATION: Route to specific instance with early exit
		int currentInstance = 1;
		for(auto& serverInstances : synthInstances){
			ofxSCServer* server = serverInstances.first;
			const auto& synths = serverInstances.second;
			for(auto synth : synths){
				if(synth != nullptr){
					if(currentInstance == instanceIndex) {
						sendMidiToInstance(server, synth, channel, 0x80, pitch, 0x40);
						return;
					}
					currentInstance++;
				}
			}
		}
		// Only log warning in verbose mode to reduce overhead
		ofLogVerbose("scVST") << "Instance " << instanceIndex << " not found for MIDI note off";
	}
}



void scVST::presetSave(ofJson &json) {
	string nodeKey = getParameterGroup().getName();
	//ofLogNotice("scVST") << "=== PRESET SAVE (WITH FXP DATA) for node '" << nodeKey << "' ===";
	
	// Create a node-specific section in the JSON
	if(!json.contains("vstNodes") || !json["vstNodes"].is_object()) {
		json["vstNodes"] = ofJson::object();
	}
	ofJson& nodeJson = json["vstNodes"][nodeKey];  // Namespace under vstNodes
	nodeJson = ofJson::object();
	
	// Save metadata
	nodeJson["currentPluginPath"] = currentPluginPath;
	nodeJson["enableMultithreading"] = enableMultithreading.get();
	nodeJson["monoInstancing"] = monoInstancing.get();
	nodeJson["transportPlay"] = transportPlay.get();
	nodeJson["transportPosition"] = transportPosition.get();
	nodeJson["tempo"] = tempo.get();
	nodeJson["timeSignatureNum"] = timeSignatureNum.get();
	nodeJson["timeSignatureDenom"] = timeSignatureDenom.get();
	nodeJson["pitchBend"] = pitchBend.get();
	nodeJson["modWheel"] = modWheel.get();
	
	// Save FXP data from first instance if available
	if(!synthInstances.empty()) {
		ofxSCSynth* firstInstance = nullptr;
		ofxSCServer* firstServer = nullptr;
		bool canCaptureFXP = true;
		
		// Find first instance
		for(auto& serverInstances : synthInstances) {
			if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
				firstInstance = serverInstances.second[0];
				firstServer = serverInstances.first;
				break;
			}
		}
		
			if(firstInstance && firstServer) {
				//ofLogNotice("scVST") << "Saving FXP preset from first instance (node " << firstInstance->nodeID << ") for '" << nodeKey << "'";
				bool hasCacheFallback = fxpCacheValid && !cachedFXP.empty();

				if(activeFXPWriteOperation != FXPWriteOperation::None ||
				   activeFXPReadOperation != FXPReadOperation::None ||
				   syncInProgress ||
				   isFXPLoading.load(std::memory_order_acquire)) {
					int busyWaitCount = 0;
					int busyMaxWait = hasCacheFallback ? 3 : 50;
					while((activeFXPWriteOperation != FXPWriteOperation::None ||
						   activeFXPReadOperation != FXPReadOperation::None ||
						   syncInProgress ||
						   isFXPLoading.load(std::memory_order_acquire)) &&
						  busyWaitCount < busyMaxWait) {
						firstServer->process();
						ofSleepMillis(100);
						busyWaitCount++;
					}

					if(activeFXPWriteOperation != FXPWriteOperation::None ||
					   activeFXPReadOperation != FXPReadOperation::None ||
					   syncInProgress ||
					   isFXPLoading.load(std::memory_order_acquire)) {
						if(hasCacheFallback) {
							ofLogWarning("scVST") << "Preset save falling back to cached FXP for '"
												  << nodeKey << "' because another FXP operation is still active";
						} else {
							ofLogError("scVST") << "Timeout waiting for previous FXP operation before preset save for '" << nodeKey << "'";
						}
						canCaptureFXP = false;
					}
				}
				
				if(canCaptureFXP) {
					// Create temporary file for FXP data
					tempFXPPath = createTempFXPPath();
					waitingForFXPSave = true;
					lastFXPWriteSucceeded = false;
					activeFXPWriteOperation = FXPWriteOperation::PresetSave;
					activeFXPWriteNodeID = firstInstance->nodeID;
					activeFXPWritePath = tempFXPPath;
					activeFXPWriteNodeKey = nodeKey;
					activeFXPWriteStartTime = ofGetElapsedTimeMillis();
					
					// Request VST to write preset to file
					ofxOscMessage writeMsg;
					writeMsg.setAddress("/u_cmd");
					writeMsg.addIntArg(firstInstance->nodeID);
					writeMsg.addIntArg(2);
					writeMsg.addStringArg("/program_write");
					writeMsg.addStringArg(tempFXPPath);
					writeMsg.addIntArg(1); // async = true
					firstServer->sendMsg(writeMsg);
					
					// Wait for write completion with timeout
					int maxWait = 50; // 5 seconds
					int waitCount = 0;
					
					while(waitingForFXPSave && waitCount < maxWait) {
						firstServer->process();
						ofSleepMillis(100);
						waitCount++;
					}
					
					if(!waitingForFXPSave && lastFXPWriteSucceeded) {
						// Successfully saved, read the file into memory
						try {
							std::ifstream file(tempFXPPath, std::ios::binary | std::ios::ate);
							if(file.is_open()) {
								std::streamsize size = file.tellg();
								file.seekg(0, std::ios::beg);
								
								std::vector<uint8_t> buffer(size);
								if(file.read(reinterpret_cast<char*>(buffer.data()), size)) {
									// Store as base64 encoded string in JSON
									string base64Data = base64Encode(buffer);
									nodeJson["fxpData"] = base64Data;
									nodeJson["fxpDataSize"] = size;
									
									// Store in memory for later use
									savedFXPData = buffer;
									hasSavedFXPData = true;
								} else {
									ofLogError("scVST") << "Error reading FXP file content for '" << nodeKey << "'";
								}
								file.close();
							} else {
								ofLogError("scVST") << "FXP file not found after write: " << tempFXPPath << " for '" << nodeKey << "'";
							}
						} catch(const std::exception& e) {
							ofLogError("scVST") << "Error reading FXP file for '" << nodeKey << "': " << e.what();
						}
						
						// Clean up temp file
						cleanupTempFXPFile();
					} else {
						if(waitingForFXPSave) {
							ofLogError("scVST") << "Timeout waiting for FXP save for '" << nodeKey << "'";
							waitingForFXPSave = false;
							clearActiveFXPWriteState();
						} else {
							ofLogError("scVST") << "FXP save failed for '" << nodeKey << "'";
						}

						cleanupTempFXPFile();
						canCaptureFXP = false;
					}
				}

				if(!canCaptureFXP && hasCacheFallback) {
					string base64Data = base64Encode(cachedFXP);
					nodeJson["fxpData"] = base64Data;
					nodeJson["fxpDataSize"] = cachedFXP.size();
					savedFXPData = cachedFXP;
					hasSavedFXPData = true;
					ofLogWarning("scVST") << "Using cached FXP fallback while saving preset for '" << nodeKey << "'";
				}
		}
	}
	
	// Save GUI parameter structure and values
	if(!dynamicVectorParameters.empty() || !dynamicParameters.empty()) {
		nodeJson["vstParameters"] = ofJson::object();
		
		// Save vector parameters with enhanced metadata
		for(auto& param : dynamicVectorParameters) {
			int paramIndex = param.first;
			try {
				ofJson paramData;
				paramData["index"] = paramIndex;
				paramData["isVector"] = true;
				paramData["vectorValue"] = param.second->getParameter().get();
				
				// Save display name, original name, and registered name
				if(parameterInfoMap.count(paramIndex) > 0) {
					paramData["name"] = parameterInfoMap[paramIndex].displayName;
					paramData["originalName"] = parameterInfoMap[paramIndex].originalName;
					paramData["registeredName"] = parameterInfoMap[paramIndex].registeredName;
					paramData["isConnected"] = parameterInfoMap[paramIndex].isConnected;
					paramData["isPersistent"] = parameterInfoMap[paramIndex].isPersistent;
				} else {
					paramData["name"] = "Param" + ofToString(paramIndex);
					paramData["originalName"] = "Param" + ofToString(paramIndex);
					paramData["registeredName"] = "Param" + ofToString(paramIndex);
					paramData["isConnected"] = false;
					paramData["isPersistent"] = true;
				}
				
				// Check if parameter has external connections
				if(param.second->getInConnection()) {
					paramData["hasExternalConnection"] = true;
				}
				
				if(!paramData["vectorValue"].empty()) {
					paramData["value"] = paramData["vectorValue"][0];
				}
				
				nodeJson["vstParameters"][ofToString(paramIndex)] = paramData;
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error saving vector parameter " << paramIndex << " for '" << nodeKey << "': " << e.what();
			}
		}
		
		// Save scalar parameters (legacy support)
		for(auto& param : dynamicParameters) {
			int paramIndex = param.first;
			if(dynamicVectorParameters.count(paramIndex) == 0) {
				try {
					ofJson paramData;
					paramData["index"] = paramIndex;
					paramData["isVector"] = false;
					paramData["value"] = param.second->getParameter().get();
					
					if(parameterInfoMap.count(paramIndex) > 0) {
						paramData["name"] = parameterInfoMap[paramIndex].displayName;
					} else {
						paramData["name"] = "Param" + ofToString(paramIndex);
					}
					
					nodeJson["vstParameters"][ofToString(paramIndex)] = paramData;
					
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error saving scalar parameter " << paramIndex << " for '" << nodeKey << "': " << e.what();
				}
			}
		}
		
		nodeJson["vstDataType"] = "fxp_plus_gui_parameters";
	} else {
		nodeJson["vstDataType"] = "fxp_only";
	}
	
	if(!midiCCParameters.empty()) {
		nodeJson["midiCCParameters"] = ofJson::object();
		
		for(auto& ccParam : midiCCParameters) {
			int ccNumber = ccParam.first;
			try {
				ofJson ccData;
				ccData["ccNumber"] = ccNumber;
				ccData["displayName"] = ccParam.second.displayName;
				ccData["enabled"] = ccParam.second.enabled;
				
				if(dynamicMidiCCParameters.count(ccNumber) > 0) {
					ccData["value"] = dynamicMidiCCParameters[ccNumber]->getParameter().get();
				} else {
					ccData["value"] = ccParam.second.value;
				}
				
				nodeJson["midiCCParameters"][ofToString(ccNumber)] = ccData;
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error saving MIDI CC parameter " << ccNumber << ": " << e.what();
			}
		}
	}
	
	//ofLogNotice("scVST") << "Finished preset save for node '" << nodeKey << "'";
}

void scVST::loadBeforeConnections(ofJson &json) {
	string nodeKey = getParameterGroup().getName();
	//ofLogNotice("scVST") << "=== LOAD BEFORE CONNECTIONS (CREATE GUI PARAMETERS) for node '" << nodeKey << "' ===";
	
	// Check if we have node-specific data
	if(!json.contains("vstNodes") || !json["vstNodes"].contains(nodeKey)) {
		ofLogWarning("scVST") << "❌ No VST data found for node '" << nodeKey << "' in preset JSON!";
		isPresetLoading = false;
		return;
	}
	
	ofJson& nodeJson = json["vstNodes"][nodeKey];
	
	// FIRST: Set the loading flag to prevent plugin selector from triggering
	isPresetLoading = true;
	//ofLogNotice("scVST") << "🔒 Set isPresetLoading = TRUE to prevent plugin selector triggering for '" << nodeKey << "'";
	
	// Deserialize basic parameters first
	deserializeParameter(nodeJson, enableMultithreading);
	deserializeParameter(nodeJson, monoInstancing);
	deserializeParameter(nodeJson, transportPlay);
	deserializeParameter(nodeJson, transportPosition);
	deserializeParameter(nodeJson, tempo);
	deserializeParameter(nodeJson, timeSignatureNum);
	deserializeParameter(nodeJson, timeSignatureDenom);
	deserializeParameter(nodeJson, pitchBend);
	deserializeParameter(nodeJson, modWheel);
	
	// NEW: Backward compatibility - only deserialize mix if it exists in the preset
	// Old presets won't have this parameter, so we skip it to avoid crashes
	if(nodeJson.contains("Mix") && !nodeJson["Mix"].is_null()) {
		try {
			deserializeParameter(nodeJson, mix);
			// Validate the deserialized value
			float mixValue = mix.get();
			if(std::isnan(mixValue) || std::isinf(mixValue) || mixValue < 0.0f || mixValue > 1.0f) {
				ofLogWarning("scVST") << "Invalid mix value detected (" << mixValue << "), resetting to 1.0";
				mix.setWithoutEventNotifications(1.0f);
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error deserializing mix parameter: " << e.what() << " - using default";
			mix.setWithoutEventNotifications(1.0f);
		}
	} else {
		// Set default value for old presets (100% wet)
		mix.setWithoutEventNotifications(1.0f);
		ofLogNotice("scVST") << "Old preset detected - setting mix to default value (1.0)";
	}
	
	if(nodeJson.contains("currentPluginPath") && !nodeJson["currentPluginPath"].is_null()) {
		string savedPluginPath = static_cast<string>(nodeJson["currentPluginPath"]);
		
		ofLogNotice("scVST") << "🔍 Preset contains plugin path: " << savedPluginPath;
		
		// CRITICAL: Set currentPluginPath FIRST - this is the authoritative source from the preset
		currentPluginPath = savedPluginPath;
		
		// Now find the correct index for GUI display
		int foundIndex = -1;
		
		// Normalize the saved path for comparison (remove trailing slashes, normalize separators)
		string normalizedSavedPath = savedPluginPath;
		while(!normalizedSavedPath.empty() && (normalizedSavedPath.back() == '/' || normalizedSavedPath.back() == '\\')) {
			normalizedSavedPath.pop_back();
		}
		
		// First try: exact path match (with normalization)
		for(int i = 0; i < pluginPaths.size(); i++) {
			string normalizedCurrentPath = pluginPaths[i];
			while(!normalizedCurrentPath.empty() && (normalizedCurrentPath.back() == '/' || normalizedCurrentPath.back() == '\\')) {
				normalizedCurrentPath.pop_back();
			}
			
			if(normalizedCurrentPath == normalizedSavedPath) {
				foundIndex = i;
				currentPluginPath = pluginPaths[i]; // Use the exact path from the list
				ofLogNotice("scVST") << "✅ Found exact plugin path match at index " << i;
				break;
			}
		}
		
		// Second try: case-insensitive path match
		if(foundIndex < 0) {
			string lowerSavedPath = ofToLower(normalizedSavedPath);
			for(int i = 0; i < pluginPaths.size(); i++) {
				string normalizedCurrentPath = pluginPaths[i];
				while(!normalizedCurrentPath.empty() && (normalizedCurrentPath.back() == '/' || normalizedCurrentPath.back() == '\\')) {
					normalizedCurrentPath.pop_back();
				}
				
				if(ofToLower(normalizedCurrentPath) == lowerSavedPath) {
					foundIndex = i;
					currentPluginPath = pluginPaths[i];
					ofLogNotice("scVST") << "✅ Found case-insensitive path match at index " << i;
					break;
				}
			}
		}
		
		// Third try: match by filename only (plugin may have moved to different folder)
		if(foundIndex < 0) {
			string savedPluginName = ofFilePath::getBaseName(savedPluginPath);
			string savedPluginExt = ofToLower(ofFilePath::getFileExt(savedPluginPath));
			
			ofLogNotice("scVST") << "⚠️ Path not found, searching by filename: " << savedPluginName << " (ext: " << savedPluginExt << ")";
			
			// First try to match with same extension
			for(int i = 0; i < pluginPaths.size(); i++) {
				string currentPluginName = ofFilePath::getBaseName(pluginPaths[i]);
				string currentPluginExt = ofToLower(ofFilePath::getFileExt(pluginPaths[i]));
				
				if(currentPluginName == savedPluginName && currentPluginExt == savedPluginExt) {
					foundIndex = i;
					currentPluginPath = pluginPaths[i];
					ofLogWarning("scVST") << "⚠️ Plugin found at different location:";
					ofLogWarning("scVST") << "   Old: " << savedPluginPath;
					ofLogWarning("scVST") << "   New: " << pluginPaths[i];
					break;
				}
			}
		}
		
		// Fourth try: match by filename, any extension (handles .vst vs .vst3)
		if(foundIndex < 0) {
			string savedPluginName = ofFilePath::getBaseName(savedPluginPath);
			
			for(int i = 0; i < pluginPaths.size(); i++) {
				string currentPluginName = ofFilePath::getBaseName(pluginPaths[i]);
				
				if(ofToLower(currentPluginName) == ofToLower(savedPluginName)) {
					foundIndex = i;
					currentPluginPath = pluginPaths[i];
					ofLogWarning("scVST") << "⚠️ Plugin found with different extension:";
					ofLogWarning("scVST") << "   Old: " << savedPluginPath;
					ofLogWarning("scVST") << "   New: " << pluginPaths[i];
					break;
				}
			}
		}
		
		// Fifth try: match by display name in availablePlugins (handles subfolder prefixes)
		if(foundIndex < 0) {
			string savedPluginName = ofFilePath::getBaseName(savedPluginPath);
			
			ofLogNotice("scVST") << "⚠️ Trying display name match for: " << savedPluginName;
			
			for(int i = 0; i < availablePlugins.size(); i++) {
				// availablePlugins might have format "Folder/PluginName" or just "PluginName"
				string displayName = availablePlugins[i];
				
				// Check if display name ends with the saved plugin name
				size_t slashPos = displayName.rfind('/');
				string pluginNameOnly = (slashPos != string::npos) ? displayName.substr(slashPos + 1) : displayName;
				
				if(ofToLower(pluginNameOnly) == ofToLower(savedPluginName)) {
					foundIndex = i;
					currentPluginPath = pluginPaths[i];
					ofLogWarning("scVST") << "⚠️ Plugin found via display name match:";
					ofLogWarning("scVST") << "   Looking for: " << savedPluginName;
					ofLogWarning("scVST") << "   Found: " << displayName << " -> " << pluginPaths[i];
					break;
				}
			}
		}
		
		// Update the selector index for GUI display
		if(foundIndex >= 0) {
			pluginSelector.setWithoutEventNotifications(foundIndex);
			ofLogNotice("scVST") << "✅ Plugin selector set to index " << foundIndex;
			ofLogNotice("scVST") << "   Display name: " << (foundIndex < availablePlugins.size() ? availablePlugins[foundIndex] : "N/A");
			ofLogNotice("scVST") << "   Path: " << currentPluginPath;
		} else {
			ofLogError("scVST") << "❌ Could not find saved plugin in current plugin list!";
			ofLogError("scVST") << "   Saved path: " << savedPluginPath;
			ofLogError("scVST") << "   Available plugins:";
			for(int i = 0; i < std::min((int)availablePlugins.size(), 10); i++) {
				ofLogError("scVST") << "     [" << i << "] " << availablePlugins[i] << " -> " << pluginPaths[i];
			}
			if(availablePlugins.size() > 10) {
				ofLogError("scVST") << "     ... and " << (availablePlugins.size() - 10) << " more";
			}
			
			// Keep currentPluginPath as savedPluginPath - might still work
			// Set selector to 0 to avoid undefined state
			if(!pluginPaths.empty()) {
				pluginSelector.setWithoutEventNotifications(0);
				ofLogWarning("scVST") << "⚠️ Selector defaulted to index 0, but will load: " << currentPluginPath;
			}
		}
		
		ofLogNotice("scVST") << "📝 Final state:";
		ofLogNotice("scVST") << "   currentPluginPath: " << currentPluginPath;
		ofLogNotice("scVST") << "   pluginSelector: " << pluginSelector.get();
	}
	
	// Load FXP data if available (but don't apply yet)
	if(nodeJson.contains("fxpData") && !nodeJson["fxpData"].is_null()) {
		try {
			string base64Data = nodeJson["fxpData"];
			savedFXPData = base64Decode(base64Data);
			hasSavedFXPData = true;
			
			//ofLogNotice("scVST") << "Loaded FXP data for later application (" << savedFXPData.size() << " bytes) for '" << nodeKey << "'";
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error loading FXP data for '" << nodeKey << "': " << e.what();
			hasSavedFXPData = false;
		}
	} else {
		hasSavedFXPData = false;
	}
	
	// Debug the JSON structure
	if(nodeJson.contains("vstParameters") && !nodeJson["vstParameters"].is_null()) {
		//ofLogNotice("scVST") << "📄 JSON contains " << nodeJson["vstParameters"].size() << " parameters to restore for '" << nodeKey << "'";
		
		// Create GUI parameters immediately so connections can be restored
		//ofLogNotice("scVST") << "🔧 Creating GUI parameters for connection restoration for '" << nodeKey << "'";
		
		// FIX: Only clear parameters if we're loading a different plugin
		// Don't clear if parameters were already created for this preset
		bool shouldClearParameters = false;
		
		// Check if we have a plugin mismatch
		if(!currentPluginPath.empty() && nodeJson.contains("currentPluginPath")) {
			string savedPluginPath = nodeJson["currentPluginPath"];
			if(savedPluginPath != currentPluginPath) {
				shouldClearParameters = true;
				ofLogWarning("scVST") << "⚠️ Plugin mismatch detected, clearing existing parameters";
			}
		}
		
		// Only clear if necessary
		if(shouldClearParameters && (!dynamicParameters.empty() || !dynamicVectorParameters.empty() || !parameterInfoMap.empty())) {
			ofLogWarning("scVST") << "⚠️ Clearing existing parameters before loading new plugin parameters";
			removeAllDynamicParametersForce();
			clearParameterMaps();
		}
		
		int createdCount = 0;
		int skippedCount = 0;
		
		for(auto& item : nodeJson["vstParameters"].items()) {
			try {
				int paramIndex = ofToInt(item.key());
				if(item.value().is_object()) {
					
					// Skip if parameter already exists
					if(dynamicVectorParameters.count(paramIndex) > 0 ||
					   dynamicParameters.count(paramIndex) > 0) {
						skippedCount++;
						//ofLogVerbose("scVST") << "⏭️ Skipping existing parameter " << paramIndex << " for '" << nodeKey << "'";
						continue;
					}
					
					// Extract parameter info from preset with enhanced metadata
					string paramName = "Param" + ofToString(paramIndex);
					string originalName = paramName;
					string registeredName = "";  // Will be set when parameter is created
					bool isConnected = false;
					bool isPersistent = true;
					
					if(item.value().contains("name") && !item.value()["name"].is_null()) {
						paramName = item.value()["name"];
					}
					
					if(item.value().contains("originalName") && !item.value()["originalName"].is_null()) {
						originalName = item.value()["originalName"];
					}
					
					// Load the registered name if available (for proper removal later)
					if(item.value().contains("registeredName") && !item.value()["registeredName"].is_null()) {
						registeredName = item.value()["registeredName"];
					}
					
					if(item.value().contains("isConnected")) {
						isConnected = static_cast<bool>(item.value()["isConnected"]);
					}
					
					if(item.value().contains("isPersistent")) {
						isPersistent = static_cast<bool>(item.value()["isPersistent"]);
					}
					
					vector<float> initialValues = {0.0f};
					if(item.value().contains("isVector") && static_cast<bool>(item.value()["isVector"])) {
						if(item.value().contains("vectorValue") && !item.value()["vectorValue"].empty()) {
							auto jsonArray = item.value()["vectorValue"];
							initialValues.clear();
							for(auto& val : jsonArray) {
								initialValues.push_back(static_cast<float>(val));
							}
						}
					} else if(item.value().contains("value")) {
						initialValues = {static_cast<float>(item.value()["value"])};
					}
					
					// Create/update parameter info BEFORE creating GUI parameter
					VSTParameterInfo info;
					info.index = paramIndex;
					info.displayName = paramName;
					info.originalName = originalName;
					info.registeredName = registeredName;
					info.value = initialValues[0];
					info.isConnected = isConnected;
					info.isPersistent = isPersistent;
					parameterInfoMap[paramIndex] = info;
					cacheParameterInfoSlot(paramIndex);
					
					// Store the saved name for later restoration if needed
					savedParameterNames[paramIndex] = paramName;
					cacheSavedParameterNameSlot(paramIndex, paramName);
					
					// Create GUI parameter with the loaded values
					createGUIParameterWithValues(paramIndex, paramName, initialValues);
					
					// Verify it was created
					if(dynamicVectorParameters.count(paramIndex) > 0) {
						createdCount++;
						//ofLogNotice("scVST") << "✅ Successfully created parameter " << paramIndex << " for '" << nodeKey << "'";
					} else {
						ofLogError("scVST") << "❌ Failed to create parameter " << paramIndex << " for '" << nodeKey << "'";
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error creating GUI parameter " << item.key() << " for '" << nodeKey << "': " << e.what();
			}
		}
		
		//ofLogNotice("scVST") << "✅ Created " << createdCount << " GUI parameters (" << skippedCount << " skipped) for '" << nodeKey << "'";
	} else {
		ofLogWarning("scVST") << "❌ No vstParameters found in JSON for node '" << nodeKey << "'!";
	}
	
	// Add to loadBeforeConnections() method after creating VST parameters:
	
	// Load MIDI CC parameters if available
	if(nodeJson.contains("midiCCParameters") && !nodeJson["midiCCParameters"].is_null()) {
		ofLogNotice("scVST") << "📄 JSON contains " << nodeJson["midiCCParameters"].size()
		<< " MIDI CC parameters to restore for '" << nodeKey << "'";
		
		int createdCCCount = 0;
		
		for(auto& item : nodeJson["midiCCParameters"].items()) {
			try {
				int ccNumber = ofToInt(item.key());
				if(item.value().is_object() && ccNumber >= 1 && ccNumber <= 127) {
					
					// Skip if CC parameter already exists
					if(midiCCParameters.count(ccNumber) > 0) {
						ofLogVerbose("scVST") << "⏭️ Skipping existing MIDI CC " << ccNumber;
						continue;
					}
					
					// Extract CC parameter info
					string displayName = "CC" + ofToString(ccNumber);
					if(item.value().contains("displayName") && !item.value()["displayName"].is_null()) {
						displayName = item.value()["displayName"];
					}
					
					float value = 0.0f;
					if(item.value().contains("value")) {
						value = static_cast<float>(item.value()["value"]);
					}
					
					// Create MIDI CC parameter
					addMidiCCParameter(ccNumber);
					
					// Set the loaded value
					if(dynamicMidiCCParameters.count(ccNumber) > 0) {
						dynamicMidiCCParameters[ccNumber]->getParameter().setWithoutEventNotifications(value);
					}
					
					// Update display name
					if(midiCCParameters.count(ccNumber) > 0) {
						midiCCParameters[ccNumber].displayName = displayName;
						midiCCParameters[ccNumber].value = value;
					}
					
					createdCCCount++;
					ofLogNotice("scVST") << "✅ Successfully restored MIDI CC " << ccNumber
					<< " (" << displayName << ") = " << value;
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error restoring MIDI CC parameter " << item.key() << ": " << e.what();
			}
		}
		
		ofLogNotice("scVST") << "✅ Restored " << createdCCCount << " MIDI CC parameters for '" << nodeKey << "'";
	}
	
	// Store the full preset data for later VST synchronization
	pendingPresetData = nodeJson; // Store only this node's data
	hasPendingPresetData = true;
	fxpAppliedInstances.clear();
	
	//ofLogNotice("scVST") << "Finished load before connections for node '" << nodeKey << "'";
}

void scVST::presetRecallAfterSettingParameters(ofJson &json) {
	string nodeKey = getParameterGroup().getName();
	ofLogNotice("scVST") << "=== PRESET RECALL AFTER SETTING PARAMETERS for node '" << nodeKey << "' ===";
	ofLogNotice("scVST") << "currentPluginPath = " << currentPluginPath;
	
	// Verify currentPluginPath is valid before loading
	if(currentPluginPath.empty()) {
		ofLogError("scVST") << "❌ No plugin path set - cannot load plugin!";
		isPresetLoading = false;
		return;
	}
	
	// Check if the plugin file actually exists
	ofFile pluginFile(currentPluginPath);
	if(!pluginFile.exists()) {
		ofLogError("scVST") << "❌ Plugin file does not exist: " << currentPluginPath;
		ofLogError("scVST") << "   The plugin may have been moved or uninstalled.";
		// Still try to proceed - maybe it's a bundle that exists as a directory
	}
	
	// Load the plugin now (isPresetLoading is still true, so it will preserve parameters)
	ofLogNotice("scVST") << "🔄 Loading plugin: " << currentPluginPath;
	loadSelectedPlugin();
	setupParameterTimer(5000);
}

void scVST::setupParameterTimer(int delayMs) {
	// Now this timer is just a backup timeout protection
	int timeoutDelay = delayMs; // Give more time since we're primarily waiting for the event
	if(timeoutDelay < 1000) timeoutDelay = 1000;   // Minimum 2s timeout
	if(timeoutDelay > 5000) timeoutDelay = 5000; // Maximum 10s timeout
	
	parameterTimerStart = ofGetElapsedTimeMillis();
	parameterTimerDelay = timeoutDelay;
	parameterTimerActive = true;
	
	//ofLogNotice("scVST") << "Set up parameter timeout protection for " << timeoutDelay << "ms (will wait for VST open confirmation)";
}

void scVST::createGUIParameterWithValues(int paramIndex, const string& paramName, const vector<float>& values) {
	// FIX: Validate parameter name to prevent empty names
	string validParamName = paramName;
	if(validParamName.empty()) {
		validParamName = "Param" + ofToString(paramIndex);
		ofLogWarning("scVST") << "Empty parameter name detected for index " << paramIndex << ", using: " << validParamName;
	}
	
	// FIX: Check if parameter already exists with this index
	if((paramIndex >= 0 && paramIndex < 1024 &&
		(dynamicVectorParameterSlots[paramIndex] || dynamicScalarParameterSlots[paramIndex])) ||
	   dynamicVectorParameters.count(paramIndex) > 0 || dynamicParameters.count(paramIndex) > 0) {
		ofLogWarning("scVST") << "Parameter " << paramIndex << " already exists, skipping creation";
		return;
	}
	
	// FIX: Remove any existing parameter with the same name to prevent duplicates
	string uniqueParamName = validParamName;
	if(getParameterGroup().contains(uniqueParamName)) {
		ofLogWarning("scVST") << "Parameter name '" << uniqueParamName << "' already exists in group";
		// Try to find and remove the existing parameter
		bool removed = false;
		for(int i = getParameterGroup().size() - 1; i >= 0; i--) {
			if(getParameterGroup().get(i).getName() == uniqueParamName) {
				try {
					ofLogWarning("scVST") << "Attempting to remove existing parameter: " << uniqueParamName;
					removeParameter(uniqueParamName);
					if(!getParameterGroup().contains(uniqueParamName)) {
						ofLogNotice("scVST") << "Successfully removed existing parameter: " << uniqueParamName;
						removed = true;
					}
					break;
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Failed to remove existing parameter: " << e.what();
				}
			}
		}
		
		if(!removed) {
			// If removal fails, we need to use a different name
			int nameCounter = 1;
			while(getParameterGroup().contains(uniqueParamName)) {
				uniqueParamName = validParamName + "_" + ofToString(nameCounter);
				nameCounter++;
				if(nameCounter > 10) {
					ofLogError("scVST") << "Too many duplicate parameters with name: " << validParamName;
					return;
				}
			}
			ofLogWarning("scVST") << "Could not remove existing parameter, using alternative name: " << uniqueParamName;
		}
	}
	
	// Create VECTOR parameter with the loaded values
	auto newParam = std::make_shared<ofParameter<vector<float>>>();
	newParam->set(uniqueParamName, values, {0.0f}, {1.0f});
	
	// Store the parameter to keep it alive BEFORE adding to GUI
	dynamicVectorFloatParameters[paramIndex] = newParam;
	
	// CRITICAL: Store the actual registered name in parameterInfoMap
	// This MUST be done whether the parameter info exists or not
	if(parameterInfoMap.count(paramIndex) == 0) {
		// Create parameter info if it doesn't exist
		VSTParameterInfo info;
		info.index = paramIndex;
		info.displayName = validParamName;
		info.originalName = validParamName;
		info.registeredName = uniqueParamName;
		info.value = values.empty() ? 0.0f : values[0];
		parameterInfoMap[paramIndex] = info;
		cacheParameterInfoSlot(paramIndex);
		ofLogNotice("scVST") << "Created parameter info for " << paramIndex
		<< " with registered name '" << uniqueParamName << "'";
	} else {
		// Update the registered name for existing parameter info
		parameterInfoMap[paramIndex].registeredName = uniqueParamName;
		ofLogNotice("scVST") << "Updated registered name to '" << uniqueParamName
		<< "' for parameter " << paramIndex
		<< " (display name: " << parameterInfoMap[paramIndex].displayName << ")";
	}
	cacheParameterInfoSlot(paramIndex);
	
	try {
		auto oceanodeParam = addParameter(*newParam);
		dynamicVectorParameters[paramIndex] = oceanodeParam;
		cacheDynamicVectorParameterSlot(paramIndex, oceanodeParam);
		
		/*
		 ofLogVerbose("scVST") << "Created GUI parameter " << uniqueParamName
		 << " (index " << paramIndex << ") with " << values.size() << " values";
		 */
		
		// Listen on the registered Oceanode parameter so restored connections
		// keep driving the VST after preset reload.
		listeners.push(oceanodeParam->getParameter().newListener([this, paramIndex](vector<float> &values) -> void {
			try {
				if(this == nullptr) return;
				
				// Always update parameter info regardless of state
				if(parameterInfoMap.count(paramIndex) > 0 && !values.empty()) {
					parameterInfoMap[paramIndex].value = values[0];
				}
				
				// Only send to VST if:
				// 1. Not during preset loading
				// 2. We have VST instances
				// 3. VST instances are ready
				if(!isPresetLoading &&
				   !synthInstances.empty() &&
				   areAllInstancesReady() &&
				   parameterInfoMap.count(paramIndex) > 0) {
					
					/*
					 ofLogNotice("scVST") << "🎛️ GUI parameter " << paramIndex
					 << " changed by user - sending to VST ("
					 << values.size() << " values)";
					 */
					handleDynamicParameterChange(paramIndex, values);
					
				} else {
					// Log why we're not sending
					/*
					 if(isPresetLoading) {
					 ofLogVerbose("scVST") << "⏳ GUI parameter " << paramIndex
					 << " changed during preset loading - not sending to VST yet";
					 } else if(synthInstances.empty()) {
					 ofLogVerbose("scVST") << "❌ GUI parameter " << paramIndex
					 << " changed but no VST instances available";
					 } else if(!areAllInstancesReady()) {
					 ofLogVerbose("scVST") << "⏳ GUI parameter " << paramIndex
					 << " changed but VST instances not ready yet";
					 }
					 */
				}
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in parameter listener for index " << paramIndex << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in parameter listener for index " << paramIndex;
			}
		}));
		
		// Add name editor and removal button
		addParameterNameEditor(paramIndex, uniqueParamName);
		addParameterRemovalButton(paramIndex, uniqueParamName);
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error adding GUI parameter: " << e.what();
		dynamicVectorFloatParameters.erase(paramIndex);
		dynamicVectorParameters.erase(paramIndex);
		clearDynamicParameterSlots(paramIndex);
	}
}

void scVST::addParameterNameEditor(int paramIndex, const string& paramName) {
	string nameEditorName = paramName + "_Name";
	string uniqueNameEditorName = nameEditorName;
	int nameEditorCounter = 1;
	while(getInspectorParameterGroup().contains(uniqueNameEditorName)) {
		uniqueNameEditorName = nameEditorName + "_" + ofToString(nameEditorCounter);
		nameEditorCounter++;
	}
	
	auto nameEditor = std::make_shared<ofParameter<string>>();
	nameEditor->set(uniqueNameEditorName, parameterInfoMap[paramIndex].displayName);
	
	dynamicStringParameters[paramIndex] = nameEditor;
	addInspectorParameter(*nameEditor);
	
	// Set up name change listener (simplified)
	listeners.push(nameEditor->newListener([this, paramIndex](string &newName) -> void {
		if(this == nullptr) return;
		
		if(!newName.empty() && parameterInfoMap.count(paramIndex) > 0) {
			parameterInfoMap[paramIndex].displayName = newName;
			//ofLogNotice("scVST") << "Renamed parameter " << paramIndex << " to '" << newName << "'";
		}
	}));
}

void scVST::addParameterRemovalButton(int paramIndex, const string& paramName) {
	string removeButtonName = "Remove " + paramName;
	string uniqueRemoveButtonName = removeButtonName;
	int removeNameCounter = 1;
	while(getInspectorParameterGroup().contains(uniqueRemoveButtonName)) {
		uniqueRemoveButtonName = removeButtonName + "_" + ofToString(removeNameCounter);
		removeNameCounter++;
	}
	
	auto removeButton = std::make_shared<ofParameter<void>>();
	removeButton->set(uniqueRemoveButtonName);
	
	dynamicRemovalButtons[paramIndex] = removeButton;
	addInspectorParameter(*removeButton);
	
	listeners.push(removeButton->newListener([this, paramIndex](){
		//ofLogNotice("scVST") << "Removing parameter " << paramIndex << " via individual button";
		removeParameterFromGUI(paramIndex);
	}));
}

void scVST::applyPendingPresetData() {
	if(!hasPendingPresetData) return;
	
	//ofLogNotice("scVST") << "=== APPLYING PENDING PRESET DATA (SYNC TO VST) ===";
	
	// Check if all VST instances are ready
	if(!areAllInstancesReady()) {
		//ofLogNotice("scVST") << "Not all VST instances ready yet, will wait for VST open confirmation";
		return;
	}
	
	//ofLogNotice("scVST") << "All VST instances ready, syncing GUI parameters to VST and activating bindings";
	
	ofJson json = pendingPresetData;
	hasPendingPresetData = false;
	pendingPresetData.clear();
	
	// Stop the parameter timer since we're applying now
	parameterTimerActive = false;
	
	// Wait a bit more to ensure all FXP data has been applied
	//ofSleepMillis(1000);
	
	// Phase 1: Send current GUI parameter values to VST (while listeners are still disabled)
	if(json.contains("vstParameters")) {
		syncGUIParametersToVST(json);
	}
	
	// Phase 2: Wait a bit for all VST messages to be processed
	for(int i = 0; i < 10; i++) {
		for(auto& serverInstances : synthInstances) {
			serverInstances.first->process();
		}
		ofSleepMillis(50);
	}
	
	// Phase 3: Activate parameter bindings for future changes
	activateParameterBindings();
	
	// Phase 4: Final verification - trigger presetHasLoaded to ensure state is consistent
	presetHasLoaded();
	
	//ofLogNotice("scVST") << "🎉 Preset restoration complete - GUI parameters now bound to VST";
}


void scVST::activateParameterBindings() {
	//ofLogNotice("scVST") << "🔗 Activating GUI parameter bindings to VST";
	
	// CRITICAL: Now we can finally set both preset loading flags to false
	isPresetLoading = false;
	oceanodePresetLoading = false;  // NEW: Also clear Oceanode preset loading flag
	resetVSTModificationTracking(); // NEW: Reset modification tracking after successful VST sync
	//ofLogNotice("scVST") << "🔓 Set both preset loading flags = FALSE, reset VST modification tracking - parameter listeners now active";
	
	// Small delay to ensure the flag change is processed
	ofSleepMillis(100);
	
	// Test the bindings
	int testParameterIndex = -1;
	for(auto& param : dynamicVectorParameters) {
		testParameterIndex = param.first;
		break;
	}
	
	if(testParameterIndex >= 0) {
		//ofLogNotice("scVST") << "🧪 Testing parameter binding on parameter " << testParameterIndex;
		
		auto currentValues = dynamicVectorParameters[testParameterIndex]->getParameter().get();
		if(!currentValues.empty()) {
			float testValue = currentValues[0];
			
			// Trigger the listener
			dynamicVectorParameters[testParameterIndex]->getParameter().set(currentValues);
			/*
			 ofLogNotice("scVST") << "🧪 Triggered test change on parameter " << testParameterIndex
			 << " with value " << testValue;
			 */
		}
	}
	
	// Log final status
	int boundParameters = 0;
	for(auto& param : dynamicVectorParameters) {
		int paramIndex = param.first;
		if(parameterInfoMap.count(paramIndex) > 0) {
			vector<float> currentValues = param.second->getParameter().get();
			ofLogVerbose("scVST") << "Parameter " << paramIndex << " bound with "
			<< currentValues.size() << " values";
			boundParameters++;
		}
	}
	
	//ofLogNotice("scVST") << "✅ " << boundParameters << " GUI parameters now bound to VST";
}

// First, let's add debugging to see what's happening:

void scVST::syncGUIParametersToVST(ofJson &nodeJson) {
	string nodeKey = getParameterGroup().getName();
	ofLogNotice("scVST") << "🎯 Syncing GUI parameter values to loaded VST for node '" << nodeKey << "'";
	
	if(!nodeJson.contains("vstParameters") || nodeJson["vstParameters"].is_null()) {
		ofLogWarning("scVST") << "❌ No vstParameters found in JSON for node '" << nodeKey << "'!";
		return;
	}
	
	ofLogNotice("scVST") << "📄 JSON contains " << nodeJson["vstParameters"].size() << " saved parameters for '" << nodeKey << "'";
	ofLogNotice("scVST") << "🎛️ Currently have " << dynamicVectorParameters.size() << " GUI vector parameters for '" << nodeKey << "'";
	ofLogNotice("scVST") << "🎛️ Currently have " << dynamicParameters.size() << " GUI scalar parameters for '" << nodeKey << "'";
	
	// Clear feedback suppression
	clearAllFeedbackSuppression();
	
	int sentCount = 0;
	int skippedCount = 0;
	
	for(auto& item : nodeJson["vstParameters"].items()) {
		try {
			int paramIndex = ofToInt(item.key());
			if(item.value().is_object()) {
				
				// Check if GUI parameter exists
				if(dynamicVectorParameters.count(paramIndex) > 0) {
					vector<float> currentValues = dynamicVectorParameters[paramIndex]->getParameter().get();
					ofLogNotice("scVST") << "✅ Found GUI parameter " << paramIndex << " with " << currentValues.size() << " values for '" << nodeKey << "'";
					
					// Update parameter info to ensure VST linkage is maintained
					if(parameterInfoMap.count(paramIndex) > 0) {
						// Restore the VST parameter index mapping
						parameterInfoMap[paramIndex].index = paramIndex;
						if(!currentValues.empty()) {
							parameterInfoMap[paramIndex].value = currentValues[0];
						}
						
						// Check if we need to restore the name from JSON
						if(item.value().contains("name") && !item.value()["name"].is_null()) {
							string savedName = item.value()["name"];
							if(parameterInfoMap[paramIndex].displayName != savedName) {
								parameterInfoMap[paramIndex].displayName = savedName;
							}
						}
					}
					
					if(currentValues.size() == 1) {
						// Scalar mode - send to all instances
						float value = currentValues[0];
						ofLogNotice("scVST") << "📤 Sending scalar parameter " << paramIndex << " = " << value << " to all VST instances for '" << nodeKey << "'";
						setVSTParameterDirectToAll(paramIndex, value);
					} else {
						// Vector mode - send per-instance values
						ofLogNotice("scVST") << "📤 Sending vector parameter " << paramIndex << " with " << currentValues.size() << " values to VST instances for '" << nodeKey << "'";
						setVSTParameterVectorDirectToAll(paramIndex, currentValues);
					}
					
					sentCount++;
				} else {
					// FIX: Don't recreate parameters during sync - they should have been created in loadBeforeConnections
					// If they're missing, it's likely because they were intentionally removed or the plugin doesn't support them
					ofLogWarning("scVST") << "⚠️ Parameter " << paramIndex << " exists in JSON but not in GUI for '" << nodeKey << "' - skipping";
					skippedCount++;
				}
				
				// Small delay between parameters
				if(sentCount % 5 == 0) {
					for(auto& serverInstances : synthInstances) {
						serverInstances.first->process();
					}
					ofSleepMillis(50);
				}
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error syncing parameter " << item.key() << " to VST for '" << nodeKey << "': " << e.what();
		}
	}
	
	ofLogNotice("scVST") << "✅ Synced " << sentCount << " GUI parameter values to VST for '" << nodeKey << "'";
	if(skippedCount > 0) {
		ofLogWarning("scVST") << "⚠️ Skipped " << skippedCount << " missing parameters for '" << nodeKey << "'";
	}
	
	// Final processing burst
	for(int i = 0; i < 5; i++) {
		for(auto& serverInstances : synthInstances) {
			serverInstances.first->process();
		}
		ofSleepMillis(100);
	}
	
	// Clear feedback suppression after delay
	auto clearTime = ofGetElapsedTimeMillis() + 200;
	for(auto& item : nodeJson["vstParameters"].items()) {
		try {
			int paramIndex = ofToInt(item.key());
			suppressFeedbackFor(paramIndex, clearTime);
		} catch(...) {}
	}
}

void scVST::setVSTParameterDirectToAll(int paramIndex, float value) {
	// OPTIMIZATION: Early validation with unlikely hint
	if(__builtin_expect(paramIndex < 0, 0)) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}

	if(__builtin_expect(activeInstanceTargets.empty(), 0)) {
		ofLogVerbose("scVST") << "No VST instances available to set parameter";
		return;
	}

	// OPTIMIZATION: Pre-build OSC message template outside the loop
	// All instances get the same parameter value, so we can reuse the message structure
	static thread_local ofxOscMessage setMsg;

	// Apply parameter change to ALL instances WITHOUT feedback suppression
	for(const auto& target : activeInstanceTargets) {
		try {
			// OPTIMIZATION: Reuse message, only update nodeID
			setMsg.clear();
			setMsg.setAddress("/u_cmd");
			setMsg.addIntArg(target.synth->nodeID);
			setMsg.addIntArg(2);
			setMsg.addStringArg("/set");
			setMsg.addIntArg(paramIndex);
			setMsg.addFloatArg(value);
			target.server->sendMsg(setMsg);
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error setting parameter on synth " << target.synth->nodeID << ": " << e.what();
		}
	}
}

void scVST::setVSTParameterVectorDirectToAll(int paramIndex, const vector<float>& values) {
	// OPTIMIZATION: Early validation
	if(__builtin_expect(paramIndex < 0, 0)) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}

	if(__builtin_expect(activeInstanceTargets.empty(), 0)) {
		ofLogVerbose("scVST") << "No VST instances available to set parameter";
		return;
	}

	// OPTIMIZATION: Cache values size and compute fallback value once
	const size_t valuesSize = values.size();
	const float fallbackValue = valuesSize > 0 ? values.back() : 0.0f;

	// OPTIMIZATION: Pre-allocate message outside loop
	static thread_local ofxOscMessage setMsg;

	// Apply parameter changes to instances based on vector indices
	for(size_t instanceIndex = 0; instanceIndex < activeInstanceTargets.size(); ++instanceIndex) {
		const auto& target = activeInstanceTargets[instanceIndex];
		try {
			// OPTIMIZATION: Use array-style access when in bounds
			float value;
			if(__builtin_expect(instanceIndex < valuesSize, 1)) {
				value = values[instanceIndex];
			} else {
				value = fallbackValue;
			}

			// OPTIMIZATION: Reuse message object
			setMsg.clear();
			setMsg.setAddress("/u_cmd");
			setMsg.addIntArg(target.synth->nodeID);
			setMsg.addIntArg(2);
			setMsg.addStringArg("/set");
			setMsg.addIntArg(paramIndex);
			setMsg.addFloatArg(value);
			target.server->sendMsg(setMsg);

		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error setting parameter on synth " << target.synth->nodeID << ": " << e.what();
		}
	}
}



void scVST::createVSTInstances(ofxSCServer* server) {
	int numInstances = calculateNumInstances();
	/*
	 ofLogNotice("scVST") << "Creating " << numInstances << " VST instances for "
	 << numChannels.get() << " channels (mono: "
	 << monoInstancing.get() << ")";
	 */
	// Clear existing instances
	freeVSTInstances(server);
	
	// Create new instances with appropriate SynthDef
	synthInstances[server].resize(numInstances);
	for(int i = 0; i < numInstances; i++) {
		// Choose the appropriate SynthDef based on instancing mode
		string synthDefName = monoInstancing.get() ? "vstMono" : "vstStereo";
		synthInstances[server][i] = new ofxSCSynth(synthDefName, server);
		
			//ofLogNotice("scVST") << "Created VST instance " << i << " using " << synthDefName;
	}

	rebuildInstanceLookupCache();
}

void scVST::freeVSTInstances(ofxSCServer* server) {
	if(!server) {
		ofLogWarning("scVST") << "Cannot free VST instances: server is null";
		return;
	}
	
	if(synthInstances.count(server) == 0) {
		//ofLogNotice("scVST") << "No VST instances to free for this server";
		return;
	}
	
	//ofLogNotice("scVST") << "Freeing " << synthInstances[server].size() << " VST instances";
	
	// Create a copy of the instances vector to avoid iterator invalidation issues
	auto instancesCopy = synthInstances[server];
	
	// Clear the original vector immediately to prevent further access
	synthInstances[server].clear();
	rebuildInstanceLookupCache();
	
	// Now safely delete each instance
	for(auto synth : instancesCopy) {
		if(synth != nullptr) {
			try {
				// First try to send a close message to the VST plugin
				try {
					ofxOscMessage closeMsg;
					closeMsg.setAddress("/u_cmd");
					closeMsg.addIntArg(synth->nodeID);
					closeMsg.addIntArg(2);
					closeMsg.addStringArg("/close");
					server->sendMsg(closeMsg);
				} catch(const std::exception& e) {
					ofLogWarning("scVST") << "Error sending close message to VST: " << e.what();
				} catch(...) {
					ofLogWarning("scVST") << "Unknown error sending close message to VST";
				}
				
				// Small delay to let the close message be processed
				ofSleepMillis(10);
				
				// Free the synth node
				try {
					synth->free();
				} catch(const std::exception& e) {
					ofLogWarning("scVST") << "Error freeing synth node: " << e.what();
				} catch(...) {
					ofLogWarning("scVST") << "Unknown error freeing synth node";
				}
				
				// Delete the synth object
				try {
					delete synth;
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error deleting synth object: " << e.what();
				} catch(...) {
					ofLogError("scVST") << "Unknown error deleting synth object";
				}
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error during synth cleanup: " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error during synth cleanup";
			}
		}
	}
	
	//ofLogNotice("scVST") << "Finished freeing VST instances";
}

void scVST::freeAll() {
	//ofLogNotice("scVST") << "Freeing all VST instances from all servers";
	
	// Create a copy of the server keys to avoid iterator invalidation
	std::vector<ofxSCServer*> servers;
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first != nullptr) {
			servers.push_back(serverInstances.first);
		}
	}
	
	// Free instances from each server safely
	for(auto server : servers) {
		try {
			freeVSTInstances(server);
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error freeing instances from server: " << e.what();
		} catch(...) {
			ofLogError("scVST") << "Unknown error freeing instances from server";
		}
	}
	
	// Clear the entire map
	synthInstances.clear();
	clearInstanceLookupCache();
	
	//ofLogNotice("scVST") << "Finished freeing all VST instances";
}

void scVST::free(ofxSCServer* server) {
	if(!server) {
		ofLogWarning("scVST") << "Cannot free VST instances: server is null";
		return;
	}
	
	try {
		freeVSTInstances(server);
		synthInstances.erase(server);
		rebuildInstanceLookupCache();
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in free(): " << e.what();
	} catch(...) {
		ofLogError("scVST") << "Unknown error in free()";
	}
}

void scVST::rebuildInstanceLookupCache() {
	ownedNodeIDs.clear();
	firstInstanceNodeIDs.clear();
	nodeIDToInstanceIndex.clear();
	activeInstanceTargets.clear();

	int instanceIndex = 0;
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		bool firstForServer = true;
		for(auto synth : serverInstances.second) {
			if(synth == nullptr) continue;

			activeInstanceTargets.push_back({serverInstances.first, synth});
			if(synth->nodeID <= 0) continue;

			ownedNodeIDs.insert(synth->nodeID);
			nodeIDToInstanceIndex[synth->nodeID] = instanceIndex++;

			if(firstForServer) {
				firstInstanceNodeIDs.insert(synth->nodeID);
				firstForServer = false;
			}
		}
	}
}

void scVST::clearInstanceLookupCache() {
	ownedNodeIDs.clear();
	firstInstanceNodeIDs.clear();
	nodeIDToInstanceIndex.clear();
	activeInstanceTargets.clear();
}

void scVST::buildSynth(ofxSCServer* server) {
	createVSTInstances(server);

	for(auto synth : synthInstances[server]) {
		if(synth != nullptr) {
			listeners.push(synth->newFeedbackMessage.newListener([this](ofxOscMessage& msg) -> void {
				if(this == nullptr) return;
				
				try {
					const string& address = msg.getAddress();
					
					if (address[0] != '/') return;
					
					if (address == "/vst_param") {
						this->handleVSTParam(msg);
						
						if (!this->oceanodePresetLoading && !this->hasPendingPresetData && !this->parameterCacheScheduled) {
							if (msg.getNumArgs() >= 3) {
								int paramIndex = (int)msg.getArgAsFloat(2);
								if (!this->isFeedbackSuppressed(paramIndex, ofGetElapsedTimeMillis())) {
									this->vstStateModifiedSincePreset = true;
									this->scheduleParameterDebouncedCache();
								}
							}
						}
					}
					else if (address == "/vst_auto") {
						this->handleVSTAuto(msg);
						
						if (!this->oceanodePresetLoading && !this->hasPendingPresetData && !this->parameterCacheScheduled) {
							this->vstStateModifiedSincePreset = true;
							this->scheduleParameterDebouncedCache();
						}
					}
					else if (address == "/vst_transport") {
						this->handleTransportPosition(msg);
					}
					else if (address == "/vst_open") {
						this->handleVSTOpen(msg);
					}
					else if (address == "/vst_midi") {
						this->handleVSTMidi(msg);
					}
					else if (address == "/vst_program_index") {
						if (msg.getNumArgs() >= 3) {
							int nodeID = msg.getArgAsInt32(0);
							int programIndex = (int)msg.getArgAsFloat(2);
							if (this->isMyVSTInstance(nodeID)) {
								if(!this->oceanodePresetLoading) {
									this->vstProgram.setWithoutEventNotifications(programIndex);
									
									this->vstStateModifiedSincePreset = true;
									this->scheduleImmediateFXPCache();
								}
							}
						}
					}
					else if (address == "/vst_program_write") {
						this->handleVSTPresetWrite(msg);
					}
					else if (address == "/vst_program_read") {
						this->handleVSTPresetRead(msg);
					}
					else if (address == "/vst_set") {
						if (msg.getNumArgs() >= 4) {
							int paramIndex = (int)msg.getArgAsFloat(2);
							float value = msg.getArgAsFloat(3);
							
							if(this->parameterInfoMap.count(paramIndex) == 0) {
								VSTParameterInfo info;
								info.index = paramIndex;
								info.displayName = "Param" + ofToString(paramIndex);
								info.value = value;
								this->parameterInfoMap[paramIndex] = info;
								this->cacheParameterInfoSlot(paramIndex);
							} else {
								this->parameterInfoMap[paramIndex].value = value;
							}
							
							this->updateParameterValue(paramIndex, value);
						}
					}
					else if (address == "/vst_update") {
						this->handleVSTUpdate(msg);

						if (!this->oceanodePresetLoading && !this->hasPendingPresetData) {
							this->vstStateModifiedSincePreset = true;
							this->scheduleImmediateFXPCache();
						}
						
						if (msg.getNumArgs() >= 2) {
							int nodeID = msg.getArgAsInt32(0);
							if (this->isMyVSTInstance(nodeID)) {
								this->queryVSTParametersAfterUpdate(nodeID);
							}
						}
					}
					
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error in OSC feedback listener: " << e.what();
				} catch(...) {
					ofLogError("scVST") << "Unknown error in OSC feedback listener";
				}
			}));
		}
	}
}

void scVST::handleVSTPresetWrite(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 3) {
		int nodeID = msg.getArgAsInt32(0);
		bool success = msg.getArgAsFloat(2) > 0.5f;
		
		if (!isMyVSTInstance(nodeID)) return;

		if(activeFXPWriteOperation == FXPWriteOperation::None || nodeID != activeFXPWriteNodeID) return;
		lastFXPWriteSucceeded = success;

		switch(activeFXPWriteOperation) {
			case FXPWriteOperation::PresetSave:
				waitingForFXPSave = false;
				break;

			case FXPWriteOperation::CacheSave:
				if(success) {
					try {
						std::ifstream file(activeFXPWritePath, std::ios::binary | std::ios::ate);
						if(file.is_open()) {
							std::streamsize size = file.tellg();
							file.seekg(0, std::ios::beg);

							cachedFXP.resize(size);
							if(file.read(reinterpret_cast<char*>(cachedFXP.data()), size)) {
								fxpCacheValid = true;
								saveCacheToGlobal();
								ofLogVerbose("scVST") << "FXP cached successfully for node '"
													 << activeFXPWriteNodeKey << "' (" << size << " bytes)";
							}
							file.close();
						}
					} catch(const std::exception& e) {
						ofLogWarning("scVST") << "Error caching FXP for node '" << activeFXPWriteNodeKey
											 << "': " << e.what();
						fxpCacheValid = false;
					}
				}

				fxpCacheSaveInProgress.store(false, std::memory_order_release);
				try {
					if(!activeFXPWritePath.empty()) ofFile::removeFile(activeFXPWritePath);
				} catch(...) {}

				if(fxpCacheSavePending.exchange(false, std::memory_order_acq_rel)) {
					scheduleImmediateFXPCache();
				}
				break;

			case FXPWriteOperation::UserExport:
				if(!success) {
					ofLogWarning("scVST") << "Failed to export FXP preset to: " << activeFXPWritePath;
				}
				break;

			case FXPWriteOperation::SyncSave:
				waitingForSyncFXPSave = false;
				if(success) {
					applySyncFXPToAllOtherInstances();
				} else {
					ofLogError("scVST") << "❌ FXP sync save failed - aborting sync";
					resetSyncFXPState(true);
				}
				break;

			case FXPWriteOperation::None:
				break;
		}

		clearActiveFXPWriteState();
	}
}

void scVST::handleVSTPresetRead(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 3) {
		int nodeID = msg.getArgAsInt32(0);
		bool success = msg.getArgAsFloat(2) > 0.5f;
		
		if (!isMyVSTInstance(nodeID)) return;
		
		// FXP PRESET LOADING: Set flag when FXP load starts to enable blocking locks
		if(success) {
			ofLogNotice("scVST") << "FXP load started for node " << nodeID << " - enabling blocking locks";
			isFXPLoading.store(true, std::memory_order_release);
			fxpLoadStartTime = ofGetElapsedTimeMillis();
			
			fxpAppliedInstances.insert(nodeID);
			/*
			 ofLogNotice("scVST") << "FXP successfully applied to instance " << nodeID
			 << " (" << fxpAppliedInstances.size() << "/"
			 << readyInstances.size() << " instances have FXP)";
			 */
		}
		
		if(waitingForFXPLoad) {
			waitingForFXPLoad = false;
		}
		
		if(activeFXPReadOperation == FXPReadOperation::SyncLoad) {
			/*
			 ofLogNotice("scVST") << "🎯 Sync FXP load " << (success ? "succeeded" : "failed")
			 << " on instance " << nodeID;
			 */
			
			if(success) {
				syncFXPAppliedInstances.insert(nodeID);
				
				// Check if all instances have been synchronized
				int expectedInstances = 0;
				for(auto& serverInstances : synthInstances) {
					expectedInstances += serverInstances.second.size();
				}
				expectedInstances--; // Subtract 1 for the source instance
				/*
				 ofLogNotice("scVST") << "📊 Sync progress: " << syncFXPAppliedInstances.size()
				 << "/" << expectedInstances << " instances synchronized";
				 */
				
				if(syncFXPAppliedInstances.size() >= expectedInstances) {
					//ofLogNotice("scVST") << "🎉 All instances synchronized via FXP!";
					
					// FXP PRESET LOADING: Clear flag after brief delay to allow parameter processing
					ofSleepMillis(500); // Allow time for all parameter updates to be processed
					ofLogNotice("scVST") << "FXP load complete - disabling blocking locks";
					
					// Optional: Update GUI parameters to reflect the new state
					queryVSTParametersAfterSync();
					
					// Clean up and reset sync state
					resetSyncFXPState(true);
					
					//ofLogNotice("scVST") << "🔓 Sync complete - feedback protection disabled";
				}
			} else {
				ofLogError("scVST") << "❌ Sync FXP load failed on instance " << nodeID;
				resetSyncFXPState(true);
			}
		}
		
		
		
		// Clean up temp file after a delay
		if(success) {
			cleanupTempFXPFile();
		}
	}
}

std::string scVST::createTempFXPPath() {
	std::string tempDir = ofFilePath::getUserHomeDir() + "/.tmp/";
	ofDirectory::createDirectory(tempDir, true, true);
	
	// Use node name + timestamp for uniqueness
	std::string safeName = getNodeCacheKey();
	// Replace any problematic characters in node name
	std::replace(safeName.begin(), safeName.end(), '/', '_');
	std::replace(safeName.begin(), safeName.end(), '\\', '_');
	std::replace(safeName.begin(), safeName.end(), ':', '_');
	std::replace(safeName.begin(), safeName.end(), ' ', '_');
	
	std::string filename = "oceanode_vst_" + safeName + "_" + ofToString(ofGetElapsedTimeMillis()) + ".fxp";
	return tempDir + filename;
}

void scVST::cleanupTempFXPFile() {
	if(!tempFXPPath.empty()) {
		try {
			ofFile::removeFile(tempFXPPath);
			//ofLogVerbose("scVST") << "Cleaned up temporary FXP file: " << tempFXPPath;
		} catch(const std::exception& e) {
			ofLogWarning("scVST") << "Could not remove temporary FXP file: " << e.what();
		}
		tempFXPPath.clear();
	}
}

void scVST::clearActiveFXPWriteState() {
	activeFXPWriteOperation = FXPWriteOperation::None;
	activeFXPWriteNodeID = -1;
	activeFXPWritePath.clear();
	activeFXPWriteNodeKey.clear();
	activeFXPWriteStartTime = 0;
}

void scVST::resetSyncFXPState(bool clearLoadingFlag) {
	waitingForSyncFXPSave = false;
	instancesBeingSynced.clear();
	syncFXPAppliedInstances.clear();
	syncInProgress = false;
	syncSourceNodeID = -1;
	activeFXPReadOperation = FXPReadOperation::None;
	cleanupTempSyncFXPFile();

	if(clearLoadingFlag) {
		isFXPLoading.store(false, std::memory_order_release);
	}

	if(fxpCacheSavePending.exchange(false, std::memory_order_acq_rel)) {
		scheduleImmediateFXPCache();
	}
}

void scVST::queryVSTParametersAfterUpdate(int nodeID) {
	// Find the server for this nodeID
	ofxSCServer* targetServer = nullptr;
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr && synth->nodeID == nodeID) {
				targetServer = serverInstances.first;
				break;
			}
		}
		if(targetServer) break;
	}
	
	if(!targetServer) return;
	
	// Query a reasonable range of parameters to capture the preset changes
	ofxOscMessage paramQueryMsg;
	paramQueryMsg.setAddress("/u_cmd");
	paramQueryMsg.addIntArg(nodeID);
	paramQueryMsg.addIntArg(2);
	paramQueryMsg.addStringArg("/param_query");
	paramQueryMsg.addIntArg(0);    // Start parameter index
	paramQueryMsg.addIntArg(128);  // Query first 128 parameters (reasonable for most VSTs)
	targetServer->sendMsg(paramQueryMsg);
}

void scVST::createSynth(ofxSCServer* server){
	if(synthInstances.count(server) == 0) return;
	
	// Canonical ordering (matches scSynthdef): queue correct bus params BEFORE /s_new
	// so they arrive as init-args and the node never runs with default out=0.
	resendParams.notify();
	
	// Phase 1: Create all synth nodes first (parallel)
	ofLogNotice("scVST") << "📤 Creating " << synthInstances[server].size() << " VST synth nodes (parallel)";
	for(int i = 0; i < synthInstances[server].size(); i++) {
		if(synthInstances[server][i] != nullptr) {
			// Create the synth on the server (no need to set channel params since they're fixed in the SynthDef)
            synthInstances[server][i]->createAndRun(0, 1, getActive());
			/*
			 ofLogVerbose("scVST") << "Created VST instance " << i
			 << " with nodeID " << synthInstances[server][i]->nodeID
			 << " (" << (monoInstancing.get() ? "mono" : "stereo") << ")";
			 */
		}
	}

	rebuildInstanceLookupCache();
	
	// Phase 2: Load the selected plugin on all instances (parallel)
	if (!currentPluginPath.empty()) {
		ofLogNotice("scVST") << "📤 Loading plugin on all instances (parallel): " << currentPluginPath;
		
		// Send all open commands without delays
		for(int i = 0; i < synthInstances[server].size(); i++) {
			if(synthInstances[server][i] != nullptr) {
				// Open the VST plugin - NO DELAY between instances
				ofxOscMessage openMsg;
				openMsg.setAddress("/u_cmd");
				openMsg.addIntArg(synthInstances[server][i]->nodeID);
				openMsg.addIntArg(2); // Keep synthIndex as 2 per Oceanode requirements
				openMsg.addStringArg("/open");
				openMsg.addStringArg(currentPluginPath);
				openMsg.addIntArg(1); // Request GUI editor
				openMsg.addIntArg(enableMultithreading.get() ? 1 : 0);
				openMsg.addIntArg(0); // Normal mode
				server->sendMsg(openMsg);
				
				//ofLogVerbose("scVST") << "Sent plugin open to instance " << i;
			}
		}
		
		// Brief processing time to kickstart plugin loading
		for(int i = 0; i < 5; i++) {
			server->process();
			ofSleepMillis(10);
		}
		
		pluginLoaded = true;
	}
}

void scVST::moveSynthBefore(ofxSCServer* server, int nodeID){
	if(!server) {
		ofLogWarning("scVST") << "moveSynthBefore: server is null";
		return;
	}
	
	// If we don't have instances for this server, nothing to do
	if(synthInstances.count(server) == 0) {
		//ofLogVerbose("scVST") << "moveSynthBefore: no instances for this server";
		return;
	}
	
	// Re-apply simple params (currently inChannels via resendParams)
	// This mirrors scSynthdef/scOutput behaviour where params are resent
	// before a graph move.
	resendParams.notify();
	
	// Move all instances before nodeID using reverse iteration so that
	// synthInstances[0] ends up as the most-upstream synth in SC's node
	// tree (matching creation order). Forward iteration would invert the
	// internal order because each moveBefore pushes previous synths back.
	auto &instances = synthInstances[server];
	for(int i = (int)instances.size() - 1; i >= 0; i--) {
		auto* synth = instances[i];
		if(synth != nullptr && synth->nodeID > 0) {
			try {
				synth->moveBefore(nodeID);
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error moving synth " << synth->nodeID
				<< " before node " << nodeID << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error moving synth before node";
			}
		}
	}
}

int scVST::getLastSynthID(ofxSCServer* server){
	if(!server) return -1;
	if(synthInstances.count(server) == 0) return -1;
	
	// moveSynthBefore uses reverse iteration, so synthInstances[0] ends up
	// as the most-upstream synth in SC's node tree. Return its nodeID so
	// that any node upstream of this VST is placed before the entire
	// instance group, not in the middle of it.
	for(auto* synth : synthInstances[server]) {
		if(synth != nullptr && synth->nodeID > 0) {
			return synth->nodeID;
		}
	}

	return -1;
}

void scVST::resetInputBusses(ofxSCServer* server, int targetBus){
	inputBuses[server].clear();
	
	if(synthInstances.count(server) == 0) return;
	
	for(auto* synth : synthInstances[server]){
		if(synth != nullptr){
			synth->set("in", targetBus);
			synth->set("inChannels", 0);
		}
	}
}

void scVST::setOutputBus(ofxSCServer* server, int index, int bus){
	outputBuses[server][index] = bus;
	
	if (synthInstances.count(server) == 0) return;
	
	const int numInstances = synthInstances[server].size();
	
	for (int i = 0; i < numInstances; ++i) {
		if (synthInstances[server][i] == nullptr) continue;
		
		int instanceOutputBus   = bus;
		int channelsPerInstance = 2; // default for stereo mode
		
		if (singleInstance.get()) {
			// One plugin instance sees the entire ambisonic bus
			instanceOutputBus   = bus;
			channelsPerInstance = numChannels.get();   // e.g., 4/9/16…
		} else if (monoInstancing.get()) {
			// 1 channel per instance → lay them out consecutively
			instanceOutputBus   = bus + i;
			channelsPerInstance = 1;
		} else {
			// Stereo instancing → 2 channels per instance
			instanceOutputBus   = bus + (i * 2);
			channelsPerInstance = 2;
		}
		
		// Tell the SuperCollider synth how many channels it should expose on 'out'
		synthInstances[server][i]->set("out",         instanceOutputBus);
		synthInstances[server][i]->set("outChannels", channelsPerInstance);
	}
}


void scVST::setInputBus(ofxSCServer* server, scNode* node, int bus){
	inputBuses[server][node] = bus;
	
	if (synthInstances.count(server) == 0) return;
	
	const int numInstances = synthInstances[server].size();
	
	for (int i = 0; i < numInstances; ++i) {
		if (synthInstances[server][i] == nullptr) continue;
		
		int instanceInputBus    = bus;
		int channelsPerInstance = 2;
		
		if (singleInstance.get()) {
			instanceInputBus    = bus;
			channelsPerInstance = numChannels.get();   // e.g., 4/9/16…
		} else if (monoInstancing.get()) {
			instanceInputBus    = bus + i;
			channelsPerInstance = 1;
		} else {
			instanceInputBus    = bus + (i * 2);
			channelsPerInstance = 2;
		}
		
		synthInstances[server][i]->set("in",          instanceInputBus);
		synthInstances[server][i]->set("inChannels",  channelsPerInstance);
	}
}



int scVST::getOutputBusIndex(ofxSCServer* server, int index){
	if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
		return outputBuses[server][index];
	}
	return -1;
}

void scVST::setVSTProgram(int programIndex) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available";
		return;
	}
	
	programIndex = ofClamp(programIndex, 0, 127);
	
	//ofLogNotice("scVST") << "=== CHANGING PROGRAM TO " << programIndex << " ===";
	
	// Method 1: Try MIDI program change first (like most VST instruments expect)
	//ofLogNotice("scVST") << "Trying MIDI program change...";
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				sendMidiProgramChange(midiChannel.get(), programIndex, serverInstances.first, synth);
			}
		}
	}
	
	// Method 2: ALSO try direct VST program set (some VSTs might prefer this)
	//ofLogNotice("scVST") << "Also trying direct VST program set...";
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				ofxOscMessage m;
				m.setAddress("/u_cmd");
				m.addIntArg(synth->nodeID);
				m.addIntArg(2);
				m.addStringArg("/program_set"); // Direct VST program change
				m.addIntArg(programIndex);
				serverInstances.first->sendMsg(m);
				/*
				 ofLogNotice("scVST") << "VST program_set: " << programIndex
				 << " -> Node" << synth->nodeID;
				 */
			}
		}
	}
	
	//ofLogNotice("scVST") << "=== PROGRAM CHANGE COMPLETE ===";
}

void scVST::sendMidiProgramChange(int channel, int program, ofxSCServer* server, ofxSCSynth* synth) {
	if(!server || !synth || synth->nodeID <= 0) {
		ofLogError("scVST") << "Invalid server/synth for MIDI program change";
		return;
	}
	
	try {
		ofxOscMessage m;
		m.setAddress("/u_cmd");
		m.addIntArg(synth->nodeID);
		m.addIntArg(2); // synthIndex
		m.addStringArg("/midi_msg");
		
		// Follow SuperCollider's exact approach: Int8Array with 3 bytes
		// Even for program change, SC sends 3 bytes with the third as 0
		std::vector<int8_t> midiData;
		midiData.push_back(0xC0 | ((channel - 1) & 0x0F)); // Program change + channel
		midiData.push_back(program & 0x7F);                // Program number
		midiData.push_back(0);                              // Third byte (unused but sent)
		
		// Convert to the format VST expects (similar to SC's Int8Array)
		ofBuffer buffer;
		buffer.set(reinterpret_cast<const char*>(midiData.data()), midiData.size());
		m.addBlobArg(buffer);
		m.addFloatArg(0.0f); // detune
		
		server->sendMsg(m);
		/*
		 ofLogNotice("scVST") << "Sent MIDI program change: CH" << channel
		 << " PRG" << program << " -> Node" << synth->nodeID;
		 */
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "MIDI program change error: " << e.what();
	}
}

void scVST::sendMidiToInstance(ofxSCServer* server, ofxSCSynth* synth, int channel, int status, int data1, int data2) {
	// OPTIMIZATION: Use stack-allocated OSC message to avoid heap allocation
	ofxOscMessage m;
	m.setAddress("/u_cmd");
	m.addIntArg(synth->nodeID);
	m.addIntArg(2); // VSTPlugin synthIndex
	m.addStringArg("/midi_msg"); // MIDI command

	// OPTIMIZATION: Reserve MIDI message capacity upfront and use emplace_back
	vector<uint8_t> midiBytes;
	midiBytes.reserve(3); // Always 3 bytes for standard MIDI messages
	midiBytes.push_back(status | ((channel - 1) & 0x0F)); // Status + channel (0-based)
	midiBytes.push_back(data1 & 0x7F); // Data 1
	if(status != 0xC0 && status != 0xD0) { // Program change and channel pressure have only 2 bytes
		midiBytes.push_back(data2 & 0x7F); // Data 2
	}
	
	// Convert to buffer
	ofBuffer buffer;
	buffer.set(reinterpret_cast<const char*>(midiBytes.data()), midiBytes.size());
	m.addBlobArg(buffer);
	m.addFloatArg(0.0f); // detune
	server->sendMsg(m);
}

void scVST::sendPitchBend(float value) {
	// Value is 0.0 to 1.0, center is 0.5
	// MIDI pitch bend is 14-bit (0-16383), center is 8192
	int bendValue = (int)(ofClamp(value, 0.0f, 1.0f) * 16383.0f);
	int lsb = bendValue & 0x7F;
	int msb = (bendValue >> 7) & 0x7F;
	
	// Send to all instances
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				sendMidiToInstance(serverInstances.first, synth, midiChannel.get(), 0xE0, lsb, msb);
			}
		}
	}
}

void scVST::sendModWheel(float value) {
	// Mod wheel is CC 1
	int ccValue = (int)(ofClamp(value, 0.0f, 1.0f) * 127.0f);
	
	// Send to all instances
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				sendMidiToInstance(serverInstances.first, synth, midiChannel.get(), 0xB0, 1, ccValue);
			}
		}
	}
}

void scVST::handleDynamicParameterChange(int paramIndex, const vector<float>& values) {
	// OPTIMIZATION: Keep feedback suppression active for echoed VST messages,
	// but never drop fresh GUI/LFO-driven changes while suppression is active.
	suppressFeedbackFor(paramIndex, ofGetElapsedTimeMillis() + 100);

	// OPTIMIZATION: Avoid redundant size checks
	const size_t valueCount = values.size();
	if(valueCount == 1) {
		// Scalar value - broadcast to all instances
		setVSTParameterDirectToAll(paramIndex, values[0]);
	} else if(valueCount > 1) {
		// Vector value - send per-instance values
		setVSTParameterVectorDirectToAll(paramIndex, values);
	}

}

void scVST::propagateFirstInstanceToAll() {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for propagation";
		return;
	}
	
	// Find first instance
	ofxSCSynth* firstInstance = nullptr;
	ofxSCServer* firstServer = nullptr;
	
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			firstInstance = serverInstances.second[0];
			firstServer = serverInstances.first;
			break;
		}
	}
	
	if(!firstInstance || !firstServer) {
		ofLogError("scVST") << "Could not find first VST instance for propagation";
		return;
	}
	
	//ofLogNotice("scVST") << "=== PROPAGATING FROM INSTANCE " << firstInstance->nodeID << " ===";
	
	// Capture parameters using the same proven method as presetSave
	int initialParamCount = parameterInfoMap.size();
	
	// Method 1: Individual parameter queries (most reliable)
	for(int paramIndex = 0; paramIndex < 256; paramIndex++) {
		ofxOscMessage getParamMsg;
		getParamMsg.setAddress("/u_cmd");
		getParamMsg.addIntArg(firstInstance->nodeID);
		getParamMsg.addIntArg(2);
		getParamMsg.addStringArg("/get");
		getParamMsg.addIntArg(paramIndex);
		firstServer->sendMsg(getParamMsg);
		
		// Essential: Force OSC processing regularly
		if(paramIndex % 10 == 0) {
			firstServer->process();
			ofSleepMillis(5);
		}
	}
	
	// Method 2: Bulk queries for efficiency
	for(int startParam = 0; startParam < 256; startParam += 16) {
		ofxOscMessage getBulkMsg;
		getBulkMsg.setAddress("/u_cmd");
		getBulkMsg.addIntArg(firstInstance->nodeID);
		getBulkMsg.addIntArg(2);
		getBulkMsg.addStringArg("/getn");
		getBulkMsg.addIntArg(startParam);
		getBulkMsg.addIntArg(16);
		firstServer->sendMsg(getBulkMsg);
		
		firstServer->process();
		ofSleepMillis(10);
	}
	
	// Wait for responses with active OSC processing
	//ofLogNotice("scVST") << "Capturing parameter state...";
	for(int i = 0; i < 100; i++) {
		firstServer->process();
		ofSleepMillis(20);
		
		// Check progress periodically
		if(i % 25 == 0) {
			int currentCount = parameterInfoMap.size();
			if(currentCount > initialParamCount + 20) {
				//ofLogNotice("scVST") << "Good progress, finishing capture early";
				break;
			}
		}
	}
	
	// Final processing burst
	for(int i = 0; i < 10; i++) {
		firstServer->process();
		ofSleepMillis(10);
	}
	
	// Collect all parameters to propagate
	std::map<int, float> parametersToPropagate;
	
	// Priority 1: GUI parameters (user is actively controlling these)
	for(auto& param : dynamicVectorParameters) {
		int paramIndex = param.first;
		auto values = param.second->getParameter().get();
		if(!values.empty()) {
			parametersToPropagate[paramIndex] = values[0];
		}
	}
	
	for(auto& param : dynamicParameters) {
		int paramIndex = param.first;
		parametersToPropagate[paramIndex] = param.second->getParameter().get();
	}
	
	// Priority 2: Captured parameters from VST
	for(auto& info : parameterInfoMap) {
		int paramIndex = info.first;
		if(parametersToPropagate.count(paramIndex) == 0) {
			parametersToPropagate[paramIndex] = info.second.value;
		}
	}
	
	//ofLogNotice("scVST") << "Propagating " << parametersToPropagate.size() << " parameters";
	
	if(parametersToPropagate.empty()) {
		ofLogError("scVST") << "No parameters to propagate";
		return;
	}
	
	// Apply to all other instances
	const uint64_t clearTime = ofGetElapsedTimeMillis() + 200;
	for(auto& param : parametersToPropagate) {
		suppressFeedbackFor(param.first, clearTime);
	}
	
	bool skipFirst = true;
	int appliedCount = 0;
	
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				if(skipFirst) {
					skipFirst = false;
					continue;
				}
				
				// Apply all parameters to this instance
				for(auto& param : parametersToPropagate) {
					ofxOscMessage setMsg;
					setMsg.setAddress("/u_cmd");
					setMsg.addIntArg(synth->nodeID);
					setMsg.addIntArg(2);
					setMsg.addStringArg("/set");
					setMsg.addIntArg(param.first);
					setMsg.addFloatArg(param.second);
					serverInstances.first->sendMsg(setMsg);
				}
				
				appliedCount++;
				ofSleepMillis(50); // Small delay between instances
			}
		}
	}
	
	//ofLogNotice("scVST") << "Successfully propagated to " << appliedCount << " instances";
}

void scVST::drawSeparator() {
	float zoom = ofxOceanodeShared::getZoomLevel();
	ImVec2 p = ImGui::GetCursorScreenPos();

	ImGui::GetWindowDrawList()->AddLine(
										ImVec2(p.x,     p.y),
										ImVec2(p.x + 240 * zoom, p.y),
										IM_COL32(200, 200, 200, 255),
										1.0f
										);

	ImGui::Dummy(ImVec2(0, 4 * zoom));
}



std::string scVST::base64Encode(const std::vector<uint8_t>& data) {
	const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	int val = 0, valb = -6;
	
	for (uint8_t c : data) {
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0) {
			result.push_back(chars[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}
	
	if (valb > -6) {
		result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
	}
	
	while (result.size() % 4) {
		result.push_back('=');
	}
	
	return result;
}

std::vector<uint8_t> scVST::base64Decode(const std::string& encoded) {
	const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::vector<uint8_t> result;
	int val = 0, valb = -8;
	
	for (char c : encoded) {
		if (c == '=') break;
		auto pos = chars.find(c);
		if (pos == std::string::npos) continue;
		
		val = (val << 6) + pos;
		valb += 6;
		if (valb >= 0) {
			result.push_back(char((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	
	return result;
}

void scVST::presetWillBeLoaded(){
	isPresetLoading = true;
	oceanodePresetLoading = true;  // NEW: Track Oceanode preset loading specifically
	
	// CRITICAL FIX: Clear all dynamic parameters before loading preset
	// This prevents parameter duplication and ensures clean state
	ofLogNotice("scVST::presetWillBeLoaded") << "🧹 Clearing all dynamic parameters before preset load";
	
	// Clear saved parameter names from previous preset
	savedParameterNames.clear();
	for(int i = 0; i < 1024; ++i) {
		clearSavedParameterNameSlot(i);
	}
	
	// Use the force removal to ensure parameters are cleared even during preset loading
	removeAllDynamicParametersForce();
	
	//ofLogNotice("scVST") << "🔒 Oceanode preset loading started - both flags = TRUE";
}

void scVST::activateConnections(){
	// CRITICAL: Keep preset loading active - don't change the flag here!
	// The base class might be expecting us to change it, but we need it to stay true
	// until VST synchronization is complete
	//ofLogNotice("scVST") << "🔗 Connections activated - keeping isPresetLoading = TRUE until VST sync";
	// DO NOT SET isPresetLoading = false here!
}

void scVST::presetHasLoaded(){
	// Still keep preset loading active if we have pending VST data
	if(!hasPendingPresetData) {
		isPresetLoading = false;
		oceanodePresetLoading = false;
		resetVSTModificationTracking();
	}
}

void scVST::handleVSTUpdate(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 2) {
		int nodeID = msg.getArgAsInt32(0);
		
		if (!isMyVSTInstance(nodeID)) return;
		
		if (syncInProgress) {
			return;
		}
		
		if (isPresetLoading || hasPendingPresetData) {
			return;
		}
		
		if (instancesBeingSynced.count(nodeID) > 0) {
			return;
		}
		
		// Check if this is the first instance for its server (we only sync FROM those)
		if(firstInstanceNodeIDs.count(nodeID) > 0) {
			//ofLogNotice("scVST") << "🎯 Update came from first instance - syncing to all others via FXP";
			syncFirstInstanceToAllViaFXP(nodeID);
		} else {
			/*
			 ofLogNotice("scVST") << "ℹ️ Update came from non-first instance (node " << nodeID
			 << ") - ignoring to avoid conflicts";
			 */
		}
	}
}

void scVST::syncFirstInstanceToAllViaFXP(int sourceNodeID) {
	// Prevent multiple concurrent syncs
	if (syncInProgress) {
		ofLogWarning("scVST") << "⚠️ Sync already in progress - ignoring new sync request";
		return;
	}

	if(activeFXPWriteOperation != FXPWriteOperation::None) {
		ofLogWarning("scVST") << "Cannot start FXP sync while another FXP write operation is active";
		return;
	}
	
	// Find the first instance and its server
	ofxSCSynth* firstInstance = nullptr;
	ofxSCServer* firstServer = nullptr;
	
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			if(serverInstances.second[0]->nodeID == sourceNodeID) {
				firstInstance = serverInstances.second[0];
				firstServer = serverInstances.first;
				break;
			}
		}
	}
	
	if(!firstInstance || !firstServer) {
		ofLogError("scVST") << "Could not find first instance for FXP sync";
		return;
	}
	
	//ofLogNotice("scVST") << "🔄 Starting FXP sync from instance " << sourceNodeID;
	
	// Mark sync as in progress
	syncInProgress = true;
	
	// Create temporary FXP file for sync
	tempSyncFXPPath = createTempSyncFXPPath();
	waitingForSyncFXPSave = true;
	activeFXPWriteOperation = FXPWriteOperation::SyncSave;
	activeFXPWriteNodeID = sourceNodeID;
	activeFXPWritePath = tempSyncFXPPath;
	activeFXPWriteNodeKey = getNodeCacheKey();
	activeFXPWriteStartTime = ofGetElapsedTimeMillis();
	syncSourceNodeID = sourceNodeID;
	syncFXPAppliedInstances.clear();
	instancesBeingSynced.clear();
	
	// Pre-populate the list of instances that will receive the FXP
	bool skipFirst = true;
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				if(skipFirst) {
					skipFirst = false;
					continue;
				}
				instancesBeingSynced.insert(synth->nodeID);
			}
		}
	}
	/*
	 ofLogNotice("scVST") << "🔒 Marked " << instancesBeingSynced.size()
	 << " instances as being synced (feedback protection)";
	 */
	// Request first instance to save its current state to FXP
	ofxOscMessage writeMsg;
	writeMsg.setAddress("/u_cmd");
	writeMsg.addIntArg(firstInstance->nodeID);
	writeMsg.addIntArg(2);
	writeMsg.addStringArg("/program_write");
	writeMsg.addStringArg(tempSyncFXPPath);
	writeMsg.addIntArg(1); // async = true
	firstServer->sendMsg(writeMsg);
	
	//ofLogNotice("scVST") << "📝 Requested FXP save from first instance";
}



void scVST::applySyncFXPToAllOtherInstances() {
	//ofLogNotice("scVST") << "🔄 Applying sync FXP to all other instances";
		
	int appliedCount = 0;
	bool skipFirst = true;
	activeFXPReadOperation = FXPReadOperation::SyncLoad;
		
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				// Skip the first instance (source of the sync)
				if(skipFirst) {
					skipFirst = false;
					//ofLogVerbose("scVST") << "⏭️ Skipping source instance " << synth->nodeID;
					continue;
				}
				
				//ofLogNotice("scVST") << "📤 Applying FXP to instance " << synth->nodeID;
				
				// Send program_read command to load the FXP
				ofxOscMessage readMsg;
				readMsg.setAddress("/u_cmd");
				readMsg.addIntArg(synth->nodeID);
				readMsg.addIntArg(2);
				readMsg.addStringArg("/program_read");
				readMsg.addStringArg(tempSyncFXPPath);
				readMsg.addIntArg(1); // async = true
				serverInstances.first->sendMsg(readMsg);
				
				appliedCount++;
			}
		}
	}
	
	//ofLogNotice("scVST") << "✅ Sent FXP load commands to " << appliedCount << " instances";
	
	if(appliedCount == 0) {
		// No other instances to sync to - clean up immediately and reset sync state
		resetSyncFXPState(false);
		
		//ofLogNotice("scVST") << "🔓 No instances to sync - feedback protection disabled";
	}
	// Otherwise, cleanup will happen when all instances confirm they've loaded
}



void scVST::queryVSTParametersAfterSync() {
	// Query parameters from first instance to update GUI
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			ofxOscMessage paramQueryMsg;
			paramQueryMsg.setAddress("/u_cmd");
			paramQueryMsg.addIntArg(serverInstances.second[0]->nodeID);
			paramQueryMsg.addIntArg(2);
			paramQueryMsg.addStringArg("/param_query");
			paramQueryMsg.addIntArg(0);    // Start parameter index
			paramQueryMsg.addIntArg(128);  // Query first 128 parameters
			serverInstances.first->sendMsg(paramQueryMsg);
			
			//ofLogNotice("scVST") << "🔍 Querying parameters after FXP sync to update GUI";
			break; // Only query from first server
		}
	}
}

std::string scVST::createTempSyncFXPPath() {
	std::string tempDir = ofFilePath::getUserHomeDir() + "/.tmp/";
	ofDirectory::createDirectory(tempDir, true, true);
	
	std::string safeName = getNodeCacheKey();
	std::replace(safeName.begin(), safeName.end(), '/', '_');
	std::replace(safeName.begin(), safeName.end(), '\\', '_');
	std::replace(safeName.begin(), safeName.end(), ':', '_');
	std::replace(safeName.begin(), safeName.end(), ' ', '_');
	
	std::string filename = "oceanode_vst_sync_" + safeName + "_" + ofToString(ofGetElapsedTimeMillis()) + ".fxp";
	return tempDir + filename;
}
void scVST::cleanupTempSyncFXPFile() {
	if(!tempSyncFXPPath.empty()) {
		try {
			ofFile::removeFile(tempSyncFXPPath);
			//ofLogVerbose("scVST") << "🗑️ Cleaned up temporary sync FXP file: " << tempSyncFXPPath;
		} catch(const std::exception& e) {
			ofLogWarning("scVST") << "Could not remove temporary sync FXP file: " << e.what();
		}
		tempSyncFXPPath.clear();
	}
}

void scVST::checkAndConvertVectorToScalar(int paramIndex) {
	if(dynamicVectorParameters.count(paramIndex) == 0) return;
	
	auto vectorParam = dynamicVectorParameters[paramIndex];
	auto currentValues = vectorParam->getParameter().get();
	
	// Check if this parameter has any connections
	bool hasConnections = vectorParam->hasInConnection();
	
	// Track connection state
	bool hadConnection = parameterHasVectorConnection[paramIndex];
	parameterHasVectorConnection[paramIndex] = hasConnections;
	
	// If parameter was disconnected (had connection but now doesn't)
	if(hadConnection && !hasConnections) {
		/*
		 ofLogNotice("scVST") << "🔄 Vector parameter " << paramIndex
		 << " disconnected - converting to scalar mode";
		 */
		// Get the current first value to use as scalar
		float scalarValue = currentValues.empty() ? 0.0f : currentValues[0];
		
		// Convert to scalar (single-element vector) and propagate to all instances
		convertVectorParameterToScalar(paramIndex, scalarValue);
	}
	
	// If parameter has multiple values but no connections, also convert to scalar
	else if(!hasConnections && currentValues.size() > 1) {
		/*
		 ofLogNotice("scVST") << "🔄 Vector parameter " << paramIndex
		 << " has multiple values but no connections - converting to scalar";
		 */
		float scalarValue = currentValues[0];
		convertVectorParameterToScalar(paramIndex, scalarValue);
	}
}

void scVST::convertVectorParameterToScalar(int paramIndex, float scalarValue) {
	if(dynamicVectorParameters.count(paramIndex) == 0) {
		//ofLogWarning("scVST") << "Cannot convert parameter " << paramIndex << " - not found";
		return;
	}
	/*
	 ofLogNotice("scVST") << "Converting parameter " << paramIndex
	 << " from vector to scalar with value " << scalarValue;
	 */
	// Update the vector parameter to single-element (scalar mode)
	try {
		// Set as single-element vector (this is scalar mode for our system)
		vector<float> scalarVector = {scalarValue};
		dynamicVectorParameters[paramIndex]->getParameter().setWithoutEventNotifications(scalarVector);
		
		// Update parameter info
		if(parameterInfoMap.count(paramIndex) > 0) {
			parameterInfoMap[paramIndex].value = scalarValue;
		}
		
		// Propagate this scalar value to ALL VST instances
		if(!synthInstances.empty() && areAllInstancesReady()) {
			/*
			 ofLogNotice("scVST") << "📡 Broadcasting scalar value " << scalarValue
			 << " to all VST instances";
			 */
			// Use direct method to ensure all instances get the same value
			setVSTParameterDirectToAll(paramIndex, scalarValue);
		}
		/*
		 ofLogNotice("scVST") << "✅ Successfully converted parameter " << paramIndex
		 << " to scalar mode and propagated to all instances";
		 */
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error converting parameter " << paramIndex
		<< " to scalar: " << e.what();
	}
}

void scVST::saveFXPToUserChosenPath() {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for FXP save";
		return;
	}

	if(activeFXPWriteOperation != FXPWriteOperation::None ||
	   activeFXPReadOperation != FXPReadOperation::None ||
	   syncInProgress) {
		ofLogWarning("scVST") << "Cannot export FXP while another FXP operation is active";
		return;
	}
	
	// Find first instance
	ofxSCSynth* firstInstance = nullptr;
	ofxSCServer* firstServer = nullptr;
	
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			firstInstance = serverInstances.second[0];
			firstServer = serverInstances.first;
			break;
		}
	}
	
	if(!firstInstance || !firstServer) {
		ofLogError("scVST") << "Could not find VST instance for FXP save";
		return;
	}
	
	// Get plugin name for default filename
	string defaultFilename = "preset.fxp";
	if(!currentPluginPath.empty()) {
		string pluginName = ofFilePath::getBaseName(currentPluginPath);
		defaultFilename = pluginName + "_preset.fxp";
	}
	
	// Open file dialog
	ofFileDialogResult result = ofSystemSaveDialog(defaultFilename, "Save VST Preset as FXP");
	
	if(result.bSuccess) {
		string savePath = result.getPath();
		
		// Ensure .fxp extension
		if(ofToLower(ofFilePath::getFileExt(savePath)) != "fxp") {
			savePath += ".fxp";
		}
		
		//ofLogNotice("scVST") << "Saving FXP preset to: " << savePath;

		activeFXPWriteOperation = FXPWriteOperation::UserExport;
		activeFXPWriteNodeID = firstInstance->nodeID;
		activeFXPWritePath = savePath;
		activeFXPWriteNodeKey.clear();
		activeFXPWriteStartTime = ofGetElapsedTimeMillis();
		
		// Send the write command
		ofxOscMessage writeMsg;
		writeMsg.setAddress("/u_cmd");
		writeMsg.addIntArg(firstInstance->nodeID);
		writeMsg.addIntArg(2);
		writeMsg.addStringArg("/program_write");
		writeMsg.addStringArg(savePath);
		writeMsg.addIntArg(1); // async = true
		firstServer->sendMsg(writeMsg);
	}
}

void scVST::scheduleImmediateFXPCache() {
	if(!pluginLoaded || synthInstances.empty() || oceanodePresetLoading || hasPendingPresetData) {
		return;
	}
	
	fxpCacheScheduledTime = ofGetElapsedTimeMillis() + 100;
	fxpCacheScheduled = true;
}

void scVST::scheduleDebouncedFXPCache(int delayMs) {
	if(!pluginLoaded || synthInstances.empty() || oceanodePresetLoading || hasPendingPresetData) {
		return;
	}
	
	std::string nodeKey = getNodeCacheKey();
	ofLogVerbose("scVST") << "📅 Scheduling debounced FXP cache update for node '" << nodeKey << "' (" << delayMs << "ms)";
	fxpCacheScheduledTime = ofGetElapsedTimeMillis() + delayMs;
	fxpCacheScheduled = true;
}

// NEW: Debounced parameter caching method
void scVST::scheduleParameterDebouncedCache() {
	if(!pluginLoaded || synthInstances.empty() || oceanodePresetLoading || hasPendingPresetData) {
		return;
	}
	
	std::string nodeKey = getNodeCacheKey();
	lastParameterChangeTime = ofGetElapsedTimeMillis();
	parameterCacheScheduled = true;
	
	ofLogVerbose("scVST") << "📅 Parameter change detected for node '" << nodeKey << "' - debounce timer reset";
}

// NEW: Update debounced parameter cache
void scVST::updateParameterDebouncedCacheIfNeeded() {
	if(!parameterCacheScheduled) return;
	
	uint64_t currentTime = ofGetElapsedTimeMillis();
	if(currentTime >= (lastParameterChangeTime + parameterDebounceDelay)) {
		parameterCacheScheduled = false;
		
		std::string nodeKey = getNodeCacheKey();
		ofLogVerbose("scVST") << "Debounce period elapsed for node '" << nodeKey
							 << "'; saving FXP to cache";
		
		saveFXPToCache();
	}
}

void scVST::updateFXPCacheIfNeeded() {
	if(!fxpCacheScheduled) return;
	
	uint64_t currentTime = ofGetElapsedTimeMillis();
	if(currentTime >= fxpCacheScheduledTime) {
		fxpCacheScheduled = false;
		saveFXPToCache();
	}
}

// NEW: Helper methods for smart source selection
bool scVST::shouldUsePresetFXP() const {
	return hasSavedFXPData && !vstStateModifiedSincePreset && !oceanodePresetLoading;
}

bool scVST::shouldUseCachedFXP() const {
	return fxpCacheValid && vstStateModifiedSincePreset && !oceanodePresetLoading;
}

void scVST::resetVSTModificationTracking() {
	vstStateModifiedSincePreset = false;
	lastParameterChangeTime = 0;
	parameterCacheScheduled = false;
	
}

void scVST::saveFXPToCache() {
	std::string nodeKey = getNodeCacheKey();
	ofLogVerbose("scVST") << "saveFXPToCache called for node '" << nodeKey
						 << "' - current fxpCacheValid:" << fxpCacheValid;
	
	if(synthInstances.empty() || oceanodePresetLoading || hasPendingPresetData) {
		ofLogVerbose("scVST") << "saveFXPToCache blocked for node '" << nodeKey << "'";
		return;
	}

	if(syncInProgress ||
	   activeFXPReadOperation != FXPReadOperation::None ||
	   isFXPLoading.load(std::memory_order_acquire)) {
		fxpCacheSavePending.store(true, std::memory_order_release);
		ofLogVerbose("scVST") << "saveFXPToCache deferred for node '" << nodeKey
							 << "' until FXP loading/sync completes";
		return;
	}

	if(fxpCacheSaveInProgress.load(std::memory_order_acquire) ||
	   activeFXPWriteOperation != FXPWriteOperation::None) {
		fxpCacheSavePending.store(true, std::memory_order_release);
		ofLogVerbose("scVST") << "saveFXPToCache deferred for node '" << nodeKey
							 << "' because another FXP write operation is already in progress";
		return;
	}
	
	// Find first instance
	ofxSCSynth* firstInstance = nullptr;
	ofxSCServer* firstServer = nullptr;
	
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			firstInstance = serverInstances.second[0];
			firstServer = serverInstances.first;
			break;
		}
	}
	
	if(!firstInstance || !firstServer) {
		return;
	}

	ofLogVerbose("scVST") << "💾 Saving current VST state to cache for node '" << nodeKey << "'";
	fxpCacheSaveInProgress.store(true, std::memory_order_release);
	string tempPath = createTempFXPPath();
	activeFXPWriteOperation = FXPWriteOperation::CacheSave;
	activeFXPWriteNodeID = firstInstance->nodeID;
	activeFXPWritePath = tempPath;
	activeFXPWriteNodeKey = nodeKey;
	activeFXPWriteStartTime = ofGetElapsedTimeMillis();

	// Send async write command
	ofxOscMessage writeMsg;
	writeMsg.setAddress("/u_cmd");
	writeMsg.addIntArg(firstInstance->nodeID);
	writeMsg.addIntArg(2);
	writeMsg.addStringArg("/program_write");
	writeMsg.addStringArg(tempPath);
	writeMsg.addIntArg(1); // async = true
	firstServer->sendMsg(writeMsg);
}

std::string scVST::getNodeCacheKey() {
	return getParameterGroup().getName();
}

void scVST::loadCacheFromGlobal() {
	std::lock_guard<std::mutex> lock(globalCacheMutex);
	std::string key = getNodeCacheKey();
	
	if(globalFXPCache.count(key) > 0 && globalFXPCacheValid.count(key) > 0 && globalFXPCacheValid[key]) {
		cachedFXP = globalFXPCache[key];
		fxpCacheValid = true;
		ofLogNotice("scVST") << "📥 Loaded cached FXP for node '" << key << "' (" << cachedFXP.size() << " bytes)";
	} else {
		fxpCacheValid = false;
		ofLogVerbose("scVST") << "📭 No cached FXP found for node '" << key << "'";
	}
}

void scVST::saveCacheToGlobal() {
	if(!fxpCacheValid || cachedFXP.empty()) return;
	
	std::lock_guard<std::mutex> lock(globalCacheMutex);
	std::string key = getNodeCacheKey();
	
	globalFXPCache[key] = cachedFXP;
	globalFXPCacheValid[key] = true;
	
	ofLogNotice("scVST") << "📤 Saved FXP cache for node '" << key << "' (" << cachedFXP.size() << " bytes)";
}

void scVST::setTransportPlay(bool playing) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for transport control";
		return;
	}
	
	ofLogNotice("scVST") << "Setting transport " << (playing ? "PLAY" : "STOP");
	
	// Send to all instances
	std::vector<float> args = {playing ? 1.0f : 0.0f};
	sendTransportCommandToAllInstances("/transport_play", args);
}

void scVST::setTransportPosition(float position) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for transport control";
		return;
	}
	
	// Prevent feedback from our own changes
	transportFeedbackSuppressed = true;
	transportFeedbackClearTime = ofGetElapsedTimeMillis() + 100; // 100ms delay
	
	ofLogNotice("scVST") << "Setting transport position to " << position << " beats";
	
	std::vector<float> args = {position};
	sendTransportCommandToAllInstances("/transport_set", args);
	
	lastKnownPosition = position;
}

void scVST::resetTransport() {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for transport control";
		return;
	}
	
	ofLogNotice("scVST") << "Resetting transport (position=0, play=true)";
	
	// Prevent feedback
	transportFeedbackSuppressed = true;
	transportFeedbackClearTime = ofGetElapsedTimeMillis() + 200; // Longer delay for multiple commands
	
	// Set position to 0
	std::vector<float> posArgs = {0.0f};
	sendTransportCommandToAllInstances("/transport_set", posArgs);
	
	// Start playing
	std::vector<float> playArgs = {1.0f};
	sendTransportCommandToAllInstances("/transport_play", playArgs);
	
	// Update GUI parameters without triggering events
	transportPosition.setWithoutEventNotifications(0.0f);
	transportPlay.setWithoutEventNotifications(true);
	
	lastKnownPosition = 0.0f;
}

void scVST::setTempo(float bpm) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for tempo control";
		return;
	}
	
	//ofLogNotice("scVST") << "Setting tempo to " << bpm << " BPM";
	
	std::vector<float> args = {bpm};
	sendTransportCommandToAllInstances("/tempo", args);
}

void scVST::setTimeSignature(int num, int denom) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for time signature control";
		return;
	}
	
	//ofLogNotice("scVST") << "Setting time signature to " << num << "/" << denom;
	
	// Note: /time_sig takes int arguments, not float
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				try {
					ofxOscMessage timeSigMsg;
					timeSigMsg.setAddress("/u_cmd");
					timeSigMsg.addIntArg(synth->nodeID);
					timeSigMsg.addIntArg(2);
					timeSigMsg.addStringArg("/time_sig");
					timeSigMsg.addIntArg(num);
					timeSigMsg.addIntArg(denom);
					serverInstances.first->sendMsg(timeSigMsg);
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error setting time signature on synth " << synth->nodeID << ": " << e.what();
				}
			}
		}
	}
}

void scVST::queryTransportPosition() {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for transport query";
		return;
	}
	
	// Query from first instance only
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			try {
				isTransportQuerying = true;
				
				ofxOscMessage queryMsg;
				queryMsg.setAddress("/u_cmd");
				queryMsg.addIntArg(serverInstances.second[0]->nodeID);
				queryMsg.addIntArg(2);
				queryMsg.addStringArg("/transport_get");
				serverInstances.first->sendMsg(queryMsg);
				
				ofLogNotice("scVST") << "Querying transport position from first instance";
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error querying transport position: " << e.what();
				isTransportQuerying = false;
			}
			break; // Only query from first server
		}
	}
}

void scVST::handleTransportPosition(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 3) {
		int nodeID = msg.getArgAsInt32(0);
		float position = msg.getArgAsFloat(2);
		
		// Verify this is our VST instance
		if (!isMyVSTInstance(nodeID)) return;
		
		//ofLogVerbose("scVST") << "Transport position received: " << position << " beats from node " << nodeID;
		
		// Update GUI parameter if not suppressed and position changed significantly
		if(!transportFeedbackSuppressed && !isPresetLoading) {
			float positionDiff = abs(position - lastKnownPosition);
			if(positionDiff > 0.01f) { // Only update if position changed by > 0.01 beats
				transportPosition.setWithoutEventNotifications(position);
				lastKnownPosition = position;
			}
		}
		
		// Clear querying flag
		isTransportQuerying = false;
	}
}

void scVST::sendTransportCommandToAllInstances(const std::string& command, const std::vector<float>& args) {
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				try {
					ofxOscMessage transportMsg;
					transportMsg.setAddress("/u_cmd");
					transportMsg.addIntArg(synth->nodeID);
					transportMsg.addIntArg(2);
					transportMsg.addStringArg(command);
					
					// Add arguments
					for(float arg : args) {
						transportMsg.addFloatArg(arg);
					}
					
					serverInstances.first->sendMsg(transportMsg);
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error sending transport command " << command
					<< " to synth " << synth->nodeID << ": " << e.what();
				}
			}
		}
	}
}

void scVST::addMidiCCParameter(int ccNumber) {
	// Validate CC number
	if(ccNumber < 1 || ccNumber > 127) {
		ofLogError("scVST") << "Invalid MIDI CC number: " << ccNumber << " (must be 1-127)";
		return;
	}
	
	if(midiCCParameters.count(ccNumber) > 0) {
		ofLogWarning("scVST") << "MIDI CC " << ccNumber << " already exists, updating value";
		// Update existing parameter value and send to VST
		if(dynamicMidiCCParameters.count(ccNumber) > 0) {
			float currentValue = dynamicMidiCCParameters[ccNumber]->getParameter().get();
			sendMidiCC(ccNumber, currentValue);
		}
		return;
	}
	
	ofLogNotice("scVST") << "Adding MIDI CC " << ccNumber << " parameter";
	
	// Create MIDI CC parameter info
	MidiCCParameter ccParam;
	ccParam.ccNumber = ccNumber;
	ccParam.value = 0.0f;
	ccParam.displayName = "CC" + ofToString(ccNumber);
	ccParam.enabled = true;
	midiCCParameters[ccNumber] = ccParam;
	
	// Create GUI parameter
	string paramName = "CC" + ofToString(ccNumber);
	
	// Ensure parameter name is unique
	string uniqueParamName = paramName;
	int nameCounter = 1;
	while(getParameterGroup().contains(uniqueParamName)) {
		uniqueParamName = paramName + "_" + ofToString(nameCounter);
		nameCounter++;
	}
	
	// Create float parameter
	auto newParam = std::make_shared<ofParameter<float>>();
	newParam->set(uniqueParamName, 0.0f, 0.0f, 1.0f);
	
	// Store parameter to keep it alive
	dynamicMidiCCFloatParameters[ccNumber] = newParam;
	
	try {
		auto oceanodeParam = addParameter(*newParam);
		dynamicMidiCCParameters[ccNumber] = oceanodeParam;
		
		ofLogNotice("scVST") << "Successfully added MIDI CC parameter " << uniqueParamName;
		
		// Set up listener to send MIDI CC when parameter changes
		listeners.push(newParam->newListener([this, ccNumber](float &value) -> void {
			try {
				if(this == nullptr) return;
				
				// Update parameter info
				if(midiCCParameters.count(ccNumber) > 0) {
					midiCCParameters[ccNumber].value = value;
				}
				
				// Send MIDI CC if VST instances are available and not during preset loading
				if(!isPresetLoading &&
				   !synthInstances.empty() &&
				   areAllInstancesReady()) {
					
					//ofLogVerbose("scVST") << "Sending MIDI CC " << ccNumber
					//					 << " = " << value << " to VST";
					sendMidiCC(ccNumber, value);
				}
				
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in MIDI CC listener for CC " << ccNumber << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in MIDI CC listener for CC " << ccNumber;
			}
		}));
		
		// Add name editor and removal button
		addMidiCCNameEditor(ccNumber, uniqueParamName);
		addMidiCCRemovalButton(ccNumber, uniqueParamName);
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error adding MIDI CC parameter: " << e.what();
		// Clean up on error
		dynamicMidiCCFloatParameters.erase(ccNumber);
		dynamicMidiCCParameters.erase(ccNumber);
		midiCCParameters.erase(ccNumber);
	}
}

void scVST::removeMidiCCParameter(int ccNumber) {
	ofLogNotice("scVST") << "Removing MIDI CC " << ccNumber << " parameter";
	
	if(midiCCParameters.count(ccNumber) == 0) {
		ofLogWarning("scVST") << "MIDI CC " << ccNumber << " not found";
		return;
	}
	
	string paramName = midiCCParameters[ccNumber].displayName;
	
	// Remove main parameter
	if(dynamicMidiCCParameters.count(ccNumber) > 0) {
		try {
			string actualParamName = paramName;
			if(!getParameterGroup().contains(actualParamName)) {
				// Parameter might have been renamed, search for it
				for(int i = 0; i < getParameterGroup().size(); i++) {
					string currentName = getParameterGroup().get(i).getName();
					if(currentName.find("CC" + ofToString(ccNumber)) == 0) {
						actualParamName = currentName;
						break;
					}
				}
			}
			
			if(getParameterGroup().contains(actualParamName)) {
				removeParameter(actualParamName);
				ofLogNotice("scVST") << "Removed main MIDI CC parameter: " << actualParamName;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing MIDI CC parameter: " << e.what();
		}
		dynamicMidiCCParameters.erase(ccNumber);
	}
	
	// Remove stored float parameter
	if(dynamicMidiCCFloatParameters.count(ccNumber) > 0) {
		dynamicMidiCCFloatParameters.erase(ccNumber);
	}
	
	// Remove name editor from inspector
	if(dynamicMidiCCNameParameters.count(ccNumber) > 0) {
		try {
			vector<string> possibleNames = {
				"CC" + ofToString(ccNumber) + "_Name",
				"CC" + ofToString(ccNumber) + "_Name_1",
				"CC" + ofToString(ccNumber) + "_Name_2"
			};
			
			bool removed = false;
			for(const string& possibleName : possibleNames) {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed CC name editor: " << possibleName;
					removed = true;
					break;
				}
			}
			
			if(!removed) {
				ofLogWarning("scVST") << "Could not find CC name editor to remove for CC " << ccNumber;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing CC name editor: " << e.what();
		}
		dynamicMidiCCNameParameters.erase(ccNumber);
	}
	
	// Remove removal button from inspector
	if(dynamicMidiCCRemovalButtons.count(ccNumber) > 0) {
		try {
			vector<string> possibleNames = {
				"Remove CC" + ofToString(ccNumber),
				"Remove CC" + ofToString(ccNumber) + "_1",
				"Remove CC" + ofToString(ccNumber) + "_2"
			};
			
			bool removed = false;
			for(const string& possibleName : possibleNames) {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed CC removal button: " << possibleName;
					removed = true;
					break;
				}
			}
			
			if(!removed) {
				ofLogWarning("scVST") << "Could not find CC removal button to remove for CC " << ccNumber;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing CC removal button: " << e.what();
		}
		dynamicMidiCCRemovalButtons.erase(ccNumber);
	}
	
	// Remove from parameter info map
	midiCCParameters.erase(ccNumber);
	
	ofLogNotice("scVST") << "Finished removing MIDI CC " << ccNumber;
}

void scVST::removeAllMidiCCParameters() {
	ofLogNotice("scVST") << "Removing all MIDI CC parameters";
	
	try {
		// Create a copy of the keys to avoid iterator invalidation
		vector<int> ccNumbers;
		for(auto& ccParam : midiCCParameters) {
			ccNumbers.push_back(ccParam.first);
		}
		
		// Remove each CC parameter individually
		for(int ccNumber : ccNumbers) {
			try {
				removeMidiCCParameter(ccNumber);
			} catch(const std::exception& e) {
				ofLogWarning("scVST") << "Error removing MIDI CC " << ccNumber << ": " << e.what();
			} catch(...) {
				ofLogWarning("scVST") << "Unknown error removing MIDI CC " << ccNumber;
			}
		}
		
		// Force clear all maps as safety measure
		try {
			midiCCParameters.clear();
			dynamicMidiCCParameters.clear();
			dynamicMidiCCFloatParameters.clear();
			dynamicMidiCCNameParameters.clear();
			dynamicMidiCCRemovalButtons.clear();
		} catch(...) {}
		
		ofLogNotice("scVST") << "Finished removing all MIDI CC parameters";
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in removeAllMidiCCParameters: " << e.what();
	} catch(...) {
		ofLogError("scVST") << "Unknown error in removeAllMidiCCParameters";
	}
}

void scVST::sendMidiCC(int ccNumber, float value) {
	// Safety checks
	if(ccNumber < 1 || ccNumber > 127) {
		ofLogError("scVST") << "Invalid MIDI CC number: " << ccNumber;
		return;
	}
	
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for MIDI CC";
		return;
	}
	
	// Convert float (0.0-1.0) to MIDI value (0-127)
	int midiValue = (int)(ofClamp(value, 0.0f, 1.0f) * 127.0f);
	
	ofLogVerbose("scVST") << "Sending MIDI CC " << ccNumber << " = " << midiValue
	<< " (float: " << value << ") to all VST instances";
	
	// Send to all instances
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				try {
					// Use existing sendMidiToInstance method with CC message (0xB0)
					sendMidiToInstance(serverInstances.first, synth, midiChannel.get(), 0xB0, ccNumber, midiValue);
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error sending MIDI CC to synth " << synth->nodeID << ": " << e.what();
				}
			}
		}
	}
}

void scVST::addMidiCCNameEditor(int ccNumber, const string& paramName) {
	string nameEditorName = paramName + "_Name";
	string uniqueNameEditorName = nameEditorName;
	int nameEditorCounter = 1;
	while(getInspectorParameterGroup().contains(uniqueNameEditorName)) {
		uniqueNameEditorName = nameEditorName + "_" + ofToString(nameEditorCounter);
		nameEditorCounter++;
	}
	
	auto nameEditor = std::make_shared<ofParameter<string>>();
	nameEditor->set(uniqueNameEditorName, midiCCParameters[ccNumber].displayName);
	
	dynamicMidiCCNameParameters[ccNumber] = nameEditor;
	addInspectorParameter(*nameEditor);
	
	// Set up name change listener
	listeners.push(nameEditor->newListener([this, ccNumber](string &newName) -> void {
		if(this == nullptr) return;
		
		if(!newName.empty() && midiCCParameters.count(ccNumber) > 0) {
			string oldName = midiCCParameters[ccNumber].displayName;
			midiCCParameters[ccNumber].displayName = newName;
			ofLogNotice("scVST") << "Renamed MIDI CC " << ccNumber << " from '" << oldName << "' to '" << newName << "'";
		}
	}));
}

void scVST::addMidiCCRemovalButton(int ccNumber, const string& paramName) {
	string removeButtonName = "Remove " + paramName;
	string uniqueRemoveButtonName = removeButtonName;
	int removeNameCounter = 1;
	while(getInspectorParameterGroup().contains(uniqueRemoveButtonName)) {
		uniqueRemoveButtonName = removeButtonName + "_" + ofToString(removeNameCounter);
		removeNameCounter++;
	}
	
	auto removeButton = std::make_shared<ofParameter<void>>();
	removeButton->set(uniqueRemoveButtonName);
	
	dynamicMidiCCRemovalButtons[ccNumber] = removeButton;
	addInspectorParameter(*removeButton);
	
	listeners.push(removeButton->newListener([this, ccNumber](){
		ofLogNotice("scVST") << "Removing MIDI CC " << ccNumber << " via individual button";
		removeMidiCCParameter(ccNumber);
	}));
}

void scVST::propagateFirstInstanceViaFXP() {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available for propagation";
		return;
	}
	
	// Find first instance
	ofxSCSynth* firstInstance = nullptr;
	ofxSCServer* firstServer = nullptr;
	
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			firstInstance = serverInstances.second[0];
			firstServer = serverInstances.first;
			break;
		}
	}
	
	if(!firstInstance || !firstServer) {
		ofLogError("scVST") << "Could not find first VST instance for propagation";
		return;
	}
	
	ofLogNotice("scVST") << "Propagating complete VST state from instance " << firstInstance->nodeID;
	syncFirstInstanceToAllViaFXP(firstInstance->nodeID);
}

std::string scVST::createTempPropagateFXPPath() {
	std::string tempDir = ofFilePath::getUserHomeDir() + "/.tmp/";
	ofDirectory::createDirectory(tempDir, true, true);
	
	std::string safeName = getNodeCacheKey();
	// Replace any problematic characters in node name
	std::replace(safeName.begin(), safeName.end(), '/', '_');
	std::replace(safeName.begin(), safeName.end(), '\\', '_');
	std::replace(safeName.begin(), safeName.end(), ':', '_');
	std::replace(safeName.begin(), safeName.end(), ' ', '_');
	
	std::string filename = "oceanode_vst_propagate_" + safeName + "_" + ofToString(ofGetElapsedTimeMillis()) + ".fxp";
	return tempDir + filename;
}

void scVST::handleVSTMidi(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 3) {
		int nodeID = msg.getArgAsInt32(0);
		
		// PERFORMANCE: Quick checks without logging
		if (!isMyVSTInstance(nodeID)) return;
		
		int instanceIndex = getInstanceIndexFromNodeID(nodeID);
		if(instanceIndex != 0) return; // Only first instance
		
		// PERFORMANCE: Pre-allocate vector with known size to avoid reallocation
		int numMidiArgs = msg.getNumArgs() - 2;
		if(numMidiArgs <= 0) return;
		
		vector<uint8_t> midiBytes;
		midiBytes.reserve(numMidiArgs);  // OPTIMIZATION: Reserve space upfront
		for(int i = 2; i < msg.getNumArgs(); i++) {
			midiBytes.push_back((uint8_t)msg.getArgAsFloat(i));
		}
		
		if(midiBytes.size() >= 2) {
			uint8_t status = midiBytes[0];
			uint8_t statusType = status & 0xF0;
			
			// PERFORMANCE: Skip timing messages early
			if(status >= 0xF8) return;
			
			bool dataChanged = false;
			
			{
				// PERFORMANCE: Try lock to avoid blocking audio thread
				std::unique_lock<std::mutex> lock(midiUpdateMutex, std::try_to_lock);
				if(!lock.owns_lock()) {
					// Skip this MIDI message if we can't get the lock immediately
					return;
				}
				
				if(statusType == 0x90 && midiBytes.size() >= 3) {
					// Note On
					uint8_t note = midiBytes[1] & 0x7F;
					uint8_t velocity = midiBytes[2] & 0x7F;
					
					if(note < 128) {
						float newValue = (velocity > 0) ? (velocity / 127.0f) : 0.0f;
						if(currentNoteStates[note] != newValue) {
							currentNoteStates[note] = newValue;
							dataChanged = true;
						}
					}
				}
				else if(statusType == 0x80 && midiBytes.size() >= 3) {
					// Note Off
					uint8_t note = midiBytes[1] & 0x7F;
					
					if(note < 128 && currentNoteStates[note] != 0.0f) {
						currentNoteStates[note] = 0.0f;
						dataChanged = true;
					}
				}
				else if(statusType == 0xB0 && midiBytes.size() >= 3) {
					// Control Change
					uint8_t ccNumber = midiBytes[1] & 0x7F;
					uint8_t ccValue = midiBytes[2] & 0x7F;
					
					if(ccNumber < 128) {
						float newValue = ccValue / 127.0f;
						if(abs(currentCCStates[ccNumber] - newValue) > 0.001f) {
							currentCCStates[ccNumber] = newValue;
							dataChanged = true;
						}
					}
				}
				
				if(dataChanged) {
					midiOutputDirty = true;
				}
			}
		}
	}
}

void scVST::updateMidiOutputs() {
	// PERFORMANCE: Early exit with relaxed memory ordering (no fence needed)
	if(!midiOutputDirty.load(std::memory_order_relaxed)) return;
	
	uint64_t currentTime = ofGetElapsedTimeMillis();
	
	// PERFORMANCE: Limit updates to ~60fps (16ms intervals) to match Oceanode's refresh
	if(currentTime - lastMidiUpdateTime < 16) return;
	
	{
		// PERFORMANCE: Try lock to avoid blocking if another thread is updating
		std::unique_lock<std::mutex> lock(midiUpdateMutex, std::try_to_lock);
		if(!lock.owns_lock()) {
			// Another thread is updating, skip this frame
			return;
		}
		
		if(midiOutputDirty.load(std::memory_order_relaxed)) {
			// OPTIMIZATION: Update both parameters in one batch
			// Use move semantics to avoid deep copy if possible
			noteOut = currentNoteStates;
			ccOut = currentCCStates;
			
			midiOutputDirty.store(false, std::memory_order_relaxed);
			lastMidiUpdateTime = currentTime;
		}
	}
}
