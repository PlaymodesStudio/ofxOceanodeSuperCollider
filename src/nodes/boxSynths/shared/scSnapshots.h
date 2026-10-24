//
//  scSnapshots.h
//  ofxOceanodeSuperCollider
//
//  The synths' snapshots, in one place: 128 slots of a node's sound, a
//  library shared by every node of the kind (data/nodeSnapshots/<folder>/
//  snapshot_<k>.json + library.json; another node's changes are picked up),
//  banks (banks/<name>.json, Factory.json rewritten from the node's factory
//  list), Store / Recall / Glide (over Morph Time, beats or seconds) and an A
//  / B Morph, the bar that draws them and the Manage window (rename, delete,
//  reorder: scSnapshotManager). Legacy named preset file helpers remain for
//  compatibility; the UI uses snapshots and banks. Its parameters keep the
//  names the nodes had
//  (Snapshot, Morph, Morph A / B, Morph Time, Morph Beats, Morph Glide, Morph
//  Curve, Morph Switch), so presets and connections keep working.
//
//  A node gives what only it knows (Host): capture() its sound as json,
//  apply(a, b, t, switched) a blend of two snapshots (t after the curve,
//  switched: whether discrete switches and modes take b's), its factory list, and an
//  import of an older format. The node registers the parameters in its own
//  publish registry (they are plain ofParameters with their own listeners).
//
//  Usage:
//      snaps.setupParameters();              // where the node creates its parameters
//      reg(snaps.snapshotP, []{}); ...       // the node's publishing
//      snaps.setup(host);                    // after: loads the library
//      update(): snaps.update();   window: snaps.drawBar(w);  (a line under it)
//      captured keys: skip snaps.isOwnKey(key) (the snapshot controls themselves)
//
//  Header-only (as PulsarBox), so the projects need no new file.
//

#pragma once

#include "ofMain.h"
#include "imgui.h"
#include "scSnapshotManager.h"
#include "scMorphPolicies.h"
#include "ofxOceanodeShared.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class scSnapshots {
public:
    static constexpr int SLOTS = 128;

    scMorphPolicies morph;             // explicit audio policies; see docs/snapshotMorphing.md

    struct Host {
        std::function<bool(const std::string&)> morphBlocked;  // connected controls take precedence
        std::function<void()> morphChanged;                  // resend effective audio values after apply
        std::string folder;                                   // data/nodeSnapshots/<folder>
        std::string title;                                    // shown in window titles
        std::function<ofJson()> capture;                      // the current sound ("name" is added here)
        std::function<void(const ofJson& a, const ofJson& b, float t, bool switched)> apply;
        std::function<std::vector<ofJson>()> factory;         // optional, each with its "name"
        // optional: no library yet -- fill the slots from an older format
        // (appendSnapshot); true when something was taken over
        std::function<bool()> importLegacy;
        std::function<float()> bpm;                           // Morph Time in beats
        std::function<void(const std::string& key)> paramMenu;   // after a parameter's widget: its menu
        // data-relative folder that held the named presets before
        // nodeSnapshots/<folder>/Presets: its files are moved there on setup
        std::string legacyPresetFolder;
        std::string defaultBank;                              // banks/<file> loaded on the first run (no library yet)
    };

    ofParameter<int> snapshotP, morphAP, morphBP, morphCurveP, morphSwitchP;
    ofParameter<float> morphP, morphTimeP;
    ofParameter<bool> morphBeatsP, morphGlideP;

    std::array<ofJson, SLOTS> slots;
    std::array<bool, SLOTS> used{};
    int active = -1;
    scSnapshotManager manager;            // the slots in the Manage window
    std::vector<std::string> presetNames; // sorted
    std::string currentPreset;            // legacy named preset state

    // ── Setup ──────────────────────────────────────────────────────────────
    void setupParameters(float morphTime = 4.0f, bool beats = true) {
        snapshotP.set("Snapshot", 0, 0, SLOTS);            // 1..: recall that slot
        morphP.set("Morph", 0.0f, 0.0f, 1.0f);
        morphAP.set("Morph A", 0, 0, SLOTS);
        morphBP.set("Morph B", 0, 0, SLOTS);
        morphTimeP.set("Morph Time", morphTime, 0.0f, 64.0f);
        morphBeatsP.set("Morph Beats", beats);
        morphGlideP.set("Morph Glide", false);
        morphCurveP.set("Morph Curve", 1, 0, 3);           // Lin, Ease, Exp, Log
        morphSwitchP.set("Morph Switch", 1, 0, 2);         // switches jump at start / middle / end
        listeners.push(snapshotP.newListener([this](int& v) {
            // (a preset sets it back to the number it showed: its own values
            // are already the sound, nothing to recall)
            if(recalling || v <= 0 || ofxOceanodeShared::isPresetLoading()) return;
            if(morphGlideP.get()) glide(v - 1); else recall(v - 1);
        }));
        listeners.push(morphP.newListener([this](float&) { applyAB(); }));
        listeners.push(morphAP.newListener([this](int&) { applyAB(); }));
        listeners.push(morphBP.newListener([this](int&) { applyAB(); }));
    }
    void setup(Host h) {
        host = std::move(h);
        // Every entry point (recall, A/B, timed glide, banks and interrupted
        // glides) uses the same audio policies. UI parameters keep their types.
        auto capture = host.capture;
        if(capture) host.capture = [this, capture]() {
            auto j = capture(); morph.capture(j); return j;
        };
        auto apply = host.apply;
        if(apply) host.apply = [this, apply](const ofJson& a, const ofJson& b, float t, bool sw) {
            morph.begin(a, b, t, host.morphBlocked);
            try {
                apply(a, b, t, sw);
                morph.begin(a, b, t, host.morphBlocked); // newly created pads/controls
                if(host.morphChanged) host.morphChanged();
            } catch(...) { morph.end(); throw; }
            morph.end();
        };
        manager.bindSlots(slots, used, active, [this](int s) { recall(s); }, [this]() { saveLibrary(); },
                          [this](const std::array<int, SLOTS>& map) {   // A / B follow their snapshots
            for(auto* p : {&morphAP, &morphBP}) {
                const int v = p->get();
                if(v > 0 && v <= SLOTS) p->setWithoutEventNotifications(map[v - 1] >= 0 ? map[v - 1] + 1 : 0);
            }
        });
        loadLibrary();
        moveLegacyPresets();
        scanPresets();
    }
    // The snapshot controls themselves (never captured, never recalled)
    static bool isOwnKey(const std::string& key) {
        return key.rfind("Snapshot", 0) == 0 || key.rfind("Morph", 0) == 0;
    }

    // ── Store / recall / glide / morph ─────────────────────────────────────
    std::string name(int k) const {
        if(k < 0 || k >= SLOTS || !used[k] || !slots[k].is_object() || !slots[k].contains("name")) return {};
        try { return slots[k]["name"].get<std::string>(); } catch(...) { return {}; }
    }
    void store(int slot, const std::string& nm = "") {
        if(slot < 0 || slot >= SLOTS || !host.capture) return;
        const std::string keep = nm.empty() ? name(slot) : nm;
        slots[slot] = host.capture();
        slots[slot]["name"] = keep.empty() ? "Snapshot " + ofToString(slot + 1) : keep;
        used[slot] = true;
        active = slot;
        saveLibrary();
    }
    // a new slot (the first free one); -1 when full
    int storeNew(const std::string& nm = "") {
        for(int k = 0; k < SLOTS; k++) if(!used[k]) { store(k, nm); return k; }
        return -1;
    }
    void recall(int slot) {
        if(slot < 0 || slot >= SLOTS || !used[slot] || !host.apply) return;
        running = false;
        host.apply(slots[slot], slots[slot], 1.0f, true);
        setActive(slot);
    }
    void glide(int slot) {
        if(slot < 0 || slot >= SLOTS || !used[slot]) return;
        glideTo(slots[slot], slot);
    }
    // to any snapshot (a preset: slot -1), over Morph Time
    void glideTo(const ofJson& snapshot, int slot) {
        const float bpm = host.bpm ? std::max(1.0f, host.bpm()) : 120.0f;
        duration = morphBeatsP.get() ? morphTimeP.get() * 60.0f / bpm : morphTimeP.get();
        if(duration <= 0.001f || !host.capture) {
            running = false;
            if(host.apply) host.apply(snapshot, snapshot, 1.0f, true);
            setActive(slot);
            return;
        }
        from = host.capture();
        to = snapshot;
        target = slot;
        t0 = ofGetElapsedTimef();
        progress = 0.0f;
        running = true;
    }
    void applyAB() {
        const int a = morphAP.get() - 1, b = morphBP.get() - 1;
        if(a < 0 || b < 0 || a >= SLOTS || b >= SLOTS || !used[a] || !used[b] || !host.apply) return;
        running = false;   // the slider takes over from a glide
        const float t = curve(morphP.get());
        host.apply(slots[a], slots[b], t, switched(t));
    }
    bool gliding() const { return running; }
    float glideProgress() const { return progress; }
    float curve(float u) const {
        u = ofClamp(u, 0.0f, 1.0f);
        switch(morphCurveP.get()) {
            case 1:  return u * u * (3.0f - 2.0f * u);                       // Ease
            case 2:  return u * u * u;                                        // Exp: slow start
            case 3:  return 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);     // Log: fast start
            default: return u;                                                // Lin
        }
    }
    bool switched(float t) const {
        const int s = morphSwitchP.get();
        return s == 0 ? t > 0.0f : s == 1 ? t >= 0.5f : t >= 1.0f;
    }
    // shows / outputs the slot, without recalling it a second time
    void setActive(int slot) {
        active = slot;
        recalling = true;
        snapshotP.set(slot >= 0 ? slot + 1 : 0);
        recalling = false;
    }
    // An imported sound replaces a running glide through the same policy-aware
    // path as Recall, without consuming or overwriting a library slot.
    void applyExternal(const ofJson& state) {
        if(!state.is_object()) return;
        running = false;
        if(host.apply) host.apply(state,state,1.0f,true);
        setActive(-1);
    }
    // MacroMixer and other external controllers share the decorated sound
    // callbacks used by snapshots, including policy metadata and effective DSP
    // values. The caller owns progress/curve; a local glide must stop competing.
    ofJson captureForExternalMorph() {
        running = false;
        return host.capture ? host.capture() : ofJson::object();
    }
    void applyExternalMorph(const ofJson& a, const ofJson& b, float t, bool switched) {
        running = false;
        if(host.apply) host.apply(a,b,ofClamp(t,0.0f,1.0f),switched);
    }
    // One more snapshot in the first free slot (imports); false when full
    bool appendSnapshot(const ofJson& s) {
        if(!s.is_object()) return false;
        for(int k = 0; k < SLOTS; k++) if(!used[k]) { slots[k] = s; used[k] = true; return true; }
        return false;
    }
    bool empty() const { for(bool u : used) if(u) return false; return true; }
    // Snapshot / Morph A / Morph B published to the node are dropdowns:
    // their names by value ("-", then "k  name", "k" when empty)
    std::vector<std::string> slotNames() const {
        std::vector<std::string> out;
        out.reserve(SLOTS + 1);
        out.push_back("-");
        for(int k = 0; k < SLOTS; k++) out.push_back(used[k] ? ofToString(k + 1) + "  " + name(k) : ofToString(k + 1));
        return out;
    }
    std::vector<std::string> slotKeys() const {
        return {snapshotP.getEscapedName(), morphAP.getEscapedName(), morphBP.getEscapedName()};
    }
    // The recall / morph settings, for a node that saves them itself (one
    // whose publish registry does not hold these parameters)
    ofJson settingsJson() const {
        return {{"glide", morphGlideP.get()}, {"time", morphTimeP.get()}, {"beats", morphBeatsP.get()},
                {"curve", morphCurveP.get()}, {"switch", morphSwitchP.get()},
                {"a", morphAP.get()}, {"b", morphBP.get()}, {"morph", morphP.get()}};
    }
    void loadSettings(const ofJson& j) {
        if(!j.is_object()) return;
        morphGlideP.set(j.value("glide", morphGlideP.get()));
        morphTimeP.set(j.value("time", morphTimeP.get()));
        morphBeatsP.set(j.value("beats", morphBeatsP.get()));
        morphCurveP.set(j.value("curve", morphCurveP.get()));
        morphSwitchP.set(j.value("switch", morphSwitchP.get()));
        // A / B / Morph without applying them (the preset's values are the sound)
        morphAP.setWithoutEventNotifications(j.value("a", 0));
        morphBP.setWithoutEventNotifications(j.value("b", 0));
        morphP.setWithoutEventNotifications(j.value("morph", 0.0f));
    }

    // A value between two snapshots' values: numbers (and lists of numbers)
    // glide, in log when asked (frequencies, times); integers, strings,
    // switches take b's at the switch point
    static ofJson blendValue(const ofJson& x, const ofJson& y, float t, bool sw, bool logScale = false) {
        if(t <= 0.0f) return x;
        if(t >= 1.0f) return y;
        auto num = [&](float a, float b) {
            if(logScale && a > 0.0f && b > 0.0f) return std::exp(ofLerp(std::log(a), std::log(b), t));
            return ofLerp(a, b, t);
        };
        if(x.is_number_float() && y.is_number_float()) return ofJson(num(x.get<float>(), y.get<float>()));
        if(x.is_array() && y.is_array() && !x.empty() && !y.empty() && x[0].is_number_float() && y[0].is_number_float()) {
            ofJson r = ofJson::array();
            const size_t n = std::max(x.size(), y.size());
            for(size_t i = 0; i < n; i++)
                r.push_back(num(x[std::min(i, x.size() - 1)].get<float>(), y[std::min(i, y.size() - 1)].get<float>()));
            return r;
        }
        return sw ? y : x;
    }

    // Names of values that glide in log (frequencies, rates, times)
    static bool logKey(const std::string& k) {
        if(k.find("Curve") != std::string::npos) return false;
        for(const char* w : {"Hz", "Freq", "Formant", "Fundamental", "Lowpass", "Ratio", "Rate", "Delay", "Attack",
                             "Decay", "Release", "Time", "Cutoff"})
            if(k.find(w) != std::string::npos) return true;
        return false;
    }
    // The blend for nodes whose controls are a list of actions (key, save,
    // load): every key both snapshots hold, only what changes is set
    template<typename Actions, typename Skip>
    static void blendActions(const Actions& actions, const ofJson& a, const ofJson& b, float t, bool sw, Skip skip) {
        for(const auto& act : actions) {
            if(!act.load || !act.save || skip(act.key)) continue;
            const bool ha = a.contains(act.key), hb = b.contains(act.key);
            ofJson v;
            if(ha && hb) v = blendValue(a[act.key], b[act.key], t, sw, logKey(act.key));
            else if(hb && sw) v = b[act.key];
            else if(ha && !sw) v = a[act.key];
            else continue;
            ofJson cur;
            act.save(cur);
            if(!cur.contains(act.key) || cur[act.key] != v) act.load(v);
        }
    }

    // The same for nodes whose controls are {key, save() -> ofJson, load(json)}
    // (EchoBox, EuclideanBox...): a sound is every control but the skipped
    template<typename Controls, typename Skip>
    static ofJson captureControls(const Controls& controls, Skip skip) {
        ofJson j = ofJson::object();
        for(const auto& c : controls) if(c.save && !isOwnKey(c.key) && !skip(c.key)) j[c.key] = c.save();
        return j;
    }
    template<typename Controls, typename Skip>
    static void blendControls(const Controls& controls, const ofJson& a, const ofJson& b, float t, bool sw, Skip skip) {
        for(const auto& c : controls) {
            if(!c.load || !c.save || isOwnKey(c.key) || skip(c.key)) continue;
            const bool ha = a.contains(c.key), hb = b.contains(c.key);
            ofJson v;
            if(ha && hb) v = blendValue(a[c.key], b[c.key], t, sw, logKey(c.key));
            else if(hb && sw) v = b[c.key];
            else if(ha && !sw) v = a[c.key];
            else continue;
            if(c.save() != v) c.load(v);
        }
    }
    // A parameter's name as its key (ofAbstractParameter::escape)
    static std::string escapeKey(std::string s) {
        for(const char* ch : {" ", "<", ">", "{", "}", "[", "]", ",", "(", ")", "/", "\\", ".", "#"}) ofStringReplace(s, ch, "_");
        return s;
    }
    // Factory snapshots from the node's defaults: each {"name", "set": {"Parameter
    // Name": value}} over a copy of base (under `sub` when the sound keeps its
    // controls there)
    static std::vector<ofJson> presetsFrom(const ofJson& base, const char* json, const std::string& sub = "") {
        std::vector<ofJson> out;
        try {
            for(const auto& p : ofJson::parse(json)) {
                ofJson j = base;
                ofJson& target = sub.empty() ? j : j[sub];
                if(p.contains("set")) for(auto it = p["set"].begin(); it != p["set"].end(); ++it) target[escapeKey(it.key())] = it.value();
                j["name"] = p.value("name", std::string("Init"));
                out.push_back(j);
            }
        } catch(const std::exception& e) { ofLogError("scSnapshots") << "factory: " << e.what(); }
        return out;
    }

    // ── Per frame ──────────────────────────────────────────────────────────
    void update() {
        if(loadDialog) {
            loadDialog = false;
            ofFileDialogResult r = ofSystemLoadDialog("Snapshots from a bank file", false, (dir() / "banks").string());
            if(r.bSuccess) loadBank(r.filePath);
        }
        const float now = ofGetElapsedTimef();
        if(now >= libCheckAt) {   // another node changed the shared library
            libCheckAt = now + 1.0f;
            if(setTime() != libTime) loadLibrary();
        }
        if(running) {
            const float u = (now - t0) / std::max(0.001f, duration);
            progress = ofClamp(u, 0.0f, 1.0f);
            const float t = progress >= 1.0f ? 1.0f : curve(progress);
            if(host.apply) host.apply(from, to, t, switched(t));
            if(progress >= 1.0f) { running = false; setActive(target); }
        }
    }

    // ── Library and banks ──────────────────────────────────────────────────
    std::filesystem::path dir() const {
        const std::filesystem::path d = ofToDataPath("nodeSnapshots/" + host.folder, true);
        try { std::filesystem::create_directories(d / "banks"); } catch(...) {}
        return d;
    }
    void saveLibrary() {
        const auto d = dir();
        for(int k = 0; k < SLOTS; k++) {
            const auto f = d / ("snapshot_" + ofToString(k) + ".json");
            try {
                if(used[k]) ofSavePrettyJson(f.string(), slots[k]);
                else if(std::filesystem::exists(f)) std::filesystem::remove(f);
            } catch(...) {}
        }
        // the marker: the library exists (empty slots stay empty, no refill)
        try { ofSaveJson((d / "library.json").string(), ofJson{{"snapshots", 1}, {"bank", currentBank}}); } catch(...) {}
        if(!currentBank.empty())saveBank((d/"banks"/currentBank).string());
        libTime = setTime();
    }
    void loadLibrary() {
        const auto d = dir();
        currentBank.clear();
        try {
            const auto saved=ofLoadJson((d/"library.json").string()).value("bank",std::string());
            const std::filesystem::path file(saved);
            if(!saved.empty() && file==file.filename() && file.extension()==".json" && ofToLower(saved)!="factory.json"
                && std::filesystem::is_regular_file(d/"banks"/file))currentBank=saved;
        }catch(...){}
        if(host.factory) {   // the factory set as a bank (rewritten: this build's sounds)
            ofJson j;
            j["snapshotBank"] = 1;
            j["slots"] = ofJson::array();
            for(auto& s : host.factory()) j["slots"].push_back(s);
            try { ofSavePrettyJson((d / "banks" / "Factory.json").string(), j); } catch(...) {}
        }
        bool anyFile = false;
        for(int k = 0; k < SLOTS && !anyFile; k++) anyFile = std::filesystem::exists(d / ("snapshot_" + ofToString(k) + ".json"));
        if(!std::filesystem::exists(d / "library.json") && !anyFile) {
            // first run: an older format, else the factory set
            used.fill(false);
            if(host.importLegacy && host.importLegacy()) { active = -1; saveLibrary(); return; }
            if(!host.defaultBank.empty() && std::filesystem::is_regular_file(d / "banks" / host.defaultBank)
               && loadBank((d / "banks" / host.defaultBank).string())) return;
            if(host.factory) { loadFactory(); return; }
            saveLibrary();
            return;
        }
        used.fill(false);
        for(int k = 0; k < SLOTS; k++) {
            const auto f = d / ("snapshot_" + ofToString(k) + ".json");
            if(!std::filesystem::exists(f)) continue;
            try {
                ofJson j = ofLoadJson(f.string());
                if(!j.is_object()) continue;
                if(j.contains("__name")) { j["name"] = j["__name"]; j.erase("__name"); }
                slots[k] = j;
                used[k] = true;
            } catch(...) {}
        }
        if(active >= 0 && !used[active]) active = -1;
        if(!std::filesystem::exists(d / "library.json")) saveLibrary();   // slot files without the marker
        libTime = setTime();
    }
    void loadFactory() {
        if(!host.factory) return;
        currentBank.clear();
        used.fill(false);
        auto f = host.factory();
        for(int k = 0; k < std::min(SLOTS, (int)f.size()); k++) { slots[k] = f[k]; used[k] = true; }
        active = -1;
        message = "Loaded the factory snapshots";
        saveLibrary();
    }
    bool newBank(const std::string& requestedName) {
        const std::string trimmed=ofTrim(requestedName);
        const std::filesystem::path leaf(trimmed);
        if(trimmed.empty() || trimmed=="." || trimmed==".." || leaf!=leaf.filename()
            || trimmed.find('\\')!=std::string::npos){message="Enter a bank name without folders";return false;}
        std::string filename=trimmed;
        if(leaf.extension()!=".json")filename+=".json";
        if(ofToLower(filename)=="factory.json"){message="Factory is reserved; choose another name";return false;}
        const auto path=dir()/"banks"/filename;
        if(std::filesystem::exists(path)){message="That bank already exists; choose another name";return false;}
        if(!host.capture){message="Could not capture the current sound";return false;}
        ofJson sound;
        try{sound=host.capture();}catch(...){message="Could not capture the current sound";return false;}
        if(!sound.is_object()){message="Could not capture the current sound";return false;}
        sound["name"]=name(active).empty()?"Snapshot 1":name(active);
        const ofJson bank{{"snapshotBank",1},{"active",0},{"slots",ofJson::array({sound})}};
        if(!ofSavePrettyJson(path.string(),bank)){
            std::error_code error;std::filesystem::remove(path,error);
            message="Could not create "+filename;return false;
        }
        // A working collection can contain edits never exported as a bank.
        // Preserve it before replacing slots, even if it has a named source.
        const auto stem="Before new bank - "+ofGetTimestampString();
        auto backup=dir()/"banks"/(stem+".json");
        for(int suffix=1;std::filesystem::exists(backup);++suffix)
            backup=dir()/"banks"/(stem+"-"+ofToString(suffix)+".json");
        if(!saveBank(backup.string())){
            std::error_code error;std::filesystem::remove(path,error);std::filesystem::remove(backup,error);
            return false;
        }
        running=false;target=-1;used.fill(false);slots.fill(ofJson{});
        slots[0]=sound;used[0]=true;setActive(0);
        morphAP.setWithoutEventNotifications(0);morphBP.setWithoutEventNotifications(0);
        morphP.setWithoutEventNotifications(0.f);
        currentBank=filename;saveLibrary();
        message="Created "+path.stem().string()+" with the current sound";
        return true;
    }
    std::vector<std::string> storeBankNames() const {
        std::vector<std::string> names;
        try{
            for(const auto& file:std::filesystem::directory_iterator(dir()/"banks"))
                if(file.is_regular_file() && file.path().extension()==".json"
                    && ofToLower(file.path().filename().string())!="factory.json")names.push_back(file.path().filename().string());
        }catch(...){}
        std::sort(names.begin(),names.end());return names;
    }
    static std::string storeSnapshotName(const ofJson& snapshot,int slot) {
        for(const char* key:{"name","__name"})
            if(snapshot.contains(key) && snapshot[key].is_string())return snapshot[key].get<std::string>();
        return "Snapshot "+ofToString(slot+1);
    }
    bool readStoreBank(const std::string& filename,ofJson& bank) {
        if(filename.empty() || filename==currentBank){
            bank=ofJson{{"snapshotBank",1},{"active",active},{"slots",ofJson::array()}};
            int last=-1;for(int k=0;k<SLOTS;++k)if(used[k])last=k;
            for(int k=0;k<=last;++k)bank["slots"].push_back(used[k]?slots[k]:ofJson(nullptr));
            return true;
        }
        const std::filesystem::path file(filename);
        if(file!=file.filename() || filename.find('\\')!=std::string::npos || file.extension()!=".json"
            || ofToLower(filename)=="factory.json"){
            message="Choose a writable bank";return false;
        }
        try{bank=ofLoadJson((dir()/"banks"/file).string());}catch(...){bank=ofJson();}
        if(!bank.is_object() || !bank.contains("slots") || !bank["slots"].is_array()){
            message="Cannot read bank "+file.stem().string();return false;
        }
        return true;
    }
    void refreshStoreDestination() {
        if(!readStoreBank(storeBank,storeDestination)){storeDestination=ofJson();storeError=message;}
        else storeError.clear();
    }
    int storeInBank(const std::string& filename,const std::string& requestedName,int replace=-1,bool matchName=false) {
        ofJson bank;if(!readStoreBank(filename,bank))return -1;
        auto& entries=bank["slots"];const std::string nm=ofTrim(requestedName);
        int slot=replace;
        if(matchName && slot<0 && !nm.empty())
            for(int k=0;k<std::min(SLOTS,(int)entries.size());++k)
                if(entries[k].is_object() && storeSnapshotName(entries[k],k)==nm){slot=k;break;}
        if(slot<0)for(int k=0;k<SLOTS;++k)
            if(k>=(int)entries.size() || !entries[k].is_object()){slot=k;break;}
        if(slot<0 || slot>=SLOTS){message="This bank is full; choose a snapshot to replace";return -1;}
        if(!host.capture){message="Could not capture the current sound";return -1;}
        if(filename.empty() || filename==currentBank){store(slot,nm);return slot;}
        ofJson sound;
        try{sound=host.capture();}catch(...){message="Could not capture the current sound";return -1;}
        if(!sound.is_object()){message="Could not capture the current sound";return -1;}
        const std::string keep=slot<(int)entries.size() && entries[slot].is_object()?storeSnapshotName(entries[slot],slot):std::string();
        sound["name"]=!nm.empty()?nm:!keep.empty()?keep:"Snapshot "+ofToString(slot+1);
        while((int)entries.size()<=slot)entries.push_back(nullptr);
        entries[slot]=sound;bank["active"]=slot;
        const auto path=dir()/"banks"/filename;
        auto temporary=std::filesystem::path(path.string()+".store.tmp");
        for(int suffix=1;std::filesystem::exists(temporary);++suffix)
            temporary=std::filesystem::path(path.string()+".store-"+ofToString(suffix)+".tmp");
        bool saved=false;try{saved=ofSavePrettyJson(temporary.string(),bank);}catch(...){}
        std::error_code error;
        if(saved)std::filesystem::rename(temporary,path,error);
        if(!saved || error){
            std::filesystem::remove(temporary,error);message="Could not save bank "+std::filesystem::path(filename).stem().string();return -1;
        }
        message="Stored "+sound["name"].get<std::string>()+" in "+std::filesystem::path(filename).stem().string();
        return slot;
    }
    bool saveBank(const std::string& path) {
        ofJson j;
        j["snapshotBank"] = 1;
        j["active"] = active;
        j["slots"] = ofJson::array();
        int last = -1;
        for(int k = 0; k < SLOTS; k++) if(used[k]) last = k;
        for(int k = 0; k <= last; k++) j["slots"].push_back(used[k] ? slots[k] : ofJson(nullptr));
        std::string p = path;
        if(std::filesystem::path(p).extension().string() != ".json") p += ".json";
        const bool saved=ofSavePrettyJson(p,j);
        message = saved ? "Saved " + std::filesystem::path(p).stem().string() : "Could not save " + p;
        return saved;
    }
    // A bank (any json with a "slots" array) replaces the set
    bool loadBank(const std::string& path) {
        ofJson j;
        try { j = ofLoadJson(path); } catch(...) {}
        const ofJson* arr = j.contains("slots") && j["slots"].is_array() ? &j["slots"] : nullptr;
        if(!arr) { message = "No snapshots in " + std::filesystem::path(path).filename().string(); return false; }
        used.fill(false);
        for(int k = 0; k < std::min(SLOTS, (int)arr->size()); k++) {
            if(!(*arr)[k].is_object()) continue;
            slots[k] = (*arr)[k];
            if(slots[k].contains("__name")) { slots[k]["name"] = slots[k]["__name"]; slots[k].erase("__name"); }
            used[k] = true;
        }
        const int a = j.value("active", -1);
        active = a >= 0 && a < SLOTS && used[a] ? a : -1;
        const std::filesystem::path source(path);
        currentBank=source.parent_path().lexically_normal()==(dir()/"banks").lexically_normal()
            && ofToLower(source.filename().string())!="factory.json"?source.filename().string():std::string();
        saveLibrary();
        message = "Loaded " + source.stem().string();   // becomes the shared set
        return true;
    }

    // ── Named presets: <presetDir>/<name>.json ─────────────────────────────
    std::filesystem::path presetDir() const {
        const std::filesystem::path d = ofToDataPath("nodeSnapshots/" + host.folder + "/Presets", true);
        try { std::filesystem::create_directories(d); } catch(...) {}
        return d;
    }
    // A preset already at the new place wins; the old folder goes once empty
    void moveLegacyPresets() {
        if(host.legacyPresetFolder.empty()) return;
        const std::filesystem::path old = ofToDataPath(host.legacyPresetFolder, true);
        std::error_code ec;
        if(!std::filesystem::is_directory(old, ec)) return;
        const std::filesystem::path d = presetDir();
        for(const auto& e : std::filesystem::directory_iterator(old, ec)) {
            if(!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
            const auto to = d / e.path().filename();
            if(!std::filesystem::exists(to, ec)) std::filesystem::rename(e.path(), to, ec);
        }
        if(std::filesystem::is_empty(old, ec)) std::filesystem::remove(old, ec);
    }
    void scanPresets() {
        presetNames.clear();
        std::error_code ec;
        for(const auto& e : std::filesystem::directory_iterator(presetDir(), ec))
            if(e.is_regular_file(ec) && e.path().extension() == ".json") presetNames.push_back(e.path().stem().string());
        std::sort(presetNames.begin(), presetNames.end());
    }
    static std::string presetFileName(const std::string& raw) {
        std::string n;
        for(char c : raw) n += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.') ? c : '_';
        return ofTrim(n);
    }
    ofJson readPreset(const std::string& nm) const {
        try {
            ofJson j = ofLoadJson((presetDir() / (nm + ".json")).string());
            if(j.is_object()) { j["name"] = nm; return j; }
        } catch(...) {}
        return ofJson();
    }
    // Loads (or glides to) a preset; the slots keep their own
    bool loadPreset(const std::string& nm, bool glideToIt = false) {
        const ofJson j = readPreset(nm);
        if(!j.is_object()) { message = "Cannot read " + nm; return false; }
        currentPreset = nm;
        if(glideToIt) glideTo(j, -1);
        else { running = false; if(host.apply) host.apply(j, j, 1.0f, true); setActive(-1); }
        return true;
    }
    bool savePreset(const std::string& raw) {
        const std::string nm = presetFileName(raw);
        if(nm.empty() || !host.capture) return false;
        ofJson j = host.capture();
        j.erase("name");   // the file's name is its name
        if(!ofSavePrettyJson((presetDir() / (nm + ".json")).string(), j)) { message = "Could not save " + nm; return false; }
        currentPreset = nm;
        scanPresets();
        return true;
    }
    bool renamePreset(const std::string& from, const std::string& raw) {
        const std::string nm = presetFileName(raw);
        if(nm.empty() || nm == from) return false;
        std::error_code ec;
        const auto to = presetDir() / (nm + ".json");
        if(std::filesystem::exists(to)) { message = "A preset named " + nm + " exists"; return false; }
        std::filesystem::rename(presetDir() / (from + ".json"), to, ec);
        if(ec) return false;
        if(currentPreset == from) currentPreset = nm;
        scanPresets();
        return true;
    }
    bool deletePreset(const std::string& nm) {
        std::error_code ec;
        std::filesystem::remove(presetDir() / (nm + ".json"), ec);
        if(currentPreset == nm) currentPreset.clear();
        scanPresets();
        return !ec;
    }
    // A preset into the first free slot (it can then be morphed by number)
    int presetToSlot(const std::string& nm) {
        ofJson j = readPreset(nm);
        if(!j.is_object()) return -1;
        for(int k = 0; k < SLOTS; k++) if(!used[k]) { slots[k] = j; used[k] = true; saveLibrary(); return k; }
        return -1;
    }

    // ── GUI ────────────────────────────────────────────────────────────────
    // Snapshot, Store..., Manage..., the Snapshots menu; withMorph: Glide,
    // Time and the A / B Morph on the same row
    // separator: a line under it, between the snapshots and the node's controls
    void drawBar(float w, bool withMorph = true, bool separator = true) {
        ImGui::PushID(this);
        const float z = std::max(0.5f, ImGui::GetFontSize() / 13.0f);
        const float barX = ImGui::GetCursorScreenPos().x;
        auto label = [this](int k) { return ofToString(k + 1) + "  " + (used[k] ? name(k) : std::string("(empty)")); };
        // label + combo are one item for the node's menu: right-click either
        // to publish Snapshot (the slot number, an int) to the node
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Snapshot");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * z);
        if(ImGui::BeginCombo("##snapCombo", active >= 0 && used[active] ? label(active).c_str() : "-", ImGuiComboFlags_HeightLarge)) {
            for(int k = 0; k < SLOTS; k++) {
                if(!used[k]) continue;
                if(ImGui::Selectable(label(k).c_str(), k == active)) recallFromGui(k);
                if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && slots[k].contains("importNote") && slots[k]["importNote"].is_string())
                    ImGui::SetTooltip("%s",slots[k].value("importNote",std::string()).c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
        menuFor(snapshotP);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)){
            std::string tip="Recall a snapshot (Glide on: it glides over Time; Alt: the other way).\nManage...: rename, delete or reorder them.\nRight-click: publish Snapshot (its number) to the node.";
            if(active>=0 && used[active] && slots[active].contains("importNote") && slots[active]["importNote"].is_string()) tip+="\n"+slots[active].value("importNote",std::string());
            ImGui::SetTooltip("%s",tip.c_str());
        }
        // the previous / next one
        auto step = [this](int dir) {
            std::vector<int> v;
            for(int k = 0; k < SLOTS; k++) if(used[k]) v.push_back(k);
            if(v.empty()) return;
            const auto it = std::find(v.begin(), v.end(), active);
            int i = it == v.end() ? (dir > 0 ? -1 : 0) : int(it - v.begin());
            i = (i + dir + (int)v.size()) % (int)v.size();
            recallFromGui(v[i]);
        };
        ImGui::SameLine(0, 2 * z);
        if(ImGui::ArrowButton("##snapPrev", ImGuiDir_Left)) step(-1);
        ImGui::SameLine(0, 2 * z);
        if(ImGui::ArrowButton("##snapNext", ImGuiDir_Right)) step(1);
        if(running) {
            ImGui::SameLine();
            ImGui::ProgressBar(progress, ImVec2(50 * z, ImGui::GetFrameHeight()), "");
        }
        ImGui::SameLine();
        if(ImGui::Button("Store...")) {
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", active >= 0 ? name(active).c_str() : "");
            storeBank=currentBank;storeBanks=storeBankNames();refreshStoreDestination();
            ImGui::OpenPopup("##snapStore");
        }
        if(ImGui::BeginPopup("##snapStore")) {
            ImGui::TextDisabled("Store the current sound");
            ImGui::SetNextItemWidth(260*z);
            const auto destination=storeBank.empty()?std::string("Current collection"):std::filesystem::path(storeBank).stem().string();
            if(ImGui::BeginCombo("Bank",destination.c_str(),ImGuiComboFlags_HeightLarge)){
                if(currentBank.empty() && ImGui::Selectable("Current collection (unnamed)",storeBank.empty())){
                    storeBank.clear();refreshStoreDestination();
                }
                ImGuiListClipper clipper;clipper.Begin((int)storeBanks.size());
                while(clipper.Step())for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i){
                    const auto& file=storeBanks[i];
                    std::string label=std::filesystem::path(file).stem().string();if(file==currentBank)label+=" (current)";
                    if(ImGui::Selectable(label.c_str(),file==storeBank)){storeBank=file;refreshStoreDestination();}
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(260*z);
            if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
            const bool enter=ImGui::InputTextWithHint("##snapName","Snapshot name",nameBuf,sizeof(nameBuf),ImGuiInputTextFlags_EnterReturnsTrue);
            auto save=[&](int replace,bool match){
                if(storeInBank(storeBank,nameBuf,replace,match)>=0){ImGui::CloseCurrentPopup();return;}
                storeError=message;
            };
            const bool sameCollection=storeBank.empty() || storeBank==currentBank;
            if(ImGui::Button("Save snapshot") || enter)save(-1,sameCollection);
            ImGui::TextDisabled(sameCollection?"Enter: new snapshot or replace matching name":"Enter: new snapshot in the selected bank");
            ImGui::Separator();
            const auto* entries=storeDestination.is_object() && storeDestination.contains("slots")?&storeDestination["slots"]:nullptr;
            if(entries){
                bool full=true;for(int k=0;k<SLOTS;++k)
                    if(k>=(int)entries->size() || !(*entries)[k].is_object()){full=false;break;}
                if(!full && ImGui::Selectable("+ New snapshot"))save(-1,false);
                if(full)ImGui::TextDisabled("Bank full: choose a snapshot to replace");
                ImGui::TextDisabled("Replace");
                for(int k=0;k<std::min(SLOTS,(int)entries->size());++k){
                    if(!(*entries)[k].is_object())continue;
                    const auto label=ofToString(k+1)+"  "+storeSnapshotName((*entries)[k],k);
                    if(ImGui::Selectable(label.c_str(),(storeBank.empty() || storeBank==currentBank) && k==active))save(k,false);
                }
            }
            if(!storeError.empty())ImGui::TextWrapped("%s",storeError.c_str());
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("Manage...")) manager.windowOpen = true;
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Rename, delete or reorder the snapshots");
        drawManageWindow();
        ImGui::SameLine();
        if(ImGui::Button("Banks")) ImGui::OpenPopup("##snapBanks");
        if(ImGui::BeginPopup("##snapBanks")) {
            if(!currentBank.empty())ImGui::TextDisabled("Current: %s",std::filesystem::path(currentBank).stem().string().c_str());
            if(ImGui::BeginMenu("New bank")){
                ImGui::SetNextItemWidth(180*z);
                const bool enter=ImGui::InputText("##newBankName",newBankName,sizeof(newBankName),ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::TextDisabled("Starts with the current sound. Store adds snapshots.");
                if((ImGui::Button("Create") || enter) && newBankName[0]){
                    if(newBank(newBankName)){newBankName[0]='\0';ImGui::CloseCurrentPopup();}
                }
                if(!message.empty())ImGui::TextWrapped("%s",message.c_str());
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if(host.factory && ImGui::MenuItem("Factory collection")) loadFactory();
            if(ImGui::BeginMenu("Load bank")) {
                bool any = false;
                try {
                    for(auto& e : std::filesystem::directory_iterator(dir() / "banks")) {
                        if(e.path().extension() != ".json") continue;
                        any = true;
                        if(ImGui::MenuItem(e.path().stem().string().c_str())) loadBank(e.path().string());
                    }
                } catch(...) {}
                if(!any) ImGui::TextDisabled("No banks yet");
                ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("Save bank as")) {
                ImGui::SetNextItemWidth(180 * z);
                const bool enter = ImGui::InputText("##snapBankName", bankName, sizeof(bankName), ImGuiInputTextFlags_EnterReturnsTrue);
                if((ImGui::Button("Save") || enter) && bankName[0]) {
                    const auto path=dir()/"banks"/bankName;
                    if(saveBank(path.string())){
                        currentBank=path.parent_path().lexically_normal()==(dir()/"banks").lexically_normal()
                            ?path.filename().string():std::string();
                        if(!currentBank.empty() && std::filesystem::path(currentBank).extension()!=".json")currentBank+=".json";
                        if(ofToLower(currentBank)=="factory.json")currentBank.clear();
                        saveLibrary();
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if(ImGui::MenuItem("From file...")) loadDialog = true;
            if(ImGui::MenuItem("Clear all")) { used.fill(false); active = -1; saveLibrary(); }
            if(!message.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", message.c_str()); }
            ImGui::EndPopup();
        }
        if(withMorph) {
            // on this row if it fits, else on the next
            const float right = std::min(barX + w, ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x);
            const float left = right - (ImGui::GetItemRectMax().x + 10 * z);
            if(left > 520 * z){ ImGui::SameLine(0, 10 * z); drawMorph(w, barX); }
            else drawMorph(w);
        }
        ImGui::PopID();
        if(separator) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
    }
    // The Manage window: snapshot slots only
    void drawManageWindow() {
        if(!manager.windowOpen) return;
        ImGui::SetNextWindowSize(ImVec2(440, 560), ImGuiCond_FirstUseEver);
        if(ImGui::Begin((host.title + " snapshots##scSnapManager").c_str(), &manager.windowOpen)) {
            ImGui::TextDisabled("SNAPSHOTS: click a number to recall, drag it to move; type to rename");
            ImGui::PushID("slots");
            manager.drawList();
            ImGui::PopID();
        }
        ImGui::End();
    }
    // Glide + Time, A / B and the Morph slider, the curve / switch settings
    // rowX: where the row of width w starts (screen x), when the morph shares the bar's row
    void drawMorph(float w, float rowX = -1.0f) {
        ImGui::PushID("snapMorph");
        const float z = std::max(0.5f, ImGui::GetFontSize() / 13.0f);
        const float x0 = rowX >= 0.0f ? rowX : ImGui::GetCursorScreenPos().x;
        bool g = morphGlideP.get();
        if(ImGui::Checkbox("Glide", &g)) morphGlideP.set(g);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("On: every recall glides from the current sound over Time.\nOff: recalls jump (Alt to glide).");
        menuFor(morphGlideP);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * z);
        float v = morphTimeP.get();
        const bool beats = morphBeatsP.get();
        if(ImGui::SliderFloat("##time", &v, 0.0f, 64.0f, beats ? "%.2f beats" : "%.2f s", ImGuiSliderFlags_Logarithmic)) morphTimeP.set(v);
        menuFor(morphTimeP);
        ImGui::SameLine(0, 2 * z);
        if(ImGui::SmallButton(beats ? "b" : "s")) morphBeatsP.set(!beats);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Time in beats (b) or seconds (s)");
        ImGui::SameLine(0, 10 * z);
        auto slotCombo = [&](const char* id, ofParameter<int>& p) {
            ImGui::SetNextItemWidth(44 * z);
            const std::string cur = p.get() > 0 ? ofToString(p.get()) : std::string("-");
            if(ImGui::BeginCombo(id, cur.c_str(), ImGuiComboFlags_HeightLarge)) {
                if(ImGui::Selectable("-", p.get() == 0)) p.set(0);
                for(int k = 0; k < SLOTS; k++)
                    if(used[k] && ImGui::Selectable((ofToString(k + 1) + "  " + name(k)).c_str(), p.get() == k + 1)) p.set(k + 1);
                ImGui::EndCombo();
            }
            menuFor(p);
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("A"); ImGui::SameLine(); slotCombo("##a", morphAP);
        ImGui::SameLine();
        ImGui::TextUnformatted("B"); ImGui::SameLine(); slotCombo("##b", morphBP);
        ImGui::SameLine();
        const float gear = ImGui::GetFrameHeight();
        // up to the row's end, and never past the window's
        const float right = std::min(x0 + w, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(std::max(80.0f * z, right - ImGui::GetCursorScreenPos().x - gear - ImGui::GetStyle().ItemSpacing.x));
        float mv = morphP.get();
        if(ImGui::SliderFloat("##morph", &mv, 0.0f, 1.0f, "Morph %.3f")) morphP.set(mv);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Blends snapshot A (0) into B (1). Publish it (right-click) to drive it from a connection.");
        menuFor(morphP);
        ImGui::SameLine();
        if(ImGui::Button("...", ImVec2(gear, 0))) ImGui::OpenPopup("##morphSettings");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Morph curve and switch point");
        if(ImGui::BeginPopup("##morphSettings")) {
            static const char* curves[4] = {"Linear", "Ease", "Exp (slow start)", "Log (fast start)"};
            static const char* sws[3] = {"At the start", "In the middle", "At the end"};
            int c = std::clamp(morphCurveP.get(), 0, 3);
            ImGui::SetNextItemWidth(160 * z);
            if(ImGui::Combo("Curve", &c, curves, 4)) morphCurveP.set(c);
            menuFor(morphCurveP);
            int s = std::clamp(morphSwitchP.get(), 0, 2);
            ImGui::SetNextItemWidth(160 * z);
            if(ImGui::Combo("Switches", &s, sws, 3)) morphSwitchP.set(s);
            menuFor(morphSwitchP);
            ImGui::TextDisabled("Switches and modes jump; numbers glide.");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    // The list inline (a node's own Snapshots tab), without the window
    void drawList(float height = 0.0f) { manager.drawList(height); }

private:
    Host host;
    ofEventListeners listeners;
    bool recalling = false;
    bool running = false;
    float t0 = 0.0f, duration = 1.0f, progress = 0.0f;
    int target = -1;
    ofJson from, to;
    bool loadDialog = false;
    std::string message;
    char nameBuf[64] = "";
    char bankName[64] = "my bank";
    char newBankName[64] = "";
    std::string currentBank,storeBank,storeError;
    std::vector<std::string> storeBanks;
    ofJson storeDestination;
    std::filesystem::file_time_type libTime{};
    float libCheckAt = 0.0f;

    template<typename T>
    void menuFor(ofParameter<T>& p) { if(host.paramMenu) host.paramMenu(p.getEscapedName()); }
    // a click in the bar: Glide decides, Alt inverts it
    void recallFromGui(int k) {
        if(morphGlideP.get() != ImGui::GetIO().KeyAlt) glide(k); else recall(k);
    }
    std::filesystem::file_time_type setTime() const {
        std::filesystem::file_time_type t{};
        try {
            for(auto& e : std::filesystem::directory_iterator(dir()))
                if(e.is_regular_file()) t = std::max(t, std::filesystem::last_write_time(e.path()));
        } catch(...) {}
        return t;
    }
};
