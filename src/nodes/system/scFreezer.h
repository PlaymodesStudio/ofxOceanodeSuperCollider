#ifndef scFreezer_h
#define scFreezer_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSCServer.h"

class scFreezer : public ofxOceanodeNodeModel {
public:
	scFreezer() : ofxOceanodeNodeModel("SC Freezer") {}
	
	void setup() override {
		description = "Freezes the main SuperCollider server by stopping OSC messages.";
		
		// Get the main server
		mainServer = ofxSCServer::local();
		
		if (!mainServer) {
			ofLogError("scFreezer") << "Could not find SuperCollider server";
			addParameter(noServer.set("Error", "SC Server not found"));
			return;
		}
		
		// Add freeze parameter
		addParameter(freeze.set("Freeze", false));
		
		// Setup listener
		freezeListener = freeze.newListener([this](bool &val){
			if (mainServer) {
				if (val) {
					// When freezing, set extremely high latency
					mainServer->setLatency(1000000.0);
					mainServer->setBLatency(true);
					ofLogNotice("SCFreezer") << "Server frozen";
				} else {
					// When unfreezing, restore normal operation
					mainServer->setLatency(0.2);
					mainServer->setBLatency(false);
					ofLogNotice("SCFreezer") << "Server unfrozen";
				}
			}
		});
	}
	
	~scFreezer() {
		// Make sure we don't leave the server in a frozen state
		if (mainServer && freeze) {
			mainServer->setLatency(0.2);
			mainServer->setBLatency(false);
		}
	}
	
private:
	// Parameters
	ofParameter<bool> freeze;
	ofParameter<string> noServer;
	
	// Server reference
	ofxSCServer* mainServer;
	
	// Event listener
	ofEventListener freezeListener;
};

#endif /* scFreezer_h */
