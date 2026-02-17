//
//  scFunction.cpp
//  ofxOceanodeSupercollider
//
//  Simplified multi-point sigmoid curve function node
//

#include "scFunction.h"

scFunction::scFunction() : scNode("Function") {
	hoveredPointIndex = -1;
	tensionDragActive = false;
}

scFunction::~scFunction() {
	try {
		listeners.unsubscribeAll();
		freeAll();
	} catch(const std::exception& e) {
		ofLogError("scFunction") << "Error in destructor: " << e.what();
	}
}

void scFunction::setup() {
	color = ofColor(120, 255, 180, 255);
	description = "Audio-rate sigmoid curve function processor";
	
	scNode::addInput("Phasor");
	scNode::addOutput("Out");
	
	// Core parameters
	addParameter(numChannels.set("N Chan", 1, 1, 16));
	addParameter(showWindow.set("Show", true));
	
	// SuperCollider parameters
	addParameter(pointsX.set("Points X", vector<float>(MAX_POINTS, 0.0f),
						   vector<float>(MAX_POINTS, 0.0f),
						   vector<float>(MAX_POINTS, 1.0f)));
	addParameter(pointsY.set("Points Y", vector<float>(MAX_POINTS, 0.0f),
						   vector<float>(MAX_POINTS, -2.0f),
						   vector<float>(MAX_POINTS, 2.0f)));
	addParameter(tensions.set("Tensions", vector<float>(MAX_POINTS-1, 1.0f),
							vector<float>(MAX_POINTS-1, MIN_TENSION),
							vector<float>(MAX_POINTS-1, MAX_TENSION)));
	addParameter(inflections.set("Inflections", vector<float>(MAX_POINTS-1, 0.5f),
							   vector<float>(MAX_POINTS-1, 0.01f),
							   vector<float>(MAX_POINTS-1, 0.99f)));
	
	// Initialize default curve
	initializeDefaultCurve();
	
	// Listeners
	listeners.push(numChannels.newListener([this](int &channels){
        if(channels < 1 || channels > 16) return;
		if(!synthInstances.empty()) {
			std::vector<ofxSCServer*> serversToRecreate;
			for(auto& serverSynth : synthInstances) {
				if(serverSynth.first != nullptr) {
					serversToRecreate.push_back(serverSynth.first);
				}
			}
			for(auto server : serversToRecreate) {
				free(server);
				createSynth(server);
			}
		}
	}));
}

void scFunction::initializeDefaultCurve() {
	points.clear();
	segments.clear();
	
	points.emplace_back(0.0f, 0.0f);
	points.emplace_back(1.0f, 1.0f);
	points[0].firstCreated = false;
	points[1].firstCreated = false;
	
	segments.emplace_back();
	
	// Update SC parameters
	vector<float> px(MAX_POINTS, 0.0f);
	vector<float> py(MAX_POINTS, 0.0f);
	px[0] = 0.0f; px[1] = 1.0f;
	py[0] = 0.0f; py[1] = 1.0f;
	
	pointsX.setWithoutEventNotifications(px);
	pointsY.setWithoutEventNotifications(py);
}

void scFunction::sendCurveDataToSuperCollider() {
	for(auto& serverSynth : synthInstances) {
		if(serverSynth.first != nullptr && serverSynth.second != nullptr) {
			try {
				serverSynth.second->set("pointsX", pointsX.get());
				serverSynth.second->set("pointsY", pointsY.get());
				serverSynth.second->set("tensions", tensions.get());
				serverSynth.second->set("inflections", inflections.get());
			} catch(const std::exception& e) {
				ofLogError("scFunction") << "Error sending curve data: " << e.what();
			}
		}
	}
}

void scFunction::buildSynth(ofxSCServer* server) {
	ofLogNotice("scFunction") << "Building synth for server";
}

void scFunction::createSynth(ofxSCServer* server) {
	if(synthInstances.count(server) > 0) {
		ofLogWarning("scFunction") << "Synth already exists for this server";
		return;
	}
	
	try {
		int channels = numChannels.get();
		string synthDefName = "scFunction" + ofToString(channels);
		
		auto synth = std::make_shared<ofxSCSynth>(synthDefName, server);
		synthInstances[server] = synth.get();  // Raw pointer for interface
		synthPtrs[server] = synth;             // Shared pointer to keep alive
		synth->create();
		
		// Apply pending input bus assignments
		for(auto& inputBus : inputBuses[server]) {
			if(inputBus.first != nullptr) {
				ofLogNotice("scFunction") << "Applying input bus: " << inputBus.second;
				synth->set("in", inputBus.second);
				break;
			}
		}
		
		ofLogNotice("scFunction") << "Created " << synthDefName;
	} catch(const std::exception& e) {
		ofLogError("scFunction") << "Error creating synth: " << e.what();
	}
}

void scFunction::free(ofxSCServer* server) {
	if(synthInstances.count(server) > 0) {
		try {
			if(synthInstances[server] != nullptr) {
				synthInstances[server]->free();
			}
			synthInstances.erase(server);
			synthPtrs.erase(server);        // Also erase shared_ptr
			outputBuses.erase(server);
			inputBuses.erase(server);
		} catch(const std::exception& e) {
			ofLogError("scFunction") << "Error freeing synth: " << e.what();
		}
	}
}

void scFunction::freeAll() {
	for(auto& serverSynth : synthInstances) {
		if(serverSynth.first != nullptr) {
			free(serverSynth.first);
		}
	}
	synthInstances.clear();
}

void scFunction::setOutputBus(ofxSCServer* server, int index, int bus) {
	outputBuses[server][index] = bus;
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			synthInstances[server]->set("out", bus);
		} catch(const std::exception& e) {
			ofLogError("scFunction") << "Error setting output bus: " << e.what();
		}
	}
}

void scFunction::setInputBus(ofxSCServer* server, scNode* node, int bus) {
	inputBuses[server][node] = bus;
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			ofLogNotice("scFunction") << "Setting input bus to " << bus;
			synthInstances[server]->set("in", bus);
		} catch(const std::exception& e) {
			ofLogError("scFunction") << "Error setting input bus: " << e.what();
		}
	}
}

int scFunction::getOutputBusIndex(ofxSCServer* server, int index) {
	if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
		return outputBuses[server][index];
	}
	return -1;
}

void scFunction::draw(ofEventArgs &args) {
	if(!showWindow) return;
	
	string windowTitle = "scFunction " + ofToString(getNumIdentifier());
	if(ImGui::Begin(windowTitle.c_str(), (bool *)&showWindow.get())) {
		ImGui::Text("Basic scFunction - Audio pass-through");
		ImGui::Text("Points: %d", (int)points.size());
	}
	ImGui::End();
}

void scFunction::presetSave(ofJson &json) {
	// Basic preset save
}

void scFunction::presetRecallAfterSettingParameters(ofJson &json) {
	// Basic preset load
}
