//
//  scBeatRepeatPro.cpp
//  ofxOceanodeSuperCollider
//

#include "scBeatRepeatPro.h"
#include "ofxOceanodeSuperColliderConfig.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
float brClamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float brWrap01(float value) {
    value = std::fmod(value, 1.0f);
    return value < 0.0f ? value + 1.0f : value;
}

float brEditorControlWidth(const char* label) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
    return std::max(72.0f, available - labelWidth
        - ImGui::GetStyle().ItemInnerSpacing.x - 3.0f);
}

struct BrTimeChoice {
    float division;
    int base;
    const char* suffix;
};

const std::array<BrTimeChoice, 21>& brTimeChoices() {
    // Sorted longest to shortest. division is the effective denominator, so
    // duration in quarter-note beats is simply 4 / division.
    static const std::array<BrTimeChoice, 21> choices {{
        {1.0f / 1.5f, 1, "dotted"}, {1.0f, 1, "straight"},
        {2.0f / 1.5f, 2, "dotted"}, {1.5f, 1, "triplet"}, {2.0f, 2, "straight"},
        {4.0f / 1.5f, 4, "dotted"}, {3.0f, 2, "triplet"}, {4.0f, 4, "straight"},
        {8.0f / 1.5f, 8, "dotted"}, {6.0f, 4, "triplet"}, {8.0f, 8, "straight"},
        {16.0f / 1.5f, 16, "dotted"}, {12.0f, 8, "triplet"}, {16.0f, 16, "straight"},
        {32.0f / 1.5f, 32, "dotted"}, {24.0f, 16, "triplet"}, {32.0f, 32, "straight"},
        {64.0f / 1.5f, 64, "dotted"}, {48.0f, 32, "triplet"}, {64.0f, 64, "straight"},
        {96.0f, 64, "triplet"}
    }};
    return choices;
}

int brNearestTimeChoice(float division) {
    int best = 0;
    float distance = std::numeric_limits<float>::max();
    const auto& choices = brTimeChoices();
    for(int i = 0; i < (int)choices.size(); ++i) {
        const float next = std::abs(std::log(std::max(0.0001f, division) / choices[i].division));
        if(next < distance) { distance = next; best = i; }
    }
    return best;
}
}

scBeatRepeatPro::scBeatRepeatPro(std::vector<serverManager*> servers)
: scNode("BeatRepeat Pro")
, allServers(std::move(servers))
, waveHigh(WAVEFORM_BINS, 0.0f)
, waveLow(WAVEFORM_BINS, 0.0f)
{}

scBeatRepeatPro::~scBeatRepeatPro() {
    nodeListeners.unsubscribeAll();
    feedbackListeners.clear();
    for(auto& entry : synths) {
        if(entry.second) {
            entry.second->free();
            delete entry.second;
        }
    }
    synths.clear();
}

void scBeatRepeatPro::setup() {
    description = "Coherent multichannel beat repeater with a live waveform, loop region and read/write heads.";

    addSeparator("BeatRepeat Pro");
    addParameter(showWindow.set("Show", false));
    addParameter(numChannels.set("N Chan", 2, 1, MAX_CHANNELS));
    addParameter(go.set("Go", false));
    addParameter(repeatDivision.set("Repeat", 16.0f, 1.0f / 1.5f, 96.0f));
    addParameter(mix.set("Mix", 1.0f, 0.0f, 1.0f));
    addParameter(level.set("Level", 1.0f, 0.0f, 2.0f));
    addParameter(reset.set("Reset", 0, 0, 1));

    adaptLength.set("Adapt Length", false);
    pitchMode.set("Pitch Mode", false);
    repeatPitch.set("Repeat MIDI", 60.0f, 0.0f, 127.0f);
    offsetMs.set("Offset ms", 0.0f, 0.0f, 1000.0f);
    gate.set("Gate", 1.0f, 0.0f, 1.0f);
    declickMs.set("Declick ms", 5.0f, 0.1f, 100.0f);
    rate.set("Rate", 1.0f, 0.125f, 4.0f);
    reverse.set("Reverse", false);
    pitchStep.set("Pitch Step", 0.0f, -12.0f, 12.0f);
    numPitchSteps.set("Pitch Steps", 12, 0, 24);
    loopAmp.set("Loop Amp", 1.0f, 0.0f, 2.0f);
    inputGain.set("Input Gain", 1.0f, 0.0f, 2.0f);
    filterType.set("Filter", 0, 0, 2);
    cutoffPitch.set("Cutoff Pitch", 135.076f, 15.487f, 135.076f);
    cutoffStep.set("Cutoff Step", 0.0f, -12.0f, 12.0f);
    numCutoffSteps.set("Cutoff Steps", 0, 0, 24);
    resonance.set("Resonance", 0.707f, 0.1f, 4.0f);
    limiter.set("Limiter", true);
    ceiling.set("Ceiling", 0.98f, 0.1f, 1.0f);
    autoReset.set("Auto Reset", false);
    autoResetMode.set("Reset Timing", 0, 0, 1);
    autoResetLoops.set("Reset Loops", 4.0f, 1.0f, 64.0f);
    autoResetBeats.set("Reset Beats", 4.0f, 0.125f, 64.0f);
    snapshotActivates.set("Recall Activates", false);
    momentary.set("Momentary", false);

    scNode::addInput("In");
    scNode::addOutput("Out");
    requireSynthdefs();

    oldNumChannels = numChannels.get();
    initializePublishActions();

    nodeListeners.push(numChannels.newListener([this](int& channels) {
        if(channels < 1 || channels > MAX_CHANNELS || channels == oldNumChannels) return;
        oldNumChannels = channels;
        recreateSynths();
    }));

    auto resendFloat = [this](ofParameter<float>& parameter) {
        nodeListeners.push(parameter.newListener([this](float&) { sendAllParams(); }));
    };
    auto resendBool = [this](ofParameter<bool>& parameter) {
        nodeListeners.push(parameter.newListener([this](bool&) { sendAllParams(); }));
    };
    auto resendInt = [this](ofParameter<int>& parameter) {
        nodeListeners.push(parameter.newListener([this](int&) { sendAllParams(); }));
    };

    resendBool(go);
    for(auto* parameter : {&repeatDivision, &mix, &level, &repeatPitch, &offsetMs, &gate, &declickMs,
                           &rate, &pitchStep, &loopAmp, &inputGain, &cutoffPitch, &cutoffStep,
                           &resonance, &ceiling, &autoResetLoops, &autoResetBeats}) resendFloat(*parameter);
    for(auto* parameter : {&adaptLength, &pitchMode, &reverse, &limiter, &autoReset}) resendBool(*parameter);
    for(auto* parameter : {&numPitchSteps, &filterType, &numCutoffSteps, &autoResetMode}) resendInt(*parameter);

    nodeListeners.push(reset.newListener([this](int& value) {
        if(value <= 0) return;
        ++resetCounter;
        for(auto& entry : synths)
            if(entry.second) entry.second->set(
                "reset", std::vector<float>(numChannels.get(), (float)resetCounter));
        resetParameterPending = true;
    }));

    // The shared bank is independent of an Oceanode project or preset. Older
    // presets are still imported later when no shared bank exists yet.
    sharedSnapshotsLoaded = loadSnapshotBank(sharedSnapshotBankPath(), false, false);
}

void scBeatRepeatPro::initializePublishActions() {
    publishActions.clear();
    auto add = [this](auto& parameter) {
        using T = std::decay_t<decltype(parameter.get())>;
        auto* parameterPtr = &parameter;
        const std::string key = parameter.getEscapedName();
        PublishAction action;
        action.key = key;
        // Capture a stable pointer to the actual ofParameter. Capturing the
        // generic lambda's reference argument risks retaining its short-lived
        // reference variable instead of an unambiguous parameter address.
        action.publish = [this, key, parameterPtr]() { return publishParameter<T>(key, *parameterPtr); };
        action.save = [key, parameterPtr](ofJson& json) { json[key] = parameterPtr->toString(); };
        action.load = [parameterPtr](const ofJson& value) {
            try {
                if(value.is_string()) parameterPtr->fromString(value.get<std::string>());
                else if(value.is_boolean()) parameterPtr->set(static_cast<T>(value.get<bool>()));
                else if(value.is_number()) parameterPtr->set(static_cast<T>(value.get<double>()));
            } catch(...) {}
        };
        action.valueText = [parameterPtr]() { return parameterPtr->toString(); };
        action.applyText = [parameterPtr](const std::string& text) {
            try { parameterPtr->fromString(text); } catch(...) {}
        };
        publishActions.push_back(std::move(action));
    };

    add(adaptLength); add(pitchMode); add(repeatPitch); add(offsetMs); add(gate); add(declickMs); add(rate);
    add(reverse); add(pitchStep); add(numPitchSteps); add(loopAmp); add(inputGain);
    add(filterType); add(cutoffPitch); add(cutoffStep); add(numCutoffSteps);
    add(resonance); add(limiter); add(ceiling);
    add(autoReset); add(autoResetMode); add(autoResetLoops); add(autoResetBeats);
    add(snapshotActivates); add(momentary);
}

const scBeatRepeatPro::PublishAction* scBeatRepeatPro::findPublishAction(const std::string& key) const {
    for(const auto& action : publishActions) if(action.key == key) return &action;
    return nullptr;
}

bool scBeatRepeatPro::isPublished(const std::string& key) const {
    return publishedHandles.count(key) != 0;
}

bool scBeatRepeatPro::hasPublishedConnection(const std::string& key) const {
    auto it = publishedHandles.find(key);
    return it != publishedHandles.end() && it->second
        && (it->second->hasInConnection() || it->second->hasOutConnections());
}

bool scBeatRepeatPro::publishByKey(const std::string& key) {
    const PublishAction* action = findPublishAction(key);
    return action && action->publish ? action->publish() : false;
}

bool scBeatRepeatPro::unpublishByKey(const std::string& key) {
    auto it = publishedHandles.find(key);
    if(it == publishedHandles.end() || hasPublishedConnection(key)) return false;
    removeParameter(it->second->getEscapedName());
    publishedHandles.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(publishedHandles.empty() && publishedSeparatorAdded) {
        removeSeparator("Published");
        publishedSeparatorAdded = false;
    }
    parameterGroupChanged.notify(this);
    return true;
}

void scBeatRepeatPro::syncPublished(const std::vector<std::string>& keys) {
    std::vector<std::string> wanted;
    for(const auto& key : keys)
        if(findPublishAction(key) && std::find(wanted.begin(), wanted.end(), key) == wanted.end())
            wanted.push_back(key);

    const auto current = publishedKeys;
    for(const auto& key : current)
        if(std::find(wanted.begin(), wanted.end(), key) == wanted.end() && !hasPublishedConnection(key))
            unpublishByKey(key);
    for(const auto& key : wanted) if(!isPublished(key)) publishByKey(key);
}

void scBeatRepeatPro::markPublishedItem(const std::string& key) const {
    if(!isPublished(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 2.0f), ImVec2(b.x, b.y - 2.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

void scBeatRepeatPro::drawPublishMenu(const std::string& key) {
    markPublishedItem(key);
    if(!ImGui::BeginPopupContextItem(("##BeatRepeatPublish_" + key).c_str())) return;
    const PublishAction* action = findPublishAction(key);
    if(action && action->valueText && action->applyText) {
        static std::map<std::string, std::array<char, 256>> buffers;
        auto& buffer = buffers[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", action->valueText().c_str());
        ImGui::TextDisabled("Value");
        ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
        if(appearing) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##BeatRepeatTypedValue", buffer.data(), buffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
            action->applyText(std::string(buffer.data()));
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(isPublished(key)) {
        ImGui::TextDisabled("Published in node");
        if(hasPublishedConnection(key)) {
            ImGui::TextDisabled("Disconnect before unpublishing");
        } else if(ImGui::MenuItem("Unpublish from Node")) {
            unpublishByKey(key);
        }
    } else if(ImGui::MenuItem("Publish to Node GUI")) {
        publishByKey(key);
    }
    ImGui::EndPopup();
}

void scBeatRepeatPro::drawValuePopup(const std::string& popupKey,
                                     const std::function<std::string()>& valueText,
                                     const std::function<void(const std::string&)>& applyText,
                                     bool) {
    if(!ImGui::BeginPopupContextItem(("##BeatRepeatValue_" + popupKey).c_str())) return;
    static std::map<std::string, std::array<char, 256>> buffers;
    auto& buffer = buffers[popupKey];
    const bool appearing = ImGui::IsWindowAppearing();
    if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", valueText().c_str());
    ImGui::TextDisabled("Value");
    ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
    if(appearing) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##BeatRepeatTypedValue", buffer.data(), buffer.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
        applyText(std::string(buffer.data()));
        if(enter) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

std::string scBeatRepeatPro::synthDefName() const {
    return "BeatRepeatPro" + ofToString(numChannels.get());
}

void scBeatRepeatPro::requireSynthdefs() {
    std::filesystem::path folder;
    const std::string expected = "BeatRepeatPro1.scsyndef";
    try {
        const std::filesystem::path dataRoot = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(dataRoot)) {
            for(const auto& entry : std::filesystem::recursive_directory_iterator(
                    dataRoot, std::filesystem::directory_options::skip_permission_denied)) {
                if(entry.is_regular_file() && entry.path().filename() == expected) {
                    folder = entry.path().parent_path();
                    break;
                }
            }
        }

        // Development fallback: the addon keeps verified binaries beside the
        // source as well as packaging them into bin/data through addon_config.
        if(folder.empty()) {
            const std::filesystem::path bundled = std::filesystem::path(__FILE__).parent_path()
                .parent_path().parent_path() / "synthdefs/Defaults/BeatRepeatPro/CompiledSynthdefs/BeatRepeatPro";
            if(std::filesystem::exists(bundled / expected)) folder = bundled;
        }
    } catch(const std::exception& error) {
        ofLogWarning("scBeatRepeatPro") << "SynthDef search failed: " << error.what();
    }

    if(folder.empty()) {
        ofLogError("scBeatRepeatPro") << "BeatRepeatPro SynthDefs were not found";
        return;
    }
    for(auto* manager : allServers) if(manager) manager->requireSynthdefFolder(folder.string());
}

void scBeatRepeatPro::sendAllParams(ofxSCSynth* synth) {
    if(!synth) return;
    const int channels = numChannels.get();
    auto set = [synth, channels](const char* name, float value) {
        synth->set(name, std::vector<float>(channels, value));
    };
    set("go", go.get() ? 1.0f : 0.0f);
    set("division", repeatDivision.get());
    set("bpm", currentBpm);
    set("adaptlength", adaptLength.get() ? 1.0f : 0.0f);
    set("pitchmode", pitchMode.get() ? 1.0f : 0.0f);
    set("repeatpitch", repeatPitch.get());
    set("offset", offsetMs.get());
    set("gate", gate.get());
    set("declick", declickMs.get() * 0.001f);
    set("rate", rate.get());
    set("reverse", reverse.get() ? 1.0f : 0.0f);
    set("pitchstep", pitchStep.get());
    set("numpitchsteps", (float)numPitchSteps.get());
    set("grainamp", loopAmp.get());
    set("reset", (float)resetCounter);
    set("mix", mix.get());
    set("levels", level.get());
    set("inputgain", inputGain.get());
    set("filtertype", (float)filterType.get());
    set("cutoffpitch", cutoffPitch.get());
    set("cutoffstep", cutoffStep.get());
    set("numcutoffsteps", (float)numCutoffSteps.get());
    set("rq", std::max(0.05f, std::min(1.0f, 1.0f / std::max(0.1f, resonance.get()))));
    set("limiter", limiter.get() ? 1.0f : 0.0f);
    set("ceiling", ceiling.get());
    set("autoreset", autoReset.get() ? 1.0f : 0.0f);
    set("autoresetmode", (float)autoResetMode.get());
    set("autoloops", autoResetLoops.get());
    set("autobeats", autoResetBeats.get());
    synth->set("vis", visOn ? 1.0f : 0.0f);
}

void scBeatRepeatPro::sendAllParams() {
    for(auto& entry : synths) sendAllParams(entry.second);
}

void scBeatRepeatPro::configureSynth(ofxSCServer* server, ofxSCSynth* synth) {
    if(!server || !synth) return;
    sendAllParams(synth);
    synth->set("in", inputBuses.count(server) ? inputBuses[server] : 0);
    synth->set("out", outputBuses.count(server) ? outputBuses[server] : 0);
    attachFeedback(server, synth);
}

void scBeatRepeatPro::attachFeedback(ofxSCServer* server, ofxSCSynth* synth) {
    feedbackListeners.erase(server);
    feedbackListeners[server] = synth->newFeedbackMessage.newListener(
        [this, server](ofxOscMessage& message) {
            if(message.getAddress() != "/beatRepeatProVis" || message.getNumArgs() < 11) return;
            auto it = synths.find(server);
            if(it == synths.end() || !it->second || message.getArgAsInt(0) != it->second->nodeID) return;

            const float hi = std::max(-2.0f, std::min(2.0f, message.getArgAsFloat(2)));
            const float lo = std::max(-2.0f, std::min(2.0f, message.getArgAsFloat(3)));
            const float newWrite = brWrap01(message.getArgAsFloat(4));
            const bool newCaptureActive = message.getArgAsFloat(9) > 0.5f;
            int from = (int)(writeHead * WAVEFORM_BINS);
            int to = (int)(newWrite * WAVEFORM_BINS);
            int count = (to - from + WAVEFORM_BINS) % WAVEFORM_BINS;
            if(count > 0) {
                if(count > WAVEFORM_BINS / 3) count = 1;
                for(int index = 1; index <= std::max(1, count); ++index) {
                    const int bin = (from + index) % WAVEFORM_BINS;
                    waveHigh[bin] = hi;
                    waveLow[bin] = lo;
                }
            }
            writeHead = newWrite;
            readHead = brWrap01(message.getArgAsFloat(5));
            loopStart = brWrap01(message.getArgAsFloat(6));
            loopEnd = brWrap01(message.getArgAsFloat(7));
            wetActive = message.getArgAsFloat(8) > 0.5f;
            captureActive = newCaptureActive;
            cyclePhase = brClamp01(message.getArgAsFloat(10));
        });
}

void scBeatRepeatPro::buildSynth(ofxSCServer* server) {
    if(!server) return;
    if(synths.count(server) && synths[server]) {
        feedbackListeners.erase(server);
        synths[server]->free();
        delete synths[server];
    }
    synths[server] = new ofxSCSynth(synthDefName(), server);
    configureSynth(server, synths[server]);
}

void scBeatRepeatPro::createSynth(ofxSCServer* server) {
    if(!server) return;
    if(!synths.count(server) || !synths[server]) buildSynth(server);
    if(synths[server]) {
        synths[server]->createAndRun(0, 1, getActive());
        // configureSynth() is also called while constructing the wrapper, but
        // those /n_set messages can precede /s_new on the server. Re-send once
        // the node exists so visualization (and every saved parameter) is live
        // immediately, without waiting for the first Go interaction.
        configureSynth(server, synths[server]);
    }
}

void scBeatRepeatPro::free(ofxSCServer* server) {
    feedbackListeners.erase(server);
    auto it = synths.find(server);
    if(it != synths.end()) {
        if(it->second) { it->second->free(); delete it->second; }
        synths.erase(it);
    }
    inputBuses.erase(server);
    outputBuses.erase(server);
}

void scBeatRepeatPro::recreateSynths() {
    for(auto& entry : synths) {
        ofxSCServer* server = entry.first;
        ofxSCSynth* oldSynth = entry.second;
        if(!server || !oldSynth) continue;
        const int oldNodeID = oldSynth->nodeID;
        feedbackListeners.erase(server);
        delete oldSynth; // action 4 replaces the server node atomically
        entry.second = new ofxSCSynth(synthDefName(), server);
        configureSynth(server, entry.second);
        entry.second->createAndRun(4, oldNodeID, getActive());
        configureSynth(server, entry.second);
    }
}

void scBeatRepeatPro::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = synths.find(server);
    if(it == synths.end() || !it->second) return;
    configureSynth(server, it->second);
    it->second->moveBefore(nodeID);
}

void scBeatRepeatPro::setOutputBus(ofxSCServer* server, int, int bus) {
    outputBuses[server] = bus;
    if(synths.count(server) && synths[server]) synths[server]->set("out", bus);
}

void scBeatRepeatPro::setInputBus(ofxSCServer* server, scNode*, int bus) {
    inputBuses[server] = bus;
    if(synths.count(server) && synths[server]) synths[server]->set("in", bus);
}

void scBeatRepeatPro::resetInputBusses(ofxSCServer* server, int targetBus) {
    setInputBus(server, nullptr, targetBus);
}

int scBeatRepeatPro::getOutputBusIndex(ofxSCServer* server, int) {
    return outputBuses.count(server) ? outputBuses[server] : -1;
}

int scBeatRepeatPro::getLastSynthID(ofxSCServer* server) {
    return synths.count(server) && synths[server] ? synths[server]->nodeID : -1;
}

void scBeatRepeatPro::activate() {
    for(auto& entry : synths) if(entry.second) entry.second->run(true);
}

void scBeatRepeatPro::deactivate() {
    for(auto& entry : synths) if(entry.second) entry.second->run(false);
}

void scBeatRepeatPro::resendParametersForNRT() {
    sendAllParams();
}

void scBeatRepeatPro::setBpm(float bpm) {
    currentBpm = std::max(1.0f, bpm);
    sendAllParams();
}

void scBeatRepeatPro::setVisualizationEnabled(bool enabled) {
    visOn = enabled;
    for(auto& entry : synths) if(entry.second) entry.second->set("vis", enabled ? 1.0f : 0.0f);
}

void scBeatRepeatPro::update(ofEventArgs&) {
    if(resetParameterPending) {
        resetParameterPending = false;
        reset.setWithoutEventNotifications(0);
    }
    // A release can happen after the editor is closed or undocked, in which
    // case ImGui no longer draws the originating button to report deactivation.
    if(momentaryHoldActive && !ofGetMousePressed(OF_MOUSE_BUTTON_LEFT))
        endMomentaryHold(momentarySource);
    // drawWindow refreshes this small lease directly. This is independent of
    // whether the host dispatches its custom GUI draw before or after update,
    // which is especially important while a preset is constructing the node.
    const bool shouldVisualize = visualizationHoldFrames > 0;
    if(visualizationHoldFrames > 0) --visualizationHoldFrames;
    if(shouldVisualize != visOn) setVisualizationEnabled(shouldVisualize);

    // Native dialogs must be opened outside the ImGui draw callback.
    if(snapshotSaveDialog) {
        snapshotSaveDialog = false;
        const std::filesystem::path folder = std::filesystem::path(sharedSnapshotBankPath()).parent_path();
        try { std::filesystem::create_directories(folder); } catch(...) {}
        ofFileDialogResult result = ofSystemSaveDialog(
            "BeatRepeatProBank.json", "Save BeatRepeat Pro snapshot bank");
        if(result.bSuccess) saveSnapshotBank(result.filePath);
    }
    if(snapshotLoadDialog) {
        snapshotLoadDialog = false;
        const std::filesystem::path folder = std::filesystem::path(sharedSnapshotBankPath()).parent_path();
        try { std::filesystem::create_directories(folder); } catch(...) {}
        ofFileDialogResult result = ofSystemLoadDialog(
            "Load BeatRepeat Pro snapshot bank (or preset)", false, folder.string());
        if(result.bSuccess) loadSnapshotBank(result.filePath, true, true);
    }
}

bool scBeatRepeatPro::sliderFloat(const char* label, ofParameter<float>& parameter, const char* format) {
    float value = parameter.get();
    ImGui::SetNextItemWidth(brEditorControlWidth(label));
    const bool changed = ImGui::SliderFloat(label, &value, parameter.getMin(), parameter.getMax(), format);
    if(changed) parameter.set(value);
    drawPublishMenu(parameter.getEscapedName());
    return changed;
}

bool scBeatRepeatPro::sliderInt(const char* label, ofParameter<int>& parameter) {
    int value = parameter.get();
    ImGui::SetNextItemWidth(brEditorControlWidth(label));
    const bool changed = ImGui::SliderInt(label, &value, parameter.getMin(), parameter.getMax());
    if(changed) parameter.set(value);
    drawPublishMenu(parameter.getEscapedName());
    return changed;
}

bool scBeatRepeatPro::checkbox(const char* label, ofParameter<bool>& parameter) {
    bool value = parameter.get();
    const bool changed = ImGui::Checkbox(label, &value);
    if(changed) parameter.set(value);
    drawPublishMenu(parameter.getEscapedName());
    return changed;
}

std::string scBeatRepeatPro::musicalTimeLabel(float division) const {
    const auto& choice = brTimeChoices()[brNearestTimeChoice(division)];
    return "1/" + ofToString(choice.base) + " " + choice.suffix;
}

bool scBeatRepeatPro::musicalTimeSlider(const char* label) {
    int choiceIndex = brNearestTimeChoice(repeatDivision.get());
    const std::string display = musicalTimeLabel(repeatDivision.get());
    ImGui::SetNextItemWidth(brEditorControlWidth(label));
    const bool changed = ImGui::SliderInt(label, &choiceIndex, 0,
        (int)brTimeChoices().size() - 1, display.c_str());
    if(changed) repeatDivision.set(brTimeChoices()[choiceIndex].division);
    drawValuePopup("Repeat", [this]() { return ofToString(repeatDivision.get(), 6); },
        [this](const std::string& text) {
            try { repeatDivision.set(ofClamp(std::stof(text), repeatDivision.getMin(), repeatDivision.getMax())); }
            catch(...) {}
        }, false);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Denominator: 16 = sixteenth, 8 = eighth.\nSlider choices are straight, dotted and triplet.\nRight-click to enter an exact denominator.");
    return changed;
}

void scBeatRepeatPro::drawSectionHeader(const char* title) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeightWithSpacing() + 7.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(start, ImVec2(start.x + width, start.y + height),
        IM_COL32(58, 70, 88, 235), 4.0f);
    ImGui::SetCursorScreenPos(ImVec2(start.x + 8.0f, start.y + 3.0f));
    ImGui::TextColored(ImVec4(0.82f, 0.91f, 1.0f, 1.0f), "%s", title);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height + 3.0f));
}

ofJson scBeatRepeatPro::captureSnapshot() const {
    ofJson snapshot;
    snapshot["repeat"] = repeatDivision.get();
    // Stable, explicit fields keep the mutually exclusive repeat modes
    // portable across snapshot/preset versions.
    snapshot["pitchMode"] = pitchMode.get();
    snapshot["repeatPitch"] = repeatPitch.get();
    snapshot["mix"] = mix.get();
    snapshot["level"] = level.get();
    for(const auto& action : publishActions)
        if(action.key != snapshotActivates.getEscapedName()
           && action.key != momentary.getEscapedName()
           && action.key != pitchMode.getEscapedName()
           && action.key != repeatPitch.getEscapedName()
           && action.save) action.save(snapshot);
    return snapshot;
}

void scBeatRepeatPro::applySnapshot(const ofJson& snapshot) {
    try {
        if(snapshot.contains("repeat")) repeatDivision.set(snapshot["repeat"].get<float>());
        if(snapshot.contains("pitchMode")) pitchMode.set(snapshot["pitchMode"].get<bool>());
        if(snapshot.contains("repeatPitch")) repeatPitch.set(snapshot["repeatPitch"].get<float>());
        if(snapshot.contains("mix")) mix.set(snapshot["mix"].get<float>());
        if(snapshot.contains("level")) level.set(snapshot["level"].get<float>());
    } catch(...) {}
    for(const auto& action : publishActions)
        if(snapshot.contains(action.key) && action.load) action.load(snapshot[action.key]);
    sendAllParams();
}

void scBeatRepeatPro::storeSnapshot(int slot) {
    if(slot < 0 || slot >= (int)snapshotSlots.size()) return;
    snapshotSlots[slot] = captureSnapshot();
    snapshotUsed[slot] = true;
    activeSnapshot = slot;
    saveSharedSnapshotBank();
}

void scBeatRepeatPro::recallSnapshot(int slot, bool forceActivate, bool suppressActivation) {
    if(slot < 0 || slot >= (int)snapshotSlots.size() || !snapshotUsed[slot]) return;
    const bool wasRunning = go.get();
    applySnapshot(snapshotSlots[slot]);
    activeSnapshot = slot;
    if(!suppressActivation && (forceActivate || snapshotActivates.get())) {
        if(!wasRunning) go.set(true);
        else reset.set(1);
    }
}

void scBeatRepeatPro::beginMomentaryHold(int source) {
    momentaryHoldActive = true;
    momentarySource = source;
    if(go.get()) reset.set(1);
    else go.set(true);
}

void scBeatRepeatPro::endMomentaryHold(int source) {
    if(!momentaryHoldActive || source != momentarySource) return;
    momentaryHoldActive = false;
    momentarySource = -1;
    if(go.get()) go.set(false);
}

std::string scBeatRepeatPro::sharedSnapshotBankPath() const {
    return ofToDataPath("nodeSnapshots/BeatRepeatPro/shared.json", true);
}

bool scBeatRepeatPro::saveSnapshotBank(const std::string& path, bool report) {
    std::string outputPath = path;
    if(std::filesystem::path(outputPath).extension() != ".json") outputPath += ".json";
    try {
        const auto parent = std::filesystem::path(outputPath).parent_path();
        if(!parent.empty()) std::filesystem::create_directories(parent);
    } catch(...) {}

    ofJson bank;
    bank["beatRepeatProSnapshotBank"] = 2;
    bank["active"] = activeSnapshot;
    bank["slots"] = ofJson::array();
    bank["names"] = ofJson::array();
    for(int slot = 0; slot < (int)snapshotSlots.size(); ++slot) {
        bank["slots"].push_back(snapshotUsed[slot] ? snapshotSlots[slot] : ofJson(nullptr));
        bank["names"].push_back(snapshotNames[slot]);
    }
    const bool saved = ofSavePrettyJson(outputPath, bank);
    if(report) snapshotBankMessage = saved
        ? "Saved " + std::filesystem::path(outputPath).filename().string()
        : "Could not save bank";
    return saved;
}

bool scBeatRepeatPro::loadSnapshotBank(const std::string& path, bool installAsShared, bool report) {
    ofJson bank;
    try { bank = ofLoadJson(path); } catch(...) {}
    const ofJson* slots = nullptr;
    if(bank.contains("slots") && bank["slots"].is_array()) slots = &bank["slots"];
    else if(bank.contains("beatRepeatSnapshots") && bank["beatRepeatSnapshots"].is_array())
        slots = &bank["beatRepeatSnapshots"];
    if(!slots) {
        if(report) snapshotBankMessage = "No BeatRepeat snapshots in "
            + std::filesystem::path(path).filename().string();
        return false;
    }

    snapshotUsed.fill(false);
    snapshotNames.fill("");
    const int count = std::min((int)snapshotSlots.size(), (int)slots->size());
    for(int slot = 0; slot < count; ++slot) {
        const ofJson& item = (*slots)[slot];
        // Version 2 stores names separately. Also accept a wrapped state/name
        // item so banks remain easy to author or transform externally.
        if(item.is_object() && item.contains("state") && item["state"].is_object()) {
            snapshotSlots[slot] = item["state"];
            snapshotUsed[slot] = true;
            snapshotNames[slot] = item.value("name", "");
        } else if(item.is_object()) {
            snapshotSlots[slot] = item;
            snapshotUsed[slot] = true;
        }
    }
    const char* namesKey = bank.contains("names") ? "names" : "beatRepeatSnapshotNames";
    if(bank.contains(namesKey) && bank[namesKey].is_array()) {
        const auto& names = bank[namesKey];
        for(int slot = 0; slot < std::min((int)snapshotNames.size(), (int)names.size()); ++slot)
            if(names[slot].is_string()) snapshotNames[slot] = names[slot].get<std::string>();
    }
    const int recalledActive = bank.value("active", bank.value("activeSnapshot", -1));
    activeSnapshot = recalledActive >= 0 && recalledActive < (int)snapshotSlots.size()
        && snapshotUsed[recalledActive] ? recalledActive : -1;

    if(installAsShared) {
        sharedSnapshotsLoaded = true;
        saveSharedSnapshotBank();
    }
    if(report) snapshotBankMessage = "Loaded " + std::filesystem::path(path).filename().string();
    return true;
}

void scBeatRepeatPro::saveSharedSnapshotBank() {
    if(saveSnapshotBank(sharedSnapshotBankPath(), false)) sharedSnapshotsLoaded = true;
}

void scBeatRepeatPro::drawSnapshotMatrix() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    constexpr int columns = 4;
    const float buttonWidth = std::max(28.0f,
        (ImGui::GetContentRegionAvail().x - spacing * (columns - 1)) / columns);
    for(int slot = 0; slot < (int)snapshotSlots.size(); ++slot) {
        ImGui::PushID(slot);
        if(slot % columns != 0) ImGui::SameLine();
        const bool used = snapshotUsed[slot];
        const bool active = activeSnapshot == slot;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.20f, 0.64f, 0.76f, 1.0f)
                                                       : (used ? ImVec4(0.22f, 0.39f, 0.48f, 1.0f)
                                                               : ImVec4(0.14f, 0.16f, 0.20f, 1.0f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.69f, 0.80f, 1.0f));
        const std::string visibleLabel = snapshotNames[slot].empty()
            ? ofToString(slot + 1)
            : ofToString(slot + 1) + " " + snapshotNames[slot];
        const std::string buttonLabel = visibleLabel + "##BeatRepeatSnapshot";
        const bool clicked = ImGui::Button(buttonLabel.c_str(), ImVec2(buttonWidth, 27.0f));
        const bool activated = ImGui::IsItemActivated();
        const bool deactivated = ImGui::IsItemDeactivated();
        const bool shift = ImGui::GetIO().KeyShift;
        if(momentary.get() && !shift) {
            if(activated && used) {
                // Recall first so Auto Reset and all sound parameters are in
                // place before the rising Go edge begins the new capture.
                recallSnapshot(slot, false, true);
                beginMomentaryHold(slot);
            }
            if(deactivated) endMomentaryHold(slot);
        } else if(clicked) {
            if(shift) storeSnapshot(slot);
            else recallSnapshot(slot);
        }
        ImGui::PopStyleColor(2);
        if(ImGui::BeginPopupContextItem("##snapshotMenu")) {
            if(used) {
                if(ImGui::IsWindowAppearing())
                    std::snprintf(snapshotNameBuffers[slot].data(), snapshotNameBuffers[slot].size(),
                                  "%s", snapshotNames[slot].c_str());
                ImGui::TextDisabled("Snapshot %d name", slot + 1);
                ImGui::SetNextItemWidth(180.0f);
                const bool enter = ImGui::InputText("##snapshotName", snapshotNameBuffers[slot].data(),
                    snapshotNameBuffers[slot].size(), ImGuiInputTextFlags_EnterReturnsTrue);
                if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
                    snapshotNames[slot] = snapshotNameBuffers[slot].data();
                    saveSharedSnapshotBank();
                    if(enter) ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
            }
            if(used && ImGui::MenuItem("Clear slot")) {
                snapshotSlots[slot] = ofJson();
                snapshotUsed[slot] = false;
                snapshotNames[slot].clear();
                if(activeSnapshot == slot) activeSnapshot = -1;
                saveSharedSnapshotBank();
            }
            ImGui::EndPopup();
        }
        if(ImGui::IsItemHovered()) {
            if(used && !snapshotNames[slot].empty())
                ImGui::SetTooltip("%s\nClick: recall\nShift-click: overwrite\nRight-click: rename / clear",
                                  snapshotNames[slot].c_str());
            else
                ImGui::SetTooltip(used ? "Click: recall\nShift-click: overwrite\nRight-click: rename / clear"
                                       : "Shift-click: save snapshot");
        }
        ImGui::PopID();
    }
}

void scBeatRepeatPro::drawLoopRegion(ImDrawList* drawList, ImVec2 min, ImVec2 max,
                                     float start, float end, ImU32 color) const {
    const float width = max.x - min.x;
    auto rect = [&](float a, float b) {
        if(b <= a) return;
        drawList->AddRectFilled(ImVec2(min.x + a * width, min.y),
                                ImVec2(min.x + b * width, max.y), color);
    };
    if(start <= end) rect(start, end);
    else { rect(start, 1.0f); rect(0.0f, end); }
}

void scBeatRepeatPro::drawGateMask(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    const float amount = brClamp01(gate.get());
    if(amount >= 0.9999f || !wetActive) return;
    float distance = loopEnd - loopStart;
    if(distance < 0.0f) distance += 1.0f;
    const float mutedStart = reverse.get() ? loopStart : brWrap01(loopStart + distance * amount);
    const float mutedEnd = reverse.get() ? brWrap01(loopEnd - distance * amount) : loopEnd;
    drawLoopRegion(drawList, min, max, mutedStart, mutedEnd, IM_COL32(0, 0, 0, 174));
}

void scBeatRepeatPro::drawWaveform(float width, float height) {
    // Docking/collapsing can transiently report an exact zero content size.
    // InvisibleButton asserts on either zero dimension, so always provide a
    // small valid hit rectangle even during that one-frame layout state.
    width = std::max(1.0f, width);
    height = std::max(1.0f, height);
    ImGui::InvisibleButton("##BeatRepeatWaveform", ImVec2(width, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 background = IM_COL32(17, 20, 27, 255);
    const ImU32 grid = IM_COL32(70, 78, 92, 80);
    const ImU32 region = wetActive ? IM_COL32(48, 171, 196, 45) : IM_COL32(105, 114, 132, 30);
    const ImU32 wave = IM_COL32(119, 208, 224, 210);
    const ImU32 startColor = IM_COL32(77, 208, 181, 230);
    const ImU32 endColor = IM_COL32(226, 145, 73, 230);

    dl->AddRectFilled(min, max, background, 4.0f);
    drawLoopRegion(dl, min, max, loopStart, loopEnd, region);
    const float mid = (min.y + max.y) * 0.5f;
    dl->AddLine(ImVec2(min.x, mid), ImVec2(max.x, mid), grid);
    for(int division = 1; division < 8; ++division) {
        const float x = min.x + (max.x - min.x) * ((float)division / 8.0f);
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), grid);
    }

    const float amplitude = (max.y - min.y) * 0.42f;
    const int pixels = std::max(1, (int)(max.x - min.x));
    for(int pixel = 0; pixel < pixels; ++pixel) {
        const int bin = std::min(WAVEFORM_BINS - 1, (pixel * WAVEFORM_BINS) / pixels);
        const float high = std::max(-1.0f, std::min(1.0f, waveHigh[bin]));
        const float low = std::max(-1.0f, std::min(1.0f, waveLow[bin]));
        const float x = min.x + (float)pixel;
        dl->AddLine(ImVec2(x, mid - high * amplitude), ImVec2(x, mid - low * amplitude), wave);
    }

    // The opaque strip maps the portion suppressed by Gate. In reverse mode
    // it moves to the opposite side, matching the actual playback direction.
    drawGateMask(dl, min, max);

    auto head = [&](float phase, ImU32 color, float thickness) {
        const float x = min.x + brClamp01(phase) * (max.x - min.x);
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), color, thickness);
    };
    head(loopStart, startColor, 1.0f);
    head(loopEnd, endColor, 1.0f);
    head(writeHead, captureActive ? IM_COL32(255, 91, 91, 255) : IM_COL32(230, 235, 242, 190), 1.5f);
    if(wetActive) head(readHead, IM_COL32(255, 196, 76, 255), 2.0f);

    const char* status = captureActive ? "CAPTURING" : (wetActive ? "REPEATING" : "THRU");
    dl->AddText(ImVec2(min.x + 9.0f, min.y + 7.0f), IM_COL32(225, 230, 238, 220), status);
    char phaseText[48];
    std::snprintf(phaseText, sizeof(phaseText), "cycle %3.0f%%", cyclePhase * 100.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(phaseText);
    dl->AddText(ImVec2(max.x - textSize.x - 9.0f, min.y + 7.0f), IM_COL32(180, 188, 201, 190), phaseText);
    dl->AddRect(min, max, IM_COL32(78, 87, 102, 220), 4.0f);
}

void scBeatRepeatPro::drawControls(float height) {
    ImGui::BeginChild("##BeatRepeatControlsRow", ImVec2(0.0f, height), false,
        ImGuiWindowFlags_HorizontalScrollbar);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float visibleWidth = ImGui::GetContentRegionAvail().x;
    const float cardWidth = std::max(230.0f, (visibleWidth - spacing * 3.0f) * 0.25f);
    const float cardHeight = std::max(80.0f, height - ImGui::GetStyle().ScrollbarSize - 3.0f);

    ImGui::BeginChild("##BeatRepeatSnapshotsCard", ImVec2(cardWidth, cardHeight), true);
    drawSectionHeader("SNAPSHOTS");
    checkbox("Recall Activates", snapshotActivates);
    if(ImGui::Button("Save bank...##BeatRepeatBankSave")) snapshotSaveDialog = true;
    ImGui::SameLine();
    if(ImGui::Button("Load bank...##BeatRepeatBankLoad")) snapshotLoadDialog = true;
    if(!snapshotBankMessage.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", snapshotBankMessage.c_str());
    drawSnapshotMatrix();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##BeatRepeatLoopCard", ImVec2(cardWidth, cardHeight), true);
    drawSectionHeader("LOOP");
    checkbox("Pitch Mode", pitchMode);
    ImGui::SameLine();
    checkbox("Momentary", momentary);
    if(pitchMode.get()) {
        sliderFloat("Repeat MIDI", repeatPitch, "%.2f MIDI");
        const float hz = 440.0f * std::pow(2.0f, (repeatPitch.get() - 69.0f) / 12.0f);
        ImGui::TextDisabled("%.2f Hz  /  %.3f ms", hz, 1000.0f / std::max(0.001f, hz));
    } else {
        musicalTimeSlider("Repeat");
    }
    sliderFloat("Offset (ms)", offsetMs, "%.1f");
    sliderFloat("Gate", gate, "%.3f");
    sliderFloat("Declick (ms)", declickMs, "%.2f");
    ImGui::Spacing();
    drawSectionHeader("AUTO RESET");
    checkbox("Auto Reset", autoReset);
    int resetTiming = autoResetMode.get();
    const char* resetTimings[] = {"Repetitions", "Beats"};
    ImGui::SetNextItemWidth(brEditorControlWidth("Timing"));
    if(ImGui::Combo("Timing", &resetTiming, resetTimings, 2)) autoResetMode.set(resetTiming);
    drawPublishMenu(autoResetMode.getEscapedName());
    if(autoResetMode.get() == 0) {
        sliderFloat("Every (repetitions)", autoResetLoops, "%.3f");
    } else {
        sliderFloat("Every (beats)", autoResetBeats, "%.3f");
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Quarter-note beats from Oceanode's shared BPM.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##BeatRepeatPlaybackCard", ImVec2(cardWidth, cardHeight), true);
    drawSectionHeader("PLAYBACK");
    checkbox("Adapt Length", adaptLength);
    ImGui::SameLine();
    checkbox("Reverse", reverse);
    sliderFloat("Rate", rate, "%.3f x");
    sliderFloat("Pitch Step", pitchStep, "%.2f st");
    sliderInt("Pitch Steps", numPitchSteps);
    sliderFloat("Loop Amp", loopAmp, "%.3f");
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##BeatRepeatToneCard", ImVec2(cardWidth, cardHeight), true);
    drawSectionHeader("TONE / SAFETY");
    sliderFloat("Input Gain", inputGain, "%.3f");
    int type = filterType.get();
    const char* filters[] = {"Off", "Low-pass", "High-pass"};
    ImGui::SetNextItemWidth(brEditorControlWidth("Filter"));
    if(ImGui::Combo("Filter", &type, filters, 3)) filterType.set(type);
    drawPublishMenu(filterType.getEscapedName());
    sliderFloat("Cutoff Pitch", cutoffPitch, "%.2f MIDI");
    const float cutoffHz = std::max(20.0f, std::min(20000.0f,
        440.0f * std::pow(2.0f, (cutoffPitch.get() - 69.0f) / 12.0f)));
    ImGui::TextDisabled("%.2f Hz", cutoffHz);
    sliderFloat("Cutoff Step", cutoffStep, "%.2f st");
    sliderInt("Cutoff Steps", numCutoffSteps);
    sliderFloat("Resonance", resonance, "Q %.3f");
    checkbox("Limiter", limiter);
    sliderFloat("Ceiling", ceiling, "%.3f");
    ImGui::EndChild();
    ImGui::EndChild();
}

void scBeatRepeatPro::drawWindow() {
    visualizationHoldFrames = 3;
    // Enable feedback in the same callback that actually displays the editor.
    // Waiting for a later update used to leave freshly recalled presets at
    // vis=0 until Go (or another parameter) happened to resend every control.
    if(!visOn) setVisualizationEnabled(true);
    std::string title = (canvasID == "Canvas" ? "" : canvasID + "/")
        + "BeatRepeat Pro " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1040.0f, 650.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float contentWidth = std::max(1.0f, available.x);
        constexpr float resizeHandleHeight = 10.0f;
        constexpr float transportHeight = 58.0f;
        constexpr float minimumControlsHeight = 170.0f;
        const float maximumWaveformHeight = std::max(120.0f,
            available.y - resizeHandleHeight - transportHeight - minimumControlsHeight);
        waveformHeight = std::max(120.0f, std::min(maximumWaveformHeight, waveformHeight));
        drawWaveform(contentWidth, waveformHeight);

        ImGui::InvisibleButton("##BeatRepeatWaveformResize", ImVec2(contentWidth, resizeHandleHeight));
        const ImVec2 handleMin = ImGui::GetItemRectMin();
        const ImVec2 handleMax = ImGui::GetItemRectMax();
        const bool handleHovered = ImGui::IsItemHovered();
        const bool handleActive = ImGui::IsItemActive();
        if(handleHovered || handleActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(handleActive) {
            waveformHeight = std::max(120.0f, std::min(maximumWaveformHeight,
                waveformHeight + ImGui::GetIO().MouseDelta.y));
        }
        ImDrawList* resizeDrawList = ImGui::GetWindowDrawList();
        const float handleY = (handleMin.y + handleMax.y) * 0.5f;
        const ImU32 handleColor = handleActive ? IM_COL32(255, 196, 76, 235)
                                  : (handleHovered ? IM_COL32(130, 202, 220, 220)
                                                   : IM_COL32(83, 96, 114, 180));
        resizeDrawList->AddLine(ImVec2(handleMin.x, handleY), ImVec2(handleMax.x, handleY),
                                handleColor, handleActive ? 3.0f : 1.5f);
        const float centerX = (handleMin.x + handleMax.x) * 0.5f;
        resizeDrawList->AddRectFilled(ImVec2(centerX - 24.0f, handleY - 2.0f),
                                      ImVec2(centerX + 24.0f, handleY + 2.0f), handleColor, 2.0f);

        ImGui::BeginChild("##BeatRepeatTransport", ImVec2(0.0f, transportHeight), false,
                          ImGuiWindowFlags_NoScrollbar);

        const bool enabled = go.get();
        const ImVec4 goColor = captureActive ? ImVec4(0.88f, 0.30f, 0.24f, 1.0f)
                              : (wetActive ? ImVec4(0.12f, 0.68f, 0.58f, 1.0f)
                                           : ImVec4(0.20f, 0.43f, 0.62f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, goColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(std::min(1.0f, goColor.x + 0.12f), std::min(1.0f, goColor.y + 0.12f),
                   std::min(1.0f, goColor.z + 0.12f), 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.95f, 0.72f, 0.20f, 1.0f));
        const char* goLabel = captureActive ? "CAPTURING..." : (wetActive ? "REPEATING" : "GO");
        const bool goClicked = ImGui::Button(goLabel, ImVec2(168.0f, 52.0f));
        const bool goActivated = ImGui::IsItemActivated();
        const bool goDeactivated = ImGui::IsItemDeactivated();
        if(momentary.get()) {
            if(goActivated) beginMomentaryHold(-2);
            if(goDeactivated) endMomentaryHold(-2);
        } else if(goClicked) {
            go.set(!enabled);
        }
        ImGui::PopStyleColor(3);
        drawValuePopup("Go", [this]() { return go.get() ? "1" : "0"; },
            [this](const std::string& text) { go.set(text == "1" || text == "true" || text == "on"); }, false);
        ImGui::SameLine();
        if(ImGui::Button("RESET", ImVec2(78.0f, 52.0f))) reset.set(1);
        ImGui::SameLine();
        const float sliderWidth = std::max(110.0f,
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) * 0.42f);
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        float wet = mix.get();
        ImGui::SetNextItemWidth(sliderWidth);
        if(ImGui::SliderFloat("Mix", &wet, 0.0f, 1.0f, "%.3f")) mix.set(wet);
        drawValuePopup("Mix", [this]() { return ofToString(mix.get(), 6); },
            [this](const std::string& text) { try { mix.set(ofClamp(std::stof(text), 0.0f, 1.0f)); } catch(...) {} }, false);
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        float output = level.get();
        ImGui::SetNextItemWidth(sliderWidth);
        if(ImGui::SliderFloat("Level", &output, 0.0f, 2.0f, "%.3f")) level.set(output);
        drawValuePopup("Level", [this]() { return ofToString(level.get(), 6); },
            [this](const std::string& text) { try { level.set(ofClamp(std::stof(text), 0.0f, 2.0f)); } catch(...) {} }, false);
        ImGui::EndGroup();
        ImGui::EndChild();

        drawControls(std::max(1.0f, ImGui::GetContentRegionAvail().y));
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow.set(open);
}

void scBeatRepeatPro::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
}

void scBeatRepeatPro::presetSave(ofJson& json) {
    json["publishedEditorParameters"] = publishedKeys;
    json["editorWaveformHeight"] = waveformHeight;
    for(const auto& action : publishActions) if(action.save) action.save(json);
    json["activeSnapshot"] = activeSnapshot;
    json["beatRepeatSnapshots"] = ofJson::array();
    json["beatRepeatSnapshotNames"] = ofJson::array();
    for(int slot = 0; slot < (int)snapshotSlots.size(); ++slot) {
        json["beatRepeatSnapshots"].push_back(snapshotUsed[slot] ? snapshotSlots[slot] : ofJson(nullptr));
        json["beatRepeatSnapshotNames"].push_back(snapshotNames[slot]);
    }
}

void scBeatRepeatPro::loadBeforeConnections(ofJson& json) {
    if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
    try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
}

void scBeatRepeatPro::presetRecallBeforeSettingParameters(ofJson& json) {
    loadBeforeConnections(json);
}

void scBeatRepeatPro::presetRecallAfterSettingParameters(ofJson& json) {
    if(json.contains("editorWaveformHeight") && json["editorWaveformHeight"].is_number())
        waveformHeight = ofClamp(json["editorWaveformHeight"].get<float>(), 120.0f, 1200.0f);
    for(const auto& action : publishActions) {
        if(isPublished(action.key) || !json.contains(action.key) || !action.load) continue;
        action.load(json[action.key]);
    }
    // Once a shared bank exists it is deliberately independent of the loaded
    // project/preset. An older embedded bank is imported only as a migration
    // fallback, then becomes the shared bank for future projects.
    if(!sharedSnapshotsLoaded) {
        snapshotUsed.fill(false);
        snapshotNames.fill("");
        if(json.contains("beatRepeatSnapshots") && json["beatRepeatSnapshots"].is_array()) {
            const auto& snapshots = json["beatRepeatSnapshots"];
            const int count = std::min((int)snapshotSlots.size(), (int)snapshots.size());
            for(int slot = 0; slot < count; ++slot) {
                if(snapshots[slot].is_object()) {
                    snapshotSlots[slot] = snapshots[slot];
                    snapshotUsed[slot] = true;
                }
            }
        }
        if(json.contains("beatRepeatSnapshotNames") && json["beatRepeatSnapshotNames"].is_array()) {
            const auto& names = json["beatRepeatSnapshotNames"];
            for(int slot = 0; slot < std::min((int)snapshotNames.size(), (int)names.size()); ++slot)
                if(names[slot].is_string()) snapshotNames[slot] = names[slot].get<std::string>();
        }
        activeSnapshot = json.value("activeSnapshot", -1);
        if(activeSnapshot < 0 || activeSnapshot >= (int)snapshotSlots.size() || !snapshotUsed[activeSnapshot])
            activeSnapshot = -1;
        bool hasSnapshots = false;
        for(bool used : snapshotUsed) if(used) { hasSnapshots = true; break; }
        if(hasSnapshots) saveSharedSnapshotBank();
    }
    if(showWindow.get()) {
        visualizationHoldFrames = 3;
        setVisualizationEnabled(true);
    }
    sendAllParams();
}
