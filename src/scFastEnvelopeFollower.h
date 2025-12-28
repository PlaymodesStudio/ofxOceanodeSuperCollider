//
//  scFastEnvelopeFollower.h
//  ofxOceanodeSupercollider
//
//  High-rate envelope follower using vumeter SynthDef with polling thread
//  Outputs envelope data at configurable rates (60-480 Hz)
//

#ifndef scFastEnvelopeFollower_h
#define scFastEnvelopeFollower_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSuperCollider.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

class scFastEnvelopeFollower : public scNode {
public:
	scFastEnvelopeFollower() : scNode("Fast Envelope Follower") {
		ofLogNotice("scFastEnvelopeFollower") << "Constructor called";
		threadRunning = false;
		
		// Initialize gate state vectors
		gateActive.resize(2, false);
		gateStartTime.resize(2, 0);
	}
	
	~scFastEnvelopeFollower() {
		try {
			// Stop polling thread first
			stopPollingThread();
			
			listeners.unsubscribeAll();
			
			// Free all synth instances
			for(auto& pair : synthInstances) {
				if(pair.second != nullptr) {
					pair.second->free();
					delete pair.second;
				}
			}
			synthInstances.clear();
			
			// Free all envelope buses
			for(auto& pair : envelopeBuses) {
				if(pair.second != nullptr) {
					pair.second->free();
					delete pair.second;
				}
			}
			envelopeBuses.clear();
			
			inputBuses.clear();
			
		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in destructor: " << e.what();
		}
	}
	
	void setup() {
		ofLogNotice("scFastEnvelopeFollower") << "Setup called";
		
		try {
			// Core parameters
			

			addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));
			
			// Envelope parameters - much faster defaults than VU meter
			addParameter(attackTime.set("Attack", 2.0f, 0.1f, 50.0f));
			addParameter(releaseTime.set("Release", 20.0f, 1.0f, 200.0f));
			
			// Polling rate parameter - NEW!
			addParameter(pollingRate.set("PollRateHz", 240, 60, 480));
			
			// Envelope output parameter
			vector<float> defaultEnvData(2, 0.0f);
			auto envDataParam = std::make_shared<ofParameter<vector<float>>>();
			envDataParam->set("Envelope", defaultEnvData,
							vector<float>(2, 0.0f),
							vector<float>(2, 1.0f));
			envelopeData = addOutputParameter(*envDataParam);
			
			// Binarization parameters
			addSeparator("GATE_MODE",ofColor(240,240,240));
			addParameter(binaryMode.set("Binary", false));
			addParameter(threshold.set("Threshold", 0.5f, 0.0f, 1.0f));
			
			// Fixed duration parameters
			addParameter(fixedDuration.set("FixedDur", false));
			addParameter(durationMs.set("DurationMs", 100.0f, 1.0f, 1000.0f));
			
			// Add single input
			scNode::addInput("In");
			
			// Add passthrough output
			scNode::addOutput("Out");
			
			// Set up parameter listeners
			listeners.push(numChannels.newListener([this](int &channels){
				// Recreate synths with proper replacement
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
						
						// Recreate envelope bus for new channel count
						recreateEnvelopeBus(server);
						
						// Restore all parameters
						if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
							newSynth->set("vubus", envelopeBuses[server]->index);
						}
						
						newSynth->set("vuattacktime", attackTime.get());
						newSynth->set("vureleasetime", releaseTime.get());
						
						// Restore input bus
						if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
							for(auto& inputPair : inputBuses[server]) {
								newSynth->set("in", inputPair.second);
								break;
							}
						}
						
						// Restore output bus
						if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
							newSynth->set("out", outputBuses[server][0]);
						}
					}
				}
				
				// Trigger graph recomputation
				for(auto& output : outputs) {
					output = output;
				}
				
				// Resize envelope data
				vector<float> newEnv(channels, 0.0f);
				
				if(envelopeData != nullptr) {
					envelopeData->getParameter().set(newEnv);
				}
				
				// Thread-safe resize of latest data
				{
					std::lock_guard<std::mutex> lock(envelopeDataMutex);
					latestEnvelopeData.resize(channels, 0.0f);
				}
			}));
			
			listeners.push(attackTime.newListener([this](float &attackTime){
				updateEnvelopeTiming(attackTime, -1.0f);
			}));
			
			listeners.push(releaseTime.newListener([this](float &releaseTime){
				updateEnvelopeTiming(-1.0f, releaseTime);
			}));
			
			listeners.push(pollingRate.newListener([this](int &rate){
				// Restart polling thread with new rate
				if(threadRunning) {
					stopPollingThread();
					startPollingThread();
				}
			}));
			
			// Initialize thread-safe buffer
			latestEnvelopeData.resize(numChannels.get(), 0.0f);
			
		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in setup(): " << e.what();
			throw;
		}
	}
	
	void update(ofEventArgs &args) override {
		// Read latest envelope data from thread-safe buffer
		vector<float> currentEnvelope;
		{
			std::lock_guard<std::mutex> lock(envelopeDataMutex);
			currentEnvelope = latestEnvelopeData;
		}
		
		// Apply binarization if enabled
		if(binaryMode.get()) {
			float thresh = threshold.get();
			
			// Ensure gate state vectors match channel count
			if(gateActive.size() != currentEnvelope.size()) {
				gateActive.resize(currentEnvelope.size(), false);
				gateStartTime.resize(currentEnvelope.size(), 0);
			}
			
			if(fixedDuration.get()) {
				// Fixed duration mode: trigger on threshold cross, hold for duration
				uint64_t currentTime = getCurrentTimeMs();
				float durationMsVal = durationMs.get();
				
				for(size_t i = 0; i < currentEnvelope.size(); i++) {
					// Check if we're crossing threshold (rising edge)
					if(currentEnvelope[i] >= thresh && !gateActive[i]) {
						// Start new gate
						gateActive[i] = true;
						gateStartTime[i] = currentTime;
						currentEnvelope[i] = 1.0f;
					}
					else if(gateActive[i]) {
						// Gate is active - check if duration expired
						uint64_t elapsed = currentTime - gateStartTime[i];
						if(elapsed >= durationMsVal) {
							// Duration expired - close gate
							gateActive[i] = false;
							currentEnvelope[i] = 0.0f;
						} else {
							// Still within duration - keep gate high
							currentEnvelope[i] = 1.0f;
						}
					} else {
						// Gate not active and below threshold
						currentEnvelope[i] = 0.0f;
					}
				}
			} else {
				// Simple binarization: immediate threshold response
				for(auto& val : currentEnvelope) {
					val = (val >= thresh) ? 1.0f : 0.0f;
				}
				// Reset gate states when not in fixed duration mode
				std::fill(gateActive.begin(), gateActive.end(), false);
			}
		}
		
		// Update output parameter - this triggers listeners immediately
		if(envelopeData != nullptr) {
			envelopeData->getParameter().set(currentEnvelope);
		}
	}
	
	string getSynthDefName() const {
		// Reuse vumeter SynthDef!
		return "vumeter" + ofToString(numChannels.get());
	}
	
	void buildSynth(ofxSCServer* server) {
		ofLogNotice("scFastEnvelopeFollower") << "buildSynth called for server: "
			<< (server != nullptr ? "valid" : "NULL");
	}
	
	void createSynth(ofxSCServer* server) {
		ofLogNotice("scFastEnvelopeFollower") << "createSynth called";
		
		if(server == nullptr) {
			ofLogError("scFastEnvelopeFollower") << "Server is NULL!";
			return;
		}
		
		try {
			// Create synth instance
			if(synthInstances[server] != nullptr) {
				synthInstances[server]->free();
				delete synthInstances[server];
			}
			
			synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
			
			// Create envelope bus
			recreateEnvelopeBus(server);
			
			// Create the synth
			synthInstances[server]->create();
			
			// Set envelope timing parameters - faster than VU meter defaults
			synthInstances[server]->set("vuattacktime", attackTime.get());
			synthInstances[server]->set("vureleasetime", releaseTime.get());
			
			// Set envelope bus
			if(envelopeBuses[server] != nullptr && envelopeBuses[server]->index >= 0) {
				synthInstances[server]->set("vubus", envelopeBuses[server]->index);
			}
			
			// Set input bus if already connected
			if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
				for(auto& pair : inputBuses[server]) {
					synthInstances[server]->set("in", pair.second);
					break;
				}
			}
			
			// Set output bus if already assigned
			if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
				synthInstances[server]->set("out", outputBuses[server][0]);
			}
			
			// Start polling thread
			startPollingThread();
			
			ofLogNotice("scFastEnvelopeFollower") << "Synth creation complete";
			
		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in createSynth: " << e.what();
		}
	}
	
	void free(ofxSCServer* server) {
		if(server == nullptr) return;
		
		try {
			// Stop polling thread
			stopPollingThread();
			
			if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
				synthInstances[server]->free();
				delete synthInstances[server];
				synthInstances.erase(server);
			}
			
			if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
				envelopeBuses[server]->free();
				delete envelopeBuses[server];
				envelopeBuses.erase(server);
			}
			
			inputBuses.erase(server);
			
		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in free(): " << e.what();
		}
	}
	
	void setInputBus(ofxSCServer* server, scNode* node, int bus) {
		if(server == nullptr || node == nullptr) return;
		
		inputBuses[server][node] = bus;
		
		if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
			try {
				synthInstances[server]->set("in", bus);
				
				// Restore envelope bus and timing parameters
				if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
					synthInstances[server]->set("vubus", envelopeBuses[server]->index);
				}
				synthInstances[server]->set("vuattacktime", attackTime.get());
				synthInstances[server]->set("vureleasetime", releaseTime.get());
				
			} catch(const std::exception& e) {
				ofLogError("scFastEnvelopeFollower") << "Error setting input bus: " << e.what();
			}
		}
	}
	
	void setOutputBus(ofxSCServer* server, int index, int bus) {
		if(server == nullptr) return;
		
		outputBuses[server][index] = bus;
		
		if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
			try {
				synthInstances[server]->set("out", bus);
			} catch(const std::exception& e) {
				ofLogError("scFastEnvelopeFollower") << "Error setting output bus: " << e.what();
			}
		}
	}
	
	int getOutputBusIndex(ofxSCServer* server, int index) {
		if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
			return outputBuses[server][index];
		}
		return -1;
	}
	
	void moveSynthBefore(ofxSCServer* server, int nodeID) {
		if(server == nullptr) return;

		auto it = synthInstances.find(server);
		if(it == synthInstances.end() || it->second == nullptr) {
			return;
		}

		ofxSCSynth* synth = it->second;

		try {
			// Restore all parameters
			if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
				synth->set("vubus", envelopeBuses[server]->index);
			}

			synth->set("vuattacktime", attackTime.get());
			synth->set("vureleasetime", releaseTime.get());

			if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
				int inBus = inputBuses[server].begin()->second;
				synth->set("in", inBus);
			}

			if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
				synth->set("out", outputBuses[server][0]);
			}

			synth->moveBefore(nodeID);

		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in moveSynthBefore(): " << e.what();
		}
	}

	int getLastSynthID(ofxSCServer* server) {
		if(server == nullptr) return -1;

		auto it = synthInstances.find(server);
		if(it != synthInstances.end() && it->second != nullptr) {
			return it->second->nodeID;
		}
		return -1;
	}
	
private:
	ofEventListeners listeners;
	
	// Parameters
	ofParameter<int> numChannels;
	ofParameter<float> attackTime;
	ofParameter<float> releaseTime;
	ofParameter<int> pollingRate;
	ofParameter<bool> binaryMode;
	ofParameter<float> threshold;
	ofParameter<bool> fixedDuration;
	ofParameter<float> durationMs;
	
	// Output parameter - shared_ptr returned by addOutputParameter
	std::shared_ptr<ofxOceanodeParameter<vector<float>>> envelopeData;
	
	// SuperCollider resources
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, ofxSCBus*> envelopeBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;
	
	// High-rate polling thread
	std::thread pollingThread;
	std::atomic<bool> threadRunning;
	std::mutex envelopeDataMutex;
	vector<float> latestEnvelopeData;
	
	// Fixed duration state tracking (per channel)
	vector<bool> gateActive;           // Is gate currently active for this channel?
	vector<uint64_t> gateStartTime;    // When did gate start (milliseconds)
	
	uint64_t getCurrentTimeMs() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
	}
	
	void recreateEnvelopeBus(ofxSCServer* server) {
		if(server == nullptr) return;
		
		// Clean up existing bus
		if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
			envelopeBuses[server]->free();
			delete envelopeBuses[server];
		}
		
		try {
			int channelCount = numChannels.get();
			ofxSCBus* newBus = new ofxSCBus(RATE_CONTROL, channelCount, server);
			
			if(newBus != nullptr && newBus->index >= 0 && newBus->index < 4096) {
				if(server->controlBusses[newBus->index] != nullptr) {
					envelopeBuses[server] = newBus;
					newBus->requestValues();
					ofLogNotice("scFastEnvelopeFollower") << "Created envelope bus with index "
						<< newBus->index << " for " << channelCount << " channels";
				} else {
					delete newBus;
					envelopeBuses[server] = nullptr;
				}
			} else {
				if(newBus != nullptr) delete newBus;
				envelopeBuses[server] = nullptr;
			}
		} catch(...) {
			envelopeBuses[server] = nullptr;
		}
	}
	
	void updateEnvelopeTiming(float attackTime, float releaseTime) {
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
					ofLogError("scFastEnvelopeFollower") << "Error setting timing: " << e.what();
				}
			}
		}
	}
	
	void startPollingThread() {
		if(threadRunning) return;
		
		threadRunning = true;
		pollingThread = std::thread(&scFastEnvelopeFollower::pollEnvelopeBuses, this);
		
		ofLogNotice("scFastEnvelopeFollower") << "Polling thread started at "
			<< pollingRate.get() << " Hz";
	}
	
	void stopPollingThread() {
		if(!threadRunning) return;
		
		threadRunning = false;
		
		if(pollingThread.joinable()) {
			pollingThread.join();
		}
		
		ofLogNotice("scFastEnvelopeFollower") << "Polling thread stopped";
	}
	
	void pollEnvelopeBuses() {
		// Calculate polling interval in microseconds
		auto getInterval = [this]() {
			return std::chrono::microseconds(1000000 / pollingRate.get());
		};
		
		auto interval = getInterval();
		
		ofLogNotice("scFastEnvelopeFollower") << "Poll thread running - interval: "
			<< interval.count() << " μs (" << pollingRate.get() << " Hz)";
		
		while(threadRunning) {
			auto startTime = std::chrono::steady_clock::now();
			
			// Poll all envelope buses
			for(auto& pair : envelopeBuses) {
				if(pair.second != nullptr) {
					// Read current values from SuperCollider
					vector<float> levels = pair.second->readValues;
					
					// Thread-safe write to shared buffer
					{
						std::lock_guard<std::mutex> lock(envelopeDataMutex);
						latestEnvelopeData = levels;
					}
					
					// Request new values for next poll
					pair.second->requestValues();
				}
			}
			
			// Sleep until next poll
			auto elapsed = std::chrono::steady_clock::now() - startTime;
			
			// Recalculate interval in case polling rate changed
			interval = getInterval();
			
			if(elapsed < interval) {
				std::this_thread::sleep_for(interval - elapsed);
			} else {
				// Log if we're missing our target rate
				static int missCount = 0;
				if(++missCount % 100 == 0) {
					ofLogWarning("scFastEnvelopeFollower") << "Polling overrun - elapsed: "
						<< std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
						<< " μs, target: " << interval.count() << " μs";
				}
			}
		}
		
		ofLogNotice("scFastEnvelopeFollower") << "Poll thread exiting";
	}
};

#endif /* scFastEnvelopeFollower_h */
