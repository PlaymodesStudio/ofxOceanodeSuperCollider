#include "scFM7.h"
#include "ofxOceanodeShared.h"

scFM7::scFM7() : scNode("SuperFM") {
	// Initialize Defaults
	allEgLevels.resize(24, 1.0f);
	allEgTimes.resize(24);
	derivedRates.resize(24);
	
	// Initialize default times
	for(int i=0; i<6; i++) {
		for(int j=0; j<4; j++) {
			allEgTimes[i*4 + j] = (float)(j+1) * 0.25f;
		}
	}
	
	presetSlots.resize(16);
	activeMatrixCell = -1;
	draggingPoint = -1;
	activePresetSlot = -1;
	isMorphing = false;
}

scFM7::~scFM7() {
	listeners.unsubscribeAll();
	for(auto& pair : synthInstances) {
		if(pair.second) { pair.second->free(); delete pair.second; }
	}
	synthInstances.clear();
}

void scFM7::setup() {
	// 1. Core
	addParameter(numChannels.set("N Chan", 1, 1, 64));
	
	// --- PRESETS ---
	addSeparator("Snapshots", ofColor(200));
	uiPresets.set("PresetsUI", [this](){ drawPresetSlots(); });
	addCustomRegion(uiPresets, [this](){ drawPresetSlots(); });
	addParameter(morphTime.set("Morph Time", 0.0f, 0.0f, 10.0f));
	
	addSeparator("Performance", ofColor(200));
	
	addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f}));
	addParameter(gate.set("Gate", {0}, {0}, {1}));
	addParameter(levels.set("Levels", {1.0f}, {0.0f}, {1.0f}));
	
	
	// New Vibrato / Tremolo Vectors
	addParameter(vibFreq.set("Vib Freq", {5.0f}, {0.1f}, {20.0f}));
	addParameter(vibAmp.set("Vib Amp", {0.0f}, {0.0f}, {2.0f})); // Pitch mod amount
	addParameter(tremFreq.set("Trem Freq", {5.0f}, {0.1f}, {20.0f}));
	addParameter(tremAmp.set("Trem Amp", {0.0f}, {0.0f}, {1.0f}));

	// 2. Global
	addParameter(masterAmp.set("Amp", 1.0f, 0.0f, 1.0f));
	addParameter(feedback.set("Feedback", 1.0f, 0.0f, 4.0f));
	
	// 3. GUI Layout
	addInspectorParameter(widgetWidth.set("Widget Width", 240.0f, 150.0f, 800.0f));
	addInspectorParameter(matrixHeight.set("Matrix Height", 200.0f, 100.0f, 500.0f));
	addInspectorParameter(envelopeHeight.set("Env Height", 120.0f, 50.0f, 300.0f));

	// --- MODULATION MATRIX ---
	addSeparator("Modulation Matrix", ofColor(200));
	addParameter(modScale.set("Scale", {1.0f}, {0.0f}, {10.0f}));
	addParameter(modMatrix.set("Data", vector<float>(36, 0.0f), vector<float>(36, 0.0f), vector<float>(36, 4.0f)));
	
	uiMatrix.set("MatrixUI", [this](){ drawModMatrix(); });
	addCustomRegion(uiMatrix, [this](){ drawModMatrix(); });

	// --- OPERATORS ---
	addSeparator("Operators", ofColor(200));

	addParameter(opAmps.set("Op Amps",
		{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
		vector<float>(6, 0.0f), vector<float>(6, 1.0f)));
		
	addParameter(opRatios.set("Op Ratios",
		vector<float>(6, 1.0f),
		vector<float>(6, 0.0f), vector<float>(6, 20.0f)));
		
	addParameter(opDetunes.set("Op Detunes",
		vector<float>(6, 0.0f),
		vector<float>(6, -5.0f), vector<float>(6, 5.0f)));
	
	// --- ENVELOPE EDITOR ---
	addSeparator("Envelopes", ofColor(200));

	addParameter(selectedOp.set("Op", 0, 0, 5));
	addParameter(totalDuration.set("Duration", 2.0f, 0.1f, 20.0f));
	
	addParameter(currentEgLevels.set("EG Levels", {1.f, 1.f, 1.f, 0.f}, vector<float>(4,0.f), vector<float>(4,1.f)), ofxOceanodeParameterFlags_DisplayMinimized);
	addParameter(currentEgTimes.set("EG Times", {0.25f, 0.5f, 0.75f, 1.0f}, vector<float>(4,0.0f), vector<float>(4,1.0f)), ofxOceanodeParameterFlags_DisplayMinimized);

	uiEnvelope.set("EnvEditor", [this](){ drawEnvelopeEditor(); });
	addCustomRegion(uiEnvelope, [this](){ drawEnvelopeEditor(); });
	
	addSeparator("Output", ofColor(200));
	scNode::addOutput("Out");

	// --- Load Presets from Disk ---
	loadAllPresetsFromDisk();

	// --- LISTENERS ---

	listeners.push(numChannels.newListener([this](int &n){
		if(oldNumChannels != n) {
			for(auto& output : outputs) output = output;
		}
		oldNumChannels = n;
	}));

	listeners.push(pitch.newListener([this](vector<float> &v){ sendFloatParameter("pitch", v); }));
	listeners.push(gate.newListener([this](vector<int> &v){ sendIntParameter("gate", v); }));
	listeners.push(levels.newListener([this](vector<float> &v){ sendFloatParameter("velocity", v); }));
	listeners.push(modScale.newListener([this](vector<float> &v){ sendFloatParameter("modScale", v); }));
	
	// New Vector Listeners
	listeners.push(vibFreq.newListener([this](vector<float> &v){ sendFloatParameter("vibFreq", v); }));
	listeners.push(vibAmp.newListener([this](vector<float> &v){ sendFloatParameter("vibAmp", v); }));
	listeners.push(tremFreq.newListener([this](vector<float> &v){ sendFloatParameter("tremFreq", v); }));
	listeners.push(tremAmp.newListener([this](vector<float> &v){ sendFloatParameter("tremAmp", v); }));

	listeners.push(opAmps.newListener([this](vector<float> &v){
		if(!isMorphing) for(auto& pair : synthInstances) if(pair.second) pair.second->set("op_amps", v);
	}));
	listeners.push(opRatios.newListener([this](vector<float> &v){
		if(!isMorphing) for(auto& pair : synthInstances) if(pair.second) pair.second->set("op_ratios", v);
	}));
	listeners.push(opDetunes.newListener([this](vector<float> &v){
		if(!isMorphing) for(auto& pair : synthInstances) if(pair.second) pair.second->set("op_detunes", v);
	}));
	
	// Mod Matrix Listener
	listeners.push(modMatrix.newListener([this](vector<float> &v){
		if(!isMorphing) updateMatrixParams();
	}));

	auto syncFloat = [this](const string& name, float& v) {
		for(auto& pair : synthInstances) if(pair.second) pair.second->set(name, v);
	};
	listeners.push(masterAmp.newListener([=](float &v){ syncFloat("amp", v); }));
	listeners.push(feedback.newListener([=](float &v){
		if(!isMorphing) syncFloat("feedback", v);
	}));

	// Envelope Logic
	listeners.push(selectedOp.newListener([this](int &op){
		refreshEnvelopeGUI();
	}));
	
	listeners.push(totalDuration.newListener([this](float &f){
		updateEnvelopeParams();
	}));

	listeners.push(currentEgLevels.newListener([this](vector<float> &v){ updateEnvelopeParams(); }));
	listeners.push(currentEgTimes.newListener([this](vector<float> &v){ updateEnvelopeParams(); }));
	
	// Initial Calc
	updateEnvelopeParams();
}

void scFM7::update(ofEventArgs& args) {
	if(isMorphing) updateMorph();
}

// --- HELPERS ---

void scFM7::updateMatrixParams() {
	// Send the vector from the parameter
	for(auto& pair : synthInstances) {
		if(pair.second) pair.second->set("mod_matrix", modMatrix.get());
	}
}

void scFM7::updateAllParamsToSynth() {
	updateMatrixParams();
	updateEnvelopeParams();
	
	for(auto& pair : synthInstances) {
		if(pair.second) {
			pair.second->set("op_amps", opAmps.get());
			pair.second->set("op_ratios", opRatios.get());
			pair.second->set("op_detunes", opDetunes.get());
			pair.second->set("feedback", feedback.get());
		}
	}
}

void scFM7::drawPresetSlots() {
	float zoom = ofxOceanodeShared::getZoomLevel();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = widgetWidth.get() * zoom;
	
	float slotSize = w / 8.0f;
	float h = slotSize * 2.0f;
	
	ImGui::InvisibleButton("##Presets", ImVec2(w, h));
	bool isActive = ImGui::IsItemActive();
	ImVec2 mouse = ImGui::GetIO().MousePos;
	bool mouseDown = ImGui::IsMouseClicked(0);
	bool rightClick = ImGui::IsMouseClicked(1);
	bool shift = ImGui::GetIO().KeyShift;
	
	for(int i=0; i<16; i++) {
		int r = i / 8;
		int c = i % 8;
		ImVec2 slotP = ImVec2(p.x + c*slotSize, p.y + r*slotSize);
		ImVec2 slotMax = ImVec2(slotP.x + slotSize - 2, slotP.y + slotSize - 2);
		
		bool hasData = presetSlots[i].hasData;
		bool hovered = (mouse.x >= slotP.x && mouse.x < slotMax.x &&
						mouse.y >= slotP.y && mouse.y < slotMax.y);
		
		if(hovered && isActive && mouseDown) {
			if(shift) storeToSlot(i);
			else recallSlot(i);
		}

		if(hovered && rightClick) {
			deletePresetFromDisk(i);
		}
		
		ImU32 col;
		if(i == activePresetSlot) {
			col = IM_COL32(180, 255, 180, 255);
		} else if (hasData) {
			col = IM_COL32(100, 180, 100, 255);
		} else {
			col = IM_COL32(60, 60, 60, 255);
		}
		
		if(hovered) {
			int r = (int)(col & 0xFF) + 30;
			int g = (int)((col >> 8) & 0xFF) + 30;
			int b = (int)((col >> 16) & 0xFF) + 30;
			col = IM_COL32(std::min(r, 255), std::min(g, 255), std::min(b, 255), 255);
		}
		
		drawList->AddRectFilled(slotP, slotMax, col);
		
		char buf[8]; sprintf(buf, "%d", i+1);
		drawList->AddText(ImVec2(slotP.x+2, slotP.y+2), IM_COL32(255,255,255,180), buf);
		if(shift && hovered) drawList->AddText(ImVec2(slotP.x+slotSize-15, slotP.y+slotSize-15), IM_COL32(255,0,0,255), "S");
		if(!shift && hovered && rightClick) drawList->AddText(ImVec2(slotP.x+slotSize-15, slotP.y+slotSize-15), IM_COL32(255,0,0,255), "X");
	}
}

void scFM7::drawEnvelopeEditor() {
	float zoom = ofxOceanodeShared::getZoomLevel();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(widgetWidth.get() * zoom, envelopeHeight.get() * zoom);
	
	ImGui::InvisibleButton("##env_interaction", size);
	bool isActive = ImGui::IsItemActive();
	bool isHovered = ImGui::IsItemHovered();
	
	drawList->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(30, 30, 30, 255));
	drawList->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(80, 80, 80, 255));
	
	vector<float> levels = currentEgLevels.get();
	vector<float> times = currentEgTimes.get();
	
	float dur = totalDuration.get();
	float pxPerSec = size.x / std::max(dur, 0.1f);
	
	for(int s=1; s <= (int)dur; s++) {
		float gx = p.x + s * pxPerSec;
		if(gx < p.x + size.x) {
			drawList->AddLine(ImVec2(gx, p.y), ImVec2(gx, p.y + size.y), IM_COL32(60,60,60,255));
		}
	}
	
	vector<ImVec2> points;
	points.push_back(ImVec2(p.x, p.y + size.y));
	
	for(int i=0; i<4; i++) {
		float x = p.x + (times[i] * size.x);
		float y = p.y + size.y - (levels[i] * size.y);
		points.push_back(ImVec2(x, y));
	}
	
	for(size_t i=0; i<points.size()-1; i++) {
		if(points[i].x <= p.x + size.x) {
			ImVec2 p2 = points[i+1];
			if(p2.x > p.x + size.x) p2.x = p.x + size.x;
			drawList->AddLine(points[i], p2, IM_COL32(200, 200, 200, 255), 2.0f);
		}
	}
	
	ImVec2 mouse = ImGui::GetIO().MousePos;
	bool mouseDown = ImGui::IsMouseDown(0);
	if(!mouseDown) draggingPoint = -1;
	
	float handleRadius = 6.0f * zoom;
	
	for(int i=0; i<4; i++) {
		ImVec2 pt = points[i+1];
		if(pt.x > p.x + size.x + handleRadius) continue;
		
		float dx = mouse.x - pt.x;
		float dy = mouse.y - pt.y;
		bool pointHovered = (dx*dx + dy*dy) < (handleRadius*handleRadius*4);
		
		if((isHovered && pointHovered) || draggingPoint == i) {
			drawList->AddCircleFilled(pt, handleRadius, IM_COL32(255, 255, 0, 255));
			if(isHovered && pointHovered) {
				ImGui::BeginTooltip();
				float t = times[i] * dur;
				float prevT = (i == 0) ? 0.0f : times[i-1] * dur;
				ImGui::Text("Time: %.2fs (Seg: %.2fs)", t, t - prevT);
				ImGui::Text("Level: %.2f", levels[i]);
				ImGui::EndTooltip();
			}
			if(ImGui::IsMouseClicked(0) && pointHovered) draggingPoint = i;
		} else {
			drawList->AddCircleFilled(pt, handleRadius-2, IM_COL32(100, 200, 255, 200));
		}
		
		if(draggingPoint == i && isActive) {
			float mouseRelY = 1.0f - (mouse.y - p.y) / size.y;
			levels[i] = ofClamp(mouseRelY, 0.0f, 1.0f);
			
			float mouseRelX = (mouse.x - p.x) / size.x;
			times[i] = ofClamp(mouseRelX, 0.0f, 1.0f);
			
			float minX = (i == 0) ? 0.001f : times[i-1] + 0.001f;
			if(times[i] < minX) times[i] = minX;
			
			if(i < 3 && times[i] >= times[i+1]) times[i+1] = times[i] + 0.001f;
			if(i < 2 && times[i+1] >= times[i+2]) times[i+2] = times[i+1] + 0.001f;
			if(i < 1 && times[i+2] >= times[i+3]) times[i+3] = times[i+2] + 0.001f;
			
			for(int k=0; k<4; k++) if(times[k] > 1.0f) times[k] = 1.0f;
			
			currentEgLevels.set(levels);
			currentEgTimes.set(times);
			updateEnvelopeParams();
		}
	}
}
void scFM7::refreshEnvelopeGUI() {
	int op = selectedOp.get();
	vector<float> levels, times;
	int offset = op * 4;
	for(int i=0; i<4; i++) {
		if(offset+i < allEgLevels.size()) {
			levels.push_back(allEgLevels[offset+i]);
			times.push_back(allEgTimes[offset+i]);
		} else {
			levels.push_back(0.0f); times.push_back((float)(i+1)*0.25f);
		}
	}
	currentEgLevels.setWithoutEventNotifications(levels);
	currentEgTimes.setWithoutEventNotifications(times);
}

void scFM7::updateEnvelopeParams() {
	if(isMorphing) return;
	
	int op = selectedOp.get();
	int offset = op * 4;
	vector<float> guiLevels = currentEgLevels.get();
	vector<float> guiTimes = currentEgTimes.get();
	
	for(int i=0; i<4; i++) {
		if(i < guiLevels.size()) allEgLevels[offset+i] = guiLevels[i];
		if(i < guiTimes.size()) allEgTimes[offset+i] = guiTimes[i];
	}
	
	float dur = totalDuration.get();
	if(dur < 0.01f) dur = 0.01f;
	
	if(derivedRates.size() != 24) derivedRates.resize(24);
	
	for(int o=0; o<6; o++) {
		int off = o*4;
		float prevTime = 0.0f;
		for(int i=0; i<4; i++) {
			float currTime = allEgTimes[off+i];
			float segFraction = currTime - prevTime;
			if(segFraction < 0.001f) segFraction = 0.001f;
			
			float segDuration = segFraction * dur;
			derivedRates[off+i] = 1.0f / segDuration;
			prevTime = currTime;
		}
	}
	
	for(auto& pair : synthInstances) {
		if(pair.second) {
			pair.second->set("eg_levels", allEgLevels);
			pair.second->set("eg_rates", derivedRates);
		}
	}
}

void scFM7::sendFloatParameter(const string& name, vector<float>& values) {
	int n = numChannels.get();
	for(auto& pair : synthInstances) {
		if(pair.second == nullptr) continue;
		if(values.size() == 1) pair.second->setMultiple(name, values[0], n);
		else pair.second->set(name, values);
	}
}

void scFM7::sendIntParameter(const string& name, vector<int>& values) {
	int n = numChannels.get();
	for(auto& pair : synthInstances) {
		if(pair.second == nullptr) continue;
		if(values.size() == 1) pair.second->setMultiple(name, values[0], n);
		else pair.second->set(name, values);
	}
}

// --- PRESET & MORPHING ---

void scFM7::storeToSlot(int slot) {
	if(slot < 0 || slot >= 16) return;
	
	FMPatch p;
	p.matrix = modMatrix.get();
	p.egLevels = allEgLevels;
	p.egTimes = allEgTimes;
	p.opAmps = opAmps.get();
	p.opRatios = opRatios.get();
	p.opDetunes = opDetunes.get();
	
	p.feedback = feedback.get();
	p.duration = totalDuration.get();
	p.masterAmp = masterAmp.get();
	
	// For vectors that are inputs, we store the first value as the "Patch Default"
	if(vibFreq.get().size() > 0) p.vibFreq = vibFreq.get()[0];
	if(vibAmp.get().size() > 0) p.vibAmp = vibAmp.get()[0];
	if(tremFreq.get().size() > 0) p.tremFreq = tremFreq.get()[0];
	if(tremAmp.get().size() > 0) p.tremAmp = tremAmp.get()[0];
	
	p.hasData = true;
	presetSlots[slot] = p;
	activePresetSlot = slot;

	// Save to disk
	savePresetToDisk(slot);
}

void scFM7::recallSlot(int slot) {
	if(slot < 0 || slot >= 16) return;
	if(!presetSlots[slot].hasData) return;
	activePresetSlot = slot;
	
	if(morphTime.get() <= 0.001f) {
		FMPatch p = presetSlots[slot];
		modMatrix.set(p.matrix); // This triggers listener -> updates synth
		allEgLevels = p.egLevels;
		allEgTimes = p.egTimes;
		
		opAmps.set(p.opAmps);
		opRatios.set(p.opRatios);
		opDetunes.set(p.opDetunes);
		feedback.set(p.feedback);
		totalDuration.set(p.duration);
		masterAmp.set(p.masterAmp);
		
		// Set vectors to scalar default
		vibFreq.set({p.vibFreq});
		vibAmp.set({p.vibAmp});
		tremFreq.set({p.tremFreq});
		tremAmp.set({p.tremAmp});
		
		refreshEnvelopeGUI();
		updateAllParamsToSynth();
	} else {
		// Capture Start
		startPatch.matrix = modMatrix.get();
		startPatch.egLevels = allEgLevels;
		startPatch.egTimes = allEgTimes;
		startPatch.opAmps = opAmps.get();
		startPatch.opRatios = opRatios.get();
		startPatch.opDetunes = opDetunes.get();
		startPatch.feedback = feedback.get();
		startPatch.duration = totalDuration.get();
		startPatch.masterAmp = masterAmp.get();
		
		if(vibFreq.get().size() > 0) startPatch.vibFreq = vibFreq.get()[0];
		if(vibAmp.get().size() > 0) startPatch.vibAmp = vibAmp.get()[0];
		if(tremFreq.get().size() > 0) startPatch.tremFreq = tremFreq.get()[0];
		if(tremAmp.get().size() > 0) startPatch.tremAmp = tremAmp.get()[0];
		
		targetPatch = presetSlots[slot];
		morphStartTime = ofGetElapsedTimef();
		isMorphing = true;
	}
}

void scFM7::updateMorph() {
	float now = ofGetElapsedTimef();
	float progress = (now - morphStartTime) / std::max(morphTime.get(), 0.001f);
	if(progress >= 1.0f) { progress = 1.0f; isMorphing = false; }
	
	auto lerpVec = [&](const vector<float>& a, const vector<float>& b, vector<float>& out, float t) {
		if(a.size() != b.size() || a.size() != out.size()) return;
		for(size_t i=0; i<out.size(); i++) out[i] = a[i] + (b[i] - a[i]) * t;
	};
	
	vector<float> currMatrix = modMatrix.get();
	lerpVec(startPatch.matrix, targetPatch.matrix, currMatrix, progress);
	modMatrix.setWithoutEventNotifications(currMatrix); // Avoid spamming if possible
	
	lerpVec(startPatch.egLevels, targetPatch.egLevels, allEgLevels, progress);
	lerpVec(startPatch.egTimes, targetPatch.egTimes, allEgTimes, progress);
	
	vector<float> currAmps = opAmps.get();
	vector<float> currRatios = opRatios.get();
	vector<float> currDetunes = opDetunes.get();
	lerpVec(startPatch.opAmps, targetPatch.opAmps, currAmps, progress);
	lerpVec(startPatch.opRatios, targetPatch.opRatios, currRatios, progress);
	lerpVec(startPatch.opDetunes, targetPatch.opDetunes, currDetunes, progress);
	
	float currFb = ofLerp(startPatch.feedback, targetPatch.feedback, progress);
	float currDur = ofLerp(startPatch.duration, targetPatch.duration, progress);
	float currAmp = ofLerp(startPatch.masterAmp, targetPatch.masterAmp, progress);
	
	// Float -> Vector Morph
	vector<float> vf = { ofLerp(startPatch.vibFreq, targetPatch.vibFreq, progress) };
	vector<float> va = { ofLerp(startPatch.vibAmp, targetPatch.vibAmp, progress) };
	vector<float> tf = { ofLerp(startPatch.tremFreq, targetPatch.tremFreq, progress) };
	vector<float> ta = { ofLerp(startPatch.tremAmp, targetPatch.tremAmp, progress) };
	
	opAmps.setWithoutEventNotifications(currAmps);
	opRatios.setWithoutEventNotifications(currRatios);
	opDetunes.setWithoutEventNotifications(currDetunes);
	feedback.setWithoutEventNotifications(currFb);
	totalDuration.setWithoutEventNotifications(currDur);
	masterAmp.setWithoutEventNotifications(currAmp);
	
	vibFreq.setWithoutEventNotifications(vf);
	vibAmp.setWithoutEventNotifications(va);
	tremFreq.setWithoutEventNotifications(tf);
	tremAmp.setWithoutEventNotifications(ta);
	
	refreshEnvelopeGUI();
	updateAllParamsToSynth();
	
	// Manually sync the Morphing variables that we set without notification
	sendFloatParameter("vibFreq", vf);
	sendFloatParameter("vibAmp", va);
	sendFloatParameter("tremFreq", tf);
	sendFloatParameter("tremAmp", ta);
}

// --- NODE LOGIC ---

void scFM7::createSynth(ofxSCServer* server) {
	if(!server) return;

	// If buildSynth wasn't called first, allocate now
	if(!synthInstances[server]) {
		synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
	}

	auto synth = synthInstances[server];
	
	updateEnvelopeParams();
	
	synth->set("amp", masterAmp.get());
	synth->set("feedback", feedback.get());
	synth->set("op_amps", opAmps.get());
	synth->set("op_ratios", opRatios.get());
	synth->set("op_detunes", opDetunes.get());
	synth->set("mod_matrix", modMatrix.get());
	synth->set("eg_levels", allEgLevels);
	synth->set("eg_rates", derivedRates);
	
	int n = numChannels.get();
	vector<float> p = pitch.get();
	vector<int> g = gate.get();
	vector<float> l = levels.get();
	
	// Init vectors
	if(p.size() != n) p.resize(n, p.empty() ? 60.0f : p[0]);
	if(g.size() != n) g.resize(n, 0);
	if(l.size() != n) l.resize(n, l.empty() ? 1.0f : l[0]);
	
	synth->set("pitch", p);
	synth->set("gate", g);
	synth->set("velocity", l);
	
	// New Vector Params
	synth->set("modScale", modScale.get());
	synth->set("vibFreq", vibFreq.get());
	synth->set("vibAmp", vibAmp.get());
	synth->set("tremFreq", tremFreq.get());
	synth->set("tremAmp", tremAmp.get());
	
	if(outputBuses.count(server) && outputBuses[server].count(0)) {
		synth->set("out", outputBuses[server][0]);
	}
    synth->createAndRun(0, 1, getActive()); //addToTail
}

void scFM7::buildSynth(ofxSCServer* server) {
	// Allocate the synth object only — no create() yet.
	// serverManager calls: buildSynth → setOutputBus → createSynth
	if(!server) return;
	if(synthInstances[server]) {
		synthInstances[server]->free();
		delete synthInstances[server];
	}
	synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

int scFM7::getLastSynthID(ofxSCServer* server) {
	if (synthInstances.count(server) && synthInstances[server]) {
		return synthInstances[server]->nodeID;
	}
	return -1;
}

void scFM7::moveSynthBefore(ofxSCServer* server, int nodeID) {
	if(!server) return;
	if(!synthInstances.count(server) || !synthInstances[server]) return;

	ofxSCSynth* synth = synthInstances[server];
	int n = numChannels.get();

	// Resend all params before moving
	updateEnvelopeParams();

	synth->set("amp",        masterAmp.get());
	synth->set("feedback",   feedback.get());
	synth->set("op_amps",    opAmps.get());
	synth->set("op_ratios",  opRatios.get());
	synth->set("op_detunes", opDetunes.get());
	synth->set("mod_matrix", modMatrix.get());
	synth->set("eg_levels",  allEgLevels);
	synth->set("eg_rates",   derivedRates);

	{
		vector<float> p = pitch.get();
		vector<int>   g = gate.get();
		vector<float> l = levels.get();
		if((int)p.size() != n) p.resize(n, p.empty() ? 60.0f : p[0]);
		if((int)g.size() != n) g.resize(n, 0);
		if((int)l.size() != n) l.resize(n, l.empty() ? 1.0f : l[0]);
		synth->set("pitch",    p);
		synth->set("gate",     g);
		synth->set("velocity", l);
	}

	synth->set("modScale", modScale.get());
	synth->set("vibFreq",  vibFreq.get());
	synth->set("vibAmp",   vibAmp.get());
	synth->set("tremFreq", tremFreq.get());
	synth->set("tremAmp",  tremAmp.get());

	if(outputBuses.count(server) && outputBuses[server].count(0))
		synth->set("out", outputBuses[server][0]);

	synth->moveBefore(nodeID);
}

void scFM7::free(ofxSCServer* server) {
	if(!server) return;
	if(synthInstances.count(server) && synthInstances[server]) {
		synthInstances[server]->free();
		delete synthInstances[server];
		synthInstances.erase(server);
	}
	outputBuses.erase(server);
}

void scFM7::activate() {
	for(auto& pair : synthInstances) {
		if(pair.second) pair.second->run(true);
	}
}

void scFM7::deactivate() {
	for(auto& pair : synthInstances) {
		if(pair.second) pair.second->run(false);
	}
}

void scFM7::setOutputBus(ofxSCServer* server, int index, int bus) {
	if(!server) return;
	outputBuses[server][index] = bus;
	if(synthInstances.count(server) && synthInstances[server]) {
		synthInstances[server]->set("out", bus);
	}
}

string scFM7::getSynthDefName() const {
	return "SuperFM7" + ofToString(numChannels.get());
}

// --- DISK I/O FOR PRESETS ---

string scFM7::getPresetsFolderPath() {
	return ofToDataPath("nodeSnapshots/scFM7/", true);
}

string scFM7::getPresetFilePath(int slot) {
	return getPresetsFolderPath() + "preset_" + ofToString(slot) + ".json";
}

void scFM7::savePresetToDisk(int slot) {
	if(slot < 0 || slot >= 16) return;
	if(!presetSlots[slot].hasData) return;

	// Ensure directory exists
	ofDirectory dir(getPresetsFolderPath());
	if(!dir.exists()) dir.create(true);

	FMPatch& p = presetSlots[slot];
	ofJson json;
	json["matrix"] = p.matrix;
	json["egLevels"] = p.egLevels;
	json["egTimes"] = p.egTimes;
	json["opAmps"] = p.opAmps;
	json["opRatios"] = p.opRatios;
	json["opDetunes"] = p.opDetunes;
	json["feedback"] = p.feedback;
	json["duration"] = p.duration;
	json["masterAmp"] = p.masterAmp;
	json["vibFreq"] = p.vibFreq;
	json["vibAmp"] = p.vibAmp;
	json["tremFreq"] = p.tremFreq;
	json["tremAmp"] = p.tremAmp;

	ofFile file(getPresetFilePath(slot), ofFile::WriteOnly);
	file << json.dump(4);
}

void scFM7::loadPresetFromDisk(int slot) {
	if(slot < 0 || slot >= 16) return;

	ofFile file(getPresetFilePath(slot));
	if(!file.exists()) return;

	ofJson json = ofJson::parse(file);
	FMPatch p;
	p.matrix = json.value("matrix", vector<float>(36, 0.0f));
	p.egLevels = json.value("egLevels", vector<float>(24, 1.0f));
	p.egTimes = json.value("egTimes", vector<float>(24, 0.0f));
	p.opAmps = json.value("opAmps", vector<float>(6, 0.0f));
	p.opRatios = json.value("opRatios", vector<float>(6, 1.0f));
	p.opDetunes = json.value("opDetunes", vector<float>(6, 0.0f));
	p.feedback = json.value("feedback", 0.0f);
	p.duration = json.value("duration", 2.0f);
	p.masterAmp = json.value("masterAmp", 0.5f);
	p.vibFreq = json.value("vibFreq", 5.0f);
	p.vibAmp = json.value("vibAmp", 0.0f);
	p.tremFreq = json.value("tremFreq", 5.0f);
	p.tremAmp = json.value("tremAmp", 0.0f);

	// Legacy support
	if(json.count("lfoFreq")) p.vibFreq = json["lfoFreq"];
	if(json.count("lfoDepth")) p.vibAmp = json["lfoDepth"];

	p.hasData = true;
	presetSlots[slot] = p;
}

void scFM7::loadAllPresetsFromDisk() {
	for(int i = 0; i < 16; i++) {
		loadPresetFromDisk(i);
	}
}

void scFM7::deletePresetFromDisk(int slot) {
	if(slot < 0 || slot >= 16) return;

	// Delete from disk
	ofFile file(getPresetFilePath(slot));
	if(file.exists()) {
		file.remove();
	}

	// Clear from memory
	presetSlots[slot] = FMPatch();
	presetSlots[slot].hasData = false;

	// Clear active if it was this slot
	if(activePresetSlot == slot) {
		activePresetSlot = -1;
	}
}

// --- PERSISTENCE ---

void scFM7::presetSave(ofJson &json) {
	json["Matrix"] = modMatrix.get();
	json["EgLevels"] = allEgLevels;
	json["EgTimes"] = allEgTimes;
	json["ActiveSlot"] = activePresetSlot;
	// Presets are now saved to disk, not in the preset file
}

void scFM7::presetRecallAfterSettingParameters(ofJson &json) {
	if(json.count("Matrix")) {
		vector<float> m = json["Matrix"].get<vector<float>>();
		if(m.size() == 36) { modMatrix.set(m); }
	}
	if(json.count("EgLevels")) {
		vector<float> v = json["EgLevels"].get<vector<float>>();
		if(v.size() == 24) allEgLevels = v;
	}
	if(json.count("EgTimes")) {
		vector<float> v = json["EgTimes"].get<vector<float>>();
		if(v.size() == 24) allEgTimes = v;
	}
	if(json.count("ActiveSlot")) {
		activePresetSlot = json["ActiveSlot"].get<int>();
	}

	// Legacy support: load old preset slots from JSON if they exist
	for(int i=0; i<16; i++) {
		string key = "PresetSlot_" + ofToString(i);
		if(json.count(key)) {
			ofJson slotJson = json[key];
			FMPatch p;
			p.matrix = slotJson.value("matrix", vector<float>(36, 0.0f));
			p.egLevels = slotJson.value("egLevels", vector<float>(24, 1.0f));
			p.egTimes = slotJson.value("egTimes", vector<float>(24, 0.0f));
			p.opAmps = slotJson.value("opAmps", vector<float>(6, 0.0f));
			p.opRatios = slotJson.value("opRatios", vector<float>(6, 1.0f));
			p.opDetunes = slotJson.value("opDetunes", vector<float>(6, 0.0f));
			p.feedback = slotJson.value("feedback", 0.0f);
			p.duration = slotJson.value("duration", 2.0f);
			p.masterAmp = slotJson.value("masterAmp", 0.5f);
			p.vibFreq = slotJson.value("vibFreq", 5.0f);
			p.vibAmp = slotJson.value("vibAmp", 0.0f);
			p.tremFreq = slotJson.value("tremFreq", 5.0f);
			p.tremAmp = slotJson.value("tremAmp", 0.0f);

			// Legacy support
			if(slotJson.count("lfoFreq")) p.vibFreq = slotJson["lfoFreq"];
			if(slotJson.count("lfoDepth")) p.vibAmp = slotJson["lfoDepth"];

			p.hasData = true;
			presetSlots[i] = p;
			// Save to new disk location
			savePresetToDisk(i);
		}
	}

	refreshEnvelopeGUI();
	updateAllParamsToSynth();
}

void scFM7::drawModMatrix() {
	float zoom = ofxOceanodeShared::getZoomLevel();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = widgetWidth.get() * zoom;
	float h = matrixHeight.get() * zoom;

	ImGui::InvisibleButton("##MatrixArea", ImVec2(w, h));
	bool isActive = ImGui::IsItemActive();

	float startX = p.x + 40 * zoom;
	float startY = p.y + 20 * zoom;
	float availableW = w - 40 * zoom;
	float availableH = h - 20 * zoom;

	float cellSize = std::min(availableW / 6.0f, availableH / 6.0f);
	int dim = 6;
	
	// GET from parameter
	vector<float> matrix = modMatrix.get();
	if(matrix.size() != 36) matrix.resize(36, 0.0f);
	bool changed = false;
	
	char buf[16];
	for(int i=0; i<dim; i++) {
		sprintf(buf, "%d", i+1);
		float x = startX + i*cellSize + (cellSize*0.5f) - 4 * zoom;
		drawList->AddText(ImVec2(x, p.y), IM_COL32(200,200,200,255), buf);
	}
	
	ImVec2 mouse = ImGui::GetIO().MousePos;
	bool mouseDown = ImGui::IsMouseDown(0);
	bool rightClick = ImGui::IsMouseClicked(1);
	
	if(!mouseDown) activeMatrixCell = -1;

	for(int row=0; row<dim; row++) {
		sprintf(buf, "M%d", row+1);
		float y = startY + row*cellSize + (cellSize*0.5f) - 6 * zoom;
		drawList->AddText(ImVec2(p.x, y), IM_COL32(200,200,200,255), buf);
		
		for(int col=0; col<dim; col++) {
			int idx = row * dim + col;
			float val = matrix[idx];
			
			ImVec2 cellP = ImVec2(startX + col*cellSize, startY + row*cellSize);
			ImVec2 cellMax = ImVec2(cellP.x + cellSize - 2, cellP.y + cellSize - 2);
			
			bool cellHovered = (mouse.x >= cellP.x && mouse.x < cellMax.x &&
								mouse.y >= cellP.y && mouse.y < cellMax.y);
			
			if(isActive && mouseDown) {
				if(activeMatrixCell == -1 && cellHovered) activeMatrixCell = idx;
				if(activeMatrixCell == idx) {
					float delta = ImGui::GetIO().MouseDelta.y * -0.01f;
					if(ImGui::GetIO().KeyShift) delta *= 0.1f;
					float newVal = ofClamp(val + delta, 0.0f, 4.0f);
					if(newVal != val) { matrix[idx] = newVal; changed = true; }
				}
			}
			if(cellHovered && rightClick) { matrix[idx] = 0.0f; changed = true; }
			
			drawList->AddRectFilled(cellP, cellMax, IM_COL32(40, 40, 40, 255));
			float fillH = (cellSize-2) * ofClamp(val, 0.0f, 1.0f);
			ImU32 fillCol = (row==col) ? IM_COL32(220,120,40,200) : IM_COL32(60,160,220,200);
			if(val > 1.0f) fillCol = IM_COL32(255,80,80,220);
			drawList->AddRectFilled(ImVec2(cellP.x, cellMax.y - fillH), cellMax, fillCol);
			drawList->AddRect(cellP, cellMax, IM_COL32(80,80,80,100));
			
			/*
			 if(val > 0.01f) {
				sprintf(buf, "%.1f", val);
				ImVec2 txtSz = ImGui::CalcTextSize(buf);
				ImVec2 txtPos = ImVec2(cellP.x+(cellSize-txtSz.x)*0.5f, cellP.y+(cellSize-txtSz.y)*0.5f);
				drawList->AddText(txtPos, IM_COL32(255,255,255,255), buf);
			}
			 */
		}
	}
	
	if(changed) {
		// SET to parameter
		modMatrix.set(matrix);
	}
}
