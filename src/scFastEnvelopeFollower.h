//
//  scFastEnvelopeFollower.h
//  ofxOceanodeSupercollider
//
//  High-rate envelope follower using vumeter SynthDef.
//
//  Thread-safety design:
//    - Background thread reads envelopeBus->readValues (safe: OSC handler only writes
//      individual floats into a pre-sized vector, never reallocates) and processes
//      the envelope at high rate. It sets requestPending = true instead of calling
//      sendMsg() directly (sendMsg is NOT thread-safe).
//    - update() on the main thread calls requestValues() whenever requestPending is set,
//      keeping all OSC sends on the main thread.
//

#ifndef scFastEnvelopeFollower_h
#define scFastEnvelopeFollower_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSuperCollider.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

class scFastEnvelopeFollower : public scNode {
public:
	scFastEnvelopeFollower() : scNode("Fast Envelope Follower") {
		ofLogNotice("scFastEnvelopeFollower") << "Constructor called";
		threadRunning = false;
		requestPending = false;

		// Initialize gate state vectors
		gateActive.resize(2, false);
		gateStartTime.resize(2, 0);
	}

	~scFastEnvelopeFollower() {
		try {
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
			addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));

			// Envelope parameters - faster defaults than VU meter
			addParameter(attackTime.set("Attack", 2.0f, 0.1f, 50.0f));
			addParameter(releaseTime.set("Release", 20.0f, 1.0f, 200.0f));

			// Polling rate: how fast the background thread reads and outputs new values
			addParameter(pollingRate.set("PollRateHz", 240, 60, 480));

			// Envelope output parameter
			vector<float> defaultEnvData(2, 0.0f);
			auto envDataParam = std::make_shared<ofParameter<vector<float>>>();
			envDataParam->set("Envelope", defaultEnvData,
							vector<float>(2, 0.0f),
							vector<float>(2, 1.0f));
			envelopeData = addOutputParameter(*envDataParam);

			// Binarization parameters
			addSeparator("GATE_MODE", ofColor(240, 240, 240));
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
				if(channels < 1 || channels > MAX_NODE_CHANNELS) return;
				// Recreate synths with proper replacement
				for(auto& pair : synthInstances) {
					if(pair.second != nullptr) {
						ofxSCServer* server = pair.first;
						int oldNodeID = pair.second->nodeID;

						ofxSCSynth *newSynth = new ofxSCSynth(getSynthDefName(), server);
						newSynth->create(4, oldNodeID); // 4 = kAddAction_replace
						newSynth->run(getActive());

						delete pair.second;
						pair.second = newSynth;

						recreateEnvelopeBus(server);

						if(envelopeBuses.count(server) > 0 && envelopeBuses[server] != nullptr) {
							newSynth->set("vubus", envelopeBuses[server]->index);
						}

						newSynth->set("vuattacktime", attackTime.get());
						newSynth->set("vureleasetime", releaseTime.get());

						if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
							for(auto& inputPair : inputBuses[server]) {
								newSynth->set("in", inputPair.second);
								break;
							}
						}

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
			}));

			listeners.push(attackTime.newListener([this](float &at){
				updateEnvelopeTiming(at, -1.0f);
			}));

			listeners.push(releaseTime.newListener([this](float &rt){
				updateEnvelopeTiming(-1.0f, rt);
			}));

		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in setup(): " << e.what();
			throw;
		}
	}

	void update(ofEventArgs &args) override {
		// Main-thread responsibilities:
		// 1. Call requestValues() if the background thread flagged it (keeps sendMsg on main thread)
		// 2. Publish the latest envelope data processed by the background thread
		if(requestPending.exchange(false)) {
			for(auto& pair : envelopeBuses) {
				if(pair.second != nullptr) pair.second->requestValues();
			}
		}

		// Publish latest values computed by background thread
		vector<float> latest;
		{
			std::lock_guard<std::mutex> lock(envelopeDataMutex);
			latest = latestEnvelopeData;
		}
		if(envelopeData != nullptr && !latest.empty()) {
			envelopeData->getParameter().set(latest);
		}
	}

	string getSynthDefName() const {
		// Reuse vumeter SynthDef
		return "vumeter" + ofToString(numChannels.get());
	}

	void activate() override {
		for(auto& pair : synthInstances) if(pair.second) pair.second->run(true);
	}

	void deactivate() override {
		for(auto& pair : synthInstances) if(pair.second) pair.second->run(false);
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
			if(synthInstances[server] != nullptr) {
				synthInstances[server]->free();
				delete synthInstances[server];
			}

			synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);

			recreateEnvelopeBus(server);

			synthInstances[server]->create();
			synthInstances[server]->run(getActive());

			synthInstances[server]->set("vuattacktime", attackTime.get());
			synthInstances[server]->set("vureleasetime", releaseTime.get());

			if(envelopeBuses[server] != nullptr && envelopeBuses[server]->index >= 0) {
				synthInstances[server]->set("vubus", envelopeBuses[server]->index);
			}

			if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
				for(auto& pair : inputBuses[server]) {
					synthInstances[server]->set("in", pair.second);
					break;
				}
			}

			if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
				synthInstances[server]->set("out", outputBuses[server][0]);
			}

			// Start background polling thread (reads readValues at high rate)
		startPollingThread();

		ofLogNotice("scFastEnvelopeFollower") << "Synth creation complete";

		} catch(const std::exception& e) {
			ofLogError("scFastEnvelopeFollower") << "Error in createSynth: " << e.what();
		}
	}

	void free(ofxSCServer* server) {
		if(server == nullptr) return;

		// Stop thread before freeing buses (thread reads envelopeBuses)
		stopPollingThread();

		try {
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

	void resetInputBusses(ofxSCServer* server, int targetBus = 0) override {
		inputBuses[server].clear();
		if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
			synthInstances[server]->set("in", targetBus);
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
	ofParameter<bool> binaryMode;
	ofParameter<float> threshold;
	ofParameter<bool> fixedDuration;
	ofParameter<float> durationMs;

	// Output parameter
	std::shared_ptr<ofxOceanodeParameter<vector<float>>> envelopeData;

	// SuperCollider resources
	std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
	std::map<ofxSCServer*, ofxSCBus*> envelopeBuses;
	std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;

	// Fixed duration state tracking (per channel)
	vector<bool> gateActive;
	vector<uint64_t> gateStartTime;

	ofParameter<int> pollingRate;

	// High-rate background thread (reads only, never calls sendMsg)
	std::thread pollingThread;
	std::atomic<bool> threadRunning;
	std::atomic<bool> requestPending; // Set by thread, consumed by update() on main thread
	std::mutex envelopeDataMutex;
	vector<float> latestEnvelopeData;

	void startPollingThread() {
		if(threadRunning.load()) return;
		latestEnvelopeData.resize(numChannels.get(), 0.0f);
		threadRunning = true;
		requestPending = false;
		pollingThread = std::thread([this]() { pollLoop(); });
	}

	void stopPollingThread() {
		if(!threadRunning.load()) return;
		threadRunning = false;
		if(pollingThread.joinable()) pollingThread.join();
	}

	void pollLoop() {
		while(threadRunning.load()) {
			auto start = std::chrono::steady_clock::now();
			int rate = pollingRate.get();
			auto interval = std::chrono::microseconds(1000000 / std::max(rate, 1));

			// Read readValues (safe: OSC handler only writes individual floats,
			// never reallocates the vector — no structural race condition)
			for(auto& pair : envelopeBuses) {
				if(pair.second == nullptr) continue;

				vector<float> levels = pair.second->readValues;

				// Sanitize NaN/inf
				for(auto& v : levels) {
					if(!std::isfinite(v)) v = 0.0f;
				}

				// Apply binarization (gate) processing
				applyGating(levels);

				{
					std::lock_guard<std::mutex> lock(envelopeDataMutex);
					latestEnvelopeData = std::move(levels);
				}
			}

			// Signal main thread to send the next /c_get request
			requestPending.store(true);

			auto elapsed = std::chrono::steady_clock::now() - start;
			if(elapsed < interval) std::this_thread::sleep_for(interval - elapsed);
		}
	}

	void applyGating(vector<float>& levels) {
		if(!binaryMode.get()) return;

		float thresh = threshold.get();

		if(gateActive.size() != levels.size()) {
			gateActive.resize(levels.size(), false);
			gateStartTime.resize(levels.size(), 0);
		}

		if(fixedDuration.get()) {
			uint64_t currentTime = getCurrentTimeMs();
			float durationMsVal = durationMs.get();
			for(size_t i = 0; i < levels.size(); i++) {
				if(levels[i] >= thresh && !gateActive[i]) {
					gateActive[i] = true;
					gateStartTime[i] = currentTime;
					levels[i] = 1.0f;
				} else if(gateActive[i]) {
					uint64_t elapsed = currentTime - gateStartTime[i];
					if(elapsed >= (uint64_t)durationMsVal) {
						gateActive[i] = false;
						levels[i] = 0.0f;
					} else {
						levels[i] = 1.0f;
					}
				} else {
					levels[i] = 0.0f;
				}
			}
		} else {
			for(auto& val : levels) val = (val >= thresh) ? 1.0f : 0.0f;
			std::fill(gateActive.begin(), gateActive.end(), false);
		}
	}

	uint64_t getCurrentTimeMs() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
	}

	void recreateEnvelopeBus(ofxSCServer* server) {
		if(server == nullptr) return;

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

	void updateEnvelopeTiming(float at, float rt) {
		for(auto& pair : synthInstances) {
			if(pair.second != nullptr) {
				try {
					if(at >= 0.0f) pair.second->set("vuattacktime", at);
					if(rt >= 0.0f) pair.second->set("vureleasetime", rt);
				} catch(const std::exception& e) {
					ofLogError("scFastEnvelopeFollower") << "Error setting timing: " << e.what();
				}
			}
		}
	}
};

#endif /* scFastEnvelopeFollower_h */
