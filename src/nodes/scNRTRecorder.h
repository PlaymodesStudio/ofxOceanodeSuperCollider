//
//  scNRTRecorder.h
//  ofxOceanodeSuperCollider
//

#ifndef scNRTRecorder_h
#define scNRTRecorder_h

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TIMELINE

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Renders the SuperCollider side of the patch offline, frame by frame, so a
// render can take as long as it needs without the audio drifting out of step
// with the frames.
//
// Arm first and wait for the light to go green: arming rebuilds the graph for
// offline rendering and waits for the server to answer, which takes a moment.
// Record then starts and stops the capture. Both are ordinary parameters with
// an inlet and an outlet, so Record can be driven by the Texture Recorder that
// also drives the frames, and both are kept out of presets so that recalling
// one never starts a render on its own.
//
// Source picks what is written: "Master" is the full mix, "All stems" every
// mixer input in the graph, "<mixer> (all)" that one mixer's inputs, and
// "<mixer> / <stem>" a single chain on its own. With Master adds the full mix
// alongside. The master is written to Filename exactly; stems append their
// names as <name>_<stem>.wav.
//
// Remove DC runs a LeakDC-style filter over each finished file, with its
// corner at 3.5 Hz so it takes out offset and drift without thinning the low
// end the way SuperCollider's own default coefficient would.
class scNRTRecorder : public ofxOceanodeNodeModel {
public:
    // The argument is deliberately not named `controller`: it would shadow the
    // member, and the lambdas below would then try to capture the argument
    // rather than reach the member through `this`, which does not compile.
    explicit scNRTRecorder(ofxOceanodeSuperColliderController* superColliderController)
    : ofxOceanodeNodeModel("SC NRT Recorder"), controller(superColliderController) {
        addSeparator("Capture");
        addParameter(arm.set("Arm", false), ofxOceanodeParameterFlags_DisableSavePreset);
        // The lamp below replaces this checkbox; the outlet stays patchable.
        addOutputParameter(ready.set("Ready", false), ofxOceanodeParameterFlags_NoGuiWidget);
        addCustomRegion(stateRegion.set("State", [this](){ drawState(); }),
                        [this](){ drawState(); });
        addParameter(record.set("Record", false), ofxOceanodeParameterFlags_DisableSavePreset);

        addSeparator("Render");
        addParameter(server.set("Server", 0, 0, 127));
        addParameter(channels.set("Channels", 2, 1, 128));
        addParameter(filename.set("Filename", "Supercollider/NRT/recording.wav"));
        // Seeded, not read from the graph. A node is constructed part-way
        // through a preset load, when the old patch has been freed and the new
        // one does not exist yet -- reading the graph there walks pointers to
        // nodes that are already gone. update() fills the real list on the
        // first frame, by which point the graph is whole again.
        sourceOptions = {"Master"};
        sourceParameter = addParameterDropdown(source, "Source", 0, sourceOptions);
        addParameter(withMaster.set("With Master", false));
        addParameter(removeDC.set("Remove DC", false));

        addSeparator("Output");
        addOutputParameter(finished.set("Finished"), ofxOceanodeParameterFlags_DisableSavePreset);
        addOutputParameter(status.set("Status", "Disarmed"));
        addInspectorParameter(parallel.set("Parallel Renders", 4, 1, 8));

        listeners.push(arm.newListener([this](bool& value){ onArmChanged(value); }));
        listeners.push(record.newListener([this](bool& value){ onRecordChanged(value); }));
        listeners.push(parallel.newListener([this](int& value){
            if(controller != nullptr) controller->setNRTMaxParallelRenders(value);
        }));
        if(controller != nullptr) controller->setNRTMaxParallelRenders(parallel.get());
    }

    ~scNRTRecorder() override {
        if(controller == nullptr) return;
        if(record.get()) controller->endNRTRecording(true);
        else if(controller->isNRTArmed() || controller->isNRTSettling()) controller->disarmNRTRecording();
    }

    void update(ofEventArgs&) override {
        if(controller == nullptr) return;
        refreshSourceOptions();

        const bool isReady = controller->isNRTArmed();
        if(ready.get() != isReady) ready = isReady;
        const std::string current = controller->getNRTStatus();
        if(status.get() != current) status = current;

        // endNRTRecording() starts the asynchronous scsynth render. Only fire
        // after that render (and optional DC cleanup) has fully completed, so
        // a downstream FFmpeg node never opens a WAV that is still growing.
        if(waitingForRender && !controller->isNRTRecordingActive() &&
           !controller->isNRTRendering()){
            if(current == "NRT render complete"){
                finished.trigger();
                waitingForRender = false;
            }else if(current.rfind("Rendering ", 0) != 0){
                // A non-rendering terminal status is a failure (or there was
                // nothing to render), so do not tell downstream nodes that a
                // usable WAV exists. If the worker only just stopped, the
                // controller still says "Rendering" until it joins it on the
                // next update; keep waiting through that one-frame handoff.
                waitingForRender = false;
            }
        }

        // The controller disarms itself when the patch changes underneath it,
        // so the button has to follow it back down.
        if(!isReady && !controller->isNRTSettling() && arm.get() && !record.get()) setArm(false);
    }

private:
    // --- layout ----------------------------------------------------------
    static constexpr float kPad = 3.0f;
    static constexpr float kLamp = 16.0f;
    static constexpr float kGap = 5.0f;
    static constexpr float kRound = 2.0f;
    static constexpr float kLooseWidth = 180.0f;
    static constexpr float kMinWidth = 40.0f;
    static constexpr float kMinHeight = 10.0f;
    // Fraction of the bar the indeterminate sweep covers, and how fast it goes.
    static constexpr float kSweepSpan = 0.25f;
    static constexpr float kSweepRate = 0.6f;

    static constexpr ImU32 kDisarmed = IM_COL32(190, 60, 60, 255);
    static constexpr ImU32 kArming = IM_COL32(225, 170, 55, 255);
    static constexpr ImU32 kReady = IM_COL32(60, 205, 110, 255);
    static constexpr ImU32 kCapturing = IM_COL32(70, 180, 235, 255);
    static constexpr ImU32 kRendering = IM_COL32(140, 130, 240, 255);
    static constexpr ImU32 kLampEdge = IM_COL32(15, 18, 22, 200);
    static constexpr ImU32 kTrough = IM_COL32(24, 30, 38, 255);
    static constexpr ImU32 kTroughEdge = IM_COL32(58, 72, 84, 255);
    static constexpr ImU32 kText = IM_COL32(225, 232, 238, 255);

    // --- state -----------------------------------------------------------
    // What the lamp shows, and how far along the bar is. A fraction below zero
    // means there is no honest number to show: either nothing is happening, or
    // the capture was started with no known end time.
    struct nrtState {
        ImU32 color = kDisarmed;
        std::string caption = "Disarmed";
        float fraction = -1.0f;
        bool busy = false;
    };

    nrtState currentState() const {
        nrtState state;
        if(controller == nullptr) return state;

        if(controller->isNRTSettling()){
            state.color = kArming;
            state.caption = "Arming";
        }else if(controller->isNRTRecordingActive()){
            state.color = kCapturing;
            state.caption = "Capturing";
            state.fraction = controller->getNRTCaptureProgress();
            state.busy = true;
        }else if(controller->isNRTRendering()){
            // Files finished, not "now on file N": the jobs run concurrently,
            // so there is no single current one. Starting at 0/5 says so.
            const int total = std::max(1, controller->getNRTRenderJobCount());
            const int done = std::min(total, controller->getNRTRenderJobsDone());
            state.color = kRendering;
            state.caption = "Rendering " + ofToString(done) + "/" + ofToString(total);
            state.fraction = controller->getNRTRenderProgress();
            state.busy = true;
        }else if(controller->isNRTArmed()){
            state.color = kReady;
            state.caption = "Ready";
        }
        return state;
    }

    // --- drawing ---------------------------------------------------------
    void drawState(){
        const float zoom = ofxOceanodeShared::getZoomLevel();
        const auto& ctx = ofxOceanodeShared::getCustomRegionRenderContext();

        const float pad = kPad * zoom;
        const float height = ctx.active ? std::max(kMinHeight * zoom, ctx.height - pad * 2.0f)
                                        : kLamp * zoom;
        const float width = ctx.active ? std::max(kMinWidth * zoom, ctx.width - pad * 2.0f)
                                       : kLooseWidth * zoom;
        const float lamp = std::min(height, kLamp * zoom);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 min(origin.x + pad, origin.y + pad);
        ImGui::InvisibleButton("##NRTState", ImVec2(width + pad * 2.0f, height + pad * 2.0f));

        const nrtState state = currentState();
        const ImVec2 lampMin(min.x, min.y + (height - lamp) * 0.5f);
        const ImVec2 lampMax(lampMin.x + lamp, lampMin.y + lamp);
        drawLamp(lampMin, lampMax, state.color, zoom);

        // The bar is the same height as the lamp and sits beside it.
        const ImVec2 barMin(lampMax.x + kGap * zoom, lampMin.y);
        const ImVec2 barMax(min.x + width, lampMax.y);
        if(barMax.x > barMin.x + 4.0f * zoom) drawBar(barMin, barMax, state, zoom);

        ImGui::Dummy(ImVec2(width + pad * 2.0f, ctx.active ? height + pad * 2.0f : 2.0f * zoom));
    }

    void drawLamp(const ImVec2& min, const ImVec2& max, ImU32 color, float zoom) const {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, color, kRound * zoom);
        drawList->AddRect(min, max, kLampEdge, kRound * zoom);
    }

    void drawBar(const ImVec2& min, const ImVec2& max, const nrtState& state, float zoom) const {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float span = max.x - min.x;
        drawList->AddRectFilled(min, max, kTrough, kRound * zoom);

        if(state.fraction >= 0.0f){
            const float filled = span * state.fraction;
            if(filled > 0.5f){
                drawList->AddRectFilled(min, ImVec2(min.x + filled, max.y), state.color, kRound * zoom);
            }
        }else if(state.busy){
            // No known duration: a sweep that says "working", not "halfway".
            const float sweep = span * kSweepSpan;
            const float travel = std::max(1.0f, span - sweep);
            const float phase = std::fmod((float)ofGetElapsedTimef() * kSweepRate, 2.0f);
            const float offset = travel * (phase < 1.0f ? phase : 2.0f - phase);
            drawList->AddRectFilled(ImVec2(min.x + offset, min.y),
                                    ImVec2(min.x + offset + sweep, max.y),
                                    state.color, kRound * zoom);
        }
        drawList->AddRect(min, max, kTroughEdge, kRound * zoom);

        std::string caption = state.caption;
        if(state.fraction >= 0.0f) caption += "  " + ofToString(state.fraction * 100.0f, 1) + "%";
        const ImVec2 textSize = ImGui::CalcTextSize(caption.c_str());
        if(textSize.x < span - 4.0f * zoom){
            drawList->AddText(ImVec2(min.x + 4.0f * zoom,
                                     min.y + (max.y - min.y - textSize.y) * 0.5f),
                              kText, caption.c_str());
        }
    }

    // --- sources ---------------------------------------------------------
    // The list comes from the live graph, so it follows repatching: "Master",
    // "All stems", one entry per mixer covering that mixer's inputs, then the
    // stems on their own.
    void refreshSourceOptions(){
        if(controller == nullptr) return;
        // Arming tears the graph down and rebuilds it, so the list churns --
        // briefly down to "Master" alone -- exactly while a choice is waiting
        // to be used. Refreshing through that would clamp the selection to
        // zero and silently render something the user never picked, so the
        // list is frozen from the moment the node is armed until it is idle
        // again.
        if(controller->isNRTSettling() || controller->isNRTArmed() ||
           controller->isNRTRecordingActive() || controller->isNRTRendering()) return;

        std::vector<std::string> next = controller->getNRTSourceNames(server.get());
        if(next == sourceOptions) return;

        // Keep the choice itself, not its position: a repatch can insert or
        // drop entries above it, which would otherwise silently slide the
        // selection onto a different stem.
        const std::string chosen = selectedSourceLabel();
        sourceOptions = std::move(next);

        int restored = 0;
        for(std::size_t i = 0; i < sourceOptions.size(); i++){
            if(sourceOptions[i] != chosen) continue;
            restored = (int)i;
            break;
        }
        const int last = std::max(0, (int)sourceOptions.size() - 1);
        source.set("Source", std::min(restored, last), 0, last);
        if(sourceParameter != nullptr) sourceParameter->setDropdownOptions(sourceOptions);
    }

    std::string selectedSourceLabel() const {
        const int index = source.get();
        if(index < 0 || index >= (int)sourceOptions.size()) return "";
        return sourceOptions[(std::size_t)index];
    }

    // --- control ---------------------------------------------------------
    // Moving a toggle from code would re-enter its own listener, so both
    // setters mute themselves for the duration of the write.
    void setArm(bool value){
        suppressArm = true;
        arm = value;
        suppressArm = false;
    }

    void setRecord(bool value){
        suppressRecord = true;
        record = value;
        suppressRecord = false;
    }

    void onArmChanged(bool value){
        if(suppressArm || controller == nullptr) return;
        if(!value){
            // Disarming mid-recording would pull the capture out from under it.
            if(!record.get()) controller->disarmNRTRecording();
            return;
        }
        if(!controller->armNRTRecording(server.get(), channels.get(), filename.get())) setArm(false);
    }

    void onRecordChanged(bool value){
        if(suppressRecord || controller == nullptr) return;
        if(!value){
            waitingForRender = controller->endNRTRecording(false);
            setArm(false);
            return;
        }
        // These only matter once the capture is done, but they are read then
        // from whatever the node last pushed, so push them before starting.
        controller->setNRTSource(selectedSourceLabel());
        controller->setNRTRecordStems(withMaster.get());
        controller->setNRTRemoveDC(removeDC.get());
        if(!controller->beginNRTRecording(server.get(), channels.get(), filename.get())) setRecord(false);
    }

    ofxOceanodeSuperColliderController* controller = nullptr;

    // In the order they appear on the node.
    ofParameter<bool> arm;
    ofParameter<bool> ready;
    customGuiRegion stateRegion;
    ofParameter<bool> record;
    ofParameter<int> server;
    ofParameter<int> channels;
    ofParameter<std::string> filename;
    ofParameter<int> source;
    ofParameter<bool> withMaster;
    ofParameter<bool> removeDC;
    ofParameter<void> finished;
    ofParameter<std::string> status;
    ofParameter<int> parallel;

    std::shared_ptr<ofxOceanodeParameter<int>> sourceParameter;
    std::vector<std::string> sourceOptions;

    ofEventListeners listeners;
    bool suppressArm = false;
    bool suppressRecord = false;
    bool waitingForRender = false;
};

#endif // OFXOCEANODESC_HAS_TIMELINE

#endif /* scNRTRecorder_h */
