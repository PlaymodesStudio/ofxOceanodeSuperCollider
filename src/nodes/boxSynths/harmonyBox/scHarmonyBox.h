//
//  scHarmonyBox.h
//  ofxOceanodeSuperCollider
//
//  HarmonyBox: a bank of chords that work together, played by Index. It does
//  not run in time itself: sequence it from outside (a counter, MetroBox, the
//  timeline) and connect Pitch to an instrument, Scale / Root to an
//  ArpeggiatorBox or a melody.
//
//  • Generate: a progression in a style (Functional, Jazz, Glass, Reich,
//    Neo-Riemann) and a key, shaped by Color (chromatic adventure),
//    Extensions (triads .. thirteenths), Tension Level and Shape. Locked chords
//    stay; New on a card draws only that one, to fit its neighbours.
//    Seed 0 draws fresh every time; Seed > 0 is one progression per number,
//    and Generate moves to the next number.
//  • Each chord is typed or edited as a symbol (Dm9, F/C, G7alt, D(2,4,5)),
//    reordered by dragging, voiced with the least movement from the chord
//    before (Voices, Low / High, Spread, Bass).
//  • AI: describe a progression ("Glass in A minor with a pedal") or ask for
//    a variation of the current one.
//  • Outputs for the chord at Index: Pitch (voiced notes), Bass, Root,
//    Scale (relative to Root: the notes to play over it), Tension (0..1).
//  • Engine and tests: scHarmonyBoxEngine.h, tools/harmonybox_test.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeAIAssistant.h"
#include "scHarmonyBoxEngine.h"
#include "../shared/scSnapshots.h"
#include "../../../ui/scDropdowns.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>
#include <string>
#include <vector>

class scHarmonyBox : public ofxOceanodeNodeModel {
public:
    scHarmonyBox() : ofxOceanodeNodeModel("HarmonyBox") {}
    ~scHarmonyBox() override { listeners.unsubscribeAll(); }

    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "HarmonyBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        using namespace harmonybox;
        description = "A bank of chords that work together, played by Index: Generate a progression in a style "
                      "(functional, jazz, Glass, Reich, neo-Riemannian) and key, lock the chords you like, type "
                      "your own, or ask the AI. Outputs the chord at Index voiced (Pitch, Bass), its Root, the Scale "
                      "to play over it and its Tension. Show opens the chord bank.";
        addParameter(showWindow.set("Show", false));
        addParameter(indexP.set("Index", 0, 0, 127));
        addParameter(generateP.set("Generate"));
        addParameterDropdown(styleP, "Style", STYLE_FUNCTIONAL, styleNames());
        addParameterDropdown(keyP, "Key", 0, keyNames());
        addParameterDropdown(modeP, "Mode", MODE_MAJOR, modeNames());
        addParameter(numChordsP.set("NumChords", 4, 1, MAX_CHORDS));
        addParameter(seedP.set("Seed", 1, 0, 99999));
        addParameter(transposeP.set("Transpose", 0, -24, 24));
        snaps.setupParameters();
        snapshotHandle = addParameter(snaps.snapshotP);   // recalls by number (1..): a dropdown of their names
        addOutputParameter(pitchOut.set("Pitch", {60.0f}, {0.0f}, {127.0f}));
        addOutputParameter(bassOut.set("Bass", 48.0f, 0.0f, 127.0f));
        addOutputParameter(rootOut.set("Root", 0.0f, 0.0f, 11.0f));
        addOutputParameter(scaleOut.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {0.0f}, {11.0f}));
        addOutputParameter(tensionOut.set("Tension", 0.0f, 0.0f, 1.0f));

        const Settings d;
        addInspectorParameter(colorP.set("Color", d.color, 0.0f, 1.0f));
        addInspectorParameter(extensionsP.set("Extensions", d.extensions, 0.0f, 1.0f));
        addInspectorParameter(tensionP.set("Tension Level", d.tension, 0.0f, 1.0f));
        addInspectorParameter(shapeP.set("Shape", d.shape, 0, NUM_SHAPES - 1));
        addInspectorParameter(voicesP.set("Voices", d.voices, 1, MAX_VOICES));
        addInspectorParameter(lowP.set("Low", d.low, 12, 96));
        addInspectorParameter(highP.set("High", d.high, 24, 120));
        addInspectorParameter(spreadP.set("Spread", d.spread, 0, NUM_SPREADS - 1));
        addInspectorParameter(bassP.set("Bass Voice", d.bass));
        addInspectorParameter(leadP.set("Voice Leading", d.voiceLeading));

        pushSettings();
        engine.generate((uint64_t)seedP.get());

        auto regenerate = [this](){ if(!quiet()) generateNow(false); };
        listeners.push(indexP.newListener([this](int&){ updateOutputs(); }));
        listeners.push(transposeP.newListener([this](int&){ updateOutputs(); }));
        listeners.push(generateP.newListener([this](){ generateNow(true); }));
        listeners.push(styleP.newListener([this, regenerate](int& v){ engine.s.style = v; regenerate(); }));
        listeners.push(modeP.newListener([this, regenerate](int& v){
            if(v == engine.s.mode) return;
            engine.s.mode = v;
            engine.changed();
            regenerate();
        }));
        listeners.push(keyP.newListener([this](int& v){
            if(syncing) return;
            engine.setKey(v);   // the chords move with the key
        }));
        listeners.push(numChordsP.newListener([this](int& v){
            if(quiet() || v == engine.size()) return;
            engine.setNumChords(v, drawSeed());
        }));
        listeners.push(seedP.newListener([this](int& v){
            if(quiet() || v <= 0) return;
            engine.generate((uint64_t)v);
        }));
        listeners.push(colorP.newListener([this](float& v){ engine.s.color = v; }));
        listeners.push(extensionsP.newListener([this](float& v){ engine.s.extensions = v; }));
        listeners.push(tensionP.newListener([this](float& v){ engine.s.tension = v; }));
        listeners.push(shapeP.newListener([this](int& v){ engine.s.shape = v; }));
        auto voicing = [this](){ if(syncing) return; pushSettings(); engine.changed(); };
        listeners.push(voicesP.newListener([voicing](int&){ voicing(); }));
        listeners.push(lowP.newListener([voicing](int&){ voicing(); }));
        listeners.push(highP.newListener([voicing](int&){ voicing(); }));
        listeners.push(spreadP.newListener([voicing](int&){ voicing(); }));
        listeners.push(bassP.newListener([voicing](bool&){ voicing(); }));
        listeners.push(leadP.newListener([voicing](bool&){ voicing(); }));

        {
            scSnapshots::Host h;
            h.folder = "HarmonyBox";
            h.title = "HarmonyBox";
            h.capture = [this](){ return ofJson{{"state", engine.toJson()}}; };
            // a chord bank does not blend: the state switches at the switch point
            h.apply = [this](const ofJson& a, const ofJson& b, float, bool sw){
                const ofJson& s = sw ? b : a;
                if(!s.contains("state") || s["state"] == appliedSnapshotState) return;
                appliedSnapshotState = s["state"];
                engine.fromJson(appliedSnapshotState);
                pullSettings();
            };
            h.bpm = [this](){ return std::max(1.0f, getFrameTransportState().current.bpm); };
            snaps.setup(h);
        }
        setupAI();
        updateOutputs();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        scDropdowns::setNames(snapshotHandle, snaps.slotNames());
        snaps.update();
        ai.update();
        if(engine.revision != shownRevision){
            // the bank changed (generate, edit, AI, snapshot): the node's controls follow it
            if(numChordsP.get() != engine.size() || keyP.get() != engine.s.key || modeP.get() != engine.s.mode) pullSettings();
            updateOutputs();
        }
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }

    // ════════════════════════════════════════════════════════════════════════
    // Presets
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        json["HarmonyBox"] = engine.toJson();
        json["HarmonyBoxSnapshots"] = snaps.settingsJson();
        ai.save(json);
    }
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("HarmonyBox") && json["HarmonyBox"].is_object()) engine.fromJson(json["HarmonyBox"]);
        if(json.contains("HarmonyBoxSnapshots")) snaps.loadSettings(json["HarmonyBoxSnapshots"]);
        ai.load(json);
        pullSettings();
    }

private:
    harmonybox::Engine engine;
    scSnapshots snaps;
    std::shared_ptr<ofxOceanodeAbstractParameter> snapshotHandle;
    ofJson appliedSnapshotState;
    ofxOceanodeAIAssistant ai;
    ofEventListeners listeners;
    uint64_t shownRevision = 0;
    bool syncing = false;

    ofParameter<bool> showWindow;
    ofParameter<int> indexP, styleP, keyP, modeP, numChordsP, seedP, transposeP;
    ofParameter<void> generateP;
    ofParameter<float> colorP, extensionsP, tensionP;
    ofParameter<int> shapeP, voicesP, lowP, highP, spreadP;
    ofParameter<bool> bassP, leadP;
    ofParameter<std::vector<float>> pitchOut, scaleOut;
    ofParameter<float> bassOut, rootOut, tensionOut;

    // window state
    int editingSlot = -1;
    char editBuffer[64] = {};
    bool focusEdit = false;
    std::string progressionBuffer, message;
    uint64_t progressionRevision = 0;

    bool quiet() const { return syncing || ofxOceanodeShared::isPresetLoading(); }

    static uint64_t freshSeed(){
        static std::random_device rd;
        return ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)ofGetElapsedTimeMicros();
    }
    // a seed for a draw the Seed does not name (dice, new slots): fresh, or
    // derived from Seed so a seeded patch stays reproducible
    uint64_t drawSeed(){
        static uint64_t count = 0;
        return seedP.get() > 0 ? (uint64_t)seedP.get() * 7919ull + (++count) : freshSeed();
    }

    // Generate: Seed 0 draws fresh, Seed > 0 moves to the next number
    // (button) or stays on it (a style or mode change)
    void generateNow(bool advance){
        if(seedP.get() <= 0){ engine.generate(freshSeed()); return; }
        if(advance){ seedP = seedP.get() >= seedP.getMax() ? 1 : seedP.get() + 1; return; }   // its listener generates
        engine.generate((uint64_t)seedP.get());
    }

    // parameters -> engine settings (not key / mode / chord count: those move the bank)
    void pushSettings(){
        auto& s = engine.s;
        s.style = styleP.get();
        s.key = keyP.get();
        s.mode = modeP.get();
        s.numChords = numChordsP.get();
        s.color = colorP.get();
        s.extensions = extensionsP.get();
        s.tension = tensionP.get();
        s.shape = shapeP.get();
        s.voices = voicesP.get();
        s.low = lowP.get();
        s.high = highP.get();
        s.spread = spreadP.get();
        s.bass = bassP.get();
        s.voiceLeading = leadP.get();
    }
    // engine -> parameters, without their listeners acting
    void pullSettings(){
        const auto s = engine.s;
        syncing = true;
        auto put = [](auto& p, auto v){ if(p.get() != v) p = v; };
        put(styleP, s.style);
        put(keyP, s.key);
        put(modeP, s.mode);
        put(numChordsP, engine.size());
        put(colorP, s.color);
        put(extensionsP, s.extensions);
        put(tensionP, s.tension);
        put(shapeP, s.shape);
        put(voicesP, s.voices);
        put(lowP, s.low);
        put(highP, s.high);
        put(spreadP, s.spread);
        put(bassP, s.bass);
        put(leadP, s.voiceLeading);
        syncing = false;
        updateOutputs();
    }

    int currentSlot() const { return engine.wrap(indexP.get()); }

    void updateOutputs(){
        shownRevision = engine.revision;
        if(engine.size() == 0) return;
        const int i = currentSlot();
        const int t = transposeP.get();
        auto pitch = engine.pitch(i);
        for(auto& v : pitch) v += (float)t;
        if(pitch != pitchOut.get()) pitchOut = pitch;
        const float bass = engine.bass(i) + (float)t;
        if(bass != bassOut.get()) bassOut = bass;
        const float root = (float)harmonybox::pc((int)engine.root(i) + t);
        if(root != rootOut.get()) rootOut = root;
        const auto scale = engine.scale(i);
        if(scale != scaleOut.get()) scaleOut = scale;
        const float tension = engine.tension(i);
        if(tension != tensionOut.get()) tensionOut = tension;
    }

    // ════════════════════════════════════════════════════════════════════════
    // AI
    // ════════════════════════════════════════════════════════════════════════

    static std::string aiSystemPrompt(){
        return
R"SYS(You are a composer and harmony teacher writing chord progressions for HarmonyBox, a chord bank in a live music patcher. The chords are played by index, usually in a loop, so the last chord should lead back to the first.

You know functional harmony (cadences, secondary dominants, modal mixture, voice leading), jazz harmony (ii-V-I chains, tritone substitutions, backdoor progressions, Coltrane changes, extensions and alterations, modal and quartal harmony), minimalism (Philip Glass: short repeated cells such as i-VI or i-bVII-bVI-V, pedal tones, one note changing at a time, additive variation; Steve Reich: static modal fields, sus chords and clusters of seconds and fourths over a slowly moving bass, as in Music for 18 Musicians), neo-Riemannian transformations (P, L, R, slides, hexatonic poles, film-score chromatic mediants), modal harmony, pop and ambient.

Answer ONLY with:
Key: <note> <mode>
<chord> <chord> <chord> ...

- 1 to 16 chords on one line, separated by spaces. No prose, no numbering, no markdown, no bar lines.
- <mode> is one of: Major, Dorian, Phrygian, Lydian, Mixolydian, Minor, Locrian, Harmonic Minor, Melodic Minor.
- Chord symbols: root (C, F#, Bb...) then a quality from: (major), m, dim, aug, sus4, sus2, 5, maj7, m7, 7, 6, m6, m7b5, dim7, mMaj7, 7sus4, maj7sus2, sus24, 7#5, maj7#5, 7b5, add9, madd9, add11, madd11, maj9, m9, 9, 6/9, m6/9, 7b9, 7#9, 9sus4, m9b5, m7b9, mMaj9, 7#11, maj7#11, 7b13, m11, 11, maj9#11, 9#11, 13, maj13, m13, 13b9, 7b9b13, 7alt, q (stacked fourths, 3 notes), q4 (4 fourths).
- A slash bass is allowed: F/C, Am7/G. Any other set of notes can be written as intervals over the root in parentheses: D(2,4,5), E(b2,5,b7).
- If the request names a number of chords, give exactly that many.)SYS";
    }

    void setupAI(){
        ofxOceanodeAIAssistant::Spec spec;
        spec.title = "Chord progression with Requesty";
        spec.subtitle = [this](){
            return "Now: " + harmonybox::keyNames()[engine.s.key] + " " + harmonybox::modeNames()[engine.s.mode] + " - "
                 + engine.progressionText();
        };
        spec.generateHint = "Describe a progression: a style, a mood, a reference, a key, how many chords "
                            "(\"8 chords like Glass's Glassworks in F minor, with a pedal\").";
        spec.fixHint = "How to change the current progression (\"more jazz substitutions\", \"darker\", "
                       "\"keep chords 1 and 5\"), or leave empty for a variation.";
        spec.generateButton = "Write";
        spec.fixButton = "Vary";
        spec.generateModeLabel = "New";
        spec.fixModeLabel = "Vary current";
        spec.pendingHint = "The answer replaces the chord bank when ready.";
        spec.readyMessage = "AI progression ready";
        spec.systemPrompt = &scHarmonyBox::aiSystemPrompt;
        spec.buildPrompt = [this](const std::string& request, bool vary){
            using namespace harmonybox;
            std::string p = "Current setting: key " + keyNames()[engine.s.key] + " " + modeNames()[engine.s.mode] +
                            ", style " + styleNames()[engine.s.style] + ", " + std::to_string(engine.size()) + " chords.\n";
            p += "Current progression: " + engine.progressionText() + "\n";
            std::string locked;
            for(int i = 0; i < engine.size(); i++) if(engine.slots[i].locked) locked += " " + std::to_string(i + 1) + " (" + engine.name(i) + ")";
            if(!locked.empty()) p += "Locked chords, keep them at their positions:" + locked + "\n";
            if(vary) p += "\nRewrite the current progression. " + (request.empty() ? std::string("Make a fresh variation in the same spirit.") : request);
            else p += "\nWrite a new progression (use the current key and length unless asked otherwise). " + request;
            return p;
        };
        spec.apply = [this](const std::string& text) -> std::string {
            int key = -1, mode = -1;
            const auto chords = harmonybox::parseProgression(text, &key, &mode);
            if(chords.empty()) return "No chord symbols found in the answer";
            if(key >= 0) engine.s.key = key;      // the chords are absolute: no transposition
            if(mode >= 0) engine.s.mode = mode;
            engine.setChords(chords);
            pullSettings();
            return "";
        };
        ai.setup(spec);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    static std::string noteName(int midi){ return harmonybox::pcName(midi, false) + std::to_string(midi / 12 - 1); }

    template<typename T>
    static bool combo(const char* label, ofParameter<T>& p, const std::vector<std::string>& names, float w){
        int v = (int)p.get();
        ImGui::SetNextItemWidth(w);
        bool changed = false;
        if(ImGui::BeginCombo(label, v >= 0 && v < (int)names.size() ? names[v].c_str() : "?")){
            for(int i = 0; i < (int)names.size(); i++){
                if(ImGui::Selectable(names[i].c_str(), i == v)){ p = (T)i; changed = true; }
                if(i == v) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    static void slider(const char* label, ofParameter<float>& p, float w, const char* tip){
        float v = p.get();
        ImGui::SetNextItemWidth(w);
        if(ImGui::SliderFloat(label, &v, p.getMin(), p.getMax(), "%.2f")) p = v;
        if(ImGui::IsItemHovered() && tip) ImGui::SetTooltip("%s", tip);
    }
    static void sliderInt(const char* label, ofParameter<int>& p, float w, const char* fmt = "%d"){
        int v = p.get();
        ImGui::SetNextItemWidth(w);
        if(ImGui::SliderInt(label, &v, p.getMin(), p.getMax(), fmt)) p = v;
    }

    void drawWindow(){
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1000.0f, 620.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(getWindowId().c_str(), &open)){
            ImGui::PushID(this);
            drawHeader();
            ImGui::Separator();
            drawCards();
            ImGui::Separator();
            drawVoicing();
            drawProgressionText();
            ImGui::Spacing();
            snaps.drawBar(ImGui::GetContentRegionAvail().x);
            ai.drawPopup();
            ImGui::PopID();
        }
        ImGui::End();
        if(!open) showWindow = false;
    }

    void drawHeader(){
        using namespace harmonybox;
        if(ImGui::Button("Generate")) generateNow(true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("New chords for every unlocked card.\nSeed 0: fresh each time; Seed > 0: the next seed number.");
        ImGui::SameLine();
        ai.drawButton(ImVec2(34, 0));
        ImGui::SameLine();
        combo("Style", styleP, styleNames(), 120);
        ImGui::SameLine();
        combo("Key", keyP, keyNames(), 56);
        ImGui::SameLine();
        combo("Mode", modeP, modeNames(), 130);
        ImGui::SameLine();
        sliderInt("Chords", numChordsP, 90);
        ImGui::SameLine();
        {
            int v = seedP.get();
            ImGui::SetNextItemWidth(80);
            if(ImGui::DragInt("Seed", &v, 0.2f, 0, seedP.getMax())) seedP = std::clamp(v, 0, seedP.getMax());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: fresh draws. > 0: one progression per number (with these settings).");
        }
        ImGui::SameLine();
        sliderInt("Transpose", transposeP, 80);

        slider("Color", colorP, 120, "How far from the key: secondary dominants, borrowed chords,\nsubstitutions, chromatic moves (applies on Generate).");
        ImGui::SameLine();
        slider("Extensions", extensionsP, 120, "Triads .. sevenths .. ninths .. thirteenths (applies on Generate).");
        ImGui::SameLine();
        slider("Tension Level", tensionP, 120, "The average tension the progression aims for (applies on Generate).");
        ImGui::SameLine();
        combo("Shape", shapeP, shapeNames(), 80);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("How tension moves across the bank: Free, Flat, Rise, Fall, Arc, Wave.");
        if(!ai.getStatus().empty()){ ImGui::SameLine(); ImGui::TextDisabled("%s", ai.getStatus().c_str()); }
    }

    // The bank: one card per chord, wrapping
    void drawCards(){
        using namespace harmonybox;
        const int n = engine.size();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = 8.0f;
        const int cols = std::clamp((int)((avail + gap) / (150.0f + gap)), 1, std::min(n, 8));
        const float w = std::floor((avail - gap * (cols - 1)) / cols);
        const float h = 150.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int rows = (n + cols - 1) / cols;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int current = currentSlot();
        const bool indexDriven = getOceanodeParameter(indexP).hasInConnection();
        const ImU32 cardBg = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        int moveFrom = -1, moveTo = -1;
        for(int i = 0; i < n; i++){
            ImGui::PushID(i);
            const ImVec2 p0(origin.x + (i % cols) * (w + gap), origin.y + (i / cols) * (h + gap));
            const ImVec2 p1(p0.x + w, p0.y + h);
            const bool locked = engine.slots[i].locked;
            dl->AddRectFilled(p0, p1, cardBg, 6.0f);
            if(i == current) dl->AddRect(p0, p1, accent, 6.0f, 0, 2.5f);
            else if(locked) dl->AddRect(p0, p1, dimCol, 6.0f, 0, 1.0f);

            // the card itself: click plays it (Index without a cable), double-click edits, drag reorders
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("card", ImVec2(w, h - 26.0f));
            if(ImGui::IsItemClicked() && !indexDriven) indexP = i;
            if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) startEdit(i);
            if(ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0))
                ImGui::SetTooltip("%s\nclick: play (Index %d)  double-click: type a chord  drag: move\nright-click: more", engine.name(i).c_str(), i);
            if(ImGui::BeginDragDropSource()){
                ImGui::SetDragDropPayload("HB_SLOT", &i, sizeof(int));
                ImGui::Text("%s", engine.name(i).c_str());
                ImGui::EndDragDropSource();
            }
            if(ImGui::BeginDragDropTarget()){
                if(const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HB_SLOT")){ moveFrom = *(const int*)pl->Data; moveTo = i; }
                ImGui::EndDragDropTarget();
            }
            if(ImGui::BeginPopupContextItem("cardMenu")){
                if(ImGui::MenuItem("Type a chord...")) startEdit(i);
                if(ImGui::MenuItem(locked ? "Unlock" : "Lock")) engine.slots[i].locked = !locked;
                if(ImGui::MenuItem("New chord here")) engine.regenerateSlot(i, drawSeed());
                if(ImGui::MenuItem("Duplicate", nullptr, false, n < MAX_CHORDS)) engine.duplicate(i);
                if(ImGui::MenuItem("Delete", nullptr, false, n > 1)) engine.erase(i);
                ImGui::EndPopup();
            }

            // index, numeral
            dl->AddText(ImVec2(p0.x + 6, p0.y + 4), dimCol, std::to_string(i).c_str());
            const std::string rn = engine.romanAt(i);
            const ImVec2 rs = ImGui::CalcTextSize(rn.c_str());
            dl->AddText(ImVec2(p1.x - rs.x - 6, p0.y + 4), dimCol, rn.c_str());

            // the name, large (or its editor)
            if(editingSlot == i){
                ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 26));
                ImGui::SetNextItemWidth(w - 12);
                if(focusEdit){ ImGui::SetKeyboardFocusHere(); focusEdit = false; }
                const bool done = ImGui::InputText("##edit", editBuffer, sizeof(editBuffer),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if(done){
                    if(engine.setChordText(i, editBuffer)){ message.clear(); editingSlot = -1; }
                    else message = std::string("Not a chord symbol: ") + editBuffer;
                }else if(!ImGui::IsItemActive() && !ImGui::IsItemFocused()) editingSlot = -1;
            }else{
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize() * 1.7f;
                const std::string name = engine.name(i);
                const ImVec2 ns = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str());
                const float x = p0.x + std::max(6.0f, (w - ns.x) * 0.5f);
                dl->PushClipRect(p0, p1, true);
                dl->AddText(font, size, ImVec2(x, p0.y + 24), textCol, name.c_str());
                dl->PopClipRect();
            }

            // tension bar
            {
                const float t = engine.tension(i);
                const float y = p0.y + 62;
                dl->AddRectFilled(ImVec2(p0.x + 8, y), ImVec2(p1.x - 8, y + 4), ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 2.0f);
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f + 0.6f * t, 0.75f - 0.4f * t, 0.7f - 0.5f * t, 1.0f));
                dl->AddRectFilled(ImVec2(p0.x + 8, y), ImVec2(p0.x + 8 + (w - 16) * t, y + 4), col, 2.0f);
            }

            // the voicing on a small keyboard
            drawMiniKeys(dl, ImVec2(p0.x + 8, p0.y + 72), ImVec2(w - 16, 44), i);

            // lock / new
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p1.y - 24));
            if(locked) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if(ImGui::SmallButton(locked ? "Locked" : "Lock")) engine.slots[i].locked = !locked;
            if(locked) ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A locked chord stays when you Generate.");
            ImGui::SameLine();
            if(ImGui::SmallButton("New")) engine.regenerateSlot(i, drawSeed());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Another chord here, chosen to fit its neighbours.");
            ImGui::PopID();
        }
        if(moveFrom >= 0 && moveTo >= 0) engine.move(moveFrom, moveTo);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (h + gap)));
        ImGui::Dummy(ImVec2(avail, 0.0f));
        if(!message.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", message.c_str());
    }

    void startEdit(int i){
        editingSlot = i;
        focusEdit = true;
        std::snprintf(editBuffer, sizeof(editBuffer), "%s", engine.name(i).c_str());
    }

    void drawMiniKeys(ImDrawList* dl, ImVec2 p, ImVec2 size, int slot){
        static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        const auto pitch = engine.pitch(slot);
        const int bassNote = (int)engine.bass(slot);
        int lo = engine.s.low, hi = engine.s.high;
        for(float v : pitch){ lo = std::min(lo, (int)v); hi = std::max(hi, (int)v); }
        lo -= harmonybox::pc(lo);
        hi += 11 - harmonybox::pc(hi);
        int whites = 0;
        for(int m = lo; m <= hi; m++) if(!black[harmonybox::pc(m)]) whites++;
        const float kw = size.x / std::max(1, whites);
        auto on = [&](int m){ for(float v : pitch) if((int)std::lround(v) == m) return true; return false; };
        const ImU32 white = IM_COL32(225, 225, 225, 255), blackCol = IM_COL32(40, 40, 40, 255);
        const ImU32 noteCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram), bassCol = IM_COL32(90, 170, 240, 255);
        int wi = 0;
        for(int m = lo; m <= hi; m++){
            if(black[harmonybox::pc(m)]) continue;
            const ImVec2 a(p.x + wi * kw, p.y), b(p.x + (wi + 1) * kw - 1, p.y + size.y);
            dl->AddRectFilled(a, b, on(m) ? (m == bassNote && engine.s.bass ? bassCol : noteCol) : white);
            wi++;
        }
        wi = 0;
        for(int m = lo; m <= hi; m++){
            if(black[harmonybox::pc(m)]){
                const float x = p.x + wi * kw - kw * 0.32f;
                dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + kw * 0.64f, p.y + size.y * 0.6f),
                                  on(m) ? (m == bassNote && engine.s.bass ? bassCol : noteCol) : blackCol);
            }else wi++;
        }
    }

    void drawVoicing(){
        using namespace harmonybox;
        ImGui::TextDisabled("Voicing");
        ImGui::SameLine();
        sliderInt("Voices", voicesP, 80);
        ImGui::SameLine();
        sliderInt("##low", lowP, 90, noteName(lowP.get()).c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Lowest note (the bass lives in its first octave)");
        ImGui::SameLine();
        sliderInt("Range##high", highP, 90, noteName(highP.get()).c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Highest note");
        ImGui::SameLine();
        combo("Spread", spreadP, spreadNames(), 90);
        ImGui::SameLine();
        bool b = bassP.get();
        if(ImGui::Checkbox("Bass", &b)) bassP = b;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("A separate bass voice (the root or slash note) under the chord.");
        ImGui::SameLine();
        bool l = leadP.get();
        if(ImGui::Checkbox("Voice leading", &l)) leadP = l;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each chord moves as little as it can from the one before (around the loop).");
    }

    // The bank as text: copy it, paste one, type one
    void drawProgressionText(){
        if(progressionRevision != engine.revision && !ImGui::IsAnyItemActive()){
            progressionBuffer = engine.progressionText();
            progressionRevision = engine.revision;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", progressionBuffer.c_str());
        ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 120.0f));
        if(ImGui::InputText("##progression", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)){
            progressionBuffer = buf;
            applyProgressionText();
        }else progressionBuffer = buf;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The whole bank as text: edit and press Enter, or paste chords\n(\"Am7 Fmaj7 | Dm9 E7b9\", an optional \"Key: A minor\" line).");
        ImGui::SameLine();
        if(ImGui::Button("Apply text")) applyProgressionText();
    }
    void applyProgressionText(){
        int key = -1, mode = -1;
        const auto chords = harmonybox::parseProgression(progressionBuffer, &key, &mode);
        if(chords.empty()){ message = "No chord symbols in the text"; return; }
        message.clear();
        if(key >= 0) engine.s.key = key;
        if(mode >= 0) engine.s.mode = mode;
        engine.setChords(chords);
        pullSettings();
    }
};
