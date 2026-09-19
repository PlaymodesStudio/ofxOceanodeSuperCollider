#ifndef scWavescope_h
#define scWavescope_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "serverManager.h"
#include "ofxSCBus.h"
#include "ofxSCSynth.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

// Real-time oscilloscope.
//
// The matching wavescope_realtime1 SynthDef maintains two 256-sample capture
// buffers for one audio channel. Stabilized captures start on a rising trigger
// crossing and are published only after the complete time window is ready, so
// the UI never sees a rotating or half-written frame. A Wavescope creates one
// lightweight synth per displayed channel and uses one bus per channel. This
// keeps every channel snapshot coherent while keeping
// OSC packets comfortably below UDP limits, even for MAX_NODE_CHANNELS.
class scWavescope : public ofxOceanodeNodeModel {
public:
	static constexpr int SAMPLES_PER_CHANNEL = 256;
	static constexpr int CHANNELS_PER_BUS = 1;

	explicit scWavescope(vector<serverManager*> outputServers)
	: ofxOceanodeNodeModel("Wavescope")
	, servers(std::move(outputServers))
	{}

	~scWavescope() override {
		clearSynthsAndBuses();
	}

	void setup() override {
		const int maxServerIndex = servers.empty() ? 0 : (int)servers.size() - 1;

		// Keep the graph node compact: Show, signal input, and texture output.
		// Configuration lives in the inspector parameter group so it remains
		// serialized without adding invisible rows to the node body. The same
		// parameters are drawn in the dock's left properties panel.
		addParameter(showWindow.set("Show", false));
		addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
		addInspectorParameter(serverIndex.set("Server", 0, 0, maxServerIndex));
		addInspectorParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
		addInspectorParameter(timeWindow.set("Time Window", 0.1f, 0.001f, 10.0f));
		addInspectorParameter(freeze.set("Freeze", false));
		addInspectorParameter(stabilize.set("Stabilize", true));
		addInspectorParameter(triggerLevel.set("Trigger Level", 0.0f, -1000000.0f, 1000000.0f));

		// The display range is independent of the captured signal. Gain remains
		// for preset compatibility and is applied before Min..Max mapping.
		addInspectorParameter(displayMin.set("Min", -1.0f, -1000000.0f, 1000000.0f));
		addInspectorParameter(displayMax.set("Max",  1.0f, -1000000.0f, 1000000.0f));
		addInspectorParameter(lineColor.set("Line Color", ofColor(0, 255, 0), ofColor(0), ofColor(255)));
		addInspectorParameter(backgroundColor.set("Background", ofColor(0, 0, 0, 180), ofColor(0), ofColor(255)));
		addInspectorParameter(gridVisible.set("Show Grid", true));
		addInspectorParameter(autoGain.set("Auto Gain", false));
		addInspectorParameter(gain.set("Gain", 1.0f, 0.001f, 1000.0f));

		addInspectorParameter(showClipping.set("Show Clipping", true));
		addInspectorParameter(clippingThreshold.set("Clip Threshold", 0.95f, 0.0f, 1000000.0f));
		addInspectorParameter(clippingColor.set("Clip Color", ofColor(255, 0, 0), ofColor(0), ofColor(255)));

		addInspectorParameter(header.set("Header", {0.f}, {0.f}, {1.f}));
		addInspectorParameter(headerThickness.set("HeaderTh", {0.01f}, {0.f}, {1.f}));
		addInspectorParameter(headerOpacity.set("HeaderOp", {1.f}, {0.f}, {1.f}));

		addOutputParameter(previewOut.set("Output", nullptr, nullptr, nullptr));
		ensurePreviewAllocated();

		listeners.push(input.newListener([this](nodePort& port) {
			if(port.getNodeRef()) recreateSynths();
			else clearSynthsAndBuses();
		}));

		listeners.push(serverIndex.newListener([this](int&) {
			attachServerGraphListener();
			if(input->getNodeRef()) recreateSynths();
			else clearSynthsAndBuses();
		}));

		listeners.push(numChannels.newListener([this](int&) {
			if(input->getNodeRef()) recreateSynths();
		}));

		listeners.push(timeWindow.newListener([this](float& seconds) {
			for(auto* scopeSynth : synths) {
				if(scopeSynth) scopeSynth->set("timeWindow", seconds);
			}
		}));

		listeners.push(stabilize.newListener([this](bool& enabled) {
			for(auto* scopeSynth : synths) {
				if(scopeSynth) scopeSynth->set("stabilize", enabled ? 1.0f : 0.0f);
			}
		}));

		listeners.push(triggerLevel.newListener([this](float& level) {
			for(auto* scopeSynth : synths) {
				if(scopeSynth) scopeSynth->set("triggerLevel", level);
			}
		}));

		attachServerGraphListener();
	}

	void update(ofEventArgs&) override {
		if(synths.empty() || sampleBuses.empty() || freeze.get()) return;

		bool copiedSamples = false;
		int firstChannel = 0;
		for(auto* bus : sampleBuses) {
			if(!bus) continue;

			const int channelsInGroup = std::min(CHANNELS_PER_BUS,
				std::max(0, numChannels.get() - firstChannel));
			const int expectedValues = channelsInGroup * SAMPLES_PER_CHANNEL;
			const auto& values = bus->readValues;

			if(channelsInGroup > 0 && (int)values.size() >= expectedValues) {
				const int destination = firstChannel * SAMPLES_PER_CHANNEL;
				std::copy_n(values.begin(), expectedValues, waveformData.begin() + destination);
				copiedSamples = true;
			}

			// requestValues() is asynchronous, so consume the completed previous
			// reply before requesting the next coherent block.
			bus->requestValues();
			firstChannel += channelsInGroup;
		}

		if(copiedSamples && autoGain.get()) updateAutoGain();
	}

	void draw(ofEventArgs&) override {
		if(showWindow) {
			const std::string title = (canvasID == "Canvas" ? "" : canvasID + "/")
				+ "Wavescope " + ofToString(getNumIdentifier());
			bool open = showWindow.get();
			ImGui::SetNextWindowSize(ImVec2(900.0f, 440.0f), ImGuiCond_FirstUseEver);
			if(ImGui::Begin(title.c_str(), &open,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
				const ImVec2 available = ImGui::GetContentRegionAvail();
				const float propertiesWidth = std::min(260.0f,
					std::max(190.0f, available.x * 0.28f));

				ImGui::BeginChild("##WaveScopeProperties",
					ImVec2(propertiesWidth, available.y), true);
				drawPropertiesPanel();
				ImGui::EndChild();

				ImGui::SameLine();
				ImGui::BeginChild("##WaveScopeDisplay", ImVec2(0.0f, available.y), false,
					ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				drawWaveform();
				ImGui::EndChild();
			}
			ImGui::End();
			if(open != showWindow.get()) showWindow.set(open);
		}

		renderPreview();
	}

	void activate() override {
		for(auto* scopeSynth : synths) if(scopeSynth) scopeSynth->run(true);
	}

	void deactivate() override {
		for(auto* scopeSynth : synths) if(scopeSynth) scopeSynth->run(false);
	}

private:
	struct DisplayRange {
		float minimum;
		float maximum;
	};

	ofEventListeners listeners;
	ofEventListener serverGraphListener;

	ofParameter<nodePort> input;
	ofParameter<int> serverIndex;
	ofParameter<int> numChannels;
	ofParameter<bool> showWindow;
	ofParameter<float> timeWindow;
	ofParameter<bool> freeze;
	ofParameter<bool> stabilize;
	ofParameter<float> triggerLevel;
	ofParameter<float> displayMin;
	ofParameter<float> displayMax;
	ofParameter<ofColor> lineColor;
	ofParameter<ofColor> backgroundColor;
	ofParameter<bool> gridVisible;
	ofParameter<bool> autoGain;
	ofParameter<float> gain;
	ofParameter<bool> showClipping;
	ofParameter<float> clippingThreshold;
	ofParameter<ofColor> clippingColor;
	ofParameter<vector<float>> header;
	ofParameter<vector<float>> headerThickness;
	ofParameter<vector<float>> headerOpacity;

	vector<float> waveformData;
	vector<ofxSCBus*> sampleBuses;
	vector<ofxSCSynth*> synths;
	vector<serverManager*> servers;

	ofFbo previewFbo;
	ofParameter<ofTexture*> previewOut;

	bool hasValidServer() const {
		const int index = serverIndex.get();
		return index >= 0 && index < (int)servers.size()
			&& servers[index] != nullptr && servers[index]->getServer() != nullptr;
	}

	void drawPropertiesPanel() {
		ImGui::TextUnformatted("Capture");
		ImGui::Separator();

		int channels = numChannels.get();
		ImGui::TextUnformatted("Channels");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::SliderInt("##Channels", &channels, numChannels.getMin(), numChannels.getMax())) {
			numChannels.set(channels);
		}

		if(serverIndex.getMax() > serverIndex.getMin()) {
			int server = serverIndex.get();
			ImGui::TextUnformatted("Server");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if(ImGui::SliderInt("##Server", &server, serverIndex.getMin(), serverIndex.getMax())) {
				serverIndex.set(server);
			}
		}

		float window = timeWindow.get();
		ImGui::TextUnformatted("Time Window");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::SliderFloat("##TimeWindow", &window, timeWindow.getMin(),
			timeWindow.getMax(), "%.4f s", ImGuiSliderFlags_Logarithmic)) {
			timeWindow.set(window);
		}

		bool stable = stabilize.get();
		if(ImGui::Checkbox("Stabilize", &stable)) stabilize.set(stable);

		float trigger = triggerLevel.get();
		ImGui::TextUnformatted("Trigger Level");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::DragFloat("##TriggerLevel", &trigger, 0.01f,
			triggerLevel.getMin(), triggerLevel.getMax(), "%.4f")) {
			triggerLevel.set(trigger);
		}

		bool frozen = freeze.get();
		if(ImGui::Checkbox("Freeze", &frozen)) freeze.set(frozen);

		ImGui::Spacing();
		ImGui::TextUnformatted("Display");
		ImGui::Separator();

		drawFloatProperty("Min", displayMin, 0.01f, "%.4f");
		drawFloatProperty("Max", displayMax, 0.01f, "%.4f");

		bool automaticGain = autoGain.get();
		if(ImGui::Checkbox("Auto Gain", &automaticGain)) autoGain.set(automaticGain);
		drawFloatProperty("Gain", gain, 0.01f, "%.4f");

		bool grid = gridVisible.get();
		if(ImGui::Checkbox("Grid", &grid)) gridVisible.set(grid);
		drawColorProperty("Line Color", lineColor);
		drawColorProperty("Background", backgroundColor);

		if(ImGui::CollapsingHeader("Clipping", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool clipping = showClipping.get();
			if(ImGui::Checkbox("Show Clipping", &clipping)) showClipping.set(clipping);
			drawFloatProperty("Threshold", clippingThreshold, 0.01f, "%.4f");
			drawColorProperty("Clip Color", clippingColor);
		}

		if(ImGui::CollapsingHeader("Header")) {
			drawVectorScalarProperty("Position", header, 0.005f, "%.3f");
			drawVectorScalarProperty("Thickness", headerThickness, 0.005f, "%.3f");
			drawVectorScalarProperty("Opacity", headerOpacity, 0.005f, "%.3f");
		}
	}

	void drawFloatProperty(const char* label, ofParameter<float>& parameter,
		float speed, const char* format) {
		float value = parameter.get();
		ImGui::TextUnformatted(label);
		ImGui::PushID(label);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::DragFloat("##value", &value, speed, parameter.getMin(),
			parameter.getMax(), format, ImGuiSliderFlags_AlwaysClamp)) {
			parameter.set(value);
		}
		ImGui::PopID();
	}

	void drawVectorScalarProperty(const char* label,
		ofParameter<vector<float>>& parameter, float speed, const char* format) {
		const auto& values = parameter.get();
		float value = values.empty() ? 0.0f : values.front();
		const auto& minimum = parameter.getMin();
		const auto& maximum = parameter.getMax();
		const float low = minimum.empty() ? 0.0f : minimum.front();
		const float high = maximum.empty() ? 1.0f : maximum.front();
		ImGui::TextUnformatted(label);
		ImGui::PushID(label);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::DragFloat("##value", &value, speed, low, high, format,
			ImGuiSliderFlags_AlwaysClamp)) {
			parameter.set(vector<float>{value});
		}
		ImGui::PopID();
	}

	void drawColorProperty(const char* label, ofParameter<ofColor>& parameter) {
		const ofColor color = parameter.get();
		float rgba[4] = {
			color.r / 255.0f, color.g / 255.0f,
			color.b / 255.0f, color.a / 255.0f
		};
		ImGui::TextUnformatted(label);
		ImGui::PushID(label);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_AlphaBar)) {
			parameter.set(ofColor(
				(unsigned char)ofClamp((int)std::round(rgba[0] * 255.0f), 0, 255),
				(unsigned char)ofClamp((int)std::round(rgba[1] * 255.0f), 0, 255),
				(unsigned char)ofClamp((int)std::round(rgba[2] * 255.0f), 0, 255),
				(unsigned char)ofClamp((int)std::round(rgba[3] * 255.0f), 0, 255)));
		}
		ImGui::PopID();
	}

	void attachServerGraphListener() {
		serverGraphListener.unsubscribe();
		if(!hasValidServer()) return;
		serverGraphListener = servers[serverIndex.get()]->graphComputed.newListener([this]() {
			if(input->getNodeRef()) recreateSynths();
		});
	}

	void clearSynthsAndBuses() {
		for(auto* scopeSynth : synths) {
			if(scopeSynth) {
				scopeSynth->free();
				delete scopeSynth;
			}
		}
		synths.clear();

		for(auto* bus : sampleBuses) {
			if(bus) {
				bus->free();
				delete bus;
			}
		}
		sampleBuses.clear();
		waveformData.clear();
	}

	void recreateSynths() {
		clearSynthsAndBuses();
		if(!hasValidServer() || !input->getNodeRef()) return;

		const int channelCount = ofClamp(numChannels.get(), 1, MAX_NODE_CHANNELS);
		ofxSCServer* server = servers[serverIndex.get()]->getServer();
		const int inputBusIndex = input->getBusIndex(server);
		waveformData.assign(channelCount * SAMPLES_PER_CHANNEL, 0.0f);

		try {
			const int groupCount = (channelCount + CHANNELS_PER_BUS - 1) / CHANNELS_PER_BUS;
			sampleBuses.reserve(groupCount);
			for(int group = 0; group < groupCount; ++group) {
				const int channelsInGroup = std::min(CHANNELS_PER_BUS,
					channelCount - group * CHANNELS_PER_BUS);
				auto* bus = new ofxSCBus(RATE_CONTROL,
					channelsInGroup * SAMPLES_PER_CHANNEL, server);
				sampleBuses.push_back(bus);
			}

			synths.reserve(channelCount);
			for(int channel = 0; channel < channelCount; ++channel) {
				const int group = channel / CHANNELS_PER_BUS;
				const int channelInGroup = channel % CHANNELS_PER_BUS;
				auto* scopeSynth = new ofxSCSynth("wavescope_realtime1", server);
				scopeSynth->set("in", inputBusIndex + channel);
				scopeSynth->set("out", sampleBuses[group]->index
					+ channelInGroup * SAMPLES_PER_CHANNEL);
				scopeSynth->set("timeWindow", timeWindow.get());
				scopeSynth->set("stabilize", stabilize.get() ? 1.0f : 0.0f);
				scopeSynth->set("triggerLevel", triggerLevel.get());
				scopeSynth->createAndRun(1, 1, getActive());
				synths.push_back(scopeSynth);
			}

			for(auto* bus : sampleBuses) if(bus) bus->requestValues();
		} catch(const std::exception& error) {
			ofLogError("scWavescope") << "Could not create scope resources: " << error.what();
			clearSynthsAndBuses();
		} catch(...) {
			ofLogError("scWavescope") << "Could not create scope resources";
			clearSynthsAndBuses();
		}
	}

	DisplayRange getDisplayRange() const {
		float minimum = displayMin.get();
		float maximum = displayMax.get();
		if(!std::isfinite(minimum)) minimum = -1.0f;
		if(!std::isfinite(maximum)) maximum = 1.0f;
		if(maximum <= minimum) maximum = minimum + 0.000001f;
		return {minimum, maximum};
	}

	float valueToUnit(float rawValue, const DisplayRange& range) const {
		const float scaledValue = rawValue * gain.get();
		return ofClamp((scaledValue - range.minimum) / (range.maximum - range.minimum), 0.0f, 1.0f);
	}

	float valueToY(float rawValue, float top, float height, const DisplayRange& range) const {
		const float margin = height * 0.05f;
		return top + margin + (1.0f - valueToUnit(rawValue, range))
			* std::max(0.0f, height - margin * 2.0f);
	}

	bool isClipping(float rawValue) const {
		return showClipping.get()
			&& std::abs(rawValue * gain.get()) >= clippingThreshold.get();
	}

	void updateAutoGain() {
		if(waveformData.empty()) return;

		double squareSum = 0.0;
		for(float sample : waveformData) squareSum += (double)sample * (double)sample;
		const float rms = (float)std::sqrt(squareSum / waveformData.size());
		if(rms <= 0.000001f || !std::isfinite(rms)) return;

		const DisplayRange range = getDisplayRange();
		const float targetSpan = std::max(std::abs(range.minimum), std::abs(range.maximum));
		const float targetGain = targetSpan * 0.7f / rms;
		const float smoothedGain = gain.get() * 0.95f + targetGain * 0.05f;
		gain.set(ofClamp(smoothedGain, gain.getMin(), gain.getMax()));
	}

	void ensurePreviewAllocated() {
		if(previewFbo.isAllocated()) return;
		ofFbo::Settings settings;
		settings.width = 320;
		settings.height = 120;
		settings.internalformat = GL_RGBA8;
		settings.useDepth = false;
		settings.useStencil = false;
		settings.minFilter = GL_LINEAR;
		settings.maxFilter = GL_LINEAR;
		settings.numColorbuffers = 1;
		settings.textureTarget = GL_TEXTURE_2D;
		previewFbo.allocate(settings);
	}

	void drawGrid(ImDrawList* drawList, const ImVec2& position, float width,
		float trackTop, float trackHeight, int channel, const DisplayRange& range) const {
		if(!gridVisible.get()) return;

		if(channel > 0) {
			drawList->AddLine(ImVec2(position.x, trackTop),
				ImVec2(position.x + width, trackTop), IM_COL32(80, 80, 80, 200));
		}

		for(int i = 1; i < 10; ++i) {
			const float x = position.x + width * i / 10.0f;
			drawList->AddLine(ImVec2(x, trackTop), ImVec2(x, trackTop + trackHeight),
				IM_COL32(60, 60, 60, 100));
		}

		if(range.minimum <= 0.0f && range.maximum >= 0.0f) {
			const float zeroY = valueToY(0.0f, trackTop, trackHeight, range);
			drawList->AddLine(ImVec2(position.x, zeroY), ImVec2(position.x + width, zeroY),
				IM_COL32(120, 120, 120, 150));
		}

		if(showClipping.get()) {
			const float threshold = clippingThreshold.get();
			for(float value : { -threshold, threshold }) {
				if(value >= range.minimum && value <= range.maximum) {
					const float y = valueToY(value / std::max(gain.get(), 0.000001f),
						trackTop, trackHeight, range);
					drawList->AddLine(ImVec2(position.x, y), ImVec2(position.x + width, y),
						IM_COL32(255, 100, 100, 100));
				}
			}
		}
	}

	void drawHeader(ImDrawList* drawList, const ImVec2& position, float width,
		float trackTop, float trackHeight, int channel) const {
		const auto& headerValues = header.get();
		const auto& thicknessValues = headerThickness.get();
		const auto& opacityValues = headerOpacity.get();

		const float headerPosition = ofClamp(headerValues.size() == 1 ? headerValues[0]
			: (channel < (int)headerValues.size() ? headerValues[channel] : 0.0f), 0.0f, 1.0f);
		const float thickness = ofClamp(thicknessValues.size() == 1 ? thicknessValues[0]
			: (channel < (int)thicknessValues.size() ? thicknessValues[channel] : 0.01f), 0.0f, 1.0f);
		const float opacity = ofClamp(opacityValues.size() == 1 ? opacityValues[0]
			: (channel < (int)opacityValues.size() ? opacityValues[channel] : 1.0f), 0.0f, 1.0f);

		const float left = position.x + (1.0f - headerPosition) * width;
		const float right = std::min(position.x + width, left + thickness * width);
		drawList->AddRectFilled(ImVec2(left, trackTop), ImVec2(right, trackTop + trackHeight),
			IM_COL32(255, 255, 255, (int)(opacity * 128.0f)));
	}

	void drawWaveform() {
		if(waveformData.empty()) return;

		const float zoom = ofxOceanodeShared::getZoomLevel();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
		ImVec2 canvasSize = ImGui::GetContentRegionAvail();
		if(canvasSize.x < 50.0f) canvasSize.x = 800.0f;
		if(canvasSize.y < 50.0f) canvasSize.y = 400.0f;

		const int channelCount = std::max(1, numChannels.get());
		const float trackHeight = canvasSize.y / channelCount;
		const DisplayRange range = getDisplayRange();
		const ImU32 background = ImGui::ColorConvertFloat4ToU32(ImVec4(
			backgroundColor->r / 255.0f, backgroundColor->g / 255.0f,
			backgroundColor->b / 255.0f, backgroundColor->a / 255.0f));
		const ImU32 normalColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
			lineColor->r / 255.0f, lineColor->g / 255.0f, lineColor->b / 255.0f,
			freeze.get() ? 0.8f : 1.0f));
		const ImU32 clipColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
			clippingColor->r / 255.0f, clippingColor->g / 255.0f,
			clippingColor->b / 255.0f, freeze.get() ? 0.8f : 1.0f));

		drawList->PushClipRect(canvasPosition,
			ImVec2(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y), true);
		drawList->AddRectFilled(canvasPosition,
			ImVec2(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y), background);

		for(int channel = 0; channel < channelCount; ++channel) {
			const float trackTop = canvasPosition.y + channel * trackHeight;
			drawGrid(drawList, canvasPosition, canvasSize.x, trackTop, trackHeight, channel, range);

			const int offset = channel * SAMPLES_PER_CHANNEL;
			for(int sample = 0; sample < SAMPLES_PER_CHANNEL - 1; ++sample) {
				const float value1 = waveformData[offset + sample];
				const float value2 = waveformData[offset + sample + 1];
				const float x1 = canvasPosition.x
					+ canvasSize.x * sample / (float)(SAMPLES_PER_CHANNEL - 1);
				const float x2 = canvasPosition.x
					+ canvasSize.x * (sample + 1) / (float)(SAMPLES_PER_CHANNEL - 1);
				const float y1 = valueToY(value1, trackTop, trackHeight, range);
				const float y2 = valueToY(value2, trackTop, trackHeight, range);
				const bool clipped = isClipping(value1) || isClipping(value2);
				drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
					clipped ? clipColor : normalColor, (clipped ? 2.0f : 1.5f) * zoom);
			}

			drawHeader(drawList, canvasPosition, canvasSize.x, trackTop, trackHeight, channel);
		}

		drawList->PopClipRect();
		ImGui::Dummy(canvasSize);
	}

	void renderPreview() {
		if(waveformData.empty()) return;
		ensurePreviewAllocated();
		if(!previewFbo.isAllocated()) return;

		const int width = (int)previewFbo.getWidth();
		const int height = (int)previewFbo.getHeight();
		const int channelCount = std::max(1, numChannels.get());
		const float trackHeight = (float)height / channelCount;
		const DisplayRange range = getDisplayRange();

		previewFbo.begin();
		ofPushStyle();
		ofClear(0, 0, 0, 255);
		ofSetColor(backgroundColor.get());
		ofDrawRectangle(0, 0, width, height);

		for(int channel = 0; channel < channelCount; ++channel) {
			const float trackTop = channel * trackHeight;
			if(gridVisible.get()) {
				ofSetColor(60, 60, 60, 100);
				for(int i = 1; i < 10; ++i) {
					const float x = width * i / 10.0f;
					ofDrawLine(x, trackTop, x, trackTop + trackHeight);
				}
				if(range.minimum <= 0.0f && range.maximum >= 0.0f) {
					ofSetColor(120, 120, 120, 150);
					const float zeroY = valueToY(0.0f, trackTop, trackHeight, range);
					ofDrawLine(0, zeroY, width, zeroY);
				}
			}

			ofPolyline waveform;
			waveform.getVertices().reserve(SAMPLES_PER_CHANNEL);
			const int offset = channel * SAMPLES_PER_CHANNEL;
			for(int sample = 0; sample < SAMPLES_PER_CHANNEL; ++sample) {
				const float x = width * sample / (float)(SAMPLES_PER_CHANNEL - 1);
				waveform.addVertex(x,
					valueToY(waveformData[offset + sample], trackTop, trackHeight, range));
			}
			ofColor color = lineColor.get();
			color.a = freeze.get() ? 204 : 255;
			ofSetColor(color);
			waveform.draw();
		}

		ofPopStyle();
		previewFbo.end();
		previewOut = &previewFbo.getTexture();
	}
};

#endif /* scWavescope_h */
