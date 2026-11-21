//
//  scBake.cpp
//  ofxOceanodeSuperCollider
//

#include "scBake.h"
#include "scBakeLoop.h"
#include "../../ofxOceanodeSuperColliderController.h"
#include "../../samples/scSampleEmbedding.h"
#include "../boxSynths/sampleBox/scSampleBox.h"
#include "../../audio/scSynthControlBatch.h"
#include "ofxOceanodeAutomationExtension.h"
#include "../../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCServer.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeNode.h"
#include "imgui.h"
#include <cctype>
#include <cstdio>
#include <filesystem>

ofxOceanodeSuperColliderController* scBake::sharedController = nullptr;

namespace {
const char* kMenuLabels[3] = {"Bake to Texture Loop", "Bake to Synced Loop", "Bake Timeline Span"};

float bpmOf(const ofxOceanodeNodeModel& model){
    return std::max(1.0f, model.getFrameTransportState().current.bpm);
}

bool tempoAutomated(const ofxOceanodeNodeModel& model){
    if(auto* container = model.getHostContainer())
        if(auto* extension = container->getAutomationExtension()) return extension->isBpmAutomationEnabled();
    return false;
}

double beatsPerBarOf(const ofxOceanodeNodeModel& model){
    if(auto* container = model.getHostContainer())
        if(auto* extension = container->getAutomationExtension())
            return std::max(0.25, extension->getBeatsPerBar());
    return 4.0;
}

std::string safeFileName(std::string name){
    for(char& c : name) if(!std::isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    return name.empty() ? std::string("bake") : name;
}

ofxOceanodeAbstractParameter* parameterNamed(ofxOceanodeNodeModel& model, const std::string& name){
    if(name.empty() || !model.getParameterGroup().contains(name)) return nullptr;
    return dynamic_cast<ofxOceanodeAbstractParameter*>(&model.getParameterGroup().get(name));
}

float renderSeconds(const scBake::Settings& s){
    // Synced: intro, loop and the repeat check. Texture: the loop, what is
    // blended into its start, the skipped start. And a little more so Match
    // Loop and the frame-stepped end never run short.
    if(s.span) return (float)(s.spanEndBeat() * s.beatSeconds()) + s.tail + 0.5f;
    if(s.synced) return (float)(3.0 * s.cycleSeconds()) + 0.5f;
    return s.skip + s.length + s.crossfade + 0.5f;
}

void drawSettings(scBake::Settings& s){
    if(s.span){
        ImGui::PushItemWidth(110);
        ImGui::DragFloat("From Bar", &s.fromBar, 0.25f, 1.0f, 10000.0f, "%.2f");
        ImGui::DragFloat("Length (bars)", &s.lengthBars, 0.25f, 0.25f, 10000.0f, "%.2f");
        ImGui::DragFloat("Tail (s)", &s.tail, 0.1f, 0.0f, 60.0f, "%.1f");
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Rendered past the span's end: reverbs and releases ring out\n"
                              "(anything the node plays there is kept too).");
        ImGui::SliderInt("Channels", &s.channels, 1, 64);
        ImGui::PopItemWidth();
        s.fromBar = std::max(1.0f, s.fromBar);
        s.lengthBars = std::max(0.25f, s.lengthBars);
        s.tail = std::max(0.0f, s.tail);
        ImGui::TextDisabled("Bars %.2f to %.2f = %.1f s at %.1f BPM, + %.1f s tail", s.fromBar,
                            s.fromBar + s.lengthBars, s.lengthBars * s.beatsPerBar * s.beatSeconds(), s.bpm, s.tail);
        return;
    }
    if(s.synced){
        ImGui::PushItemWidth(110);
        ImGui::SliderInt("Bars", &s.bars, 1, 64);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("How often the sequence repeats: the loop's length.\n"
                              "Too short and the loop leaves notes out (the repeat check says so).");
        ImGui::SliderInt("Channels", &s.channels, 1, 64);
        ImGui::PopItemWidth();
        s.bars = std::max(1, s.bars);
        ImGui::TextDisabled("%d bars = %.0f beats = %.2f s at %.1f BPM", s.bars, s.loopBeats(), s.cycleSeconds(), s.bpm);
        return;
    }
    ImGui::PushItemWidth(110);
    ImGui::DragFloat("Loop Length (s)", &s.length, 0.1f, 1.0f, 600.0f, "%.1f");
    ImGui::DragFloat("Crossfade (s)", &s.crossfade, 0.01f, 0.0f, 10.0f, "%.2f");
    ImGui::DragFloat("Skip Start (s)", &s.skip, 0.1f, 0.0f, 60.0f, "%.1f");
    ImGui::SliderInt("Channels", &s.channels, 1, 64);
    ImGui::PopItemWidth();
    ImGui::Checkbox("Match Loop Point", &s.matchLoop);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Shortens the loop by up to a tenth so its end meets its start in phase.\n"
                          "Off: exactly the length asked for.");
    s.length = std::max(1.0f, s.length);
    s.crossfade = std::max(0.0f, std::min(s.crossfade, s.length * 0.5f));
    s.skip = std::max(0.0f, s.skip);
}
} // namespace

scBake::scBake(ofxOceanodeSuperColliderController* superColliderController)
: scNode("Bake"), controller(superColliderController) {
    if(sharedController == nullptr) sharedController = superColliderController;
    color = ofColor(120, 170, 200);
}

scBake::~scBake(){
    if(controller != nullptr && bakingSource != nullptr) controller->cancelNRTBake(bakingSource);
    sourceDestroyedListener.unsubscribe();
    if(loopThread.joinable()) loopThread.join();
    for(auto& entry : serverStates){
        if(entry.second.synth != nullptr){ entry.second.synth->free(); delete entry.second.synth; }
        for(ofxSCBuffer* buffer : {entry.second.buffer, entry.second.introBuffer}){
            if(buffer == nullptr) continue;
            if(entry.first != nullptr && buffer->generation == entry.first->getAllocationGeneration()) buffer->free();
            delete buffer;
        }
    }
    serverStates.clear();
}

void scBake::setup(){
    addOnToggle();
    addOutput("Out");
    addParameter(level.set("Level", 1.0f, 0.0f, 2.0f));
    addCustomRegion(statusRegion.set("Bake", [this](){ drawStatus(); }), [this](){ drawStatus(); });
    // What Rebake uses (and what the menu offered first)
    addInspectorParameter(loopLength.set("Loop Length", 20.0f, 1.0f, 600.0f));
    addInspectorParameter(crossfade.set("Crossfade", 2.0f, 0.0f, 10.0f));
    addInspectorParameter(skipStart.set("Skip Start", 4.0f, 0.0f, 60.0f));
    addInspectorParameter(renderChannels.set("Render Channels", 2, 1, 64));
    addInspectorParameter(matchLoop.set("Match Loop", true));
    addInspectorParameter(syncedMode.set("Synced", false));
    addInspectorParameter(barsParam.set("Bars", 4, 1, 64));
    addInspectorParameter(spanMode.set("Span", false));
    addInspectorParameter(fromBarParam.set("From Bar", 1.0f, 1.0f, 10000.0f));
    addInspectorParameter(lengthBarsParam.set("Length Bars", 16.0f, 0.25f, 10000.0f));
    addInspectorParameter(tailParam.set("Tail", 4.0f, 0.0f, 60.0f));
    listeners.push(level.newListener([this](float& value){
        for(auto& entry : serverStates)
            if(entry.second.synth != nullptr) entry.second.synth->set("levels", value);
    }));
}

std::string scBake::synthdefFolder(){
    static std::string cached;
    if(!cached.empty()) return cached;
    namespace fs = std::filesystem;
    const std::string expected = "BakeLoop2.scsyndef";
    try {
        const fs::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        // where the SynthDef sync installs Defaults/Bake
        const fs::path usual = root / "Defaults/Bake";
        if(fs::exists(usual / expected)) return cached = usual.string();
        if(fs::exists(root))
            for(const auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
                if(e.is_regular_file() && e.path().filename() == expected) return cached = e.path().parent_path().string();
        // The addon's own build, during development
        const fs::path bundled = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
            / "synthdefs/Defaults/Bake/CompiledSynthdefs/Bake";
        if(fs::exists(bundled / expected)) return cached = bundled.string();
    } catch(const std::exception& e) {
        ofLogWarning("scBake") << e.what();
    }
    ofLogError("scBake") << "BakeLoop/BakeTap SynthDefs not found: compile synthdefs/Defaults/Bake/bake.scd";
    return {};
}

int scBake::guessChannels(scNode& source, int outputIndex){
    const int declared = source.getOutputNumChannels(outputIndex);
    if(declared > 0) return std::min(64, declared);
    auto& group = source.getParameterGroup();
    for(const char* name : {"N Chan", "Num Channels", "Channels", "Outputs"}){
        if(!group.contains(name)) continue;
        auto& p = group.get(name);
        if(p.valueType() == typeid(int).name()) return std::max(1, std::min(64, p.cast<int>().get()));
    }
    return 2;
}

// ── Menus ────────────────────────────────────────────────────────────────────

bool scBake::drawNodeMenu(scNode& node){
    if(auto* bakeNode = dynamic_cast<scBake*>(&node)) return bakeNode->drawOwnMenu();
    if(node.getNumOutputs() == 0 || sharedController == nullptr || node.getHostContainer() == nullptr) return false;

    ImGui::Separator();
    // One menu open at a time: its settings belong to the node it was opened on
    static Settings settings[3];
    static int output[3] = {0, 0, 0};
    static const scNode* openedOn[3] = {nullptr, nullptr, nullptr};
    for(int mode = 0; mode < 3; mode++){
        if(!ImGui::BeginMenu(kMenuLabels[mode])) continue;
        Settings& s = settings[mode];
        int& out = output[mode];
        if(openedOn[mode] != &node){
            openedOn[mode] = &node;
            out = 0;
            s = Settings();
            s.synced = mode == 1;
            s.span = mode == 2;
            s.channels = guessChannels(node, 0);
            if(s.span){
                // the timeline's loop range marks the span, when there is one
                const auto transport = node.getFrameTransportState().current;
                const double bpb = beatsPerBarOf(node);
                if(transport.loopEndBeat > transport.loopStartBeat + 1e-6){
                    s.fromBar = (float)(transport.loopStartBeat / bpb + 1.0);
                    s.lengthBars = (float)((transport.loopEndBeat - transport.loopStartBeat) / bpb);
                }
            }
        }
        // the tempo and meter the render will run at
        s.bpm = bpmOf(node);
        s.beatsPerBar = beatsPerBarOf(node);
        if(node.getNumOutputs() > 1){
            ImGui::SetNextItemWidth(110);
            if(ImGui::BeginCombo("Output", node.getOutputName(out).c_str())){
                for(int i = 0; i < node.getNumOutputs(); i++){
                    if(ImGui::Selectable(node.getOutputName(i).c_str(), i == out)){
                        out = i;
                        s.channels = guessChannels(node, i);
                    }
                }
                ImGui::EndCombo();
            }
        }
        drawSettings(s);
        ImGui::TextDisabled("Renders %.0f s from the transport's start; the\n"
                            "transport plays it once, then stops. The original\n"
                            "stays in the patch, unplugged (Unbake restores it).",
                            renderSeconds(s));
        if(s.synced)
            ImGui::TextDisabled("Keep the tempo steady while it renders: the loop\n"
                                "is cut at %.1f BPM.", s.bpm);
        if(s.span)
            ImGui::TextDisabled("Silent before and after the span. The timeline loop\n"
                                "must be off (or end after the render), and the\n"
                                "tempo not automated.");

        const bool busy = sharedController->isNRTBusy();
        if(busy) ImGui::BeginDisabled();
        const bool bakeNow = ImGui::Button("Bake", ImVec2(110, 0));
        if(busy){
            ImGui::EndDisabled();
            ImGui::TextDisabled("Another offline render is running");
        }
        ImGui::EndMenu();

        if(bakeNow){
            auto* container = node.getHostContainer();
            auto* outputParameter = parameterNamed(node, node.getOutputName(out));
            auto* sourceNode = outputParameter != nullptr ? container->getNodeFromParameter(*outputParameter) : nullptr;
            auto* created = container->createNodeFromName("SC Bake");
            auto* bakeNode = created != nullptr ? dynamic_cast<scBake*>(&created->getNodeModel()) : nullptr;
            if(bakeNode == nullptr){
                ofLogError("scBake") << "Could not create an SC Bake node";
                if(created != nullptr) created->deleteSelf();
            }else{
                if(sourceNode != nullptr)
                    created->getNodeGui().setPosition(sourceNode->getNodeGui().getPosition() + glm::vec2(30, 40));
                bakeNode->startBake(node, out, s);
            }
            openedOn[mode] = nullptr;
            ImGui::CloseCurrentPopup();
        }
    }
    return false;
}

bool scBake::drawOwnMenu(){
    // An action that deletes this node returns at once: `this` is gone
    ImGui::Separator();
    const bool busy = state == State::Rendering || state == State::Looping;
    if(busy) ImGui::BeginDisabled();
    bool removeNow = false, rebakeNow = false;
    if(ImGui::Selectable("Unbake and Remove")) removeNow = true;
    if(ImGui::Selectable("Rebake")) rebakeNow = true;
    if(busy) ImGui::EndDisabled();

    // SampleBox holds mono and stereo samples
    const bool canReplace = state == State::Baked && !span && fileChannels >= 1 && fileChannels <= 2;
    if(!canReplace) ImGui::BeginDisabled();
    const bool replaceNow = ImGui::Selectable("Replace with SampleBox") && canReplace;
    if(!canReplace) ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(span ? "A timeline span stays in SC Bake (SampleBox plays loops)" :
                          state != State::Baked ? "Only while baked (the loop is playing)" :
                          fileChannels > 2 ? "SampleBox plays mono and stereo samples; this bake has %d channels" :
                          synced ? "A SampleBox takes this node's place: pad 1 loops the baked loop in sync\n"
                                   "(Fit, Sync To Transport, Play With Transport), without the intro.\n"
                                   "This node is removed; the original stays unplugged."
                                 : "A SampleBox takes this node's place: pad 1 loops the baked file\n"
                                   "(Play Always), with its sound, effects and modulation to play with.\n"
                                   "This node is removed; the original stays unplugged.", fileChannels);

    if(replaceNow) return replaceWithSampleBox();
    if(rebakeNow){ rebake(); return false; }
    if(removeNow && !busy && (state != State::Baked || unbake())){
        if(auto* container = getHostContainer()){
            if(auto* own = ownOutputParameter()){
                if(auto* node = container->getNodeFromParameter(*own)){
                    node->deleteSelf();
                    return true;
                }
            }
        }
    }
    return false;
}

// ── Status region ────────────────────────────────────────────────────────────

void scBake::drawStatus(){
    ImGui::PushID(this);
    const float width = 220.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    if(!sourceName.empty()) ImGui::TextDisabled("%s / %s", sourceName.c_str(), sourceOutput.c_str());

    switch(state){
        case State::Rendering: {
            std::string stage = controller != nullptr ? controller->getNRTBakeStage() : std::string();
            float progress = -1.0f;
            if(controller != nullptr){
                if(stage == "Capturing") progress = controller->getNRTCaptureProgress();
                else if(stage == "Rendering") progress = controller->getNRTRenderProgress();
            }
            ImGui::TextUnformatted(stage.empty() ? "Waiting..." : (stage + "...").c_str());
            if(progress >= 0.0f) ImGui::ProgressBar(progress, ImVec2(width, 0));
            break;
        }
        case State::Looping:
            ImGui::TextUnformatted("Making the loop...");
            break;
        case State::Baked:
            ImGui::TextUnformatted(loopInfo.c_str());
            if(followsTransport()){
                const float now = transportBpm();
                if(bakedBpm > 0.0f && std::abs(now - bakedBpm) > 0.01f)
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                       "Tempo changed since the bake (%.1f -> %.1f BPM): it is resampled. Rebake.",
                                       bakedBpm, now);
            }
            break;
        default:
            if(!statusText.empty()) ImGui::TextUnformatted(statusText.c_str());
            break;
    }
    ImGui::PopTextWrapPos();

    const bool busy = state == State::Rendering || state == State::Looping;
    if(!busy){
        if(state == State::Baked){
            if(ImGui::Button("Unbake")) unbake();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the cables back on the original; this node stays, silent.");
            ImGui::SameLine();
        }else if(!filePath.empty() && ofFile::doesFileExist(filePath) && findSource() != nullptr){
            if(ImGui::Button("Use Baked File")) applyLoop();
            ImGui::SameLine();
        }
        if(!sourceName.empty() && ImGui::Button("Rebake")) rebake();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Renders the original again with the Inspector's settings.");
    }
    ImGui::PopID();
}

// ── Baking ───────────────────────────────────────────────────────────────────

scBake::Settings scBake::currentSettings() const {
    Settings s;
    s.length = loopLength.get();
    s.crossfade = crossfade.get();
    s.skip = skipStart.get();
    s.channels = renderChannels.get();
    s.matchLoop = matchLoop.get();
    // Span wins when both are ticked in the Inspector
    s.synced = syncedMode.get() && !spanMode.get();
    s.bars = barsParam.get();
    s.span = spanMode.get();
    s.fromBar = fromBarParam.get();
    s.lengthBars = lengthBarsParam.get();
    s.tail = tailParam.get();
    s.bpm = transportBpm();
    s.beatsPerBar = transportBeatsPerBar();
    return s;
}

float scBake::transportBpm() const { return bpmOf(*this); }
double scBake::transportBeatsPerBar() const { return beatsPerBarOf(*this); }

void scBake::startBake(scNode& source, int outputIndex, const Settings& settings){
    if(state == State::Rendering || state == State::Looping) return;
    if(controller == nullptr){ state = State::Failed; statusText = "No SuperCollider controller"; return; }
    const std::string folder = synthdefFolder();
    if(folder.empty()){ state = State::Failed; statusText = "Compile synthdefs/Defaults/Bake first"; return; }
    if(settings.synced || settings.span){
        // The render plays from beat 0 to its end: a timeline loop ending
        // sooner would wrap the transport inside it
        const auto transport = getFrameTransportState().current;
        const double renderBeats = settings.synced ? 3.0 * settings.loopBeats()
            : settings.spanEndBeat() + settings.tail / settings.beatSeconds();
        std::string refusal;
        if(transport.loopActive() && transport.loopEndBeat < renderBeats)
            refusal = "The timeline loop ends at beat " + ofToString(transport.loopEndBeat, 1) +
                      ", inside the render (" + ofToString(renderBeats, 0) +
                      " beats): switch the loop off or make it longer, then Rebake";
        else if(settings.span && tempoAutomated(*this))
            refusal = "The tempo is automated: a span is placed by beats at one tempo. "
                      "Switch the tempo automation off, then Rebake";
        else if(settings.span && controller != nullptr && !controller->getServers().empty() &&
                controller->getServers().front() != nullptr){
            const double rate = controller->getServers().front()->getSampleRate();
            const double seconds = settings.lengthBars * settings.beatsPerBar * settings.beatSeconds() + settings.tail;
            if(rate > 0.0 && seconds * rate >= (double)(1 << 24))
                refusal = "The span and its tail are " + ofToString(seconds / 60.0, 1) + " minutes; at most " +
                          ofToString((double)(1 << 24) / rate / 60.0, 1) + " fit one bake";
        }
        if(!refusal.empty()){
            state = State::Failed;
            statusText = refusal;
            sourceName = source.getParameterGroup().getName();
            sourceOutput = source.getOutputName(outputIndex);
            syncedMode.set(settings.synced);
            barsParam.set(settings.bars);
            spanMode.set(settings.span);
            fromBarParam.set(settings.fromBar);
            lengthBarsParam.set(settings.lengthBars);
            tailParam.set(settings.tail);
            return;
        }
    }

    sourceName = source.getParameterGroup().getName();
    sourceOutput = source.getOutputName(outputIndex);
    bakingSource = &source;
    bakingOutput = outputIndex;
    bakingSettings = settings;
    bakingSettings.channels = settings.channels > 0 ? settings.channels : guessChannels(source, outputIndex);
    // Rebake starts from what this bake used
    loopLength.set(bakingSettings.length);
    crossfade.set(bakingSettings.crossfade);
    skipStart.set(bakingSettings.skip);
    renderChannels.set(bakingSettings.channels);
    matchLoop.set(bakingSettings.matchLoop);
    syncedMode.set(bakingSettings.synced);
    barsParam.set(bakingSettings.bars);
    spanMode.set(bakingSettings.span);
    fromBarParam.set(bakingSettings.fromBar);
    lengthBarsParam.set(bakingSettings.lengthBars);
    tailParam.set(bakingSettings.tail);

    sourceDestroyedListener = source.destroyedNode.newListener([this](){
        if(controller != nullptr && bakingSource != nullptr) controller->cancelNRTBake(bakingSource);
        bakingSource = nullptr;
        if(state == State::Rendering){
            state = State::Failed;
            statusText = "The original was deleted during the bake";
        }
    });

    const std::string folderPath = ofToDataPath("Supercollider/Bakes", true);
    ofDirectory::createDirectory(folderPath, false, true);
    const std::string base = ofFilePath::join(folderPath, safeFileName(sourceName) + "_" + ofGetTimestampString("%Y%m%d-%H%M%S"));
    renderPath = base + "_render.wav";
    loopPath = base + ".wav";
    introOutPath = base + "_intro.wav";

    ofxOceanodeSuperColliderController::NRTBakeRequest request;
    request.node = &source;
    request.outputIndex = outputIndex;
    request.channels = bakingSettings.channels;
    request.seconds = renderSeconds(bakingSettings);
    request.outputPath = renderPath;
    request.tapSynthdefFolder = folder;
    request.done = [this](bool ok, const std::string& message, const std::vector<std::string>& files, int filesChannels){
        onRendered(ok, message, files, filesChannels);
    };
    state = State::Rendering;
    statusText.clear();
    if(!controller->requestNRTBake(request)){
        state = State::Failed;
        statusText = "Could not start: " + controller->getNRTStatus();
        forgetBakeSource();
    }
}

void scBake::forgetBakeSource(){
    sourceDestroyedListener.unsubscribe();
    bakingSource = nullptr;
}

void scBake::onRendered(bool ok, const std::string& message, const std::vector<std::string>& files, int filesChannels){
    forgetBakeSource();
    if(!ok){
        state = State::Failed;
        statusText = "Bake failed: " + message;
        return;
    }
    state = State::Looping;
    loopDone = false;
    if(loopThread.joinable()) loopThread.join();
    const std::string out = loopPath, introOut = introOutPath;
    const Settings settings = bakingSettings;
    loopThread = std::thread([this, files, filesChannels, out, introOut, settings](){
        scBakeLoop::Audio render, loop, intro;
        std::string error;
        bool ok = false;
        std::string message;
        int channels = 0, frames = 0, rate = render.sampleRate;
        if(!scBakeLoop::readParts(files, filesChannels, settings.channels, render, error)){
            message = "Could not read the render: " + error;
        }else if(settings.span){
            const auto result = scBakeLoop::makeSpan(render, settings.spanStartBeat() * settings.beatSeconds(),
                                                     settings.spanEndBeat() * settings.beatSeconds() + settings.tail, loop);
            message = result.message;
            if(result.ok && !scBakeLoop::writeFloatWav(out, loop)) message = "Could not write " + out;
            else if(result.ok){ ok = true; channels = result.channels; frames = result.frames; }
        }else if(settings.synced){
            const auto result = scBakeLoop::makeSyncedLoop(render, settings.cycleSeconds(), intro, loop);
            message = result.message;
            if(result.ok && result.repeat < 0.9)
                message += " -- it does not repeat every " + ofToString(settings.bars) +
                           " bars (a random draw, a free LFO, a longer pattern?)";
            if(result.ok && (!scBakeLoop::writeFloatWav(out, loop) || !scBakeLoop::writeFloatWav(introOut, intro)))
                message = "Could not write " + out;
            else if(result.ok){ ok = true; channels = result.channels; frames = result.frames; }
        }else{
            scBakeLoop::Settings loopSettings;
            loopSettings.skip = settings.skip;
            loopSettings.length = settings.length;
            loopSettings.crossfade = settings.crossfade;
            loopSettings.matchLoop = settings.matchLoop;
            const auto result = scBakeLoop::makeTextureLoop(render, loopSettings, loop);
            message = result.message;
            if(result.ok && !scBakeLoop::writeFloatWav(out, loop)) message = "Could not write " + out;
            else if(result.ok){ ok = true; channels = result.channels; }
        }
        for(const auto& file : files){
            std::remove(file.c_str());
            std::remove((file + ".osc").c_str());
            std::remove((file + ".log").c_str());
        }
        loopOk = ok;
        loopMessage = message;
        loopChannels = channels;
        loopFramesOut = frames;
        loopRateOut = rate;
        loopDone = true;
    });
}

void scBake::update(ofEventArgs&){
    if(followsTransport() && state == State::Baked) sendAnchors();
    if(state != State::Looping || !loopDone.load()) return;
    if(loopThread.joinable()) loopThread.join();
    if(!loopOk){
        state = State::Failed;
        statusText = "Bake failed: " + loopMessage;
        return;
    }
    // A rebake's earlier file stays on disk: an undo step or a saved preset
    // may still refer to it.
    filePath = loopPath;
    fileChannels = loopChannels;
    loopInfo = loopMessage;
    synced = bakingSettings.synced;
    span = bakingSettings.span;
    introPath = synced ? introOutPath : std::string();
    loopBeats = synced ? bakingSettings.loopBeats() : 0.0;
    spanStartBeat = span ? bakingSettings.spanStartBeat() : 0.0;
    loopFrames = followsTransport() ? loopFramesOut : 0;
    fileRate = loopRateOut;
    bakedBpm = followsTransport() ? bakingSettings.bpm : 0.0f;
    applyLoop();
}

// The transport's position for the synced player (as SampleBox's Sync): the
// beat modulo the loop, timetagged at the anchor's instant; a jump into the
// first cycle starts the intro, any other jump ends it.
void scBake::sendAnchors(){
    if(synced ? loopBeats <= 0.0 : loopFrames < 1 || bakedBpm <= 0.0f) return;
    bool any = false;
    for(auto& entry : serverStates) if(entry.second.synth != nullptr) any = true;
    if(!any) return;
    bool pending = false;
    for(const auto& a : follower.poll(getFrameTransportState())){
        anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
        scTransportSync::AnchorScope scope(a);
        if(span){
            // the file's frame at the anchor's beat, at the file's own rate:
            // whole, so the clock reads whole frames (exact); a jump sets the
            // clock there. Not clamped: the clock spans +-1e9 frames.
            double rate = fileRate;
            if(rate <= 0.0)
                for(auto& [server, st] : serverStates) if(server != nullptr) rate = serverManager::getSampleRateForServer(server);
            const double framesPerBeat = (rate > 0.0 ? rate : 48000.0) * 60.0 / bakedBpm;
            const double position = std::round((a.beat - spanStartBeat) * framesPerBeat);
            // one decision for every server: a jump, a loop wrap, or a synth
            // that has not been anchored yet
            bool hard = a.discontinuity || a.loopWrap;
            for(auto& [server, st] : serverStates)
                if(st.synth != nullptr && st.synth->isCreated() && st.fresh) hard = true;
            if(hard) anchorHardCounter = anchorHardCounter % (1 << 20) + 1;
            for(auto& [server, st] : serverStates){
                if(st.synth == nullptr) continue;
                if(!st.synth->isCreated()){ pending = true; continue; }
                st.fresh = false;
                scSynthControlBatch controls(st.synth, server);
                controls.set("tbpm", a.bpm);
                controls.set("run", a.playing ? 1.0f : 0.0f);
                controls.set("anchorPos", (float)std::max(-9.0e8, std::min(9.0e8, position)));
                controls.set("anchorHard", (float)anchorHardCounter);
                controls.set("anchorId", (float)anchorIdCounter);
                controls.send();
            }
            continue;
        }
        const float pos = (float)scTransportSync::positiveMod(a.beat, loopBeats);
        const bool inFirstCycle = a.beat > -1e-6 && a.beat < loopBeats - 1e-6;
        for(auto& [server, st] : serverStates){
            if(st.synth == nullptr) continue;
            // sets on a synth not confirmed yet would go out without the timetag
            if(!st.synth->isCreated()){ pending = true; continue; }
            int intro = 0;
            if((a.discontinuity && !a.loopWrap) || st.fresh) intro = inFirstCycle ? 1 : -1;
            st.fresh = false;
            scSynthControlBatch controls(st.synth, server);
            controls.set("tbpm", a.bpm);
            controls.set("run", a.playing ? 1.0f : 0.0f);
            controls.set("anchorPos", pos);
            controls.set("anchorIntro", (float)intro);
            controls.set("anchorId", (float)anchorIdCounter);
            controls.send();
        }
    }
    if(pending) follower.requestAnchor();
}

void scBake::applyLoop(){
    scNode* source = findSource();
    auto* from = source != nullptr ? sourceOutputParameter(*source) : nullptr;
    auto* to = ownOutputParameter();
    if(from == nullptr || to == nullptr){
        state = State::Failed;
        statusText = "The original (" + sourceName + ") is not in this canvas any more";
        return;
    }
    if(fileChannels < 1 || filePath.empty() || (synced && (introPath.empty() || loopFrames < 1)) || (span && loopFrames < 1)){
        state = State::Failed;
        statusText = "Nothing baked yet";
        return;
    }
    moveCables(*from, *to);
    state = State::Baked;
    statusText.clear();
}

bool scBake::replaceWithSampleBox(){
    auto* container = getHostContainer();
    auto* own = ownOutputParameter();
    auto* ownNode = container != nullptr && own != nullptr ? container->getNodeFromParameter(*own) : nullptr;
    if(ownNode == nullptr || span || filePath.empty() || fileChannels < 1 || fileChannels > 2) return false;

    auto* created = container->createNodeFromName("SC SampleBox");
    auto* box = created != nullptr ? dynamic_cast<scSampleBox*>(&created->getNodeModel()) : nullptr;
    auto* boxOut = box != nullptr ? parameterNamed(*box, box->getOutputName(0)) : nullptr;
    if(boxOut == nullptr){
        if(created != nullptr) created->deleteSelf();
        statusText = "Could not create a SampleBox";
        return false;
    }
    created->getNodeGui().setPosition(ownNode->getNodeGui().getPosition());
    // As wide as the bake: a mono texture stays on one channel
    if(box->getParameterGroup().contains("N Chan")) box->getParameterGroup().getInt("N Chan") = fileChannels;
    // Synced: the pad follows the transport (Fit = the loop's beats, Sync,
    // Play With Transport); it plays the loop, without the intro
    box->loadLoopPad(filePath, level.get(), synced ? (float)loopBeats : 0.0f);
    moveCables(*own, *boxOut);
    ownNode->deleteSelf();
    return true;
}

bool scBake::unbake(){
    scNode* source = findSource();
    auto* from = ownOutputParameter();
    auto* to = source != nullptr ? sourceOutputParameter(*source) : nullptr;
    if(from == nullptr || to == nullptr){
        statusText = "The original (" + sourceName + ") is not in this canvas any more";
        state = State::Failed;
        return false;
    }
    moveCables(*from, *to);
    state = State::Unbaked;
    statusText = "Unbaked: the original plays again.";
    return true;
}

void scBake::rebake(){
    if(state == State::Rendering || state == State::Looping) return;
    if(state == State::Baked && !unbake()) return;
    scNode* source = findSource();
    if(source == nullptr){
        state = State::Failed;
        statusText = "The original (" + sourceName + ") is not in this canvas any more";
        return;
    }
    int output = 0;
    for(int i = 0; i < source->getNumOutputs(); i++) if(source->getOutputName(i) == sourceOutput) output = i;
    startBake(*source, output, currentSettings());
}

scNode* scBake::findSource() const {
    auto* container = getHostContainer();
    if(container == nullptr || sourceName.empty()) return nullptr;
    for(auto* node : container->getAllModules()){
        if(node == nullptr) continue;
        auto* model = dynamic_cast<scNode*>(&node->getNodeModel());
        if(model != nullptr && model != this && model->getParameterGroup().getName() == sourceName) return model;
    }
    return nullptr;
}

ofxOceanodeAbstractParameter* scBake::sourceOutputParameter(scNode& source) const {
    return parameterNamed(source, sourceOutput);
}

ofxOceanodeAbstractParameter* scBake::ownOutputParameter(){
    return parameterNamed(*this, getOutputName(0));
}

void scBake::moveCables(ofxOceanodeAbstractParameter& from, ofxOceanodeAbstractParameter& to){
    auto* container = getHostContainer();
    if(container == nullptr) return;
    struct Sink { ofxOceanodeAbstractParameter* parameter; bool persistent; };
    std::vector<Sink> sinks;
    const auto connections = from.getOutConnections();
    for(auto* connection : connections) sinks.push_back({&connection->getSinkParameter(), connection->getIsPersistent()});
    for(auto* connection : connections) connection->deleteSelf();
    for(const auto& sink : sinks){
        if(auto* connection = container->createConnection(to, *sink.parameter))
            connection->setIsPersistent(sink.persistent);
    }
}

// ── Preset ───────────────────────────────────────────────────────────────────

void scBake::presetSave(ofJson& json){
    json["bakeFile"] = filePath;
    json["bakeChannels"] = fileChannels;
    json["bakeSource"] = sourceName;
    json["bakeSourceOutput"] = sourceOutput;
    json["bakeInfo"] = loopInfo;
    json["bakeState"] = state == State::Baked ? "baked" : state == State::Unbaked ? "unbaked" : "none";
    json["bakeMode"] = span ? "span" : synced ? "synced" : "texture";
    if(synced){
        json["bakeIntro"] = introPath;
        json["bakeBeats"] = loopBeats;
    }
    if(span) json["bakeSpanStart"] = spanStartBeat;
    json["bakeRate"] = fileRate;
    if(followsTransport()){
        json["bakeFrames"] = loopFrames;
        json["bakeBpm"] = bakedBpm;
    }
}

void scBake::macroSave(ofJson& json, std::string path){
    // The file goes with the preset, like SampleBox's samples
    const std::string root = scSampleEmbedding::saveRoot(path);
    if(json.contains("bakeFile")) scSampleEmbedding::embedInto(json["bakeFile"], root);
    if(json.contains("bakeIntro")) scSampleEmbedding::embedInto(json["bakeIntro"], root);
}

void scBake::presetRecallAfterSettingParameters(ofJson& json){
    // Only a full save of this node says what it plays; other recalls (a
    // parameter snapshot) leave the bake alone
    if(!json.contains("bakeFile")) return;
    filePath = scSampleEmbedding::resolve(json.value("bakeFile", std::string()));
    fileChannels = json.value("bakeChannels", 0);
    sourceName = json.value("bakeSource", std::string());
    sourceOutput = json.value("bakeSourceOutput", std::string());
    loopInfo = json.value("bakeInfo", std::string());
    const std::string mode = json.value("bakeMode", std::string());
    synced = mode == "synced";
    span = mode == "span";
    introPath = synced ? scSampleEmbedding::resolve(json.value("bakeIntro", std::string())) : std::string();
    loopBeats = synced ? json.value("bakeBeats", 0.0) : 0.0;
    spanStartBeat = span ? json.value("bakeSpanStart", 0.0) : 0.0;
    fileRate = json.value("bakeRate", 0);
    loopFrames = followsTransport() ? json.value("bakeFrames", 0) : 0;
    bakedBpm = followsTransport() ? json.value("bakeBpm", 0.0f) : 0.0f;
    const std::string saved = json.value("bakeState", std::string());
    statusText.clear();
    if(filePath.empty()) state = State::Empty;
    else if(!ofFile::doesFileExist(filePath) || (synced && !ofFile::doesFileExist(introPath))){
        state = State::Failed;
        statusText = "Baked file missing: " + filePath;
    }else state = saved == "unbaked" ? State::Unbaked : State::Baked;
    if(state == State::Unbaked) statusText = "Unbaked: the original plays.";
    // Cables are the preset's; the graph rebuild loads the file per server
}

// ── scNode ───────────────────────────────────────────────────────────────────

void scBake::releaseServerState(ofxSCServer* server){
    auto entry = serverStates.find(server);
    if(entry == serverStates.end()) return;
    if(entry->second.synth != nullptr){ entry->second.synth->free(); delete entry->second.synth; entry->second.synth = nullptr; }
}

void scBake::buildSynth(ofxSCServer* server){
    if(server == nullptr) return;
    releaseServerState(server);
    auto& st = serverStates[server];
    if(filePath.empty() || fileChannels < 1 || fileChannels > 64) return;

    // One buffer per file and server; a restarted server's allocations are
    // gone (its generation moved on), so never free those.
    auto ensure = [server](ofxSCBuffer*& buffer, std::string& loaded, const std::string& path) -> bool {
        const bool stale = buffer != nullptr && buffer->generation != server->getAllocationGeneration();
        if(buffer != nullptr && (stale || loaded != path)){
            if(!stale) buffer->free();
            delete buffer;
            buffer = nullptr;
        }
        if(buffer == nullptr){
            buffer = new ofxSCBuffer(0, 0, server);
            if(buffer->index < 0){ delete buffer; buffer = nullptr; return false; }
            buffer->read(path);
            loaded = path;
        }
        return true;
    };
    if(!ensure(st.buffer, st.bufferPath, filePath)) return;
    if(span){
        if(loopFrames < 1) return;
        st.synth = new ofxSCSynth("BakeSpan" + ofToString(fileChannels), server);
        st.synth->set("buf", st.buffer->index);
        st.synth->set("frames", loopFrames);
        st.synth->set("bakedbpm", bakedBpm);
    }else if(synced){
        if(introPath.empty() || loopFrames < 1 || loopBeats <= 0.0) return;
        if(!ensure(st.introBuffer, st.introBufferPath, introPath)) return;
        st.synth = new ofxSCSynth("BakeSync" + ofToString(fileChannels), server);
        st.synth->set("loopbuf", st.buffer->index);
        st.synth->set("introbuf", st.introBuffer->index);
        st.synth->set("frames", loopFrames);
        st.synth->set("loopbeats", loopBeats);
        st.synth->set("bakedbpm", bakedBpm);
    }else{
        st.synth = new ofxSCSynth("BakeLoop" + ofToString(fileChannels), server);
        st.synth->set("buf", st.buffer->index);
    }
    st.synth->set("levels", level.get());
    if(st.outputBus >= 0) st.synth->set("out", st.outputBus);
}

void scBake::createSynth(ofxSCServer* server){
    auto entry = serverStates.find(server);
    if(entry == serverStates.end() || entry->second.synth == nullptr) return;
    entry->second.synth->createAndRun(0, 1, runsOn(server));
    if(followsTransport()){
        // a new synth knows nothing of the transport: anchor it (its intro, its jump)
        entry->second.fresh = true;
        follower.requestAnchor();
    }
}

void scBake::free(ofxSCServer* server){
    releaseServerState(server);
    auto entry = serverStates.find(server);
    if(entry != serverStates.end()) entry->second.outputBus = -1;
}

void scBake::setOutputBus(ofxSCServer* server, int index, int bus){
    if(index != 0) return;
    auto& st = serverStates[server];
    st.outputBus = bus;
    if(st.synth != nullptr) st.synth->set("out", bus);
}

int scBake::getOutputBusIndex(ofxSCServer* server, int index){
    if(index != 0) return -1;
    auto entry = serverStates.find(server);
    return entry == serverStates.end() ? -1 : entry->second.outputBus;
}

void scBake::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto entry = serverStates.find(server);
    if(entry != serverStates.end() && entry->second.synth != nullptr) entry->second.synth->moveBefore(nodeID);
}

int scBake::getLastSynthID(ofxSCServer* server){
    auto entry = serverStates.find(server);
    return entry != serverStates.end() && entry->second.synth != nullptr ? entry->second.synth->nodeID : -1;
}

void scBake::applyGraphSuspended(ofxSCServer* server, bool){
    auto entry = serverStates.find(server);
    if(entry == serverStates.end() || entry->second.synth == nullptr) return;
    const bool running = runsOn(server);
    entry->second.synth->run(running);
    if(running && followsTransport()){
        // a paused synth missed the transport's moves: anchor it again
        entry->second.fresh = true;
        follower.requestAnchor();
    }
}
