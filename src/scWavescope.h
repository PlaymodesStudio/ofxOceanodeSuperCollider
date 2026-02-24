#ifndef scWavescope_h
#define scWavescope_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scWavescope : public ofxOceanodeNodeModel {
public:
	scWavescope(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("Wavescope"), servers(outputServers){
		synth = nullptr;
		
		// Frame-based capture settings
		frameRate = 60.0f;
		sampleRate = 44100.0f;
		samplesPerFrame = 64; // Fixed frame size like original
		
		// Sliding buffer for maximum time window
		maxBufferTime = 10.0f; // 10 seconds maximum
		maxBufferSize = (int)(maxBufferTime * sampleRate);
		slidingBuffer.resize(maxBufferSize * MAX_NODE_CHANNELS, 0.0f); // Max 24 channels
		writeIndex = 0;
		
	}
	
	void setup(){
		loadServerConfig();

		addParameter(showWindow.set("Show", false));
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
		addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
		
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
		
		addOutputParameter(previewOut.set("Output", nullptr, nullptr, nullptr));
		ensurePreviewAllocated();

		
		ofFbo::Settings s;
			s.internalformat = GL_RGBA8;
			s.useDepth = false;
			s.useStencil = false;
			s.minFilter = GL_LINEAR;
			s.maxFilter = GL_LINEAR;
			s.numColorbuffers = 1;
			s.textureTarget = GL_TEXTURE_2D;
			previewFbo.allocate(s);
		
		
		listeners.push(input.newListener([this](nodePort &port){
			if(port.getNodeRef() != nullptr) recreateSynth();
			else {
				if(synth){ synth->free(); delete synth; synth = nullptr; }
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
	
	
	void update(ofEventArgs &) override{
		if(!synth || controlBuses.empty()) return;

		if(!freeze.get()){
			for(auto bus : controlBuses) {
				if(bus != nullptr) {
					bus->requestValues();
				}
			}
			
			vector<float> frameData(controlBuses.size(), 0.0f);
			
			// Map physical bus indices to data positions
			int lowestBusIndex = controlBuses[0]->index;
			for(auto bus : controlBuses) {
				if(bus->index < lowestBusIndex) lowestBusIndex = bus->index;
			}
			
			// Read data based on physical bus order, not array order
			for(int i = 0; i < controlBuses.size(); i++) {
				if(controlBuses[i] != nullptr) {
					int physicalBus = controlBuses[i]->index;
					int dataPosition = physicalBus - lowestBusIndex;
					if(dataPosition >= 0 && dataPosition < frameData.size()) {
						frameData[dataPosition] = controlBuses[i]->readValues[0];
					}
				}
			}
			
			slideBufferAndInsertFrame(frameData);
			
			if(autoGain.get()){
				updateAutoGain();
			}
		}
	}
	
	void ensurePreviewAllocated(){
		if(previewFbo.isAllocated()) return;
		ofFbo::Settings s;
		s.width = 320;                  // fixed; Oceanode will scale
		s.height = 120;
		s.internalformat = GL_RGBA8;
		s.useDepth = false; s.useStencil = false;
		s.minFilter = GL_LINEAR; s.maxFilter = GL_LINEAR;
		s.numColorbuffers = 1; s.textureTarget = GL_TEXTURE_2D;
		previewFbo.allocate(s);
	}

	void renderPreviewFromSliding(){
		if(slidingBuffer.empty() || maxBufferSize <= 0) return;
		ensurePreviewAllocated();

		const int w = previewFbo.getWidth();
		const int h = previewFbo.getHeight();
		const int numChans = std::max(1, numChannels.get());
		const float gainValue = gain.get();

		// How much history (same logic as main view)
		float timeWindowSeconds = ofClamp(timeWindow.get(), 0.001f, maxBufferTime);
		int samplesToDisplay = (int)(timeWindowSeconds * sampleRate);
		samplesToDisplay = ofClamp(samplesToDisplay, 1, maxBufferSize);

		const int startSample = maxBufferSize - samplesToDisplay;
		const int endSample   = maxBufferSize;

		previewFbo.begin();
		ofPushStyle();
		ofClear(0,0,0,255);

		// Background
		ofSetColor(backgroundColor->r, backgroundColor->g, backgroundColor->b, backgroundColor->a);
		ofDrawRectangle(0,0,w,h);

		// Per-channel bands
		const float bandH = (float)h / (float)numChans;

		for(int ch = 0; ch < numChans; ++ch){
			const int channelOffset = ch * maxBufferSize;
			const float yTop = ch * bandH;
			const float yMid = yTop + bandH * 0.5f;

			// grid/zero line
			if(gridVisible){
				ofSetColor(120); // center line
				ofDrawLine(0, yMid, w, yMid);
				ofSetColor(60);
				for(int i = 1; i < 10; ++i){
					float gx = (w * i) / 10.0f;
					ofDrawLine(gx, yTop, gx, yTop + bandH);
				}
				if(ch > 0){
					ofSetColor(80);
					ofDrawLine(0, yTop, w, yTop);
				}
			}

			// waveform polyline (one point per pixel)
			ofPolyline poly;
			poly.getVertices().reserve(w);

			for(int px = 0; px < w; ++px){
				float t = (w <= 1) ? 0.0f : (float)px / (float)(w - 1);
				int idx = startSample + (int)std::round(t * (samplesToDisplay - 1));
				idx = ofClamp(idx, startSample, endSample - 1);

				float raw = slidingBuffer[channelOffset + idx];
				float s = ofClamp(raw * gainValue, -1.0f, 1.0f);

				float x = (float)px;
				float y = yMid - s * bandH * 0.45f; // keep margins inside band
				poly.addVertex(glm::vec3(x, y, 0.0f));
			}

			// choose color (same for all, or vary alpha slightly by channel)
			ofColor lc = lineColor.get();
			lc.a = (unsigned char)ofClamp((int)(freeze.get() ? 204 : 255) - ch*4, 40, 255);
			ofSetColor(lc);
			poly.draw();
		}

		ofPopStyle();
		previewFbo.end();

		// Expose texture for Oceanode minimized header
		previewOut = &previewFbo.getTexture();
	}


	
	void renderPreview(const std::vector<float>& samples){
		if(!previewFbo.isAllocated() || samples.size() < 2) return;

		previewFbo.begin();
		ofPushStyle();
		ofClear(0,0,0,255);

		// background
		ofSetColor(30,30,30,255);
		ofDrawRectangle(0,0,previewFbo.getWidth(), previewFbo.getHeight());

		// zero line
		ofSetColor(80);
		float midY = previewFbo.getHeight() * 0.5f;
		ofDrawLine(0, midY, previewFbo.getWidth(), midY);

		// waveform
		ofSetColor(255);
		ofPolyline poly;
		poly.resize(samples.size());
		float w = previewFbo.getWidth();
		float h = previewFbo.getHeight();
		for(size_t i = 0; i < samples.size(); ++i){
			float x = ofMap(i, 0, (int)samples.size()-1, 0, w, true);
			float y = ofMap(samples[i], -1.0f, 1.0f, h, 0, true);
			poly[i] = glm::vec3(x, y, 0.0f);
		}
		poly.draw();

		ofPopStyle();
		previewFbo.end();

		// make it available for the minimized header
		previewOut = &previewFbo.getTexture();
	}
	
	void slideBufferAndInsertFrame(const vector<float>& frameData){
		int numChans = numChannels.get();
		int expectedSize = numChans * samplesPerFrame;
		
		if(frameData.size() != expectedSize){
			ofLogWarning("scWavescope2") << "Frame data size mismatch: "
				<< frameData.size() << " expected: " << expectedSize;
			return;
		}
		
		// Slide buffer with correctly ordered data (already handled bus order in update())
		for(int ch = 0; ch < numChans; ch++){
			int channelOffset = ch * maxBufferSize;
			
			// Shift left by samplesPerFrame
			memmove(&slidingBuffer[channelOffset],
					&slidingBuffer[channelOffset + samplesPerFrame],
					sizeof(float) * (maxBufferSize - samplesPerFrame));

			// Insert new samples at end
			for(int i = 0; i < samplesPerFrame; i++){
				int dataIdx = ch * samplesPerFrame + i;
				slidingBuffer[channelOffset + maxBufferSize - samplesPerFrame + i] = frameData[dataIdx];
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
		if(showWindow){
			std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "Wavescope2 " + ofToString(getNumIdentifier());
			if(ImGui::Begin(title.c_str(), (bool *)&showWindow.get())){
				drawWaveform();
			}
			ImGui::End();
		}

		renderPreviewFromSliding();
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
			int displayChannel = ch;
			
			float trackY = canvasPos.y + ch * trackHeight;
			float trackCenterY = trackY + trackHeight * 0.5f;
			int channelOffset = displayChannel * maxBufferSize;

			// Grid
			if(gridVisible){
				ImU32 gridColor = IM_COL32(60, 60, 60, 100);
				ImU32 centerLineColor = IM_COL32(120, 120, 120, 150);
				
				if(ch > 0){
					drawList->AddLine(ImVec2(canvasPos.x, trackY),
						ImVec2(canvasPos.x + canvasSize.x, trackY), IM_COL32(80, 80, 80, 200));
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
					ImU32 thresholdColor = IM_COL32(255, 100, 100, 100);
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
				
				int sampleIdx1 = startSample + (int)(progress1 * samplesToDisplay);
				int sampleIdx2 = startSample + (int)(progress2 * samplesToDisplay);
				
				sampleIdx1 = ofClamp(sampleIdx1, startSample, endSample - 1);
				sampleIdx2 = ofClamp(sampleIdx2, startSample, endSample - 1);
				
				if(channelOffset + sampleIdx1 < slidingBuffer.size() &&
				   channelOffset + sampleIdx2 < slidingBuffer.size()){
					
					float rawS1 = slidingBuffer[channelOffset + sampleIdx1];
					float rawS2 = slidingBuffer[channelOffset + sampleIdx2];
					
					float s1 = ofClamp(rawS1 * gainValue, -1.f, 1.f);
					float s2 = ofClamp(rawS2 * gainValue, -1.f, 1.f);
					
					float y1 = trackCenterY - s1 * trackHeight * 0.45f;
					float y2 = trackCenterY - s2 * trackHeight * 0.45f;
					
					bool isClipping = false;
					if(showClippingEnabled){
						isClipping = (abs(rawS1) >= clipThreshold || abs(rawS2) >= clipThreshold);
					}
					
					ImU32 lineColor = isClipping ? clippingLineCol : normalLineCol;
					float lineWidth = isClipping ? 2.0f : 1.5f;
					
					drawList->AddLine(ImVec2(canvasPos.x + i, y1),
						ImVec2(canvasPos.x + i + 1, y2), lineColor, lineWidth);
				}
			}

			// Draw header - use displayChannel for header parameters
			const auto& hdrVec = header.get();
			const auto& thVec = headerThickness.get();
			const auto& opVec = headerOpacity.get();

			float hdr = ofClamp((hdrVec.size() == 1 ? hdrVec[0] :
				(displayChannel < hdrVec.size() ? hdrVec[displayChannel] : 0.f)), 0.f, 1.f);
			float th = ofClamp((thVec.size() == 1 ? thVec[0] :
				(displayChannel < thVec.size() ? thVec[displayChannel] : 0.01f)), 0.f, 1.f);
			float op = ofClamp((opVec.size() == 1 ? opVec[0] :
				(displayChannel < opVec.size() ? opVec[displayChannel] : 1.f)), 0.f, 1.f);

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
		
		//drawList->AddText(infoPos, IM_COL32(200, 200, 200, 180), timeInfo.c_str());
		
		if(autoGain.get()){
			ImVec2 gainPos = ImVec2(canvasPos.x + 10, canvasPos.y + 25);
			string gainInfo = "Auto Gain: " + ofToString(gainValue, 2);
			//drawList->AddText(gainPos, IM_COL32(200, 200, 200, 180), gainInfo.c_str());
		}
		
		if(showClippingEnabled){
			ImVec2 clipPos = ImVec2(canvasPos.x + 10, canvasPos.y + 40);
			string clipInfo = "Clip Threshold: " + ofToString(clipThreshold, 2);
			//drawList->AddText(clipPos, IM_COL32(255, 100, 100, 180), clipInfo.c_str());
			
			bool recentClipping = checkRecentClipping();
			if(recentClipping){
				ImVec2 warningPos = ImVec2(canvasPos.x + canvasSize.x - 100, canvasPos.y + 10);
				//drawList->AddText(warningPos, IM_COL32(255, 0, 0, 255), "CLIPPING!");
			}
		}
		
		if(freeze.get()){
			ImVec2 textPos = ImVec2(canvasPos.x + canvasSize.x - 80, canvasPos.y + 10);
			//drawList->AddText(textPos, IM_COL32(255, 200, 100, 255), "FROZEN");
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
	
	void activate() override {
		if(synth) synth->run(true);
	}

	void deactivate() override {
		if(synth) synth->run(false);
	}

	void recreateSynth(){
		if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
		if(synth){ synth->free(); delete synth; synth = nullptr; }
		
		// Free all individual buses
		for(auto bus : controlBuses) {
			if(bus) { bus->free(); delete bus; }
		}
		controlBuses.clear();

		if(input->getNodeRef() == nullptr) return;

		int numChans = numChannels.get();
		int totalBuses = samplesPerFrame * numChans;
		
		try {
			// Create individual single-channel buses
			controlBuses.resize(totalBuses);
			for(int i = 0; i < totalBuses; i++) {
				controlBuses[i] = new ofxSCBus(RATE_CONTROL, 1, servers[serverIndex]->getServer());
			}
			
			// Find lowest bus index
			int lowestBusIndex = controlBuses[0]->index;
			for(auto bus : controlBuses) {
				if(bus->index < lowestBusIndex) {
					lowestBusIndex = bus->index;
				}
			}
			
			synth = new ofxSCSynth("wavescope_realtime" + ofToString(numChans),
				servers[serverIndex]->getServer());
			synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
			synth->set("out", lowestBusIndex);
			synth->set("refreshRate", frameRate);
			synth->addToTail();
			synth->run(getActive());

		} catch (const std::exception &e) {
			for(auto bus : controlBuses) {
				if(bus) { bus->free(); delete bus; }
			}
			controlBuses.clear();
			if(synth){ synth->free(); delete synth; synth = nullptr; }
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
						slidingBuffer.resize(maxBufferSize * MAX_NODE_CHANNELS, 0.0f);
						
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
	
	// Server configuration
	int serverBlockSize = 128; // Default fallback
	bool configLoaded = false;

	vector<ofxSCBus*> controlBuses;

	ofxSCSynth* synth = nullptr;
	vector<serverManager*> servers;
	
	ofFbo previewFbo;
	ofParameter<ofTexture*> previewOut;   // this is the key for the thumbnail

};

#endif /* scWavescope_h */
