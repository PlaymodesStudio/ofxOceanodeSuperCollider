#ifndef waveTrack_h
#define waveTrack_h

#include "scNode.h"
#include "ofxSuperCollider.h"
#include "serverManager.h"
#include "imgui.h"
#include "ppqTimeline.h"
#include "transportTrack.h"
#include <filesystem>
#include <cmath>
#include <algorithm>

class waveTrack : public scNode, public transportTrack {
public:
	explicit waveTrack(std::vector<serverManager*> srv)
	: scNode("Wave Track")
	, servers(std::move(srv))
	{
		color = ofColor(180, 120, 255);
	}

	~waveTrack() override {
		if(currentTimeline) currentTimeline->unsubscribeTrack(this);
		freeAllSynths();
		cleanupBuffers();
		destroyedNode.notify();
	}

	void setup() override {
		// --- Timeline ---
		refreshTimelineList();
		addParameterDropdown(timelineSelect, "Timeline", 0, timelineOptions);
		addParameter(trackName.set("Track Name", "Wave " + ofToString(getNumIdentifier())));

		// --- File ---
		addParameter(pathParam.set("Path", ""));
		addParameter(openFileDialog.set("Open"));

		// --- Server ---
		addParameter(serverIdx.set("Server", 0, 0, servers.empty() ? 0 : int(servers.size()-1)));

		// --- Clip ---
		addParameter(clipStart.set("Clip Start", 0.0f, 0.0f, 1024.0f));
		addParameter(gainParam.set("Gain", 1.0f, 0.0f, 2.0f));

		// --- Appearance ---
		addParameter(lineColor.set("Color", ofColor(0, 200, 255), ofColor(0), ofColor(255)));

		// --- SC Output (nodePort) ---
		scNode::addOutput("Out");

		// --- Info outputs ---
		addOutputParameter(numChannelsOut.set("Channels", 0, 0, 16));
		addOutputParameter(durationMsOut.set("Duration", 0.0f, 0.0f, FLT_MAX));
		numChannelsOut.setSerializable(false);
		durationMsOut.setSerializable(false);

		// --- Listeners ---
		trackListeners.push(timelineSelect.newListener([this](int &val){ updateSubscription(); }));

		trackListeners.push(openFileDialog.newListener([this]{
			auto result = ofSystemLoadDialog("Select WAV file", false, ofToDataPath("Supercollider/Samples", true));
			if(result.bSuccess){
				string p = result.getPath();
				ofStringReplace(p, ofToDataPath("Supercollider/Samples/", true), "");
				pathParam = p;
			}
		}));

		trackListeners.push(pathParam.newListener([this](string &s){
			loadFile(s);
		}));

		trackListeners.push(serverIdx.newListener([this](int &i){
			if(!pathParam.get().empty()) loadFile(pathParam.get());
		}));

		trackListeners.push(clipStart.newListener([this](float &val){
			updateSynthClipParams();
		}));

		trackListeners.push(gainParam.newListener([this](float &val){
			for(auto &pair : synthMap) {
				if(pair.second) pair.second->set("gain", val);
			}
		}));

		updateSubscription();
	}

	void update(ofEventArgs &args) override {
		if(!currentTimeline) return;

		double currentBeat = currentTimeline->getBeatPosition();
		float currentBpm = currentTimeline->getBpm();
		bool playing = currentTimeline->isPlaying();

		// Send transport data to all synths
		for(auto &pair : synthMap) {
			if(!pair.second) continue;
			pair.second->set("beatTransport", (float)currentBeat);
			pair.second->set("bpm", currentBpm);
			pair.second->set("play", playing ? 1.0f : 0.0f);
		}

		// Jump detection
		double delta = std::abs(currentBeat - lastBeatTransport);
		double expectedDelta = (currentBpm / 60.0) * (1.0 / 60.0) * 2.0;
		if(delta > expectedDelta && lastBeatTransport >= 0) {
			for(auto &pair : synthMap) {
				if(pair.second) pair.second->set("jump", 1.0f);
			}
			needsJumpReset = true;
		} else if(needsJumpReset) {
			for(auto &pair : synthMap) {
				if(pair.second) pair.second->set("jump", 0.0f);
			}
			needsJumpReset = false;
		}

		lastBeatTransport = currentBeat;

		// Recalculate duration in beats if BPM changed
		if(std::abs(currentBpm - lastBpm) > 0.01f) {
			recalcDurationInBeats(currentBpm);
			updateSynthClipParams();
			lastBpm = currentBpm;
		}
	}

	// ========== scNode interface ==========

	void buildSynth(ofxSCServer* server) override {
		if(fileNumChannels <= 0) return;
		std::string synthName = "waveTrackPlayer" + ofToString(fileNumChannels);
		synthMap[server] = new ofxSCSynth(synthName, server);
	}

	void createSynth(ofxSCServer* server) override {
		if(synthMap.count(server) == 0 || !synthMap[server]) return;

		auto* s = synthMap[server];

		// Set buffer args
		for(int ch = 0; ch < fileNumChannels && ch < (int)scBuffers.size(); ch++) {
			s->set(("buf" + ofToString(ch)).c_str(), scBuffers[ch]->index);
		}

		// Set output bus (will be set by serverManager via setOutputBus)
		s->set("gain", gainParam.get());
		s->set("clipStartBeat", clipStart.get());
		s->set("clipLenBeats", (float)durationInBeats);
		s->set("bpm", lastBpm);
		s->set("play", 0.0f);
		s->set("jump", 0.0f);

		s->create();
	}

	void moveSynthBefore(ofxSCServer* server, int nodeID) override {
		if(synthMap.count(server) && synthMap[server]) {
			// Re-send params after move
			auto* s = synthMap[server];
			for(int ch = 0; ch < fileNumChannels && ch < (int)scBuffers.size(); ch++) {
				s->set(("buf" + ofToString(ch)).c_str(), scBuffers[ch]->index);
			}
			s->set("gain", gainParam.get());
			s->set("clipStartBeat", clipStart.get());
			s->set("clipLenBeats", (float)durationInBeats);
			s->moveBefore(nodeID);
		}
	}

	void free(ofxSCServer* server) override {
		if(synthMap.count(server) && synthMap[server]) {
			synthMap[server]->free();
			delete synthMap[server];
			synthMap.erase(server);
		}
	}

	void setOutputBus(ofxSCServer* server, int index, int bus) override {
		outputBuses[server][index] = bus;
		if(synthMap.count(server) && synthMap[server]) {
			synthMap[server]->set("out", bus);
		}
	}

	int getOutputBusIndex(ofxSCServer* server, int index) override {
		if(outputBuses.count(server) && outputBuses[server].count(index))
			return outputBuses[server][index];
		return -1;
	}

	int getLastSynthID(ofxSCServer* server) override {
		if(synthMap.count(server) && synthMap[server])
			return synthMap[server]->nodeID;
		return -1;
	}

	// ========== transportTrack interface ==========

	std::string getTrackName() override { return trackName.get(); }
	float getHeight() const override { return trackHeight; }
	void setHeight(float h) override { trackHeight = ofClamp(h, MIN_TRACK_HEIGHT, MAX_TRACK_HEIGHT); }
	bool isCollapsed() const override { return collapsed; }
	void setCollapsed(bool c) override { collapsed = c; }

	void drawInTimeline(ImDrawList* dl, ImVec2 pos, ImVec2 sz, double viewStart, double viewEnd) override {
		// 1. Create interaction button
		std::string buttonId = "##waveTrkBtn" + ofToString(getNumIdentifier());
		ImGui::InvisibleButton(buttonId.c_str(), sz);

		ImVec2 p = ImGui::GetItemRectMin();
		ImVec2 s = ImGui::GetItemRectSize();
		ImVec2 endP = ImGui::GetItemRectMax();

		ImVec2 mousePos = ImGui::GetMousePos();
		bool isHovered = ImGui::IsItemHovered();
		bool isAltHeld = ImGui::GetIO().KeyAlt;

		// 2. Background
		dl->AddRectFilled(p, endP, IM_COL32(30, 30, 30, 255));
		dl->AddRect(p, endP, IM_COL32(60, 60, 60, 255));

		double visibleLen = viewEnd - viewStart;
		if(visibleLen <= 0.001) return;

		// 3. Coordinate helpers
		int gridTicks = 0;
		double beatsPerBar = 4.0;
		double currentPlayheadBeat = 0.0;

		if(currentTimeline) {
			gridTicks = currentTimeline->getGridTicks();
			beatsPerBar = double(currentTimeline->getNumerator()) *
						  (4.0 / double(currentTimeline->getDenominator()));
			currentPlayheadBeat = currentTimeline->getBeatPosition();
		}

		auto beatToX = [&](double b) {
			return p.x + float((b - viewStart) / visibleLen) * s.x;
		};

		auto xToBeat = [&](float x) {
			return viewStart + ((x - p.x) / s.x) * visibleLen;
		};

		// 4. Draw grid lines
		int viewStartBar = int(viewStart / beatsPerBar);
		int viewEndBar = int(viewEnd / beatsPerBar) + 1;

		for(int bar = viewStartBar; bar <= viewEndBar; ++bar) {
			double barBeat = bar * beatsPerBar;
			float barX = beatToX(barBeat);
			if(barX < p.x - 5 || barX > endP.x + 5) continue;

			dl->AddLine(ImVec2(barX, p.y), ImVec2(barX, endP.y),
				IM_COL32(120, 120, 120, 255), 2.0f);

			if(gridTicks > 0 && bar < viewEndBar) {
				double gridBeats = gridTicks / 24.0;
				double nextBarBeat = (bar + 1) * beatsPerBar;
				for(double b = barBeat + gridBeats; b < nextBarBeat; b += gridBeats) {
					if(b < viewStart || b > viewEnd) continue;
					float gridX = beatToX(b);
					dl->AddLine(ImVec2(gridX, p.y), ImVec2(gridX, endP.y),
						IM_COL32(70, 70, 70, 100), 0.5f);
				}
			}
		}

		// 5. Draw loop region
		if(currentTimeline && currentTimeline->isLoopEnabled()) {
			double loopStart = currentTimeline->getLoopStart();
			double loopEnd = currentTimeline->getLoopEnd();
			float lx1 = std::max(beatToX(loopStart), p.x);
			float lx2 = std::min(beatToX(loopEnd), endP.x);
			dl->AddRectFilled(ImVec2(lx1, p.y), ImVec2(lx2, endP.y),
				IM_COL32(80, 80, 160, 50));
			dl->AddLine(ImVec2(lx1, p.y), ImVec2(lx1, endP.y),
				IM_COL32(160, 160, 255, 180), 2.0f);
			dl->AddLine(ImVec2(lx2, p.y), ImVec2(lx2, endP.y),
				IM_COL32(160, 160, 255, 180), 2.0f);
		}

		// 6. Draw waveform clip region
		if(!waveformCache.empty() && fileNumChannels > 0 && durationInBeats > 0) {
			double clipEnd = clipStart.get() + durationInBeats;
			float clipX1 = beatToX(clipStart.get());
			float clipX2 = beatToX(clipEnd);

			float drawX1 = std::max(clipX1, p.x);
			float drawX2 = std::min(clipX2, endP.x);

			if(drawX2 > drawX1) {
				dl->AddRectFilled(ImVec2(drawX1, p.y), ImVec2(drawX2, endP.y),
					IM_COL32(lineColor->r, lineColor->g, lineColor->b, 20));

				float trackH = s.y / fileNumChannels;
				int cachePointsPerChannel = (int)waveformCache.size() / fileNumChannels;
				ImU32 waveCol = IM_COL32(lineColor->r, lineColor->g, lineColor->b, 220);

				for(int ch = 0; ch < fileNumChannels; ch++) {
					float chY = p.y + ch * trackH;
					float midY = chY + trackH * 0.5f;
					int cacheBase = ch * cachePointsPerChannel;

					if(ch > 0) {
						dl->AddLine(ImVec2(drawX1, chY), ImVec2(drawX2, chY),
							IM_COL32(80, 80, 80, 100), 1.0f);
					}

					for(int px = (int)drawX1; px < (int)drawX2; px++) {
						float beatAtPx = xToBeat((float)px);
						float beatAtPxNext = xToBeat((float)(px + 1));

						float t1 = (beatAtPx - clipStart.get()) / (float)durationInBeats;
						float t2 = (beatAtPxNext - clipStart.get()) / (float)durationInBeats;

						if(t2 < 0.0f || t1 > 1.0f) continue;
						t1 = ofClamp(t1, 0.0f, 1.0f);
						t2 = ofClamp(t2, 0.0f, 1.0f);

						int idx1 = cacheBase + (int)(t1 * (cachePointsPerChannel - 1));
						int idx2 = cacheBase + (int)(t2 * (cachePointsPerChannel - 1));
						idx1 = ofClamp(idx1, cacheBase, cacheBase + cachePointsPerChannel - 1);
						idx2 = ofClamp(idx2, cacheBase, cacheBase + cachePointsPerChannel - 1);
						if(idx1 > idx2) std::swap(idx1, idx2);

						float minV = 1.0f, maxV = -1.0f;
						for(int i = idx1; i <= idx2; i++) {
							float v = waveformCache[i];
							if(v < minV) minV = v;
							if(v > maxV) maxV = v;
						}

						float y1 = midY - maxV * trackH * 0.45f;
						float y2 = midY - minV * trackH * 0.45f;
						if(std::abs(y1 - y2) < 1.0f) y2 = y1 + 1.0f;

						dl->AddLine(ImVec2((float)px, y1), ImVec2((float)px, y2), waveCol, 1.0f);
					}

					dl->AddLine(ImVec2(drawX1, midY), ImVec2(drawX2, midY),
						IM_COL32(80, 80, 80, 80), 0.5f);
				}

				if(clipX1 >= p.x && clipX1 <= endP.x) {
					dl->AddLine(ImVec2(clipX1, p.y), ImVec2(clipX1, endP.y),
						IM_COL32(lineColor->r, lineColor->g, lineColor->b, 150), 1.5f);
				}
				if(clipX2 >= p.x && clipX2 <= endP.x) {
					dl->AddLine(ImVec2(clipX2, p.y), ImVec2(clipX2, endP.y),
						IM_COL32(lineColor->r, lineColor->g, lineColor->b, 150), 1.5f);
				}
			}

			if(clipX1 >= p.x && clipX1 < endP.x) {
				dl->AddText(ImVec2(clipX1 + 4, p.y + 2),
					IM_COL32(200, 200, 200, 200), fileName.c_str());
			}
		} else {
			dl->AddText(ImVec2(p.x + 10, p.y + s.y * 0.5f - 6),
				IM_COL32(100, 100, 100, 200), "No audio file loaded");
		}

		// 7. Draw playhead
		float playheadX = beatToX(currentPlayheadBeat);
		if(playheadX >= p.x && playheadX <= endP.x) {
			dl->AddLine(ImVec2(playheadX, p.y), ImVec2(playheadX, endP.y),
				IM_COL32(255, 80, 80, 255), 2.5f);
		}

		// 8. Handle Alt+drag to move clip
		if(isAltHeld && isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			isDraggingClip = true;
			dragStartBeat = xToBeat(mousePos.x);
			dragStartClipStart = clipStart.get();
		}

		if(isDraggingClip && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
			double currentBeat = xToBeat(mousePos.x);
			double delta = currentBeat - dragStartBeat;
			float newClipStart = std::max(0.0f, (float)(dragStartClipStart + delta));
			clipStart = newClipStart;
		}

		if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			isDraggingClip = false;
		}

		if(isAltHeld && isHovered && !isDraggingClip) {
			dl->AddText(ImVec2(mousePos.x + 10, mousePos.y - 15),
				IM_COL32(255, 255, 150, 255), "Alt+Drag: Move clip");
		}
	}

	// --- Preset Save/Load ---
	void presetSave(ofJson &json) override {
		json["trackHeight"] = trackHeight;
		json["collapsed"] = collapsed;
	}

	void presetRecallAfterSettingParameters(ofJson &json) override {
		if(json.count("trackHeight") > 0) {
			trackHeight = json["trackHeight"];
			trackHeight = ofClamp(trackHeight, MIN_TRACK_HEIGHT, MAX_TRACK_HEIGHT);
		}
		if(json.count("collapsed") > 0) {
			collapsed = json["collapsed"];
		}
	}

private:
	// --- Parameters ---
	ofParameter<int> timelineSelect;
	ofParameter<std::string> trackName;
	ofParameter<std::string> pathParam;
	ofParameter<void> openFileDialog;
	ofParameter<int> serverIdx;
	ofParameter<float> clipStart;
	ofParameter<float> gainParam;
	ofParameter<ofColor> lineColor;

	// Info outputs
	ofParameter<int> numChannelsOut;
	ofParameter<float> durationMsOut;

	// --- Timeline ---
	ppqTimeline* currentTimeline = nullptr;
	std::vector<std::string> timelineOptions;

	// --- SC Resources ---
	std::vector<serverManager*> servers;
	std::vector<ofxSCBuffer*> scBuffers;
	std::map<ofxSCServer*, ofxSCSynth*> synthMap;
	std::map<ofxSCServer*, std::map<int, int>> outputBuses;

	// --- File info ---
	std::string fileName;
	int fileNumChannels = 0;
	float fileSampleRate = 0.0f;
	float fileDurationMs = 0.0f;
	double durationInBeats = 0.0;

	// --- Waveform cache for display ---
	std::vector<float> waveformCache;
	static constexpr int CACHE_POINTS_PER_CHANNEL = 4000;

	// --- Transport tracking ---
	double lastBeatTransport = -1.0;
	float lastBpm = 120.0f;
	bool needsJumpReset = false;

	// --- Interaction ---
	bool isDraggingClip = false;
	double dragStartBeat = 0.0;
	float dragStartClipStart = 0.0f;

	// --- Track layout ---
	float trackHeight = 100.0f;
	bool collapsed = false;
	static constexpr float MIN_TRACK_HEIGHT = 40.0f;
	static constexpr float MAX_TRACK_HEIGHT = 400.0f;

	ofEventListeners trackListeners;

	// --- Helpers ---

	bool validServer() const {
		return !servers.empty() && serverIdx.get() >= 0 && serverIdx.get() < (int)servers.size();
	}

	void refreshTimelineList() {
		timelineOptions.clear();
		timelineOptions.push_back("None");
		for(auto* tl : ppqTimeline::getTimelines()) {
			timelineOptions.push_back("Timeline " + ofToString(tl->getNumIdentifier()));
		}
		timelineSelect.set("Timeline", 0, 0, (int)timelineOptions.size()-1);
	}

	void updateSubscription() {
		if(currentTimeline) currentTimeline->unsubscribeTrack(this);
		int idx = timelineSelect.get() - 1;
		auto& tls = ppqTimeline::getTimelines();
		if(idx >= 0 && idx < (int)tls.size()) {
			currentTimeline = tls[idx];
			currentTimeline->subscribeTrack(this);
			if(fileDurationMs > 0) {
				recalcDurationInBeats(currentTimeline->getBpm());
				updateSynthClipParams();
			}
		} else {
			currentTimeline = nullptr;
		}
	}

	void recalcDurationInBeats(float bpm) {
		if(fileDurationMs <= 0 || bpm <= 0) {
			durationInBeats = 0;
			return;
		}
		double durationSec = fileDurationMs / 1000.0;
		durationInBeats = durationSec * (bpm / 60.0);
	}

	void updateSynthClipParams() {
		for(auto &pair : synthMap) {
			if(pair.second) {
				pair.second->set("clipStartBeat", clipStart.get());
				pair.second->set("clipLenBeats", (float)durationInBeats);
			}
		}
	}

	// --- File Loading ---

	string resolveToAbsolutePath(const string& inputPath) {
		string sClean = inputPath;
		if(sClean.size() >= 2 && sClean[0] == '.' && sClean[1] == '/') {
			sClean = sClean.substr(2);
		}
		if(!sClean.empty() && sClean[0] == '/') return sClean;

		string dataRelativePath = ofToDataPath(sClean, true);
		if(ofFile::doesFileExist(dataRelativePath)) return dataRelativePath;

		return ofToDataPath("Supercollider/Samples/" + sClean, true);
	}

	void getFileInfo(string filepath, int &numChannels, float &durationMs, float &sampleRate) {
		numChannels = 0;
		durationMs = 0.0f;
		sampleRate = 0.0f;

		ofFile file(filepath, ofFile::ReadOnly, true);
		if(!file.is_open()) return;

		char riff[4], wave[4];
		uint32_t riffSize = 0;
		if(!file.read((char*)&riff, 4) || !file.read((char*)&riffSize, 4) || !file.read((char*)&wave, 4)) {
			file.close(); return;
		}
		if(std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
			file.close(); return;
		}

		bool haveFmt = false, haveData = false;
		uint16_t bitsPerSample = 0;
		uint32_t dataSize = 0;

		while(true) {
			char chunkID[4];
			uint32_t chunkSize = 0;
			if(!file.read((char*)&chunkID, 4)) break;
			if(!file.read((char*)&chunkSize, 4)) break;

			if(std::strncmp(chunkID, "fmt ", 4) == 0 && chunkSize >= 16) {
				uint16_t formatType, channels, bits;
				uint32_t srate, byterate;
				uint16_t blockAlign;

				file.read((char*)&formatType, 2);
				file.read((char*)&channels, 2);
				file.read((char*)&srate, 4);
				file.read((char*)&byterate, 4);
				file.read((char*)&blockAlign, 2);
				file.read((char*)&bits, 2);

				numChannels = channels;
				sampleRate = (float)srate;
				bitsPerSample = bits;
				haveFmt = true;

				if(chunkSize > 16) file.seekg(chunkSize - 16, std::ios::cur);
			} else if(std::strncmp(chunkID, "data", 4) == 0) {
				dataSize = chunkSize;
				haveData = true;
				file.seekg(chunkSize, std::ios::cur);
			} else {
				file.seekg(chunkSize, std::ios::cur);
			}

			if(chunkSize & 1u) file.seekg(1, std::ios::cur);
			if(haveFmt && haveData) break;
		}

		if(haveFmt && haveData && sampleRate > 0.0f && numChannels > 0 && bitsPerSample > 0) {
			int bytesPerSample = bitsPerSample / 8;
			int totalSamples = (bytesPerSample > 0 && numChannels > 0)
				? (dataSize / (bytesPerSample * numChannels)) : 0;
			durationMs = (totalSamples > 0) ? (float(totalSamples) / sampleRate * 1000.0f) : 0.0f;
		}

		file.close();
	}

	void loadWaveformCache(const string& filepath, int numChannels, float sampleRate) {
		waveformCache.clear();
		if(numChannels <= 0 || sampleRate <= 0) return;

		ofFile file(filepath, ofFile::ReadOnly, true);
		if(!file.is_open()) return;

		char riff[4], wave[4];
		uint32_t riffSize;
		file.read((char*)&riff, 4);
		file.read((char*)&riffSize, 4);
		file.read((char*)&wave, 4);

		uint16_t bitsPerSample = 0;
		uint32_t dataSize = 0;
		bool foundData = false;

		while(true) {
			char chunkID[4];
			uint32_t chunkSize = 0;
			if(!file.read((char*)&chunkID, 4)) break;
			if(!file.read((char*)&chunkSize, 4)) break;

			if(std::strncmp(chunkID, "fmt ", 4) == 0 && chunkSize >= 16) {
				uint16_t fmt, ch, bits;
				uint32_t sr, br;
				uint16_t ba;
				file.read((char*)&fmt, 2);
				file.read((char*)&ch, 2);
				file.read((char*)&sr, 4);
				file.read((char*)&br, 4);
				file.read((char*)&ba, 2);
				file.read((char*)&bits, 2);
				bitsPerSample = bits;
				if(chunkSize > 16) file.seekg(chunkSize - 16, std::ios::cur);
			} else if(std::strncmp(chunkID, "data", 4) == 0) {
				dataSize = chunkSize;
				foundData = true;
				break;
			} else {
				file.seekg(chunkSize, std::ios::cur);
			}
			if(chunkSize & 1u) file.seekg(1, std::ios::cur);
		}

		if(!foundData || bitsPerSample == 0) { file.close(); return; }

		int bytesPerSample = bitsPerSample / 8;
		int totalFrames = dataSize / (bytesPerSample * numChannels);
		int pointsPerChannel = CACHE_POINTS_PER_CHANNEL;
		int framesPerPoint = std::max(1, totalFrames / pointsPerChannel);

		waveformCache.resize(numChannels * pointsPerChannel, 0.0f);

		std::vector<char> frameBuf(bytesPerSample * numChannels);

		for(int pt = 0; pt < pointsPerChannel; pt++) {
			int startFrame = pt * framesPerPoint;
			int endFrame = std::min(startFrame + framesPerPoint, totalFrames);

			std::vector<float> minVals(numChannels, 1.0f);
			std::vector<float> maxVals(numChannels, -1.0f);

			for(int f = startFrame; f < endFrame; f++) {
				if(!file.read(frameBuf.data(), bytesPerSample * numChannels)) break;

				for(int ch = 0; ch < numChannels; ch++) {
					float sample = 0.0f;
					int offset = ch * bytesPerSample;

					if(bitsPerSample == 16) {
						int16_t val = *(int16_t*)(frameBuf.data() + offset);
						sample = val / 32768.0f;
					} else if(bitsPerSample == 24) {
						int32_t val = 0;
						memcpy(&val, frameBuf.data() + offset, 3);
						if(val & 0x800000) val |= 0xFF000000;
						sample = val / 8388608.0f;
					} else if(bitsPerSample == 32) {
						sample = *(float*)(frameBuf.data() + offset);
					}

					if(sample < minVals[ch]) minVals[ch] = sample;
					if(sample > maxVals[ch]) maxVals[ch] = sample;
				}
			}

			for(int ch = 0; ch < numChannels; ch++) {
				float peak = (std::abs(maxVals[ch]) >= std::abs(minVals[ch]))
					? maxVals[ch] : minVals[ch];
				waveformCache[ch * pointsPerChannel + pt] = peak;
			}
		}

		file.close();
	}

	void loadFile(const string& inputPath) {
		freeAllSynths();
		cleanupBuffers();
		waveformCache.clear();
		fileNumChannels = 0;
		fileSampleRate = 0.0f;
		fileDurationMs = 0.0f;
		durationInBeats = 0;
		fileName = "";

		if(inputPath.empty()) {
			numChannelsOut = 0;
			durationMsOut = 0.0f;
			return;
		}

		if(!validServer()) return;

		string absolutePath = resolveToAbsolutePath(inputPath);
		if(!ofFile::doesFileExist(absolutePath)) {
			ofLogError("waveTrack") << "File not found: " << absolutePath;
			return;
		}

		int numCh = 0;
		float durMs = 0.0f, srate = 0.0f;
		getFileInfo(absolutePath, numCh, durMs, srate);

		if(numCh <= 0 || numCh > 16) {
			ofLogError("waveTrack") << "Unsupported channel count: " << numCh;
			return;
		}

		fileNumChannels = numCh;
		fileSampleRate = srate;
		fileDurationMs = durMs;
		fileName = ofFilePath::getFileName(absolutePath);

		// Load SC buffers (one per channel)
		auto* srv = servers[serverIdx]->getServer();
		for(int ch = 0; ch < numCh; ch++) {
			auto* buf = new ofxSCBuffer(0, 0, srv);
			buf->readChannel(absolutePath, {ch});
			scBuffers.push_back(buf);
		}

		// Load waveform cache for display
		loadWaveformCache(absolutePath, numCh, srate);

		// Calculate duration in beats
		float bpm = currentTimeline ? currentTimeline->getBpm() : 120.0f;
		recalcDurationInBeats(bpm);
		lastBpm = bpm;

		numChannelsOut = numCh;
		durationMsOut = durMs;

		lastBeatTransport = -1.0;

		// Trigger graph recompute so serverManager creates our synth
		for(auto& output : outputs) output = output;

		ofLogNotice("waveTrack") << "Loaded: " << fileName
			<< " (" << numCh << "ch, " << (durMs/1000.0f) << "s, "
			<< srate << "Hz, " << durationInBeats << " beats)";
	}

	void freeAllSynths() {
		for(auto &pair : synthMap) {
			if(pair.second) {
				pair.second->free();
				delete pair.second;
			}
		}
		synthMap.clear();
		outputBuses.clear();
	}

	void cleanupBuffers() {
		for(auto* buf : scBuffers) {
			if(buf) {
				buf->free();
				delete buf;
			}
		}
		scBuffers.clear();
	}
};

#endif /* waveTrack_h */
