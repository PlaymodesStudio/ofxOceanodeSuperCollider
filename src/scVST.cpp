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

void scVST::setup(){
	
	scNode::addInput("in");
	
	// Basic parameters
	addParameter(numChannels.set("N Chan", 2, 1, 100));
	
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
	
	// VST control parameters
	addParameter(openEditor.set("Editor"));
	addParameter(addLastTouched.set("Add Last"));
	
	// Set up listeners
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
			addParameterToGUI(lastTouchedIndex);
		}
	}));
	
	// Plugin selector listener
	listeners.push(pluginSelector.newListener([this](int &selection){
		if (selection >= 0 && selection < pluginPaths.size()) {
			currentPluginPath = pluginPaths[selection];
			loadSelectedPlugin();
		}
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
	for(auto& param : dynamicParameters) {
		removeParameter(parameterInfoMap[param.first].displayName);
	}
	dynamicParameters.clear();
	
	// Remove parameter name editors from inspector
	for(auto& editor : parameterNameEditors) {
		removeInspectorParameter(parameterInfoMap[editor.first].displayName + "_Name");
	}
	parameterNameEditors.clear();
	
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
				
				// Open new plugin
				ofxOscMessage openMsg;
				openMsg.setAddress("/u_cmd");
				openMsg.addIntArg(synth->nodeID);
				openMsg.addIntArg(2);
				openMsg.addStringArg("/open");
				openMsg.addStringArg(currentPluginPath);
				openMsg.addIntArg(1); // Request GUI editor
				openMsg.addIntArg(0); // No multithreading
				openMsg.addIntArg(0); // Normal mode
				serverInstances.first->sendMsg(openMsg);
			}
		}
		
		// Query parameters from first instance only (they should all be the same)
		if(!serverInstances.second.empty() && serverInstances.second[0] != nullptr) {
			ofLogNotice("scVST") << "Querying all parameters after plugin load";
			ofxOscMessage getAllMsg;
			getAllMsg.setAddress("/u_cmd");
			getAllMsg.addIntArg(serverInstances.second[0]->nodeID);
			getAllMsg.addIntArg(2);
			getAllMsg.addStringArg("/getn");
			getAllMsg.addIntArg(0);  // Start at parameter 0
			getAllMsg.addIntArg(128); // Get first 128 parameters (should cover most VSTs)
			serverInstances.first->sendMsg(getAllMsg);
		}
	}
	
	pluginLoaded = true;
}

void scVST::addParameterToGUI(int paramIndex) {
	if(dynamicParameters.count(paramIndex) > 0) {
		// Parameter already exists, just update its value
		if(parameterInfoMap.count(paramIndex) > 0) {
			dynamicParameters[paramIndex]->getParameter().setWithoutEventNotifications(parameterInfoMap[paramIndex].value);
		}
		return;
	}
	
	// Create parameter info if it doesn't exist
	if(parameterInfoMap.count(paramIndex) == 0) {
		VSTParameterInfo info;
		info.index = paramIndex;
		info.displayName = "Param" + ofToString(paramIndex);
		info.value = 0.0f;
		parameterInfoMap[paramIndex] = info;
	}
	
	// Create new parameter
	string paramName = parameterInfoMap[paramIndex].displayName;
	
	ofParameter<float> newParam;
	newParam.set(paramName, parameterInfoMap[paramIndex].value, 0.0f, 1.0f);
	
	auto oceanodeParam = addParameter(newParam);
	dynamicParameters[paramIndex] = oceanodeParam;
	
	// Set up listener for this parameter
	listeners.push(newParam.newListener([this, paramIndex](float &value){
		if(!isPresetLoading) {
			setVSTParameter(paramIndex, value);
			parameterInfoMap[paramIndex].value = value;
		}
	}));
	
	// Add name editor to inspector
	ofParameter<string> nameEditor;
	nameEditor.set(paramName + "_Name", paramName);
	addInspectorParameter(nameEditor);
	
	// Create and store the parameter reference for the listener
	auto nameParam = make_shared<ofxOceanodeParameter<string>>();
	nameParam->bindParameter(nameEditor);
	parameterNameEditors[paramIndex] = nameParam;
	
	// Set up listener for name changes
	listeners.push(nameEditor.newListener([this, paramIndex](string &newName){
		if(!newName.empty() && newName != parameterInfoMap[paramIndex].displayName) {
			// Update the parameter info
			parameterInfoMap[paramIndex].displayName = newName;
			
			// Update the main parameter name
			if(dynamicParameters.count(paramIndex) > 0) {
				dynamicParameters[paramIndex]->getParameter().setName(newName);
			}
		}
	}));
}

void scVST::removeParameterFromGUI(int paramIndex) {
	if(dynamicParameters.count(paramIndex) > 0) {
		removeParameter(parameterInfoMap[paramIndex].displayName);
		dynamicParameters.erase(paramIndex);
	}
	
	if(parameterNameEditors.count(paramIndex) > 0) {
		removeInspectorParameter(parameterInfoMap[paramIndex].displayName + "_Name");
		parameterNameEditors.erase(paramIndex);
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
	ofLogNotice("scVST") << "Setting VST parameter " << paramIndex << " to " << value << " on all instances";
	
	// Apply parameter change to ALL instances
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				ofxOscMessage setMsg;
				setMsg.setAddress("/u_cmd");
				setMsg.addIntArg(synth->nodeID);
				setMsg.addIntArg(2);
				setMsg.addStringArg("/set");
				setMsg.addIntArg(paramIndex);
				setMsg.addFloatArg(value);
				serverInstances.first->sendMsg(setMsg);
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

void scVST::presetSave(ofJson &json) {
	ofLogNotice("scVST") << "=== PRESET SAVE ===";
	ofLogNotice("scVST") << "Dynamic parameters count: " << dynamicParameters.size();
	ofLogNotice("scVST") << "Parameter info map count: " << parameterInfoMap.size();
	
	// Save ALL VST parameter values that have been touched, not just GUI ones
	ofJson vstParams;
	
	// Save parameters that are in the GUI
	for(auto& param : dynamicParameters) {
		int paramIndex = param.first;
		ofJson paramData;
		paramData["value"] = param.second->getParameter().get();
		paramData["name"] = parameterInfoMap[paramIndex].displayName;
		paramData["inGUI"] = true;  // Mark as being in GUI
		vstParams[ofToString(paramIndex)] = paramData;
		
		ofLogNotice("scVST") << "Saving GUI param " << paramIndex << ": " << param.second->getParameter().get();
	}
	
	// Save parameters that are NOT in the GUI but have been changed
	for(auto& info : parameterInfoMap) {
		int paramIndex = info.first;
		// Skip if already saved above
		if(dynamicParameters.count(paramIndex) == 0) {
			ofJson paramData;
			paramData["value"] = info.second.value;
			paramData["name"] = info.second.displayName;
			paramData["inGUI"] = false;  // Mark as NOT in GUI
			vstParams[ofToString(paramIndex)] = paramData;
			
			ofLogNotice("scVST") << "Saving non-GUI param " << paramIndex << ": " << info.second.value;
		}
	}
	
	json["vstParameters"] = vstParams;
	json["currentPluginPath"] = currentPluginPath;
	
	ofLogNotice("scVST") << "Total parameters saved: " << vstParams.size();
}

void scVST::presetRecallAfterSettingParameters(ofJson &json) {
	ofLogNotice("scVST") << "=== PRESET RECALL CALLED ===";
	
	// Store the preset data to apply after plugin loads
	pendingPresetData = json;
	hasPendingPresetData = true;
	
	// Handle plugin path change
	if(json.contains("currentPluginPath") && !json["currentPluginPath"].is_null()) {
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
	
	ofJson json = pendingPresetData;
	hasPendingPresetData = false;
	
	// Restore ALL VST parameters
	if(json.contains("vstParameters") && json["vstParameters"].is_object()) {
		ofLogNotice("scVST") << "Found vstParameters in preset data";
		
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
					
					// Create/update parameter info
					VSTParameterInfo info;
					info.index = paramIndex;
					info.displayName = name;
					info.value = value;
					parameterInfoMap[paramIndex] = info;
					
					// Set the actual VST parameter value (for ALL instances)
					setVSTParameter(paramIndex, value);
					
					// Only add to GUI if it was previously in the GUI
					if(wasInGUI) {
						addParameterToGUI(paramIndex);
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scVST") << "Error restoring parameter: " << e.what();
			}
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
		synthInstances[server][i]->create();
		
		ofLogNotice("scVST") << "Created VST instance " << i << " with nodeID " << synthInstances[server][i]->nodeID;
	}
	
	// Load the selected plugin on all instances
	if (!currentPluginPath.empty()) {
		for(int i = 0; i < numInstances; i++) {
			ofxOscMessage m;
			m.setAddress("/u_cmd");
			m.addIntArg(synthInstances[server][i]->nodeID);
			m.addIntArg(2);
			m.addStringArg("/open");
			m.addStringArg(currentPluginPath);
			m.addIntArg(1); // Request GUI editor
			m.addIntArg(0); // No multithreading
			m.addIntArg(0); // Normal mode
			server->sendMsg(m);
		}
		
		pluginLoaded = true;
	}
}

void scVST::freeVSTInstances(ofxSCServer* server) {
	if(synthInstances.count(server) > 0) {
		for(auto synth : synthInstances[server]) {
			if(synth != nullptr) {
				synth->free();
				delete synth;
			}
		}
		synthInstances[server].clear();
	}
}

void scVST::createSynth(ofxSCServer* server){
	createVSTInstances(server);
	resendParams.notify();
}

void scVST::free(ofxSCServer* server){
	freeVSTInstances(server);
	synthInstances.erase(server);
}

void scVST::freeAll(){
	for(auto& serverInstances : synthInstances) {
		for(auto synth : serverInstances.second) {
			if(synth != nullptr) {
				synth->free();
				delete synth;
			}
		}
	}
	synthInstances.clear();
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
