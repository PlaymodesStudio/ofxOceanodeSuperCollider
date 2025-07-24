#ifndef scWavescope2_h
#define scWavescope2_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scWavescope2 : public ofxOceanodeNodeModel {
public:
	scWavescope2(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("Wavescope2"), servers(outputServers){
		synth = nullptr;
		waveformBus = nullptr;
		
		// Frame-based capture settings
		frameRate = 60.0f;
		sampleRate = 44100.0f;
		samplesPerFrame = 64; // Fixed frame size like original
		
		// Sliding buffer for maximum time window
		maxBufferTime = 10.0f; // 10 seconds maximum
		maxBufferSize = (int)(maxBufferTime * sampleRate);
		slidingBuffer.resize(maxBufferSize * 24, 0.0f); // Max 24 channels
		writeIndex = 0;
		
		// OSC setup for additional phase tracking
		setupPhaseListener();
	}
	
	void setup(){
		addParameter(showWindow.set("Show", false));
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
		addParameter(numChannels.set("N Chan", 1, 1, 24));
		
		// Time window zoom - this is the main zoom control
		addParameter(timeWindow.set("Time Window", 0.1f, 0.001f, maxBufferTime)); // 1ms to 10s
		addParameter(freeze.set("Freeze", false));
		
		// Visual parameters
		addParameter(lineColor.set("Line Color", ofColor(0, 255, 0), ofColor(0), ofColor(255)));
		addParameter(backgroundColor.set("Background", ofColor(0, 0, 0, 180), ofColor(0), ofColor(255)));
		addParameter(gridVisible.set("Show Grid", true));
		addParameter(autoGain.set("Auto Gain", false));
		addParameter(gain.set("Gain", 1.0f, 0.1f, 10.0f));
		
		// Clipping detection parameters
		addParameter(showClipping.set("Show Clipping", true));
		addParameter(clippingThreshold.set("Clip Threshold", 0.95f, 0.8f, 1.0f));
		addParameter(clippingColor.set("Clip Color", ofColor(255, 0, 0), ofColor(0), ofColor(255))); // Red
		
		// Header parameters
		addParameter(header.set("Header", {0.f}, {0.f}, {1.f}));
		addParameter(headerThickness.set("HeaderTh", {0.01f}, {0.f}, {1.f}));
		addParameter(headerOpacity.set("HeaderOp", {1.f}, {0.f}, {1.f}));
		
		// Listeners
		listeners.push(input.newListener([this](nodePort &port){
			if(port.getNodeRef() != nullptr) recreateSynth();
			else {
				if(synth){ synth->free(); delete synth; synth = nullptr; }
				if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; }
			}
		}));

		listeners.push(serverIndex.newListener([this](int &i){
			configLoaded = false; // Force reload of config for new server
			loadServerConfig();   // Load config for the new server
			
			if(input->getNodeRef() != nullptr) recreateSynth();
			serverGraphListener.unsubscribe();
			serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){ recreateSynth(); });
		}));

		listeners.push(numChannels.newListener([this](int &i){
			if(input->getNodeRef() != nullptr) recreateSynth();
		}));
	}
	
	void setupPhaseListener(){
		// Setup OSC receiver for phase updates from SuperCollider
		phaseReceiver.setup(57120); // Default SC port
	}
	
	void update(ofEventArgs &) override{
		// Process OSC messages for phase tracking
		while(phaseReceiver.hasWaitingMessages()){
			ofxOscMessage m;
			phaseReceiver.getNextMessage(m);
			
			// Handle phase messages
			if(m.getAddress() == "/wavescope_phase" && m.getNumArgs() >= 2){
				float newPhase = m.getArgAsFloat(0);
				int channels = m.getArgAsInt(1);
				
				// Debug output (comment out in production)
				// ofLogNotice("scWavescope2") << "Received phase: " << newPhase << " for " << channels << " channels";
				
				if(!freeze.get()){
					currentWritePhase = newPhase;
				}
			}
		}
		
		// Original bus reading logic
		if(!synth || !waveformBus) return;
		if(waveformBus->index < 0) return;

		auto newFrameData = waveformBus->readValues;
		int expectedFrameSize = numChannels.get() * samplesPerFrame;

		if(newFrameData.size() != expectedFrameSize){
			newFrameData.resize(expectedFrameSize, 0.0f);
		}

		if(!freeze.get()){
			// Slide buffer and insert new frame
			slideBufferAndInsertFrame(newFrameData);
			
			if(autoGain.get()){
				updateAutoGain();
			}
		}
		
		waveformBus->requestValues();
	}
	
	void slideBufferAndInsertFrame(const vector<float>& frameData){
		int numChans = numChannels.get();

		// Sanity check
		if((int)frameData.size() != numChans * samplesPerFrame){
			ofLogError("slideBuffer") << "Unexpected frame size: " << frameData.size()
									  << " (expected: " << numChans * samplesPerFrame << ")";
			return;
		}

		for(int ch = 0; ch < numChans; ch++){
			int channelOffset = ch * maxBufferSize;

			// Shift left by samplesPerFrame
			memmove(&slidingBuffer[channelOffset],
					&slidingBuffer[channelOffset + samplesPerFrame],
					sizeof(float) * (maxBufferSize - samplesPerFrame));

			// Insert new samples at end
			for(int i = 0; i < samplesPerFrame; i++){
				int interleavedIdx = i * numChans + ch;
				slidingBuffer[channelOffset + maxBufferSize - samplesPerFrame + i] = frameData[interleavedIdx];
			}
		}
	}
	
	void updateAutoGain(){
		// Calculate RMS for auto-scaling
		float maxRMS = 0.0f;
		int numChans = numChannels.get();
		int samplesToCheck = min(maxBufferSize, samplesPerFrame * 10); // Last 10 frames
		
		for(int ch = 0; ch < numChans; ch++){
			float channelRMS = 0.0f;
			int channelOffset = ch * maxBufferSize;
			
			for(int i = maxBufferSize - samplesToCheck; i < maxBufferSize; i++){
				float sample = slidingBuffer[channelOffset + i];
				channelRMS += sample * sample;
			}
			channelRMS = sqrt(channelRMS / samplesToCheck);
			maxRMS = max(maxRMS, channelRMS);
		}
		
		if(maxRMS > 0.001f){
			float targetGain = 0.7f / maxRMS;
			float currentGain = gain.get();
			float smoothedGain = currentGain * 0.95f + targetGain * 0.05f;
			gain.set(ofClamp(smoothedGain, 0.1f, 10.0f));
		}
	}
	
	void draw(ofEventArgs &) override{
		if(!showWindow) return;
		std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "Wavescope2 " + ofToString(getNumIdentifier());
		if(ImGui::Begin(title.c_str(), (bool *)&showWindow.get())){
			drawWaveform();
		}
		ImGui::End();
	}
	
	void drawWaveform(){
		if(slidingBuffer.empty()) return;

		auto drawList = ImGui::GetWindowDrawList();
		auto canvasPos = ImGui::GetCursorScreenPos();
		auto canvasSize = ImGui::GetContentRegionAvail();
		if(canvasSize.x < 50) canvasSize.x = 800;
		if(canvasSize.y < 50) canvasSize.y = 400;

		// Calculate which samples to display based on time window
		float timeWindowSeconds = timeWindow.get();
		int samplesToDisplay = (int)(timeWindowSeconds * sampleRate);
		samplesToDisplay = min(samplesToDisplay, maxBufferSize);
		samplesToDisplay = max(samplesToDisplay, 1);
		
		// Always show the most recent samples (rightmost in buffer)
		int startSample = maxBufferSize - samplesToDisplay;
		int endSample = maxBufferSize;
		
		int numChans = numChannels.get();
		float trackHeight = canvasSize.y / max(numChans, 1);
		float gainValue = gain.get();
		
		// Get clipping parameters
		float clipThreshold = clippingThreshold.get();
		bool showClippingEnabled = showClipping.get();
		
		// Background
		ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
			backgroundColor->r/255.f, backgroundColor->g/255.f,
			backgroundColor->b/255.f, backgroundColor->a/255.f));
		drawList->AddRectFilled(canvasPos,
			ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), bgColor);

		for(int ch = 0; ch < numChans; ch++){
			float trackY = canvasPos.y + ch * trackHeight;
			float trackCenterY = trackY + trackHeight * 0.5f;
			int channelOffset = ch * maxBufferSize;

			// Grid
			if(gridVisible){
				ImU32 gridColor = IM_COL32(60, 60, 60, 100);
				ImU32 centerLineColor = IM_COL32(120, 120, 120, 150);
				
				if(ch < numChans - 1){
					float separatorY = trackY + trackHeight;
					drawList->AddLine(ImVec2(canvasPos.x, separatorY),
						ImVec2(canvasPos.x + canvasSize.x, separatorY), IM_COL32(80, 80, 80, 200));
				}
				
				drawList->AddLine(ImVec2(canvasPos.x, trackCenterY),
					ImVec2(canvasPos.x + canvasSize.x, trackCenterY), centerLineColor);
				
				// Vertical grid lines
				for(int i = 1; i < 10; i++){
					float x = canvasPos.x + (canvasSize.x * i / 10);
					drawList->AddLine(ImVec2(x, trackY),
						ImVec2(x, trackY + trackHeight), gridColor);
				}
				
				// Draw clipping threshold lines if enabled
				if(showClippingEnabled){
					float clipY1 = trackCenterY - clipThreshold * trackHeight * 0.45f;
					float clipY2 = trackCenterY + clipThreshold * trackHeight * 0.45f;
					ImU32 thresholdColor = IM_COL32(255, 100, 100, 100); // Light red
					drawList->AddLine(ImVec2(canvasPos.x, clipY1),
						ImVec2(canvasPos.x + canvasSize.x, clipY1), thresholdColor);
					drawList->AddLine(ImVec2(canvasPos.x, clipY2),
						ImVec2(canvasPos.x + canvasSize.x, clipY2), thresholdColor);
				}
			}

			// Prepare colors
			ImU32 normalLineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
				lineColor->r/255.f, lineColor->g/255.f, lineColor->b/255.f,
				freeze.get() ? 0.8f : 1.0f));
			
			ImU32 clippingLineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
				clippingColor->r/255.f, clippingColor->g/255.f, clippingColor->b/255.f,
				freeze.get() ? 0.8f : 1.0f));
			
			// High-quality waveform drawing within time window
			for(int i = 0; i < canvasSize.x - 1; i++){
				float progress1 = (float)i / (canvasSize.x - 1);
				float progress2 = (float)(i + 1) / (canvasSize.x - 1);
				
				// Map to sample indices within our time window
				int sampleIdx1 = startSample + (int)(progress1 * samplesToDisplay);
				int sampleIdx2 = startSample + (int)(progress2 * samplesToDisplay);
				
				sampleIdx1 = ofClamp(sampleIdx1, startSample, endSample - 1);
				sampleIdx2 = ofClamp(sampleIdx2, startSample, endSample - 1);
				
				if(channelOffset + sampleIdx1 < slidingBuffer.size() &&
				   channelOffset + sampleIdx2 < slidingBuffer.size()){
					
					// Get raw samples (before gain)
					float rawS1 = slidingBuffer[channelOffset + sampleIdx1];
					float rawS2 = slidingBuffer[channelOffset + sampleIdx2];
					
					// Apply gain for display
					float s1 = ofClamp(rawS1 * gainValue, -1.f, 1.f);
					float s2 = ofClamp(rawS2 * gainValue, -1.f, 1.f);
					
					float y1 = trackCenterY - s1 * trackHeight * 0.45f;
					float y2 = trackCenterY - s2 * trackHeight * 0.45f;
					
					// Check for clipping on raw samples (before gain)
					bool isClipping = false;
					if(showClippingEnabled){
						isClipping = (abs(rawS1) >= clipThreshold || abs(rawS2) >= clipThreshold);
					}
					
					// Choose color based on clipping
					ImU32 lineColor = isClipping ? clippingLineCol : normalLineCol;
					float lineWidth = isClipping ? 2.0f : 1.5f; // Thicker line for clipping
					
					drawList->AddLine(ImVec2(canvasPos.x + i, y1),
						ImVec2(canvasPos.x + i + 1, y2), lineColor, lineWidth);
				}
			}

			// Draw header
			const auto& hdrVec = header.get();
			const auto& thVec = headerThickness.get();
			const auto& opVec = headerOpacity.get();

			float hdr = ofClamp((hdrVec.size() == 1 ? hdrVec[0] : (ch < hdrVec.size() ? hdrVec[ch] : 0.f)), 0.f, 1.f);
			float th = ofClamp((thVec.size() == 1 ? thVec[0] : (ch < thVec.size() ? thVec[ch] : 0.01f)), 0.f, 1.f);
			float op = ofClamp((opVec.size() == 1 ? opVec[0] : (ch < opVec.size() ? opVec[ch] : 1.f)), 0.f, 1.f);

			float xLeft = canvasPos.x + (1.0f - hdr) * canvasSize.x;
			float xRight = xLeft + th * canvasSize.x;

			ImU32 headerCol = IM_COL32(255, 255, 255, int(op * 128));
			drawList->AddRectFilled(ImVec2(xLeft, trackY), ImVec2(xRight, trackY + trackHeight), headerCol);
		}

		// Status info
		ImVec2 infoPos = ImVec2(canvasPos.x + 10, canvasPos.y + 10);
		string timeInfo = "Window: " + ofToString(timeWindowSeconds * 1000, 1) + "ms";
		if(timeWindowSeconds >= 1.0f){
			timeInfo = "Window: " + ofToString(timeWindowSeconds, 2) + "s";
		}
		float samplesPerPixel = (float)samplesToDisplay / canvasSize.x;
		timeInfo += " (" + ofToString(samplesPerPixel, 1) + " samples/px)";
		
		// Show phase info if we're receiving OSC
		if(currentWritePhase > 0){
			timeInfo += " | Phase: " + ofToString((int)currentWritePhase);
		}
		
		drawList->AddText(infoPos, IM_COL32(200, 200, 200, 180), timeInfo.c_str());
		
		if(autoGain.get()){
			ImVec2 gainPos = ImVec2(canvasPos.x + 10, canvasPos.y + 25);
			string gainInfo = "Auto Gain: " + ofToString(gainValue, 2);
			drawList->AddText(gainPos, IM_COL32(200, 200, 200, 180), gainInfo.c_str());
		}
		
		// Clipping info
		if(showClippingEnabled){
			ImVec2 clipPos = ImVec2(canvasPos.x + 10, canvasPos.y + 40);
			string clipInfo = "Clip Threshold: " + ofToString(clipThreshold, 2);
			drawList->AddText(clipPos, IM_COL32(255, 100, 100, 180), clipInfo.c_str());
			
			// Check if any recent samples are clipping
			bool recentClipping = checkRecentClipping();
			if(recentClipping){
				ImVec2 warningPos = ImVec2(canvasPos.x + canvasSize.x - 100, canvasPos.y + 10);
				drawList->AddText(warningPos, IM_COL32(255, 0, 0, 255), "CLIPPING!");
			}
		}
		
		if(freeze.get()){
			ImVec2 textPos = ImVec2(canvasPos.x + canvasSize.x - 80, canvasPos.y + 10);
			drawList->AddText(textPos, IM_COL32(255, 200, 100, 255), "FROZEN");
		}
		
		ImGui::Dummy(canvasSize);
	}
	
	bool checkRecentClipping(){
		if(!showClipping.get()) return false;
		
		int numChans = numChannels.get();
		float clipThreshold = clippingThreshold.get();
		int recentSamples = min(maxBufferSize, samplesPerFrame * 5); // Last 5 frames
		
		for(int ch = 0; ch < numChans; ch++){
			int channelOffset = ch * maxBufferSize;
			for(int i = maxBufferSize - recentSamples; i < maxBufferSize; i++){
				if(channelOffset + i < slidingBuffer.size()){
					if(abs(slidingBuffer[channelOffset + i]) >= clipThreshold){
						return true;
					}
				}
			}
		}
		return false;
	}
	
	void recreateSynth(){
		if(synth){ synth->free(); delete synth; synth = nullptr; }
		if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; }

		if(input->getNodeRef() == nullptr) return;

		int totalBusSize = samplesPerFrame * numChannels.get();
		
		try {
			waveformBus = new ofxSCBus(RATE_CONTROL, totalBusSize, servers[serverIndex]->getServer());
			if(waveformBus->index < 0){ delete waveformBus; waveformBus = nullptr; return; }
			
			synth = new ofxSCSynth("wavescope_realtime" + ofToString(numChannels.get()),
				servers[serverIndex]->getServer());
			synth->addToTail();
			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
			synth->set("out", waveformBus->index);
			synth->set("refreshRate", frameRate);

		} catch (const std::exception &e) {
			if(synth){ synth->free(); delete synth; synth = nullptr; }
			if(waveformBus){ waveformBus->free(); delete waveformBus; waveformBus = nullptr; }
		}
	}
	
	void loadServerConfig() {
		if(configLoaded) return; // Only load once
		
		int currentServerIndex = serverIndex.get();
		
		// Construct path to server config file
		string configPath = ofToDataPath("Supercollider/Config/ServerPreferences_" +
										ofToString(currentServerIndex) + ".json");
		
		ofLogNotice("scWavescope2") << "Loading server config from: " << configPath;
		
		// Try to load and parse the JSON file
		ofFile configFile(configPath);
		if(configFile.exists()) {
			try {
				ofJson configJson = ofLoadJson(configPath);
				
				if(configJson.contains("blockSize")) {
					serverBlockSize = configJson["blockSize"].get<int>();
					ofLogNotice("scWavescope2") << "Loaded blockSize: " << serverBlockSize
											   << " for server " << currentServerIndex;
					
					// Log other relevant settings
					if(configJson.contains("hardwareSampleRate")) {
						int loadedSampleRate = configJson["hardwareSampleRate"].get<int>();
						sampleRate = (float)loadedSampleRate;
						
						// Update buffer size based on actual sample rate
						maxBufferSize = (int)(maxBufferTime * sampleRate);
						slidingBuffer.resize(maxBufferSize * 24, 0.0f);
						
						float controlRate = sampleRate / serverBlockSize;
						ofLogNotice("scWavescope2") << "Sample rate: " << loadedSampleRate
												   << "Hz, Control rate: " << controlRate << "Hz";
					}
				} else {
					ofLogWarning("scWavescope2") << "No blockSize found in config, using default: "
												<< serverBlockSize;
				}
				
				configLoaded = true;
				
			} catch(const std::exception& e) {
				ofLogError("scWavescope2") << "Error parsing server config: " << e.what();
				ofLogNotice("scWavescope2") << "Using default blockSize: " << serverBlockSize;
			}
		} else {
			ofLogWarning("scWavescope2") << "Server config file not found: " << configPath;
			ofLogNotice("scWavescope2") << "Using default blockSize: " << serverBlockSize;
		}
	}

private:
	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	// Parameters
	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<bool> showWindow;
	ofParameter<float> timeWindow; // This is the zoom control
	ofParameter<bool> freeze;
	ofParameter<ofColor> lineColor;
	ofParameter<ofColor> backgroundColor;
	ofParameter<bool> gridVisible;
	ofParameter<bool> autoGain;
	ofParameter<float> gain;
	
	// Clipping detection parameters
	ofParameter<bool> showClipping;
	ofParameter<float> clippingThreshold;
	ofParameter<ofColor> clippingColor;
	
	// Header parameters
	ofParameter<vector<float>> header;
	ofParameter<vector<float>> headerThickness;
	ofParameter<vector<float>> headerOpacity;

	// Frame-synchronized capture (back to original approach)
	vector<float> slidingBuffer;    // Large circular buffer
	int maxBufferSize;              // Maximum samples in buffer
	float maxBufferTime;            // Maximum time (10 seconds)
	int samplesPerFrame;            // Samples per frame (64)
	float frameRate;
	float sampleRate;
	int writeIndex;
	
	// OSC phase tracking (additional info)
	ofxOscReceiver phaseReceiver;
	float currentWritePhase = 0.0f;
	
	// Server configuration
	int serverBlockSize = 128; // Default fallback
	bool configLoaded = false;

	ofxSCBus* waveformBus = nullptr;
	ofxSCSynth* synth = nullptr;
	vector<serverManager*> servers;
};

#endif /* scWavescope2_h */
