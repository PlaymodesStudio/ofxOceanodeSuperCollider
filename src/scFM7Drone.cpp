#include "scFM7Drone.h"

// Static helper: build SC control name like "op_amp_1", "op_ratio_3", etc.
string scFM7Drone::opParamName(const string& prefix, int opIdx) {
	return prefix + "_" + ofToString(opIdx + 1);
}

scFM7Drone::scFM7Drone() : scNode("SuperFMDrone") {
	presetSlots.resize(16);
	activeMatrixCell = -1;
	activePresetSlot = -1;
	isMorphing = false;
}

scFM7Drone::~scFM7Drone() {
	listeners.unsubscribeAll();
	for(auto& pair : synthInstances) {
		if(pair.second) { pair.second->free(); delete pair.second; }
	}
	synthInstances.clear();
}

void scFM7Drone::setup() {
	// 1. Core
	addParameter(numChannels.set("N Chan", 1, 1, 64));

	// --- PRESETS ---
	addSeparator("Snapshots", ofColor(200));
	uiPresets.set("PresetsUI", [this](){ drawPresetSlots(); });
	addCustomRegion(uiPresets, [this](){ drawPresetSlots(); });
	addParameter(morphTime.set("Morph Time", 0.0f, 0.0f, 10.0f));

	addSeparator("Performance", ofColor(200));

	addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f}));

	// Vibrato / Tremolo
	addParameter(vibFreq.set("Vib Freq",  {5.0f}, {0.1f}, {20.0f}));
	addParameter(vibAmp.set("Vib Amp",    {0.0f}, {0.0f}, {2.0f}));
	addParameter(tremFreq.set("Trem Freq",{5.0f}, {0.1f}, {20.0f}));
	addParameter(tremAmp.set("Trem Amp",  {0.0f}, {0.0f}, {1.0f}));

	// Global
	addParameter(masterAmp.set("Amp",     {1.0f}, {0.0f}, {1.0f}));
	addParameter(feedback.set("Feedback",  1.0f,   0.0f,   4.0f));

	// GUI Layout
	addInspectorParameter(widgetWidth.set("Widget Width", 240.0f, 150.0f, 800.0f));
	addInspectorParameter(matrixHeight.set("Matrix Height", 200.0f, 100.0f, 500.0f));

	// --- MODULATION MATRIX ---
	addSeparator("Modulation Matrix", ofColor(200));
	addParameter(modScale.set("Scale", {1.0f}, {0.0f}, {10.0f}));
	addParameter(modMatrix.set("Data", vector<float>(36, 0.0f), vector<float>(36, 0.0f), vector<float>(36, 4.0f)));

	uiMatrix.set("MatrixUI", [this](){ drawModMatrix(); });
	addCustomRegion(uiMatrix, [this](){ drawModMatrix(); });

	// --- OPERATORS ---
	static const std::array<float,6> ampDefaults   = {1.f, 0.f, 0.f, 0.f, 0.f, 0.f};
	static const std::array<float,6> ratioDefaults  = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
	static const std::array<float,6> detuneDefaults = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};

	addSeparator("Op Amps", ofColor(200));
	for(int i = 0; i < 6; i++) {
		addParameter(opAmps[i].set("Op Amp " + ofToString(i+1),
			{ampDefaults[i]}, {0.0f}, {1.0f}));
	}

	addSeparator("Op Ratios", ofColor(200));
	for(int i = 0; i < 6; i++) {
		addParameter(opRatios[i].set("Op Ratio " + ofToString(i+1),
			{ratioDefaults[i]}, {0.0f}, {20.0f}));
	}

	addSeparator("Op Detunes", ofColor(200));
	for(int i = 0; i < 6; i++) {
		addParameter(opDetunes[i].set("Op Detune " + ofToString(i+1),
			{detuneDefaults[i]}, {-5.0f}, {5.0f}));
	}

	addSeparator("Output", ofColor(200));
	scNode::addOutput("Out");

	// --- Load Presets from Disk ---
	loadAllPresetsFromDisk();

	// --- LISTENERS ---

	listeners.push(numChannels.newListener([this](int &n){
		for(auto& output : outputs) output = output;
	}));

	listeners.push(pitch.newListener([this](vector<float> &v){    sendFloatParameter("pitch", v); }));
	listeners.push(modScale.newListener([this](vector<float> &v){ sendFloatParameter("modScale", v); }));
	listeners.push(vibFreq.newListener([this](vector<float> &v){  sendFloatParameter("vibFreq", v); }));
	listeners.push(vibAmp.newListener([this](vector<float> &v){   sendFloatParameter("vibAmp", v); }));
	listeners.push(tremFreq.newListener([this](vector<float> &v){ sendFloatParameter("tremFreq", v); }));
	listeners.push(tremAmp.newListener([this](vector<float> &v){  sendFloatParameter("tremAmp", v); }));
	listeners.push(masterAmp.newListener([this](vector<float> &v){ sendFloatParameter("amp", v); }));

	listeners.push(feedback.newListener([this](float &v){
		if(!isMorphing)
			for(auto& pair : synthInstances) if(pair.second) pair.second->set("feedback", v);
	}));

	listeners.push(modMatrix.newListener([this](vector<float> &v){
		if(!isMorphing) updateMatrixParams();
	}));

	for(int i = 0; i < 6; i++) {
		string ampName    = opParamName("op_amp",    i);
		string ratioName  = opParamName("op_ratio",  i);
		string detuneName = opParamName("op_detune", i);

		listeners.push(opAmps[i].newListener([this, ampName](vector<float> &v) mutable {
			if(!isMorphing) sendFloatParameter(ampName, v);
		}));
		listeners.push(opRatios[i].newListener([this, ratioName](vector<float> &v) mutable {
			if(!isMorphing) sendFloatParameter(ratioName, v);
		}));
		listeners.push(opDetunes[i].newListener([this, detuneName](vector<float> &v) mutable {
			if(!isMorphing) sendFloatParameter(detuneName, v);
		}));
	}
}

void scFM7Drone::update(ofEventArgs& args) {
	if(isMorphing) updateMorph();
}

// --- HELPERS ---

void scFM7Drone::updateMatrixParams() {
	for(auto& pair : synthInstances) {
		if(pair.second) pair.second->set("mod_matrix", modMatrix.get());
	}
}

void scFM7Drone::updateAllParamsToSynth() {
	updateMatrixParams();

	for(auto& pair : synthInstances) {
		if(pair.second) pair.second->set("feedback", feedback.get());
	}

	auto amp = masterAmp.get();
	sendFloatParameter("amp", amp);

	for(int i = 0; i < 6; i++) {
		auto v_amp    = opAmps[i].get();
		auto v_ratio  = opRatios[i].get();
		auto v_detune = opDetunes[i].get();
		string sAmp    = opParamName("op_amp",    i);
		string sRatio  = opParamName("op_ratio",  i);
		string sDetune = opParamName("op_detune", i);
		sendFloatParameter(sAmp,    v_amp);
		sendFloatParameter(sRatio,  v_ratio);
		sendFloatParameter(sDetune, v_detune);
	}
}

void scFM7Drone::sendFloatParameter(const string& name, vector<float>& values) {
	int n = numChannels.get();
	for(auto& pair : synthInstances) {
		if(pair.second == nullptr) continue;
		if(values.size() == 1) pair.second->setMultiple(name, values[0], n);
		else pair.second->set(name, values);
	}
}

// --- PRESET SLOTS GUI ---

void scFM7Drone::drawPresetSlots() {
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = widgetWidth.get();

	float slotSize = w / 8.0f;
	float h = slotSize * 2.0f;

	ImGui::InvisibleButton("##Presets", ImVec2(w, h));
	bool isActive = ImGui::IsItemActive();
	ImVec2 mouse = ImGui::GetIO().MousePos;
	bool mouseDown = ImGui::IsMouseClicked(0);
	bool rightClick = ImGui::IsMouseClicked(1);
	bool shift = ImGui::GetIO().KeyShift;

	for(int i=0; i<16; i++) {
		int row = i / 8;
		int col = i % 8;
		ImVec2 slotP   = ImVec2(p.x + col*slotSize, p.y + row*slotSize);
		ImVec2 slotMax = ImVec2(slotP.x + slotSize - 2, slotP.y + slotSize - 2);

		bool hasData = presetSlots[i].hasData;
		bool hovered = (mouse.x >= slotP.x && mouse.x < slotMax.x &&
						mouse.y >= slotP.y && mouse.y < slotMax.y);

		if(hovered && isActive && mouseDown) {
			if(shift) storeToSlot(i);
			else recallSlot(i);
		}
		if(hovered && rightClick) deletePresetFromDisk(i);

		ImU32 fillCol;
		if(i == activePresetSlot)    fillCol = IM_COL32(180, 255, 180, 255);
		else if(hasData)             fillCol = IM_COL32(100, 180, 100, 255);
		else                         fillCol = IM_COL32(60,  60,  60,  255);

		if(hovered) {
			int r2 = std::min((int)(fillCol & 0xFF)         + 30, 255);
			int g2 = std::min((int)((fillCol >> 8)  & 0xFF) + 30, 255);
			int b2 = std::min((int)((fillCol >> 16) & 0xFF) + 30, 255);
			fillCol = IM_COL32(r2, g2, b2, 255);
		}

		drawList->AddRectFilled(slotP, slotMax, fillCol);
		char buf[8]; sprintf(buf, "%d", i+1);
		drawList->AddText(ImVec2(slotP.x+2, slotP.y+2), IM_COL32(255,255,255,180), buf);
		if(shift && hovered)
			drawList->AddText(ImVec2(slotP.x+slotSize-15, slotP.y+slotSize-15), IM_COL32(255,0,0,255), "S");
		if(!shift && hovered && rightClick)
			drawList->AddText(ImVec2(slotP.x+slotSize-15, slotP.y+slotSize-15), IM_COL32(255,0,0,255), "X");
	}
}

// --- PRESET & MORPHING ---

void scFM7Drone::storeToSlot(int slot) {
	if(slot < 0 || slot >= 16) return;

	FMDronePatch p;
	p.matrix    = modMatrix.get();
	p.feedback  = feedback.get();
	p.masterAmp = masterAmp.get();
	if(p.masterAmp.empty()) p.masterAmp = {1.0f};

	for(int i = 0; i < 6; i++) {
		p.opAmps[i]    = opAmps[i].get();
		p.opRatios[i]  = opRatios[i].get();
		p.opDetunes[i] = opDetunes[i].get();
	}

	if(vibFreq.get().size()  > 0) p.vibFreq  = vibFreq.get()[0];
	if(vibAmp.get().size()   > 0) p.vibAmp   = vibAmp.get()[0];
	if(tremFreq.get().size() > 0) p.tremFreq = tremFreq.get()[0];
	if(tremAmp.get().size()  > 0) p.tremAmp  = tremAmp.get()[0];

	p.hasData = true;
	presetSlots[slot] = p;
	activePresetSlot = slot;
	savePresetToDisk(slot);
}

void scFM7Drone::recallSlot(int slot) {
	if(slot < 0 || slot >= 16) return;
	if(!presetSlots[slot].hasData) return;
	activePresetSlot = slot;

	static const std::array<float,6> ampDef = {1.f,0.f,0.f,0.f,0.f,0.f};

	if(morphTime.get() <= 0.001f) {
		FMDronePatch& p = presetSlots[slot];
		modMatrix.set(p.matrix);
		feedback.set(p.feedback);
		masterAmp.set(p.masterAmp.empty() ? vector<float>{1.0f} : p.masterAmp);

		for(int i = 0; i < 6; i++) {
			opAmps[i].set(p.opAmps[i].empty()    ? vector<float>{ampDef[i]} : p.opAmps[i]);
			opRatios[i].set(p.opRatios[i].empty() ? vector<float>{1.f}      : p.opRatios[i]);
			opDetunes[i].set(p.opDetunes[i].empty()? vector<float>{0.f}     : p.opDetunes[i]);
		}

		vibFreq.set({p.vibFreq});
		vibAmp.set({p.vibAmp});
		tremFreq.set({p.tremFreq});
		tremAmp.set({p.tremAmp});

		updateAllParamsToSynth();
	} else {
		startPatch.matrix   = modMatrix.get();
		startPatch.feedback = feedback.get();
		startPatch.masterAmp = masterAmp.get();
		if(startPatch.masterAmp.empty()) startPatch.masterAmp = {1.0f};

		for(int i = 0; i < 6; i++) {
			startPatch.opAmps[i]    = opAmps[i].get();
			startPatch.opRatios[i]  = opRatios[i].get();
			startPatch.opDetunes[i] = opDetunes[i].get();
		}

		if(vibFreq.get().size()  > 0) startPatch.vibFreq  = vibFreq.get()[0];
		if(vibAmp.get().size()   > 0) startPatch.vibAmp   = vibAmp.get()[0];
		if(tremFreq.get().size() > 0) startPatch.tremFreq = tremFreq.get()[0];
		if(tremAmp.get().size()  > 0) startPatch.tremAmp  = tremAmp.get()[0];

		targetPatch = presetSlots[slot];
		morphStartTime = ofGetElapsedTimef();
		isMorphing = true;
	}
}

void scFM7Drone::updateMorph() {
	float now = ofGetElapsedTimef();
	float progress = (now - morphStartTime) / std::max(morphTime.get(), 0.001f);
	if(progress >= 1.0f) { progress = 1.0f; isMorphing = false; }

	auto lerpVec = [](const vector<float>& a, const vector<float>& b, float t) -> vector<float> {
		size_t sz = std::max(a.size(), b.size());
		vector<float> out(sz);
		for(size_t i = 0; i < sz; i++) {
			float av = i < a.size() ? a[i] : 0.f;
			float bv = i < b.size() ? b[i] : 0.f;
			out[i] = av + (bv - av) * t;
		}
		return out;
	};

	// Matrix
	{
		vector<float> curr = modMatrix.get();
		auto& a = startPatch.matrix; auto& b = targetPatch.matrix;
		if(a.size() == b.size() && a.size() == curr.size())
			for(size_t i = 0; i < curr.size(); i++) curr[i] = a[i] + (b[i]-a[i])*progress;
		modMatrix.setWithoutEventNotifications(curr);
	}

	float currFb   = ofLerp(startPatch.feedback, targetPatch.feedback, progress);
	auto  currAmp  = lerpVec(startPatch.masterAmp, targetPatch.masterAmp, progress);
	vector<float> vf = { ofLerp(startPatch.vibFreq,  targetPatch.vibFreq,  progress) };
	vector<float> va = { ofLerp(startPatch.vibAmp,   targetPatch.vibAmp,   progress) };
	vector<float> tf = { ofLerp(startPatch.tremFreq, targetPatch.tremFreq, progress) };
	vector<float> ta = { ofLerp(startPatch.tremAmp,  targetPatch.tremAmp,  progress) };

	feedback.setWithoutEventNotifications(currFb);
	masterAmp.setWithoutEventNotifications(currAmp);
	vibFreq.setWithoutEventNotifications(vf);
	vibAmp.setWithoutEventNotifications(va);
	tremFreq.setWithoutEventNotifications(tf);
	tremAmp.setWithoutEventNotifications(ta);

	for(int i = 0; i < 6; i++) {
		auto va2 = lerpVec(startPatch.opAmps[i],    targetPatch.opAmps[i],    progress);
		auto vr  = lerpVec(startPatch.opRatios[i],  targetPatch.opRatios[i],  progress);
		auto vd  = lerpVec(startPatch.opDetunes[i], targetPatch.opDetunes[i], progress);
		opAmps[i].setWithoutEventNotifications(va2);
		opRatios[i].setWithoutEventNotifications(vr);
		opDetunes[i].setWithoutEventNotifications(vd);
	}

	updateAllParamsToSynth();

	sendFloatParameter("vibFreq", vf);
	sendFloatParameter("vibAmp",  va);
	sendFloatParameter("tremFreq", tf);
	sendFloatParameter("tremAmp",  ta);
	sendFloatParameter("amp", currAmp);
}

// --- NODE LOGIC ---

// buildSynth: allocate the synth object only (no create() yet).
// serverManager calls: buildSynth → setOutputBus → createSynth
// So the output bus is set on the object before create() is called.
void scFM7Drone::buildSynth(ofxSCServer* server) {
	if(!server) return;
	if(synthInstances[server]) {
		synthInstances[server]->free();
		delete synthInstances[server];
	}
	synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

// createSynth: set all parameters and call create() to instantiate in SC.
// At this point setOutputBus() has already been called, so outputBuses is populated.
void scFM7Drone::createSynth(ofxSCServer* server) {
	if(!server) return;

	// If buildSynth wasn't called (e.g. standalone use), allocate now
	if(!synthInstances[server]) {
		synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
	}

	auto synth = synthInstances[server];
	int n = numChannels.get();

	synth->set("feedback",   feedback.get());
	synth->set("mod_matrix", modMatrix.get());

	// Pitch
	{
		vector<float> p = pitch.get();
		if((int)p.size() != n) p.resize(n, p.empty() ? 60.0f : p[0]);
		synth->set("pitch", p);
	}

	synth->set("modScale", modScale.get());
	synth->set("vibFreq",  vibFreq.get());
	synth->set("vibAmp",   vibAmp.get());
	synth->set("tremFreq", tremFreq.get());
	synth->set("tremAmp",  tremAmp.get());

	// Master Amp
	{
		auto amp = masterAmp.get();
		if(amp.size() == 1) synth->setMultiple("amp", amp[0], n);
		else synth->set("amp", amp);
	}

	// Per-operator params
	for(int i = 0; i < 6; i++) {
		auto sendOp = [&](const string& prefix, vector<float> v) {
			string name = opParamName(prefix, i);
			if(v.size() == 1) synth->setMultiple(name, v[0], n);
			else synth->set(name, v);
		};
		sendOp("op_amp",    opAmps[i].get());
		sendOp("op_ratio",  opRatios[i].get());
		sendOp("op_detune", opDetunes[i].get());
	}

	// Output bus (already set by setOutputBus, but set explicitly to be safe)
	if(outputBuses.count(server) && outputBuses[server].count(0)) {
		synth->set("out", outputBuses[server][0]);
	}

	synth->create();
}

int scFM7Drone::getLastSynthID(ofxSCServer* server) {
	if(synthInstances.count(server) && synthInstances[server]) {
		return synthInstances[server]->nodeID;
	}
	return -1;
}

void scFM7Drone::moveSynthBefore(ofxSCServer* server, int nodeID) {
	if(!server) return;
	if(!synthInstances.count(server) || !synthInstances[server]) return;

	ofxSCSynth* synth = synthInstances[server];
	int n = numChannels.get();

	// Resend all params before moving (mirrors scSynthdef's resendParams pattern)
	synth->set("feedback",   feedback.get());
	synth->set("mod_matrix", modMatrix.get());

	{
		vector<float> p = pitch.get();
		if((int)p.size() != n) p.resize(n, p.empty() ? 60.0f : p[0]);
		synth->set("pitch", p);
	}

	synth->set("modScale", modScale.get());
	synth->set("vibFreq",  vibFreq.get());
	synth->set("vibAmp",   vibAmp.get());
	synth->set("tremFreq", tremFreq.get());
	synth->set("tremAmp",  tremAmp.get());

	{
		auto amp = masterAmp.get();
		if(amp.size() == 1) synth->setMultiple("amp", amp[0], n);
		else synth->set("amp", amp);
	}

	for(int i = 0; i < 6; i++) {
		auto sendOp = [&](const string& prefix, vector<float> v) {
			string name = opParamName(prefix, i);
			if(v.size() == 1) synth->setMultiple(name, v[0], n);
			else synth->set(name, v);
		};
		sendOp("op_amp",    opAmps[i].get());
		sendOp("op_ratio",  opRatios[i].get());
		sendOp("op_detune", opDetunes[i].get());
	}

	if(outputBuses.count(server) && outputBuses[server].count(0))
		synth->set("out", outputBuses[server][0]);

	synth->moveBefore(nodeID);
}

void scFM7Drone::free(ofxSCServer* server) {
	if(!server) return;
	if(synthInstances.count(server) && synthInstances[server]) {
		synthInstances[server]->free();
		delete synthInstances[server];
		synthInstances.erase(server);
	}
	outputBuses.erase(server);
}

void scFM7Drone::activate() {
	for(auto& pair : synthInstances)
		if(pair.second) pair.second->run(true);
}

void scFM7Drone::deactivate() {
	for(auto& pair : synthInstances)
		if(pair.second) pair.second->run(false);
}

void scFM7Drone::setOutputBus(ofxSCServer* server, int index, int bus) {
	if(!server) return;
	outputBuses[server][index] = bus;
	if(synthInstances.count(server) && synthInstances[server])
		synthInstances[server]->set("out", bus);
}

string scFM7Drone::getSynthDefName() const {
	return "SuperFMDrone" + ofToString(numChannels.get());
}

// --- DISK I/O FOR PRESETS ---

string scFM7Drone::getPresetsFolderPath() {
	return ofToDataPath("nodeSnapshots/scFM7Drone/", true);
}

string scFM7Drone::getPresetFilePath(int slot) {
	return getPresetsFolderPath() + "preset_" + ofToString(slot) + ".json";
}

void scFM7Drone::savePresetToDisk(int slot) {
	if(slot < 0 || slot >= 16) return;
	if(!presetSlots[slot].hasData) return;

	ofDirectory dir(getPresetsFolderPath());
	if(!dir.exists()) dir.create(true);

	FMDronePatch& p = presetSlots[slot];
	ofJson json;
	json["matrix"]    = p.matrix;
	json["feedback"]  = p.feedback;
	json["masterAmp"] = p.masterAmp.empty() ? vector<float>{1.0f} : p.masterAmp;
	json["vibFreq"]   = p.vibFreq;
	json["vibAmp"]    = p.vibAmp;
	json["tremFreq"]  = p.tremFreq;
	json["tremAmp"]   = p.tremAmp;

	static const std::array<float,6> ampDef = {1.f,0.f,0.f,0.f,0.f,0.f};
	for(int i = 0; i < 6; i++) {
		json["opAmps"][i]    = p.opAmps[i].empty()    ? vector<float>{ampDef[i]} : p.opAmps[i];
		json["opRatios"][i]  = p.opRatios[i].empty()  ? vector<float>{1.f}       : p.opRatios[i];
		json["opDetunes"][i] = p.opDetunes[i].empty() ? vector<float>{0.f}       : p.opDetunes[i];
	}

	ofFile file(getPresetFilePath(slot), ofFile::WriteOnly);
	file << json.dump(4);
}

void scFM7Drone::loadPresetFromDisk(int slot) {
	if(slot < 0 || slot >= 16) return;

	ofFile file(getPresetFilePath(slot));
	if(!file.exists()) return;

	ofJson json = ofJson::parse(file);
	FMDronePatch p;

	p.matrix    = json.value("matrix",    vector<float>(36, 0.0f));
	p.feedback  = json.value("feedback",  0.0f);
	p.masterAmp = json.value("masterAmp", vector<float>{1.0f});
	if(p.masterAmp.empty()) p.masterAmp = {1.0f};
	p.vibFreq   = json.value("vibFreq",   5.0f);
	p.vibAmp    = json.value("vibAmp",    0.0f);
	p.tremFreq  = json.value("tremFreq",  5.0f);
	p.tremAmp   = json.value("tremAmp",   0.0f);

	static const std::array<float,6> ampDef = {1.f,0.f,0.f,0.f,0.f,0.f};
	for(int i = 0; i < 6; i++) {
		if(json.count("opAmps")    && (int)json["opAmps"].size()    > i)
			p.opAmps[i]    = json["opAmps"][i].get<vector<float>>();
		else
			p.opAmps[i] = {ampDef[i]};

		if(json.count("opRatios")  && (int)json["opRatios"].size()  > i)
			p.opRatios[i]  = json["opRatios"][i].get<vector<float>>();
		else
			p.opRatios[i] = {1.f};

		if(json.count("opDetunes") && (int)json["opDetunes"].size() > i)
			p.opDetunes[i] = json["opDetunes"][i].get<vector<float>>();
		else
			p.opDetunes[i] = {0.f};
	}

	p.hasData = true;
	presetSlots[slot] = p;
}

void scFM7Drone::loadAllPresetsFromDisk() {
	for(int i = 0; i < 16; i++) loadPresetFromDisk(i);
}

void scFM7Drone::deletePresetFromDisk(int slot) {
	if(slot < 0 || slot >= 16) return;

	ofFile file(getPresetFilePath(slot));
	if(file.exists()) file.remove();

	presetSlots[slot] = FMDronePatch();
	presetSlots[slot].hasData = false;
	if(activePresetSlot == slot) activePresetSlot = -1;
}

// --- PERSISTENCE ---

void scFM7Drone::presetSave(ofJson &json) {
	json["Matrix"]     = modMatrix.get();
	json["ActiveSlot"] = activePresetSlot;
}

void scFM7Drone::presetRecallAfterSettingParameters(ofJson &json) {
	if(json.count("Matrix")) {
		vector<float> m = json["Matrix"].get<vector<float>>();
		if(m.size() == 36) modMatrix.set(m);
	}
	if(json.count("ActiveSlot"))
		activePresetSlot = json["ActiveSlot"].get<int>();

	updateAllParamsToSynth();
}

void scFM7Drone::drawModMatrix() {
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = widgetWidth.get();
	float h = matrixHeight.get();

	ImGui::InvisibleButton("##MatrixArea", ImVec2(w, h));
	bool isActive = ImGui::IsItemActive();

	float startX   = p.x + 40;
	float startY   = p.y + 20;
	float cellSize = std::min((w - 40) / 6.0f, (h - 20) / 6.0f);
	int   dim      = 6;

	vector<float> matrix = modMatrix.get();
	if(matrix.size() != 36) matrix.resize(36, 0.0f);
	bool changed = false;

	char buf[16];
	for(int i = 0; i < dim; i++) {
		sprintf(buf, "%d", i+1);
		drawList->AddText(ImVec2(startX + i*cellSize + cellSize*0.5f - 4, p.y),
						  IM_COL32(200,200,200,255), buf);
	}

	ImVec2 mouse    = ImGui::GetIO().MousePos;
	bool mouseDown  = ImGui::IsMouseDown(0);
	bool rightClick = ImGui::IsMouseClicked(1);
	if(!mouseDown) activeMatrixCell = -1;

	for(int row = 0; row < dim; row++) {
		sprintf(buf, "M%d", row+1);
		drawList->AddText(ImVec2(p.x, startY + row*cellSize + cellSize*0.5f - 6),
						  IM_COL32(200,200,200,255), buf);

		for(int col = 0; col < dim; col++) {
			int   idx = row * dim + col;
			float val = matrix[idx];

			ImVec2 cellP   = ImVec2(startX + col*cellSize,        startY + row*cellSize);
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
		}
	}

	if(changed) modMatrix.set(matrix);
}
