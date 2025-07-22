//
//  scVST.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//

#include "scVST.h"
#include "ofxSCSynth.h"

scVST::scVST() : scNode("VST"){
	
}

void scVST::setup(){
	
	scNode::addInput("in");
	
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
	
	ofParameter<void> editor;
	addParameter(editor.set("Editor"));
	
	listeners.push(editor.newListener([this]{
		for(auto synthServer : synths){
			ofxOscMessage m;
			m.setAddress("/u_cmd");
			m.addIntArg(synthServer.second->nodeID);
			m.addIntArg(2);
			m.addStringArg("/vis");
			m.addIntArg(1);
			synthServer.first->sendMsg(m);
		}
	}));
	
	ofParameter<float> wet;
	addParameter(wet.set("Mix", 0, 0, 1));
	
	listeners.push(wet.newListener([this](float &f){
		for(auto synthServer : synths){
			ofxOscMessage m;
			m.setAddress("/u_cmd");
			m.addIntArg(synthServer.second->nodeID);
			m.addIntArg(2);
			m.addStringArg("/set");
			m.addStringArg("Mix");
			m.addFloatArg(f);
			synthServer.first->sendMsg(m);
		}
	}));
	
	
	
	ofParameter<void> pa;
	addParameter(pa.set("Params"));
	
	listeners.push(pa.newListener([this]{
		for(auto synthServer : synths){
			ofxOscMessage m;
			m.setAddress("/u_cmd");
			m.addIntArg(synthServer.second->nodeID);
			m.addIntArg(2);
			m.addStringArg("/getn");
			m.addStringArg("Decay");
			m.addIntArg(2);
			synthServer.first->sendMsg(m);
		}
	}));
	
	// Plugin selector listener
	listeners.push(pluginSelector.newListener([this](int &selection){
		if (selection >= 0 && selection < pluginPaths.size()) {
			currentPluginPath = pluginPaths[selection];
			loadSelectedPlugin();
		}
	}));
	
	scNode::addOutput("out");
	
	oldNumChannels = numChannels;
	listeners.push(numChannels.newListener([this](int &i){
		// Handle channel number changes if needed
		oldNumChannels = numChannels;
	}));
	
	listeners.push(resendParams.newListener([this](){
		for(auto synthServer : synths){
			if(synthServer.second != nullptr){
				synthServer.second->set("inChannels", numChannels);
			}
		}
	}));
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
	if (currentPluginPath.empty()) return;
	
	for(auto synthServer : synths) {		
		// Close current plugin if any
		ofxOscMessage closeMsg;
		closeMsg.setAddress("/u_cmd");
		closeMsg.addIntArg(synthServer.second->nodeID);
		closeMsg.addIntArg(2);
		closeMsg.addStringArg("/close");
		synthServer.first->sendMsg(closeMsg);
		
		// Open new plugin
		ofxOscMessage openMsg;
		openMsg.setAddress("/u_cmd");
		openMsg.addIntArg(synthServer.second->nodeID);
		openMsg.addIntArg(2);
		openMsg.addStringArg("/open");
		openMsg.addStringArg(currentPluginPath);
		openMsg.addIntArg(1); // Request GUI editor
		openMsg.addIntArg(0); // No multithreading
		openMsg.addIntArg(0); // Normal mode
		synthServer.first->sendMsg(openMsg);
	}
}

void scVST::createSynth(ofxSCServer* server){
	synths[server] = new ofxSCSynth("vst", server);
	synths[server]->create();
	
	// Load the selected plugin
	if (!currentPluginPath.empty()) {
		ofxOscMessage m;
		m.setAddress("/u_cmd");
		m.addIntArg(synths[server]->nodeID);
		m.addIntArg(2);
		m.addStringArg("/open");
		m.addStringArg(currentPluginPath);
		m.addIntArg(1); // Request GUI editor
		m.addIntArg(0); // No multithreading
		m.addIntArg(0); // Normal mode
		server->sendMsg(m);
	}
	
	resendParams.notify();
}

void scVST::free(ofxSCServer* server){
	if(synths.count(server) == 1){
		synths[server]->free();
		delete synths[server];
		synths.erase(server);
	}
}

void scVST::freeAll(){
	for(auto &synth : synths) synth.second->free();
	synths.clear();
}

void scVST::setOutputBus(ofxSCServer* server, int index, int bus){
	outputBuses[server][index] = bus;
	for(int i = 0; i < outputs.size(); i++){
		if(outputs[i]->getIndex() == index){
			string paramName = ofToLower(outputs[i].getName());
			if(synths[server] != nullptr){
				synths[server]->set(paramName, bus);
			}
		}
	}
}

void scVST::setInputBus(ofxSCServer* server, scNode* node, int bus){
	inputBuses[server][node] = bus;
	for(int i = 0; i < inputs.size(); i++){
		if(inputs[i]->getNodeRef() == node){
			string paramName = ofToLower(inputs[i].getName());
			if(synths[server] != nullptr){
				synths[server]->set(paramName, bus);
			}
		}
	}
}

int scVST::getOutputBusIndex(ofxSCServer* server, int index){
	return outputBuses[server][index];
}
