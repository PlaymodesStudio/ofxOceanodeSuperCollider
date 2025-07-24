//
//  scVST.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//

#include "scVST.h"
#include "ofxSCSynth.h"

scVST::scVST() : scNode("VST") {
	isPresetLoading = false;
	pluginLoaded = false;
	hasPendingPresetData = false;
	parameterTimerActive = false;
	lastTouchedIndex = -1;
}

// Add this to the setup() method after the other parameters:

void scVST::setup(){
	
	scNode::addInput("in");
	
	// Basic parameters
	addParameter(numChannels.set("N Chan", 2, 1, 100));
	
	// MIDI parameters (integrated from scVSTI)
	addParameter(gate.set("Gate", {0}, {0}, {1}));
	addParameter(pitch.set("Pitch", {60}, {0}, {127}));
	addParameter(velocity.set("Velocity", {0.5}, {0}, {1}));
	addParameter(midiChannel.set("MIDI Chan", 1, 1, 16));
	
	// Search for VST plugins on setup
	searchForVSTPlugins();
	
	// VST Plugin selector dropdown
	if (!availablePlugins.empty()) {
		addParameterDropdown(pluginSelector, "Plugin", 0, availablePlugins);
		currentPluginPath = pluginPaths[0]; // Set default to first plugin
	} else {
		// Fallback if no plugins found
		availablePlugins.push_back("No VST plugins found");
		pluginPaths.push_back("");
		addParameterDropdown(pluginSelector, "Plugin", 0, availablePlugins);
	}
	
	// VST Program parameter - NOT SAVED IN PRESETS
	// Use special flags to exclude it from preset saving
	auto programParam = addParameter(vstProgram.set("Program", 0, 0, 127),
									ofxOceanodeParameterFlags_DisableSavePreset |
									ofxOceanodeParameterFlags_DisableSaveProject);
	
	// VST control parameters
	addParameter(openEditor.set("Editor"));
	addParameter(addLastTouched.set("Add Last"));
	
	// Inspector parameters
	addInspectorParameter(enableMultithreading.set("Multithreading", false));
	addInspectorParameter(removeAllParams.set("Remove All Params"));
	
	// Add button to refresh VST parameters
	ofParameter<void> refreshParams;
	addInspectorParameter(refreshParams.set("Refresh VST Params"));

	// Add button to capture current VST state
	ofParameter<void> captureState;
	addInspectorParameter(captureState.set("Capture VST State"));

	// Add button to query VST programs (optional, for program names)
	ofParameter<void> queryPrograms;
	addInspectorParameter(queryPrograms.set("Query VST Programs"));
	
	listeners.push(refreshParams.newListener([this]{
		ofLogNotice("scVST") << "Manual VST parameter refresh requested";
		queryAllVSTParameters();
	}));
	
	listeners.push(captureState.newListener([this]{
		ofLogNotice("scVST") << "Manual VST state capture requested";
		captureCurrentVSTState();
	}));
	
	listeners.push(queryPrograms.newListener([this]{
		ofLogNotice("scVST") << "Querying VST programs";
		queryVSTPrograms();
	}));
	
	// VST Program change listener
	listeners.push(vstProgram.newListener([this](int &programIndex){
		if(!isPresetLoading) {  // Only respond to user changes, not preset loading
			setVSTProgram(programIndex);
		}
	}));
	
	// Set up other listeners...
	listeners.push(numChannels.newListener([this](int &i){
		ofLogNotice("scVST") << "Channels changed to " << i << ", will need " << calculateNumInstances() << " instances";
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
				
				ofLogNotice("scVST") << "Opening editor for first instance on server";
			}
		}
	}));
	
	listeners.push(addLastTouched.newListener([this]{
		if(lastTouchedIndex >= 0) {
			ofLogNotice("scVST") << "Adding last touched parameter: " << lastTouchedIndex;
			addParameterToGUI(lastTouchedIndex);
		} else {
			ofLogWarning("scVST") << "No last touched parameter to add (index: " << lastTouchedIndex << ")";
		}
	}));
	
	listeners.push(removeAllParams.newListener([this]{
		removeAllDynamicParameters();
	}));
	
	// Plugin selector listener
	listeners.push(pluginSelector.newListener([this](int &selection){
		if (selection >= 0 && selection < pluginPaths.size()) {
			currentPluginPath = pluginPaths[selection];
			loadSelectedPlugin();
		}
	}));
	
	// MIDI gate listener (integrated from scVSTI)
	listeners.push(gate.newListener([this](vector<int> &gates){
		processGates(gates);
	}));
	
	listeners.push(resendParams.newListener([this](){
		// Apply numChannels to all instances
		for(auto& serverInstances : synthInstances){
			for(auto synth : serverInstances.second){
				if(synth != nullptr){
					synth->set("inChannels", numChannels);
				}
			}
		}
	}));
	
	scNode::addOutput("out");
	
	// Set up update listener for parameter timer
	listeners.push(ofEvents().update.newListener([this](ofEventArgs&) {
		if(parameterTimerActive) {
			uint64_t currentTime = ofGetElapsedTimeMillis();
			if(currentTime - parameterTimerStart >= parameterTimerDelay) {
				parameterTimerActive = false;
				ofLogNotice("scVST") << "Timer finished, applying parameters now";
				applyPendingPresetData();
			}
		}
	}));
}

int scVST::calculateNumInstances() const {
	// Each instance handles 2 channels (stereo pair)
	// For odd numbers, we create an extra instance and use only its left channel
	return (numChannels.get() + 1) / 2;
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
	
	// Search for VST plugins
	for (const string& path : vstSearchPaths) {
		ofDirectory dir(path);
		if (dir.exists()) {
			dir.listDir();
			for (int i = 0; i < dir.size(); i++) {
				string filename = dir.getName(i);
				string filepath = dir.getPath(i);
				
				// Check for VST extensions
				if (ofToLower(ofFilePath::getFileExt(filename)) == "vst" ||
					ofToLower(ofFilePath::getFileExt(filename)) == "vst3" ||
					filename.find(".vst") != string::npos) {
					
					string pluginName = ofFilePath::getBaseName(filename);
					availablePlugins.push_back(pluginName);
					pluginPaths.push_back(filepath);
				}
			}
		}
	}
	
	// If no plugins found, add a default message
	if (availablePlugins.empty()) {
		availablePlugins.push_back("No VST plugins found");
		pluginPaths.push_back("");
	}
}

void scVST::loadSelectedPlugin() {
	if (currentPluginPath.empty() || isPresetLoading) return;
	
	ofLogNotice("scVST") << "Loading plugin: " << currentPluginPath;
	
	// Clear existing parameter mappings
	parameterInfoMap.clear();
	
	// Remove dynamic parameters from GUI
	removeAllDynamicParameters();
	
	// Load plugin on all instances
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				// Close current plugin if any
				ofxOscMessage closeMsg;
				closeMsg.setAddress("/u_cmd");
				closeMsg.addIntArg(synth->nodeID);
				closeMsg.addIntArg(2);
				closeMsg.addStringArg("/close");
				serverInstances.first->sendMsg(closeMsg);
				
				// Open new plugin with multithreading setting
				ofxOscMessage openMsg;
				openMsg.setAddress("/u_cmd");
				openMsg.addIntArg(synth->nodeID);
				openMsg.addIntArg(2);
				openMsg.addStringArg("/open");
				openMsg.addStringArg(currentPluginPath);
				openMsg.addIntArg(1); // Request GUI editor
				openMsg.addIntArg(enableMultithreading.get() ? 1 : 0); // Multithreading setting
				openMsg.addIntArg(0); // Normal mode
				serverInstances.first->sendMsg(openMsg);
			}
		}
		
		// Query parameters from first instance only (they should all be the same)
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			ofLogNotice("scVST") << "Querying initial parameters after plugin load";
			
			// Use the efficient /param_query method to get ALL parameters at once
			ofxOscMessage paramQueryMsg;
			paramQueryMsg.setAddress("/u_cmd");
			paramQueryMsg.addIntArg(serverInstances.second[0]->nodeID);
			paramQueryMsg.addIntArg(2);
			paramQueryMsg.addStringArg("/param_query");
			paramQueryMsg.addIntArg(0);    // Start parameter index
			paramQueryMsg.addIntArg(-1);   // -1 means query ALL parameters
			serverInstances.first->sendMsg(paramQueryMsg);
		}
	}
	
	pluginLoaded = true;
}

void scVST::handleVSTParam(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 4) {
		int nodeID = msg.getArgAsInt32(0);
		int paramIndex = (int)msg.getArgAsFloat(2);
		float value = msg.getArgAsFloat(3);
		
		// Verify this is our VST instance
		if (!isMyVSTInstance(nodeID)) return;
		
		ofLogNotice("scVST") << "VST Parameter " << paramIndex << " = " << value;
		
		// Update parameter info
		if(parameterInfoMap.count(paramIndex) == 0) {
			VSTParameterInfo info;
			info.index = paramIndex;
			info.displayName = "Param" + ofToString(paramIndex); // Use simple default name
			info.value = value;
			parameterInfoMap[paramIndex] = info;
		} else {
			parameterInfoMap[paramIndex].value = value;
			// NEVER overwrite user-defined parameter names
			// The displayName should only be changed by the user via the inspector
		}
		
		// Update GUI parameter if it exists
		updateParameterValue(paramIndex, value);
	}
}

void scVST::handleVSTAuto(ofxOscMessage& msg) {
	if (msg.getNumArgs() >= 4) {
		int nodeID = msg.getArgAsInt32(0);
		int paramIndex = (int)msg.getArgAsFloat(2);
		float value = msg.getArgAsFloat(3);
		
		// Verify this is our VST instance
		if (!isMyVSTInstance(nodeID)) return;
		
		ofLogNotice("scVST") << "VST Parameter " << paramIndex << " automated to " << value;
		
		// Update last touched parameter
		lastTouchedIndex = paramIndex;
		
		// Update parameter info
		if(parameterInfoMap.count(paramIndex) == 0) {
			VSTParameterInfo info;
			info.index = paramIndex;
			info.displayName = "Param" + ofToString(paramIndex);
			info.value = value;
			parameterInfoMap[paramIndex] = info;
		} else {
			parameterInfoMap[paramIndex].value = value;
		}
		
		// Update GUI parameter if it exists
		updateParameterValue(paramIndex, value);
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
		
		ofLogNotice("scVST") << "VST Plugin opened - Success: " << success
							<< ", Has Editor: " << hasEditor
							<< ", Latency: " << latency;
		
		if(success && hasPendingPresetData) {
			ofLogNotice("scVST") << "Plugin ready, applying pending preset data";
			// Cancel any pending timer since we got the event
			parameterTimerActive = false;
			applyPendingPresetData();
		}
	}
}

void scVST::addParameterToGUI(int paramIndex) {
	// Safety check
	if(paramIndex < 0) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}
	
	if(dynamicParameters.count(paramIndex) > 0) {
		// Parameter already exists, just update its value
		if(parameterInfoMap.count(paramIndex) > 0) {
			dynamicParameters[paramIndex]->getParameter().setWithoutEventNotifications(parameterInfoMap[paramIndex].value);
		}
		ofLogNotice("scVST") << "Parameter " << paramIndex << " already exists in GUI";
		return;
	}
	
	// Create parameter info if it doesn't exist
	if(parameterInfoMap.count(paramIndex) == 0) {
		VSTParameterInfo info;
		info.index = paramIndex;
		info.displayName = "Param" + ofToString(paramIndex);
		info.value = 0.0f;
		parameterInfoMap[paramIndex] = info;
		ofLogNotice("scVST") << "Created parameter info for index " << paramIndex;
	}
	
	// Create new parameter
	string paramName = parameterInfoMap[paramIndex].displayName;
	
	// Ensure parameter name is unique to avoid conflicts
	string uniqueParamName = paramName;
	int nameCounter = 1;
	while(getParameterGroup().contains(uniqueParamName)) {
		uniqueParamName = paramName + "_" + ofToString(nameCounter);
		nameCounter++;
	}
	
	// Create parameter directly in the parameters group to avoid scope issues
	auto newParam = std::make_shared<ofParameter<float>>();
	newParam->set(uniqueParamName, parameterInfoMap[paramIndex].value, 0.0f, 1.0f);
	
	// Store the parameter to keep it alive BEFORE adding to GUI
	dynamicFloatParameters[paramIndex] = newParam;
	
	try {
		auto oceanodeParam = addParameter(*newParam);
		dynamicParameters[paramIndex] = oceanodeParam;
		
		ofLogNotice("scVST") << "Successfully added parameter " << uniqueParamName << " to GUI";
		
		// Set up listener for this parameter with better error handling
		listeners.push(newParam->newListener([this, paramIndex](float &value) -> void {
			try {
				if(this == nullptr) return; // Safety check
				if(!isPresetLoading && parameterInfoMap.count(paramIndex) > 0) {
					setVSTParameter(paramIndex, value);
					parameterInfoMap[paramIndex].value = value;
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in parameter listener for index " << paramIndex << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in parameter listener for index " << paramIndex;
			}
		}));
		
		// Add name editor to inspector
		string inspectorName = uniqueParamName + "_Name";
		// Ensure inspector parameter name is also unique
		int inspectorCounter = 1;
		while(getInspectorParameterGroup().contains(inspectorName)) {
			inspectorName = uniqueParamName + "_Name_" + ofToString(inspectorCounter);
			inspectorCounter++;
		}
		
		auto nameEditor = std::make_shared<ofParameter<string>>();
		nameEditor->set(inspectorName, uniqueParamName);
		
		// Store the name editor parameter to keep it alive BEFORE adding to inspector
		dynamicStringParameters[paramIndex] = nameEditor;
		
		addInspectorParameter(*nameEditor);
		
		// Set up listener for name changes with better error handling
		listeners.push(nameEditor->newListener([this, paramIndex](string &newName) -> void {
			try {
				if(this == nullptr) return; // Safety check
				if(!newName.empty() && parameterInfoMap.count(paramIndex) > 0 &&
				   newName != parameterInfoMap[paramIndex].displayName) {
					// Update the parameter info
					parameterInfoMap[paramIndex].displayName = newName;
					
					// Update the main parameter name
					if(dynamicParameters.count(paramIndex) > 0) {
						dynamicParameters[paramIndex]->getParameter().setName(newName);
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in name editor listener for index " << paramIndex << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in name editor listener for index " << paramIndex;
			}
		}));
		
		// Add remove button to inspector
		string buttonName = "Remove " + uniqueParamName;
		// Ensure button name is also unique
		int buttonCounter = 1;
		while(getInspectorParameterGroup().contains(buttonName)) {
			buttonName = "Remove " + uniqueParamName + "_" + ofToString(buttonCounter);
			buttonCounter++;
		}
		
		auto removeButton = std::make_shared<ofParameter<void>>();
		removeButton->set(buttonName);
		
		// Store the remove button parameter to keep it alive BEFORE adding to inspector
		dynamicRemovalButtons[paramIndex] = removeButton;
		
		addInspectorParameter(*removeButton);
		
		// Set up listener for removal button with better error handling
		listeners.push(removeButton->newListener([this, paramIndex]() -> void {
			try {
				if(this == nullptr) return; // Safety check
				removeParameterFromGUI(paramIndex);
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error in removal button listener for index " << paramIndex << ": " << e.what();
			} catch(...) {
				ofLogError("scVST") << "Unknown error in removal button listener for index " << paramIndex;
			}
		}));
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error adding parameter to GUI: " << e.what();
		// Clean up on error
		dynamicFloatParameters.erase(paramIndex);
		dynamicParameters.erase(paramIndex);
	}
}

void scVST::removeParameterFromGUI(int paramIndex) {
	ofLogNotice("scVST") << "Removing parameter " << paramIndex << " from GUI";
	
	// Safety check
	if(parameterInfoMap.count(paramIndex) == 0) {
		ofLogWarning("scVST") << "Parameter " << paramIndex << " not found in parameter info map";
		return;
	}
	
	string paramName = parameterInfoMap[paramIndex].displayName;
	
	// Remove main parameter
	if(dynamicParameters.count(paramIndex) > 0) {
		try {
			// Try to find the actual parameter name in the group
			string actualParamName = paramName;
			if(!getParameterGroup().contains(actualParamName)) {
				// Parameter might have been renamed with a suffix, search for it
				for(int i = 0; i < getParameterGroup().size(); i++) {
					string currentName = getParameterGroup().get(i).getName();
					if(currentName.find(paramName) == 0) {
						actualParamName = currentName;
						break;
					}
				}
			}
			
			if(getParameterGroup().contains(actualParamName)) {
				removeParameter(actualParamName);
				ofLogNotice("scVST") << "Removed main parameter: " << actualParamName;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing main parameter: " << e.what();
		}
		dynamicParameters.erase(paramIndex);
	}
	
	// Remove stored float parameter
	if(dynamicFloatParameters.count(paramIndex) > 0) {
		dynamicFloatParameters.erase(paramIndex);
	}
	
	// Remove name editor from inspector
	if(dynamicStringParameters.count(paramIndex) > 0) {
		try {
			// Try multiple possible names for the name editor
			vector<string> possibleNames = {
				paramName + "_Name",
				paramName + "_Name_1",
				paramName + "_Name_2"
			};
			
			bool removed = false;
			for(const string& possibleName : possibleNames) {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed name editor: " << possibleName;
					removed = true;
					break;
				}
			}
			
			if(!removed) {
				ofLogWarning("scVST") << "Could not find name editor parameter to remove for " << paramName;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing name editor parameter: " << e.what();
		}
		dynamicStringParameters.erase(paramIndex);
	}
	
	// Remove removal button from inspector
	if(dynamicRemovalButtons.count(paramIndex) > 0) {
		try {
			// Try multiple possible names for the removal button
			vector<string> possibleNames = {
				"Remove " + paramName,
				"Remove " + paramName + "_1",
				"Remove " + paramName + "_2"
			};
			
			bool removed = false;
			for(const string& possibleName : possibleNames) {
				if(getInspectorParameterGroup().contains(possibleName)) {
					removeInspectorParameter(possibleName);
					ofLogNotice("scVST") << "Removed removal button: " << possibleName;
					removed = true;
					break;
				}
			}
			
			if(!removed) {
				ofLogWarning("scVST") << "Could not find removal button parameter to remove for " << paramName;
			}
		} catch(const std::exception& e) {
			ofLogError("scVST") << "Error removing removal button parameter: " << e.what();
		}
		dynamicRemovalButtons.erase(paramIndex);
	}
	
	ofLogNotice("scVST") << "Finished removing parameter " << paramIndex;
}

void scVST::removeAllDynamicParameters() {
	ofLogNotice("scVST") << "Removing all dynamic parameters";
	
	try {
		// Create a copy of the keys to avoid iterator invalidation
		vector<int> paramIndices;
		for(auto& param : dynamicParameters) {
			paramIndices.push_back(param.first);
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
		} catch(...) {}
		
		try {
			dynamicFloatParameters.clear();
		} catch(...) {}
		
		try {
			dynamicStringParameters.clear();
		} catch(...) {}
		
		try {
			dynamicRemovalButtons.clear();
		} catch(...) {}
		
		ofLogNotice("scVST") << "Finished removing all dynamic parameters";
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in removeAllDynamicParameters: " << e.what();
	} catch(...) {
		ofLogError("scVST") << "Unknown error in removeAllDynamicParameters";
	}
}

void scVST::updateParameterValue(int paramIndex, float value) {
	if(dynamicParameters.count(paramIndex) > 0) {
		dynamicParameters[paramIndex]->getParameter().setWithoutEventNotifications(value);
	}
	
	if(parameterInfoMap.count(paramIndex) > 0) {
		parameterInfoMap[paramIndex].value = value;
	}
}

void scVST::setVSTParameter(int paramIndex, float value) {
	// Safety checks
	if(paramIndex < 0) {
		ofLogError("scVST") << "Invalid parameter index: " << paramIndex;
		return;
	}
	
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available to set parameter";
		return;
	}
	
	ofLogNotice("scVST") << "Setting VST parameter " << paramIndex << " to " << value << " on all instances";
	
	// Apply parameter change to ALL instances
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				try {
					ofxOscMessage setMsg;
					setMsg.setAddress("/u_cmd");
					setMsg.addIntArg(synth->nodeID);
					setMsg.addIntArg(2);
					setMsg.addStringArg("/set");
					setMsg.addIntArg(paramIndex);
					setMsg.addFloatArg(value);
					serverInstances.first->sendMsg(setMsg);
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error setting parameter on synth " << synth->nodeID << ": " << e.what();
				}
			}
		}
	}
}

bool scVST::isMyVSTInstance(int nodeID) const {
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr && synth->nodeID == nodeID) {
				return true;
			}
		}
	}
	return false;
}

void scVST::restoreAllParameterValues() {
	for(auto& param : dynamicParameters) {
		int paramIndex = param.first;
		float value = param.second->getParameter().get();
		setVSTParameter(paramIndex, value);
	}
}

// MIDI functionality (integrated from scVSTI)
void scVST::processGates(vector<int>& gates) {
	// Ensure we have enough space in our tracking vectors
	if (previousGates.size() != gates.size()) {
		previousGates.resize(gates.size(), 0);
		activeNotes.resize(gates.size(), -1); // -1 means no active note
	}
	
	// Get current pitch and velocity vectors
	auto currentPitch = pitch.get();
	auto currentVelocity = velocity.get();
	
	// Process each gate
	for (int i = 0; i < gates.size(); i++) {
		int currentGate = gates[i];
		int previousGate = (i < previousGates.size()) ? previousGates[i] : 0;
		
		// Rising edge - gate went from 0 to 1
		if (currentGate == 1 && previousGate == 0) {
			// Get pitch for this index (with bounds checking)
			int noteNumber = 60; // Default middle C
			if (i < currentPitch.size()) {
				noteNumber = ofClamp(currentPitch[i], 0, 127);
			} else if (!currentPitch.empty()) {
				noteNumber = ofClamp(currentPitch[0], 0, 127); // Use first pitch if index out of bounds
			}
			
			// Get velocity for this index (with bounds checking)
			float vel = 0.5; // Default velocity
			if (i < currentVelocity.size()) {
				vel = ofClamp(currentVelocity[i], 0.0, 1.0);
			} else if (!currentVelocity.empty()) {
				vel = ofClamp(currentVelocity[0], 0.0, 1.0); // Use first velocity if index out of bounds
			}
			
			// Convert velocity to MIDI range (0-127)
			int midiVelocity = int(vel * 127);
			
			// Send note on
			sendMidiNoteOn(midiChannel.get(), noteNumber, midiVelocity);
			
			// Store the active note
			activeNotes[i] = noteNumber;
		}
		
		// Falling edge - gate went from 1 to 0
				 else if (currentGate == 0 && previousGate == 1) {
			   // Send note off for the previously active note
			   if (i < activeNotes.size() && activeNotes[i] >= 0) {
				   sendMidiNoteOff(midiChannel.get(), activeNotes[i]);
				   activeNotes[i] = -1; // Clear active note
			   }
		   }
	   }
	   
	   // Update previous gates for next comparison
	   previousGates = gates;
   }

   void scVST::sendMidiNoteOn(int channel, int pitch, int velocity) {
	   for(auto& serverInstances : synthInstances){
		   for(auto synth : serverInstances.second){
			   if(synth != nullptr){
				   ofxOscMessage m;
				   m.setAddress("/u_cmd");
				   m.addIntArg(synth->nodeID);
				   m.addIntArg(2); // VSTPlugin synthIndex
				   m.addStringArg("/midi_msg"); // MIDI command
				   
				   // Create MIDI note on message [status, data1, data2]
				   std::vector<int8_t> midiData;
				   midiData.push_back(0x90 | ((channel - 1) & 0x0F)); // Note on + channel (0-based)
				   midiData.push_back(pitch & 0x7F); // Note number
				   midiData.push_back(velocity & 0x7F); // Velocity
				   
				   // Convert to ofBuffer for blob
				   ofBuffer buffer;
				   buffer.set((char*)midiData.data(), midiData.size());
				   m.addBlobArg(buffer);
				   m.addFloatArg(0.0f); // detune
				   serverInstances.first->sendMsg(m);
			   }
		   }
	   }
   }

   void scVST::sendMidiNoteOff(int channel, int pitch) {
	   for(auto& serverInstances : synthInstances){
		   for(auto synth : serverInstances.second){
			   if(synth != nullptr){
				   ofxOscMessage m;
				   m.setAddress("/u_cmd");
				   m.addIntArg(synth->nodeID);
				   m.addIntArg(2); // VSTPlugin synthIndex
				   m.addStringArg("/midi_msg"); // MIDI command
				   
				   // Create MIDI note off message [status, data1, data2]
				   std::vector<int8_t> midiData;
				   midiData.push_back(0x80 | ((channel - 1) & 0x0F)); // Note off + channel (0-based)
				   midiData.push_back(pitch & 0x7F); // Note number
				   midiData.push_back(0x40); // Release velocity (64)
				   
				   // Convert to ofBuffer for blob
				   ofBuffer buffer;
				   buffer.set((char*)midiData.data(), midiData.size());
				   m.addBlobArg(buffer);
				   m.addFloatArg(0.0f); // detune
				   serverInstances.first->sendMsg(m);
			   }
		   }
	   }
   }

   void scVST::queryAllVSTParameters() {
	   ofLogNotice("scVST") << "Querying all VST parameters before saving preset";
	   
	   // Query parameters from the first instance of each server (they should all have same values)
	   for(auto& serverInstances : synthInstances) {
		   if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			   ofLogNotice("scVST") << "Using alternative approach: query VST program data directly";
			   
			   // Alternative approach: Instead of trying to query individual parameters,
			   // let's use the VST's built-in program/preset system which captures ALL internal state
			   // This is much more reliable for complex synths like Diva
			   
			   // First, try to get the current program data (this includes ALL parameter states)
			   ofxOscMessage getProgramMsg;
			   getProgramMsg.setAddress("/u_cmd");
			   getProgramMsg.addIntArg(serverInstances.second[0]->nodeID);
			   getProgramMsg.addIntArg(2);
			   getProgramMsg.addStringArg("/program_write");
			   getProgramMsg.addStringArg("temp_preset_capture.fxp");  // Temporary file
			   getProgramMsg.addIntArg(1); // Asynchronous
			   serverInstances.first->sendMsg(getProgramMsg);
			   
			   // Also try a comprehensive parameter range query as backup
			   // Query in smaller, more reliable chunks
			   for(int startParam = 0; startParam < 512; startParam += 32) {
				   ofxOscMessage getParamsMsg;
				   getParamsMsg.setAddress("/u_cmd");
				   getParamsMsg.addIntArg(serverInstances.second[0]->nodeID);
				   getParamsMsg.addIntArg(2);
				   getParamsMsg.addStringArg("/getn");
				   getParamsMsg.addIntArg(startParam);
				   getParamsMsg.addIntArg(32);
				   serverInstances.first->sendMsg(getParamsMsg);
				   
				   ofSleepMillis(5); // Small delay between queries
			   }
			   
			   // We only need to query from one server since all instances should have the same parameter values
			   break;
		   }
	   }
	   
	   // Give VST time to respond
	   ofLogNotice("scVST") << "Waiting for parameter responses...";
	   
	   int initialParamCount = parameterInfoMap.size();
	   
	   // Shorter wait time since we're not expecting hundreds of individual responses
	   for(int i = 0; i < 50; i++) {
		   ofSleepMillis(30);
		   
		   if(i % 10 == 0) {
			   int currentParamCount = parameterInfoMap.size();
			   ofLogNotice("scVST") << "Parameter query progress: " << currentParamCount << " parameters (iteration " << i << ")";
		   }
	   }
	   
	   ofLogNotice("scVST") << "Parameter query complete. Total parameters now tracked: " << parameterInfoMap.size();
   }

   void scVST::queryAllVSTParametersSync() {
	   ofLogNotice("scVST") << "Performing comprehensive synchronous parameter query";
	   
	   for(auto& serverInstances : synthInstances) {
		   if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			   // Try multiple approaches to get as many parameters as possible
			   
			   // Approach 1: Individual parameter queries for known parameters
			   for(int paramIndex = 0; paramIndex < 256; paramIndex++) {
				   ofxOscMessage getParamMsg;
				   getParamMsg.setAddress("/u_cmd");
				   getParamMsg.addIntArg(serverInstances.second[0]->nodeID);
				   getParamMsg.addIntArg(2);
				   getParamMsg.addStringArg("/get");
				   getParamMsg.addIntArg(paramIndex);
				   serverInstances.first->sendMsg(getParamMsg);
				   
				   if(paramIndex % 10 == 0) {
					   ofSleepMillis(10); // Small delays to avoid overwhelming
				   }
			   }
			   
			   // Approach 2: Bulk queries in small chunks
			   for(int startParam = 0; startParam < 1024; startParam += 16) {
				   ofxOscMessage getBulkMsg;
				   getBulkMsg.setAddress("/u_cmd");
				   getBulkMsg.addIntArg(serverInstances.second[0]->nodeID);
				   getBulkMsg.addIntArg(2);
				   getBulkMsg.addStringArg("/getn");
				   getBulkMsg.addIntArg(startParam);
				   getBulkMsg.addIntArg(16);
				   serverInstances.first->sendMsg(getBulkMsg);
				   
				   ofSleepMillis(5);
			   }
			   
			   break;
		   }
	   }
	   
	   // Wait for responses
	   ofLogNotice("scVST") << "Waiting for comprehensive parameter responses...";
	   
	   int initialCount = parameterInfoMap.size();
	   for(int i = 0; i < 100; i++) {
		   ofSleepMillis(20);
		   
		   if(i % 20 == 0) {
			   int currentCount = parameterInfoMap.size();
			   ofLogNotice("scVST") << "Comprehensive query progress: " << currentCount << " parameters";
			   
			   if(currentCount > initialCount + 50) {
				   ofLogNotice("scVST") << "Found significant parameters, continuing...";
				   initialCount = currentCount;
			   }
		   }
	   }
	   
	   ofLogNotice("scVST") << "Comprehensive parameter query complete. Total: " << parameterInfoMap.size();
   }

   void scVST::captureCurrentVSTState() {
	   ofLogNotice("scVST") << "Capturing current VST state by querying all parameters individually";
	   
	   for(auto& serverInstances : synthInstances) {
		   if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			   
			   // Query a large range of parameters individually
			   // This is more reliable than bulk queries for some VSTs
			   for(int paramIndex = 0; paramIndex < 512; paramIndex++) {
				   ofxOscMessage getParamMsg;
				   getParamMsg.setAddress("/u_cmd");
				   getParamMsg.addIntArg(serverInstances.second[0]->nodeID);
				   getParamMsg.addIntArg(2);
				   getParamMsg.addStringArg("/get");
				   getParamMsg.addIntArg(paramIndex);
				   serverInstances.first->sendMsg(getParamMsg);
				   
				   // Small delay every 20 parameters to avoid overwhelming
				   if(paramIndex % 20 == 0) {
					   ofSleepMillis(10);
				   }
			   }
			   
			   break;
		   }
	   }
	   
	   // Wait for responses
	   ofLogNotice("scVST") << "Waiting for individual parameter responses...";
	   
	   int initialCount = parameterInfoMap.size();
	   for(int i = 0; i < 150; i++) {  // Longer wait for individual queries
		   ofSleepMillis(20);
		   
		   if(i % 30 == 0) {
			   int currentCount = parameterInfoMap.size();
			   ofLogNotice("scVST") << "State capture progress: " << currentCount << " parameters (iteration " << i << ")";
		   }
	   }
	   
	   int finalCount = parameterInfoMap.size();
	   ofLogNotice("scVST") << "VST state capture complete. Captured " << (finalCount - initialCount) << " new parameters. Total: " << finalCount;
   }

   void scVST::handleParameterQueryResponse(ofxOscMessage& msg) {
	   // Handle bulk parameter query responses from /getn command
	   // The /getn response format might be: nodeID, synthIndex, startIndex, numParams, value1, value2, ...
	   // OR it might be: nodeID, synthIndex, param1Index, param1Value, param2Index, param2Value, ...
	   
	   if (msg.getNumArgs() >= 4) {
		   int nodeID = msg.getArgAsInt32(0);
		   
		   // Verify this is our VST instance
		   if (!isMyVSTInstance(nodeID)) return;
		   
		   ofLogNotice("scVST") << "Received bulk parameter response with " << msg.getNumArgs() << " arguments";
		   
		   // Try to parse as continuous values format: nodeID, synthIndex, startIndex, numParams, values...
		   if(msg.getNumArgs() >= 5) {
			   int startIndex = (int)msg.getArgAsFloat(2);
			   int numParams = (int)msg.getArgAsFloat(3);
			   
			   ofLogNotice("scVST") << "Parsing as continuous format: startIndex=" << startIndex << ", numParams=" << numParams;
			   
			   // Check if we have enough arguments for this format
			   if((4 + numParams) <= msg.getNumArgs()) {
				   // Process each parameter value in the response
				   for(int i = 0; i < numParams; i++) {
					   int paramIndex = startIndex + i;
					   float value = msg.getArgAsFloat(4 + i);
					   
					   // Only process non-zero values or if we don't have this parameter yet
					   if(value != 0.0f || parameterInfoMap.count(paramIndex) == 0) {
						   // Update or create parameter info
						   if(parameterInfoMap.count(paramIndex) == 0) {
							   VSTParameterInfo info;
							   info.index = paramIndex;
							   info.displayName = "Param" + ofToString(paramIndex);
							   info.value = value;
							   parameterInfoMap[paramIndex] = info;
						   } else {
							   parameterInfoMap[paramIndex].value = value;
						   }
						   
						   // Update GUI parameter if it exists
						   updateParameterValue(paramIndex, value);
						   
						   if(i < 10 || value != 0.0f) { // Log first 10 or non-zero values
							   ofLogNotice("scVST") << "Updated param " << paramIndex << " = " << value;
						   }
					   }
				   }
				   
				   ofLogNotice("scVST") << "Updated " << numParams << " parameters from bulk query";
				   return;
			   }
		   }
		   
		   // Try to parse as index-value pairs: nodeID, synthIndex, index1, value1, index2, value2, ...
		   ofLogNotice("scVST") << "Trying to parse as index-value pairs";
		   int processedParams = 0;
		   for(int i = 2; i < msg.getNumArgs() - 1; i += 2) {
			   try {
				   int paramIndex = (int)msg.getArgAsFloat(i);
				   float value = msg.getArgAsFloat(i + 1);
				   
				   // Sanity check for parameter index
				   if(paramIndex >= 0 && paramIndex < 4096) {
					   // Update or create parameter info
					   if(parameterInfoMap.count(paramIndex) == 0) {
						   VSTParameterInfo info;
						   info.index = paramIndex;
						   info.displayName = "Param" + ofToString(paramIndex);
						   info.value = value;
						   parameterInfoMap[paramIndex] = info;
					   } else {
						   parameterInfoMap[paramIndex].value = value;
					   }
					   
					   // Update GUI parameter if it exists
					   updateParameterValue(paramIndex, value);
					   
					   if(processedParams < 10 || value != 0.0f) {
						   ofLogNotice("scVST") << "Updated param " << paramIndex << " = " << value;
					   }
					   processedParams++;
				   }
			   } catch(const std::exception& e) {
				   ofLogError("scVST") << "Error parsing parameter at position " << i << ": " << e.what();
				   break;
			   }
		   }
		   
		   ofLogNotice("scVST") << "Processed " << processedParams << " parameters from index-value pairs";
	   }
   }

   void scVST::saveCurrentVSTProgram(ofJson &json) {
	   ofLogNotice("scVST") << "Saving current VST program data";
	   
	   // Use the VST plugin's built-in program save functionality
	   // This captures the complete internal state much more reliably than individual parameters
	   
	   for(auto& serverInstances : synthInstances) {
		   if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			   // Create a unique temporary file name for this preset
			   string tempFileName = "oceanode_vst_temp_" + ofToString(ofGetElapsedTimeMillis()) + ".fxp";
			   
			   // Save current program to a temporary file
			   ofxOscMessage saveProgramMsg;
			   saveProgramMsg.setAddress("/u_cmd");
			   saveProgramMsg.addIntArg(serverInstances.second[0]->nodeID);
			   saveProgramMsg.addIntArg(2);
			   saveProgramMsg.addStringArg("/program_write");
			   saveProgramMsg.addStringArg(tempFileName);
			   saveProgramMsg.addIntArg(0); // Synchronous (changed from async to ensure completion)
			   serverInstances.first->sendMsg(saveProgramMsg);
			   
			   // Wait longer for the save to complete
			   ofLogNotice("scVST") << "Waiting for VST program write to complete...";
			   ofSleepMillis(500); // Increased wait time
			   
			   // Try to read the file and store it as binary data in JSON
			   string tempFilePath = ofToDataPath(tempFileName, true);
			   ofFile tempFile(tempFilePath);
			   
			   ofLogNotice("scVST") << "Looking for VST program file at: " << tempFilePath;
			   
			   if(tempFile.exists()) {
				   ofBuffer programData = tempFile.readToBuffer();
				   if(programData.size() > 0) {
					   // Store the binary data as a vector of integers (simpler than base64)
					   vector<int> binaryData;
					   const char* data = programData.getData();
					   for(size_t i = 0; i < programData.size(); i++) {
						   binaryData.push_back((unsigned char)data[i]);
					   }
					   
					   json["vstProgramData"] = binaryData;
					   json["vstDataSize"] = (int)programData.size();
					   
					   ofLogNotice("scVST") << "Successfully saved VST program data (" << programData.size() << " bytes)";
					   
					   // Clean up temporary file
					   tempFile.remove();
					   return; // Success, we can return early
				   } else {
					   ofLogError("scVST") << "VST program file exists but is empty";
				   }
			   } else {
				   ofLogWarning("scVST") << "VST program file was not created at expected location";
				   
				   // Try alternative approaches
				   // Method 1: Try using a buffer instead of file
				   ofLogNotice("scVST") << "Trying alternative method: save to buffer";
				   
				   // Create a temporary buffer number (using a high number to avoid conflicts)
				   int tempBufferNum = 9999;
				   
				   ofxOscMessage saveToBufMsg;
				   saveToBufMsg.setAddress("/u_cmd");
				   saveToBufMsg.addIntArg(serverInstances.second[0]->nodeID);
				   saveToBufMsg.addIntArg(2);
				   saveToBufMsg.addStringArg("/program_write");
				   saveToBufMsg.addIntArg(tempBufferNum); // Try buffer instead of file
				   saveToBufMsg.addIntArg(0); // Synchronous
				   serverInstances.first->sendMsg(saveToBufMsg);
				   
				   ofSleepMillis(300);
				   
				   // Method 2: Try comprehensive parameter query as fallback
				   ofLogNotice("scVST") << "Fallback: attempting comprehensive parameter query";
				   queryAllVSTParametersSync();
			   }
			   
			   // Only need to try from one instance
			   break;
		   }
	   }
	   
	   // If we reach here, program save didn't work
	   ofLogWarning("scVST") << "VST program save failed, relying on individual parameter capture";
   }

   void scVST::restoreVSTProgram(ofJson &json) {
	   ofLogNotice("scVST") << "Restoring VST program data";
	   
	   // Check if we have program data to restore
	   if(!json.contains("vstProgramData") || json["vstProgramData"].is_null()) {
		   ofLogNotice("scVST") << "No VST program data found, will restore individual parameters";
		   return;
	   }
	   
	   try {
		   // Restore the binary data from the vector of integers
		   vector<int> binaryData = json["vstProgramData"];
		   
		   if(binaryData.size() > 0) {
			   // Convert back to binary buffer
			   ofBuffer programData;
			   string dataStr;
			   for(int value : binaryData) {
				   dataStr += (char)(value & 0xFF);
			   }
			   programData.set(dataStr.c_str(), dataStr.size());
			   
			   // Create a temporary file with the program data
			   string tempFileName = "oceanode_vst_restore_" + ofToString(ofGetElapsedTimeMillis()) + ".fxp";
			   string tempFilePath = ofToDataPath(tempFileName, true);
			   
			   ofFile tempFile(tempFilePath, ofFile::WriteOnly, true);
			   tempFile.writeFromBuffer(programData);
			   tempFile.close();
			   
			   // Load the program data into all VST instances
			   for(auto& serverInstances : synthInstances) {
				   for(auto synth : serverInstances.second) {
					   if(synth != nullptr) {
						   ofxOscMessage loadProgramMsg;
						   loadProgramMsg.setAddress("/u_cmd");
						   loadProgramMsg.addIntArg(synth->nodeID);
						   loadProgramMsg.addIntArg(2);
						   loadProgramMsg.addStringArg("/program_read");
						   loadProgramMsg.addStringArg(tempFileName);
						   loadProgramMsg.addIntArg(1); // Asynchronous
						   serverInstances.first->sendMsg(loadProgramMsg);
					   }
				   }
			   }
			   
			   // Wait for the load to complete
			   ofSleepMillis(200);
			   
			   // Clean up temporary file
			   ofFile(tempFilePath).remove();
			   
			   ofLogNotice("scVST") << "Restored VST program data (" << programData.size() << " bytes)";
		   }
		   
	   } catch(const std::exception& e) {
		   ofLogError("scVST") << "Error restoring VST program data: " << e.what();
	   }
   }

void scVST::presetSave(ofJson &json) {
	ofLogNotice("scVST") << "=== PRESET SAVE ===";
	ofLogNotice("scVST") << "Dynamic parameters count: " << dynamicParameters.size();
	ofLogNotice("scVST") << "Parameter info map count (before capture): " << parameterInfoMap.size();
	
	try {
		// AUTOMATICALLY capture current VST state before saving
		ofLogNotice("scVST") << "Automatically capturing current VST state for preset save...";
		
		// Store initial count to track progress
		int initialParamCount = parameterInfoMap.size();
		
		// Query all VST parameters synchronously
		for(auto& serverInstances : synthInstances) {
			if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
				
				// Use a more targeted approach - query parameters in smaller batches
				// and force OSC processing between batches
				
				// Method 1: Query individual parameters with forced OSC processing
				for(int paramIndex = 0; paramIndex < 256; paramIndex++) {
					ofxOscMessage getParamMsg;
					getParamMsg.setAddress("/u_cmd");
					getParamMsg.addIntArg(serverInstances.second[0]->nodeID);
					getParamMsg.addIntArg(2);
					getParamMsg.addStringArg("/get");
					getParamMsg.addIntArg(paramIndex);
					serverInstances.first->sendMsg(getParamMsg);
					
					// Force OSC processing every few parameters
					if(paramIndex % 10 == 0) {
						// Force the server to process OSC messages
						serverInstances.first->process();
						ofSleepMillis(5);
					}
				}
				
				// Method 2: Try bulk queries with OSC processing
				for(int startParam = 0; startParam < 256; startParam += 16) {
					ofxOscMessage getBulkMsg;
					getBulkMsg.setAddress("/u_cmd");
					getBulkMsg.addIntArg(serverInstances.second[0]->nodeID);
					getBulkMsg.addIntArg(2);
					getBulkMsg.addStringArg("/getn");
					getBulkMsg.addIntArg(startParam);
					getBulkMsg.addIntArg(16);
					serverInstances.first->sendMsg(getBulkMsg);
					
					// Force OSC processing after each bulk query
					serverInstances.first->process();
					ofSleepMillis(10);
				}
				
				break; // Only need to query from one server
			}
		}
		
		// Now wait for responses while actively processing OSC messages
		ofLogNotice("scVST") << "Waiting for VST parameter responses while processing OSC...";
		
		int maxWaitIterations = 100; // 2 seconds total
		for(int i = 0; i < maxWaitIterations; i++) {
			// Actively process OSC messages during wait
			for(auto& serverInstances : synthInstances) {
				if(serverInstances.first != nullptr) {
					serverInstances.first->process();
				}
			}
			
			ofSleepMillis(20);
			
			// Check progress every 25 iterations (500ms)
			if(i % 25 == 0) {
				int currentCount = parameterInfoMap.size();
				ofLogNotice("scVST") << "Parameter capture progress: " << currentCount << " parameters (iteration " << i << ")";
				
				// If we've captured a good number of parameters, we can reduce wait time
				if(currentCount > initialParamCount + 30) {
					ofLogNotice("scVST") << "Good parameter capture progress, reducing wait time";
					maxWaitIterations = i + 25; // Wait just a bit more
				}
			}
		}
		
		// Final OSC processing burst
		for(int i = 0; i < 10; i++) {
			for(auto& serverInstances : synthInstances) {
				if(serverInstances.first != nullptr) {
					serverInstances.first->process();
				}
			}
			ofSleepMillis(10);
		}
		
		int finalParamCount = parameterInfoMap.size();
		ofLogNotice("scVST") << "VST state capture complete. Captured " << (finalParamCount - initialParamCount)
							<< " new parameters. Total: " << finalParamCount;
		
		// Now save all the parameters we have
		ofJson vstParams = ofJson::object();
		
		// Save parameters that are in the GUI
		for(auto& param : dynamicParameters) {
			try {
				int paramIndex = param.first;
				ofJson paramData = ofJson::object();
				paramData["value"] = param.second->getParameter().get();
				paramData["name"] = parameterInfoMap[paramIndex].displayName;
				paramData["inGUI"] = true;  // Mark as being in GUI
				vstParams[ofToString(paramIndex)] = paramData;
				
				ofLogNotice("scVST") << "Saving GUI param " << paramIndex << ": " << param.second->getParameter().get();
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error saving GUI parameter " << param.first << ": " << e.what();
			}
		}
		
		// Save parameters that are NOT in the GUI but have been captured
		for(auto& info : parameterInfoMap) {
			try {
				int paramIndex = info.first;
				// Skip if already saved above
				if(dynamicParameters.count(paramIndex) == 0) {
					ofJson paramData = ofJson::object();
					paramData["value"] = info.second.value;
					paramData["name"] = info.second.displayName;
					paramData["inGUI"] = false;  // Mark as NOT in GUI
					vstParams[ofToString(paramIndex)] = paramData;
					
					// Only log non-zero values to reduce spam
					if(info.second.value != 0.0f) {
						ofLogNotice("scVST") << "Saving non-GUI param " << paramIndex << ": " << info.second.value;
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error saving non-GUI parameter " << info.first << ": " << e.what();
			}
		}
		
		json["vstParameters"] = vstParams;
		json["currentPluginPath"] = currentPluginPath;
		json["enableMultithreading"] = enableMultithreading.get();
		
		ofLogNotice("scVST") << "Total parameters saved: " << vstParams.size();
		
		// Log a few example saved parameters for debugging
		int logCount = 0;
		for(auto& item : vstParams.items()) {
			if(logCount++ < 5) {
				ofLogNotice("scVST") << "Example saved param: " << item.key() << " = " << item.value()["value"];
			}
		}
		
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error during preset save: " << e.what();
	}
}

   void scVST::presetRecallAfterSettingParameters(ofJson &json) {
	   ofLogNotice("scVST") << "=== PRESET RECALL CALLED ===";
	   
	   // Clear any existing pending data first
	   hasPendingPresetData = false;
	   pendingPresetData.clear();
	   
	   // Store the preset data to apply after plugin loads (make a deep copy)
	   try {
		   pendingPresetData = json; // ofJson handles deep copying automatically
		   hasPendingPresetData = true;
	   } catch(const std::exception& e) {
		   ofLogError("scVST") << "Error copying preset data: " << e.what();
		   hasPendingPresetData = false;
		   return;
	   }
	   
	   // Restore multithreading setting if saved
	   if(json.contains("enableMultithreading") && !json["enableMultithreading"].is_null()) {
		   try {
			   enableMultithreading = static_cast<bool>(json["enableMultithreading"]);
		   } catch(const std::exception& e) {
			   ofLogError("scVST") << "Error restoring multithreading setting: " << e.what();
		   }
	   }
	   
	   // Handle plugin path change
	   if(json.contains("currentPluginPath") && !json["currentPluginPath"].is_null()) {
		   try {
			   string savedPluginPath = static_cast<string>(json["currentPluginPath"]);
			   ofLogNotice("scVST") << "Saved plugin path: " << savedPluginPath;
			   ofLogNotice("scVST") << "Current plugin path: " << currentPluginPath;
			   
			   if(savedPluginPath != currentPluginPath) {
				   // Find the plugin in our available plugins
				   for(int i = 0; i < pluginPaths.size(); i++) {
					   if(pluginPaths[i] == savedPluginPath) {
						   ofLogNotice("scVST") << "Changing plugin to: " << savedPluginPath;
						   pluginSelector = i;
						   currentPluginPath = savedPluginPath;
						   loadSelectedPlugin();
						   setupParameterTimer(3000); // 3 second fallback timer
						   return;
					   }
				   }
			   }
		   } catch(const std::exception& e) {
			   ofLogError("scVST") << "Error handling plugin path change: " << e.what();
		   }
	   }
	   
	   // If same plugin, apply parameters with a delay since we might still be reloading
	   ofLogNotice("scVST") << "Same plugin, applying parameters with delay";
	   setupParameterTimer(1000); // 1 second delay for same plugin
   }

   void scVST::setupParameterTimer(int delayMs) {
	   parameterTimerStart = ofGetElapsedTimeMillis();
	   parameterTimerDelay = delayMs;
	   parameterTimerActive = true;
	   
	   ofLogNotice("scVST") << "Set up parameter timer for " << delayMs << "ms";
   }

   void scVST::applyPendingPresetData() {
	   if(!hasPendingPresetData) return;
	   
	   ofLogNotice("scVST") << "=== APPLYING PRESET DATA ===";
	   
	   // Make a local copy and clear the pending data immediately to avoid issues during destruction
	   ofJson json;
	   try {
		   json = pendingPresetData; // Deep copy
		   hasPendingPresetData = false;
		   pendingPresetData.clear(); // Clear immediately
	   } catch(const std::exception& e) {
		   ofLogError("scVST") << "Error copying pending preset data: " << e.what();
		   hasPendingPresetData = false;
		   pendingPresetData.clear();
		   return;
	   }
	   
	   // Restore VST parameters using INDICES only
	   if(json.contains("vstParameters") && json["vstParameters"].is_object()) {
		   ofLogNotice("scVST") << "Found vstParameters in preset data";
		   
		   try {
			   for(auto& item : json["vstParameters"].items()) {
				   try {
					   int paramIndex = ofToInt(item.key());
					   
					   // Check if the item value has the expected structure
					   if(item.value().is_object() && item.value().contains("value")) {
						   
						   float value = item.value()["value"];
						   string name = item.value().contains("name") ?
							   static_cast<string>(item.value()["name"]) :
							   ("Param" + ofToString(paramIndex));
						   bool wasInGUI = item.value().contains("inGUI") ?
							   static_cast<bool>(item.value()["inGUI"]) :
							   false;
						   
						   ofLogNotice("scVST") << "Restoring param " << paramIndex << " (" << name << ") = " << value << " (inGUI: " << wasInGUI << ")";
						   
						   // Create/update parameter info - preserve user-defined names
						   if(parameterInfoMap.count(paramIndex) == 0) {
							   VSTParameterInfo info;
							   info.index = paramIndex;
							   info.displayName = name;  // Use the saved name
							   info.value = value;
							   parameterInfoMap[paramIndex] = info;
						   } else {
							   // Only update value, preserve existing display name if user changed it
							   parameterInfoMap[paramIndex].value = value;
							   // Only update name if it's still the default
							   if(parameterInfoMap[paramIndex].displayName == "Param" + ofToString(paramIndex)) {
								   parameterInfoMap[paramIndex].displayName = name;
							   }
						   }
						   
						   // Set the actual VST parameter value using INDEX (for ALL instances)
						   setVSTParameter(paramIndex, value);
						   
						   // Only add to GUI if it was previously in the GUI
						   if(wasInGUI) {
							   addParameterToGUI(paramIndex);
						   }
					   }
				   } catch(const std::exception& e) {
					   ofLogError("scVST") << "Error restoring individual parameter: " << e.what();
					   continue; // Skip this parameter but continue with others
				   }
			   }
		   } catch(const std::exception& e) {
			   ofLogError("scVST") << "Error iterating through vstParameters: " << e.what();
		   }
	   } else {
		   ofLogWarning("scVST") << "No vstParameters found in preset data";
	   }
   }

   void scVST::createVSTInstances(ofxSCServer* server) {
	   int numInstances = calculateNumInstances();
	   
	   ofLogNotice("scVST") << "Creating " << numInstances << " VST instances for " << numChannels.get() << " channels";
	   
	   // Clear existing instances
	   freeVSTInstances(server);
	   
	   // Create new instances
	   synthInstances[server].resize(numInstances);
	   for(int i = 0; i < numInstances; i++) {
		   synthInstances[server][i] = new ofxSCSynth("vst", server);
		   // Note: Don't call create() here - that's done in createSynth()
		   
		   ofLogNotice("scVST") << "Created VST instance " << i;
	   }
   }

void scVST::freeVSTInstances(ofxSCServer* server) {
	if(!server) {
		ofLogWarning("scVST") << "Cannot free VST instances: server is null";
		return;
	}
	
	if(synthInstances.count(server) == 0) {
		ofLogNotice("scVST") << "No VST instances to free for this server";
		return;
	}
	
	ofLogNotice("scVST") << "Freeing " << synthInstances[server].size() << " VST instances";
	
	// Create a copy of the instances vector to avoid iterator invalidation issues
	auto instancesCopy = synthInstances[server];
	
	// Clear the original vector immediately to prevent further access
	synthInstances[server].clear();
	
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
	
	ofLogNotice("scVST") << "Finished freeing VST instances";
}

void scVST::freeAll() {
	ofLogNotice("scVST") << "Freeing all VST instances from all servers";
	
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
	
	ofLogNotice("scVST") << "Finished freeing all VST instances";
}

void scVST::free(ofxSCServer* server) {
	if(!server) {
		ofLogWarning("scVST") << "Cannot free VST instances: server is null";
		return;
	}
	
	try {
		freeVSTInstances(server);
		synthInstances.erase(server);
	} catch(const std::exception& e) {
		ofLogError("scVST") << "Error in free(): " << e.what();
	} catch(...) {
		ofLogError("scVST") << "Unknown error in free()";
	}
}

   void scVST::buildSynth(ofxSCServer* server) {
	   // Phase 1: Create synth instances
	   createVSTInstances(server);
	   
	   // Set up OSC feedback listeners for each synth instance
	   // Update the OSC message handler in buildSynth() method:

	   // Set up OSC feedback listeners for each synth instance
	   for(auto synth : synthInstances[server]) {
		   if(synth != nullptr) {
			   listeners.push(synth->newFeedbackMessage.newListener([this](ofxOscMessage& msg) -> void {
				   if(this == nullptr) return;
				   
				   try {
					   // Handle VST-specific OSC messages
					   if (msg.getAddress() == "/vst_param") {
						   this->handleVSTParam(msg);
					   }
					   else if (msg.getAddress() == "/vst_auto") {
						   this->handleVSTAuto(msg);
					   }
					   else if (msg.getAddress() == "/vst_open") {
						   this->handleVSTOpen(msg);
					   }
					   else if (msg.getAddress() == "/vst_program") {
						   // Optional: Handle program information messages for display purposes
						   this->handleVSTProgram(msg);
					   }
					   // Remove /vst_program_index handling since MIDI doesn't provide feedback
					   else if (msg.getAddress() == "/vst_set") {
						   // Handle parameter query responses
						   if (msg.getNumArgs() >= 4) {
							   if(msg.getNumArgs() > 4) {
								   this->handleParameterQueryResponse(msg);
							   } else {
								   int paramIndex = (int)msg.getArgAsFloat(2);
								   float value = msg.getArgAsFloat(3);
								   
								   if(this->parameterInfoMap.count(paramIndex) == 0) {
									   VSTParameterInfo info;
									   info.index = paramIndex;
									   info.displayName = "Param" + ofToString(paramIndex);
									   info.value = value;
									   this->parameterInfoMap[paramIndex] = info;
								   } else {
									   this->parameterInfoMap[paramIndex].value = value;
								   }
								   
								   this->updateParameterValue(paramIndex, value);
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

   void scVST::createSynth(ofxSCServer* server){
	   // Phase 2: Actually create the synths on the server and configure them
	   if(synthInstances.count(server) == 0) return;
	   
	   for(int i = 0; i < synthInstances[server].size(); i++) {
		   if(synthInstances[server][i] != nullptr) {
			   // Now actually create the synth on the server
			   synthInstances[server][i]->create();
			   
			   ofLogNotice("scVST") << "Created VST instance " << i << " with nodeID " << synthInstances[server][i]->nodeID;
		   }
	   }
	   
	   // Load the selected plugin on all instances
	   if (!currentPluginPath.empty()) {
		   for(int i = 0; i < synthInstances[server].size(); i++) {
			   if(synthInstances[server][i] != nullptr) {
				   ofxOscMessage m;
				   m.setAddress("/u_cmd");
				   m.addIntArg(synthInstances[server][i]->nodeID);
				   m.addIntArg(2);
				   m.addStringArg("/open");
				   m.addStringArg(currentPluginPath);
				   m.addIntArg(1); // Request GUI editor
				   m.addIntArg(enableMultithreading.get() ? 1 : 0); // Multithreading setting
				   m.addIntArg(0); // Normal mode
				   server->sendMsg(m);
			   }
		   }
		   
		   pluginLoaded = true;
	   }
	   
	   resendParams.notify();
   }

   void scVST::setOutputBus(ofxSCServer* server, int index, int bus){
	   outputBuses[server][index] = bus;
	   
	   // Route each VST instance to its appropriate channel pair in the output bus
	   if(synthInstances.count(server) > 0) {
		   int numInstances = synthInstances[server].size();
		   
		   for(int i = 0; i < numInstances; i++) {
			   if(synthInstances[server][i] != nullptr) {
				   // Each instance outputs to a stereo pair starting at bus + (i * 2)
				   int instanceOutputBus = bus + (i * 2);
				   synthInstances[server][i]->set("out", instanceOutputBus);
				   
				   ofLogNotice("scVST") << "Instance " << i << " routed to bus " << instanceOutputBus;
			   }
		   }
	   }
   }

   void scVST::setInputBus(ofxSCServer* server, scNode* node, int bus){
	   inputBuses[server][node] = bus;
	   
	   // Route input bus to all VST instances
	   if(synthInstances.count(server) > 0) {
		   for(int i = 0; i < synthInstances[server].size(); i++) {
			   if(synthInstances[server][i] != nullptr) {
				   // Each instance gets input from bus + (i * 2) for its stereo pair
				   int instanceInputBus = bus + (i * 2);
				   synthInstances[server][i]->set("in", instanceInputBus);
				   
				   ofLogNotice("scVST") << "Instance " << i << " input from bus " << instanceInputBus;
			   }
		   }
	   }
   }

   int scVST::getOutputBusIndex(ofxSCServer* server, int index){
	   if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
		   return outputBuses[server][index];
	   }
	   return -1;
   }

// Add these methods to your scVST class:

// Replace the setVSTProgram method with this simpler MIDI-based version:

void scVST::setVSTProgram(int programIndex) {
	if(synthInstances.empty()) {
		ofLogWarning("scVST") << "No VST instances available to set program";
		return;
	}
	
	// Clamp program index to valid MIDI range (0-127)
	programIndex = ofClamp(programIndex, 0, 127);
	
	ofLogNotice("scVST") << "Sending MIDI program change to " << programIndex << " on all instances";
	
	// Send MIDI program change to ALL instances
	for(auto& serverInstances : synthInstances) {
		if(serverInstances.first == nullptr) continue;
		
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				try {
					sendMidiProgramChange(midiChannel.get(), programIndex, serverInstances.first, synth);
				} catch(const std::exception& e) {
					ofLogError("scVST") << "Error sending MIDI program change to synth " << synth->nodeID << ": " << e.what();
				}
			}
		}
	}
}

// Add this new method for sending MIDI program changes:
void scVST::sendMidiProgramChange(int channel, int program, ofxSCServer* server, ofxSCSynth* synth) {
	ofxOscMessage m;
	m.setAddress("/u_cmd");
	m.addIntArg(synth->nodeID);
	m.addIntArg(2); // VSTPlugin synthIndex
	m.addStringArg("/midi_msg"); // MIDI command
	
	// Create MIDI program change message [status, data1]
	// Program change messages only have 2 bytes: status and program number
	std::vector<int8_t> midiData;
	midiData.push_back(0xC0 | ((channel - 1) & 0x0F)); // Program change + channel (0-based)
	midiData.push_back(program & 0x7F); // Program number (0-127)
	
	// Convert to ofBuffer for blob
	ofBuffer buffer;
	buffer.set((char*)midiData.data(), midiData.size());
	m.addBlobArg(buffer);
	m.addFloatArg(0.0f); // detune (not used for program change)
	server->sendMsg(m);
	
	ofLogNotice("scVST") << "Sent MIDI program change: channel " << channel << ", program " << program << " to node " << synth->nodeID;
}

// Simplify the queryVSTPrograms method (or remove it entirely since we're using MIDI):
void scVST::queryVSTPrograms() {
	ofLogNotice("scVST") << "MIDI program changes don't require querying - programs 0-127 are available";
	
	// Optional: You could still query VST programs for display purposes
	for(auto& serverInstances : synthInstances) {
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			
			// Query program names for reference (optional)
			ofxOscMessage queryProgramsMsg;
			queryProgramsMsg.setAddress("/u_cmd");
			queryProgramsMsg.addIntArg(serverInstances.second[0]->nodeID);
			queryProgramsMsg.addIntArg(2);
			queryProgramsMsg.addStringArg("/program_query");
			queryProgramsMsg.addIntArg(0);   // Start program index
			queryProgramsMsg.addIntArg(32);  // Query first 32 programs for reference
			serverInstances.first->sendMsg(queryProgramsMsg);
			
			ofLogNotice("scVST") << "Queried VST program names for reference";
			break;
		}
	}
}

// Remove the testVSTProgram method since we don't need it anymore
// Remove the handleVSTProgramIndex method since MIDI doesn't give us feedback
// Keep handleVSTProgram for optional program name display

void scVST::handleVSTProgram(ofxOscMessage& msg) {
	// Handle /vst_program messages that contain program information
	if (msg.getNumArgs() >= 4) {
		int nodeID = msg.getArgAsInt32(0);
		
		// Verify this is our VST instance
		if (!isMyVSTInstance(nodeID)) return;
		
		int programIndex = (int)msg.getArgAsFloat(2);
		
		// The program name is encoded as floats (Pascal string format)
		// Skip for now - we're mainly interested in the program index
		
		ofLogNotice("scVST") << "Received VST program info: index " << programIndex;
		
		// You could store program names here if needed for a dropdown
		// but since you just want a simple int parameter, we'll keep it simple
	}
}
