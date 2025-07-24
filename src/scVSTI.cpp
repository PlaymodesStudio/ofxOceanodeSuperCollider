//
//  scVSTI.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 23/8/24.
//

#include "scVSTI.h"
#include "ofxSCSynth.h"

scVSTI::scVSTI() : scNode("VSTI"){
	
}

void scVSTI::setup(){
	
	scNode::addInput("in");
	
	addParameter(numChannels.set("N Chan", 2, 1, 100));
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
	
	// MIDI parameters
	addParameter(gate.set("Gate", {0}, {0}, {1}));
	addParameter(pitch.set("Pitch", {60}, {0}, {127}));
	addParameter(velocity.set("Velocity", {0.5}, {0}, {1}));
	
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
	
	ofParameter<float> mix;
	addParameter(mix.set("Mix", 1.0, 0, 1));
	
	listeners.push(mix.newListener([this](float &f){
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
	
	// Plugin selector listener
	listeners.push(pluginSelector.newListener([this](int &selection){
		if (selection >= 0 && selection < pluginPaths.size()) {
			currentPluginPath = pluginPaths[selection];
			loadSelectedPlugin();
		}
	}));
	
	// Gate listener - this is where the magic happens
	listeners.push(gate.newListener([this](vector<int> &gates){
		processGates(gates);
	}));
	
	scNode::addOutput("out");
	
	oldNumChannels = numChannels;
	listeners.push(numChannels.newListener([this](int &i){
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

void scVSTI::searchForVSTPlugins() {
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

void scVSTI::loadSelectedPlugin() {
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

void scVSTI::processGates(vector<int>& gates) {
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
			
			//ofLogNotice("scVSTI") << "Note ON: " << noteNumber << " velocity: " << midiVelocity << " at index: " << i;
		}
		
		// Falling edge - gate went from 1 to 0
		else if (currentGate == 0 && previousGate == 1) {
			// Send note off for the previously active note
			if (i < activeNotes.size() && activeNotes[i] >= 0) {
				sendMidiNoteOff(midiChannel.get(), activeNotes[i]);
				//ofLogNotice("scVSTI") << "Note OFF: " << activeNotes[i] << " at index: " << i;
				activeNotes[i] = -1; // Clear active note
			}
		}
	}
	
	// Update previous gates for next comparison
	previousGates = gates;
}

void scVSTI::sendMidiNoteOn(int channel, int pitch, int velocity) {
	for(auto synthServer : synths){
		ofxOscMessage m;
		m.setAddress("/u_cmd");
		m.addIntArg(synthServer.second->nodeID);
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
		synthServer.first->sendMsg(m);
	}
}

void scVSTI::sendMidiNoteOff(int channel, int pitch) {
	for(auto synthServer : synths){
		ofxOscMessage m;
		m.setAddress("/u_cmd");
		m.addIntArg(synthServer.second->nodeID);
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
		synthServer.first->sendMsg(m);
	}
}

void scVSTI::createSynth(ofxSCServer* server){
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

void scVSTI::free(ofxSCServer* server){
	if(synths.count(server) == 1){
		synths[server]->free();
		delete synths[server];
		synths.erase(server);
	}
}

void scVSTI::freeAll(){
	for(auto &synth : synths) synth.second->free();
	synths.clear();
}

void scVSTI::setOutputBus(ofxSCServer* server, int index, int bus){
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

void scVSTI::setInputBus(ofxSCServer* server, scNode* node, int bus){
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

int scVSTI::getOutputBusIndex(ofxSCServer* server, int index){
	return outputBuses[server][index];
}
