//
//  scCodeNode.cpp
//  ofxOceanodeSuperCollider
//

#include "scCodeNode.h"
#include "serverManager.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include "scSynthdef.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
// Bumped when the runner's output changes, so older caches are rebuilt
constexpr const char* kCacheVersion = "1";
// A voice whose /n_go never came (SynthDef missing) is forgotten after this
constexpr float kVoiceStartTimeout = 5.0f;
// A stolen gated voice is released this fast (EnvGen forced release: -1 - t)
constexpr float kStealRelease = -1.02f;
constexpr int kScratchChannels = 64;

const char* kModeNames[] = {"Auto", "Persistent", "Voices"};

const std::vector<std::string> kInputTypes = {"In", "InFeedback", "LagIn"};
const std::vector<std::string> kOutputTypes = {"Out", "ReplaceOut", "OffsetOut", "XOut"};

std::string readFile(const std::string& path){
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool contains(const std::vector<std::string>& list, const std::string& value){
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::string displayName(std::string name){
    if(!name.empty()) name[0] = std::toupper((unsigned char)name[0]);
    return name;
}

std::string lower(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

// Slider range for a control without a spec: by its name, else its default
void guessRange(const scCodeNode::ControlDesc& c, float& min, float& max){
    float lo = 0, hi = 0;
    for(float v : c.defaults){ lo = std::min(lo, v); hi = std::max(hi, v); }
    const std::string n = lower(c.name);
    auto has = [&](std::initializer_list<const char*> words){
        for(auto* w : words) if(n.find(w) != std::string::npos) return true;
        return false;
    };
    if(has({"freq", "cutoff", "ffreq", "lpf", "hpf", "hz"})){ min = 20; max = 20000; return; }
    if(has({"midi", "note", "pitch"})){ min = 0; max = 127; return; }
    if(has({"atk", "attack", "dec", "rel", "sus", "dur", "time", "len"})){ min = 0; max = std::max(4.0f, hi * 4); return; }
    if(has({"pan", "pos"}) && lo >= -1 && hi <= 1){ min = -1; max = 1; return; }
    if(lo >= 0 && hi <= 1){ min = 0; max = 1; return; }
    const float m = std::max(std::abs(lo), std::abs(hi));
    min = lo < 0 ? -m * 4 : 0;
    max = m * 4;
}

// The ERROR lines sclang posted, with the line that locates each
std::vector<std::string> logErrors(const std::string& text){
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line, previous;
    bool wantLocation = false;
    while(std::getline(in, line)){
        if(line.rfind("ERROR:", 0) == 0 && line.find("Command line parse failed") == std::string::npos){
            out.push_back(ofTrim(line));
            wantLocation = true;
        }else if(wantLocation && line.find("line ") != std::string::npos && line.find(" char ") != std::string::npos){
            out.back() += " (" + ofTrim(line) + ")";
            wantLocation = false;
        }
        if(out.size() > 12) break;
    }
    return out;
}

std::string stripFences(std::string text){
    text = ofTrim(text);
    if(text.rfind("```", 0) == 0){
        const size_t eol = text.find('\n');
        text = eol == std::string::npos ? "" : text.substr(eol + 1);
        const size_t close = text.rfind("```");
        if(close != std::string::npos) text = text.substr(0, close);
    }
    return ofTrim(text) + "\n";
}

const char* kDefaultCode =
R"SC(// Paste SuperCollider code here and press Compile (or Cmd+Return).
// Every SynthDef it builds can be played by this node:
//   - an audio bus control (In.ar(\in), Out.ar(\out)) becomes a port,
//   - any other control becomes a parameter,
//   - a SynthDef that frees itself (doneAction: 2) plays voices from Gate.
(
SynthDef(\pluck, { |out = 0, freq = 220, amp = 0.3, decay = 2, pan = 0|
    var exciter = WhiteNoise.ar * EnvGen.ar(Env.perc(0.001, 0.01));
    var sig = CombL.ar(exciter, 0.05, freq.reciprocal, decay) * amp;
    DetectSilence.ar(sig, doneAction: 2);
    Out.ar(out, Pan2.ar(sig, pan));
}).add;
)
)SC";
}

// ════════════════════════════════════════════════════════════════════════════
// Description and blocks
// ════════════════════════════════════════════════════════════════════════════

bool scCodeNode::parseMeta(const std::string& text, Meta& meta, std::string& error){
    meta = Meta();
    try {
        const ofJson json = ofJson::parse(text);
        for(auto& e : json.value("errors", ofJson::array())) meta.errors.push_back(e.get<std::string>());
        for(auto& d : json.value("synthdefs", ofJson::array())){
            DefDesc def;
            def.name = d.value("name", "");
            def.defName = d.value("defName", "");
            def.canFreeSynth = d.value("canFreeSynth", false);
            def.hasGate = d.value("hasGate", false);
            def.functionPlay = d.value("functionPlay", false);
            if(d.contains("creator") && d["creator"].is_object()){
                const auto& cr = d["creator"];
                def.creator = true;
                def.creatorChannels = cr.value("channels", 1);
                def.description = cr.value("description", "");
                def.category = cr.value("category", "");
                for(auto& v : cr.value("variables", ofJson::array())){
                    DefDesc::Variable var;
                    var.name = v.value("name", "");
                    var.dim = std::max(1, v.value("dim", 1));
                    var.value = ofClamp(v.value("value", 1), 1, var.dim);
                    if(!var.name.empty()) def.variables.push_back(var);
                }
            }
            for(auto& c : d.value("controls", ofJson::array())){
                ControlDesc control;
                control.name = c.value("name", "");
                control.rate = c.value("rate", "control");
                for(auto& v : c.value("default", ofJson::array())) control.defaults.push_back(v.is_number() ? v.get<float>() : 0.0f);
                if(control.defaults.empty()) control.defaults.push_back(0);
                if(c.contains("spec") && c["spec"].is_object()){
                    const auto& s = c["spec"];
                    if(s.value("min", ofJson()).is_number() && s.value("max", ofJson()).is_number()){
                        control.hasSpec = true;
                        control.min = s["min"].get<float>();
                        control.max = s["max"].get<float>();
                        control.units = s.value("units", "");
                    }
                }
                if(!control.name.empty()) def.controls.push_back(control);
            }
            auto readBuses = [](const ofJson& list){
                std::vector<BusDesc> out;
                for(auto& b : list){
                    BusDesc bus;
                    bus.type = b.value("type", "");
                    bus.rate = b.value("rate", "");
                    bus.channels = b.value("channels", 1);
                    if(b.contains("bus") && b["bus"].is_string()) bus.control = b["bus"].get<std::string>();
                    else if(b.contains("bus") && b["bus"].is_number()){ bus.fixed = true; bus.number = (int)b["bus"].get<float>(); }
                    out.push_back(bus);
                }
                return out;
            };
            def.inputs = readBuses(d.value("inputs", ofJson::array()));
            def.outputs = readBuses(d.value("outputs", ofJson::array()));
            if(!def.defName.empty()) meta.defs.push_back(def);
        }
    } catch(const std::exception& e) {
        error = std::string("unreadable description: ") + e.what();
        return false;
    }
    return true;
}

std::vector<std::pair<int, std::string>> scCodeNode::splitBlocks(const std::string& code){
    std::vector<std::pair<int, std::string>> blocks;
    auto addLoose = [&](size_t from, size_t to, int line){
        const std::string text = code.substr(from, to - from);
        if(ofTrim(text).empty()) return;
        // Skip the leading blank lines so the block's line is its first code
        int skipped = 0;
        size_t i = 0;
        while(i < text.size()){
            size_t eol = text.find('\n', i);
            if(eol == std::string::npos || !ofTrim(text.substr(i, eol - i)).empty()) break;
            skipped++;
            i = eol + 1;
        }
        blocks.emplace_back(line + skipped, text.substr(i));
    };

    int depth = 0, blockDepth = -1, line = 1, looseLine = 1, blockLine = 1;
    int commentDepth = 0;
    bool lineComment = false, inString = false, inSymbol = false, lineStart = true;
    size_t looseStart = 0, blockStart = 0;
    for(size_t i = 0; i < code.size(); i++){
        const char c = code[i];
        const char next = i + 1 < code.size() ? code[i + 1] : 0;
        if(c == '\n'){
            line++;
            lineComment = false;
            lineStart = true;
            // A block ends at the end of the line of its closing parenthesis,
            // so "( ... ).play;" stays in one piece
            if(blockDepth == -2){
                blocks.emplace_back(blockLine, code.substr(blockStart, i + 1 - blockStart));
                looseStart = i + 1;
                looseLine = line;
                blockDepth = -1;
            }
            continue;
        }
        if(lineComment) continue;
        if(commentDepth > 0){
            if(c == '*' && next == '/'){ commentDepth--; i++; }
            else if(c == '/' && next == '*'){ commentDepth++; i++; }
            continue;
        }
        if(inString){ if(c == '\\') i++; else if(c == '"') inString = false; continue; }
        if(inSymbol){ if(c == '\\') i++; else if(c == '\'') inSymbol = false; continue; }
        const bool wasLineStart = lineStart;
        if(c != ' ' && c != '\t' && c != '\r') lineStart = false;
        if(c == '/' && next == '/'){ lineComment = true; i++; continue; }
        if(c == '/' && next == '*'){ commentDepth = 1; i++; continue; }
        if(c == '"'){ inString = true; continue; }
        if(c == '\''){ inSymbol = true; continue; }
        if(c == '$'){ if(next == '\\') i++; i++; continue; }
        if(c == '(' || c == '[' || c == '{'){
            if(c == '(' && depth == 0 && wasLineStart && blockDepth == -1){
                addLoose(looseStart, i, looseLine);
                blockStart = i;
                blockLine = line;
                blockDepth = 0;
            }
            depth++;
        }else if(c == ')' || c == ']' || c == '}'){
            depth = std::max(0, depth - 1);
            if(depth == 0 && blockDepth == 0) blockDepth = -2;   // ends with this line
        }
    }
    if(blockDepth == -2 || blockDepth == 0){
        // Unbalanced or unterminated: still run it, sclang reports why
        blocks.emplace_back(blockLine, code.substr(blockStart));
    }else{
        addLoose(looseStart, code.size(), looseLine);
    }
    return blocks;
}

// ════════════════════════════════════════════════════════════════════════════
// Lifetime
// ════════════════════════════════════════════════════════════════════════════

scCodeNode::scCodeNode(std::vector<serverManager*> outputServers)
    : scNode("Code"), servers(outputServers) {
    description = "Plays SuperCollider code pasted from anywhere: Compile runs it without a server and keeps every "
                  "SynthDef it builds. Audio bus controls (In.ar(\\in), Out.ar(\\out)) become ports, every other "
                  "control a parameter. A SynthDef that frees itself plays polyphonic voices from the Gate lanes. "
                  "Promote turns the tested code into a regular Oceanode SynthDef node.";
    editorText = kDefaultCode;
    setupAIAssistant();
    pruneCache();
}

scCodeNode::~scCodeNode(){
    for(auto& [server, st] : states) freeState(st);
    states.clear();
    // A compile / install still running finishes on its own (it only touches
    // its own files): kept aside instead of waited for, so deleting the node
    // does not freeze the app for up to 30 s
    static std::vector<std::future<BuildOutcome>> orphanBuilds;
    static std::vector<std::future<PromoteOutcome>> orphanInstalls;
    auto ready = [](auto& f){ return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    orphanBuilds.erase(std::remove_if(orphanBuilds.begin(), orphanBuilds.end(), ready), orphanBuilds.end());
    orphanInstalls.erase(std::remove_if(orphanInstalls.begin(), orphanInstalls.end(), ready), orphanInstalls.end());
    if(buildTask.valid()) orphanBuilds.push_back(std::move(buildTask));
    if(installTask.valid()) orphanInstalls.push_back(std::move(installTask));
}

void scCodeNode::setup(){
    addParameter(showWindow.set("Show", false));
    gate.set("Gate", {0.0f}, {0.0f}, {1.0f});
    trig.set("Trig");
    maxVoices.set("Voices", 16, 1, MAX_VOICES);
    addInspectorParameter(trigLength.set("Trig Length", 0.2f, 0.005f, 10.0f));
    numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS);
    listeners.push(numChannels.newListener([this](int&){
        creatorSettingsDirty = true;
        creatorSettingsTime = ofGetElapsedTimef();
    }));

    listeners.push(gate.newListener([this](std::vector<float>& v){ if(voiceMode()) onGate(v); }));
    listeners.push(trig.newListener([this](){
        const DefDesc* def = currentDef();
        if(def && voiceMode()) spawn(0, 1.0f, def->hasGate ? trigLength.get() : -1.0f);
    }));

    statusRegion.set("SC Code", [this](){ drawStatusRegion(); });
    addParameter(statusRegion,
        ofxOceanodeParameterFlags_DisableInConnection | ofxOceanodeParameterFlags_DisableOutConnection |
        ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);

    ofDirectory::createDirectory(ofToDataPath("Supercollider/codeScripts"), false, true);
}

// ════════════════════════════════════════════════════════════════════════════
// Compile and cache
// ════════════════════════════════════════════════════════════════════════════

std::string scCodeNode::hashCode(const std::string& code){
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const std::string& s){
        for(unsigned char c : s){ h ^= c; h *= 1099511628211ULL; }
    };
    mix(code);
    mix(std::string("\n#oceanode-code-v") + kCacheVersion);
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", (unsigned long long)h);
    return out;
}

std::string scCodeNode::compileKey(const std::string& code, int channels, const std::vector<int>& variables){
    std::string key = code + "\n#oceanode-channels=" + ofToString(channels) + ";variables=";
    for(int v : variables) key += ofToString(v) + ",";
    return hashCode(key);
}

std::string scCodeNode::cacheRoot(){
    return ofToDataPath("Supercollider/CodeCache", true);
}

// Every code / channel count / variables combination ever compiled leaves a
// folder: those not used for 30 days go (a use touches the folder). Once per
// run, in the background.
void scCodeNode::pruneCache(){
    static bool done = false;
    if(done) return;
    done = true;
    std::thread([root = cacheRoot()](){
        try {
            if(!fs::exists(root)) return;
            const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
            for(auto& entry : fs::directory_iterator(root)){
                std::error_code ec;
                if(fs::last_write_time(entry.path(), ec) >= limit || ec) continue;
                fs::remove_all(entry.path(), ec);
            }
        } catch(...) {}
    }).detach();
}

std::string scCodeNode::findRunnerFolder(){
    std::vector<fs::path> candidates;
    const std::string sources = scSynthdefCompiler::get().getSourcesFolder();
    if(!sources.empty()) candidates.push_back(fs::path(sources) / "OceanodeCode");
    candidates.push_back(fs::path(addonSynthdefsFolder()) / "OceanodeCode");
    for(auto& folder : candidates){
        if(fs::exists(folder / "captureCode.scd") && fs::exists(folder / "classes")) return folder.string();
    }
    return "";
}

std::string scCodeNode::addonSynthdefsFolder(){
    // This file lives in <addon>/src/nodes
    return (fs::path(__FILE__).parent_path().parent_path().parent_path() / "synthdefs").string();
}

void scCodeNode::compile(const std::string& source){
    if(buildTask.valid()){
        queuedSource = source;
        hasQueuedSource = true;
        return;
    }
    // The channel count and variables only matter to ~synthCreator sources;
    // anything else ignores them
    buildingChannels = creatorParamsRegistered ? numChannels.get() : compiledChannels;
    buildingVariables = currentVariableValues();
    const std::string hash = compileKey(source, buildingChannels, buildingVariables);
    const std::string folder = (fs::path(cacheRoot()) / hash).string();
    const std::string cachedMeta = (fs::path(folder) / "meta.json").string();
    if(fs::exists(cachedMeta)){
        std::error_code touched;
        fs::last_write_time(folder, fs::file_time_type::clock::now(), touched);   // in use: kept by pruneCache
        const std::string cachedLog = (fs::path(cacheRoot()) / (hash + ".log")).string();
        logPath = cachedLog;
        logText = fs::exists(cachedLog) ? readFile(cachedLog) : "";
        blockLines.clear();
        for(auto& block : splitBlocks(source)) blockLines.push_back(block.first);
        applyMeta(source, hash, readFile(cachedMeta), false);
        return;
    }

    const std::string runner = findRunnerFolder();
    if(runner.empty()){
        status = "Cannot compile: synthdefs/OceanodeCode was not found";
        statusError = true;
        return;
    }
    auto job = scSynthdefCompiler::get().prepareScript("code", {(fs::path(runner) / "classes").string()});
    if(job.sclang.empty()){
        status = "Cannot compile: no sclang (bundled or /Applications/SuperCollider.app)";
        statusError = true;
        return;
    }
    auto blocks = splitBlocks(source);
    if(blocks.empty()){
        status = "Nothing to compile";
        statusError = true;
        return;
    }
    blockLines.clear();
    for(auto& block : blocks) blockLines.push_back(block.first);
    try { fs::create_directories(cacheRoot()); } catch(...) {}
    buildingHash = hash;
    buildingSource = source;
    buildTask = std::async(std::launch::async, &scCodeNode::runCapture, job,
                           (fs::path(runner) / "captureCode.scd").string(), hash, folder,
                           "oc" + hash.substr(0, 8), buildingChannels, buildingVariables,
                           fs::exists(sourceFile) ? sourceFile : std::string(), blocks);
    status = "Compiling...";
    statusError = false;
}

scCodeNode::BuildOutcome scCodeNode::runCapture(scSynthdefCompiler::ScriptJob job, std::string runner,
                                                std::string hash, std::string folder, std::string prefix,
                                                int channels, std::vector<int> variables, std::string sourcePath,
                                                std::vector<std::pair<int, std::string>> blocks){
    BuildOutcome out;
    out.hash = hash;
    out.folder = folder;
    const fs::path tmp = folder + ".tmp";
    std::vector<std::string> files;
    try {
        fs::remove_all(tmp);
        fs::create_directories(tmp / "blocks");
        for(size_t i = 0; i < blocks.size(); i++){
            const fs::path file = tmp / "blocks" / ("block" + ofToString(i + 1) + ".scd");
            std::ofstream(file) << blocks[i].second;
            files.push_back(file.string());
        }
    } catch(const std::exception& e) {
        out.error = e.what();
        return out;
    }
    std::string variableList;
    for(size_t i = 0; i < variables.size(); i++) variableList += (i ? "," : "") + ofToString(variables[i]);
    job.args = {runner, tmp.string(), prefix, ofToString(channels), variableList, sourcePath};
    job.args.insert(job.args.end(), files.begin(), files.end());
    out.logPath = (fs::path(folder).parent_path() / (hash + ".log")).string();
    job.logPath = out.logPath;
    job.timeoutSeconds = 30;

    const std::string error = scSynthdefCompiler::runScript(job);
    if(!fs::exists(tmp / "meta.json")){
        out.error = error.empty() ? "sclang wrote no description (see the log)" : error;
        try { fs::remove_all(tmp); } catch(...) {}
        return out;
    }
    try {
        fs::remove_all(tmp / "blocks");
        fs::remove_all(folder);
        fs::rename(tmp, folder);
    } catch(const std::exception& e) {
        out.error = e.what();
        return out;
    }
    out.metaText = readFile((fs::path(folder) / "meta.json").string());
    out.ok = true;
    return out;
}

void scCodeNode::finishCompile(BuildOutcome outcome){
    logPath = outcome.logPath;
    logText = fs::exists(logPath) ? readFile(logPath) : "";
    if(!outcome.ok){
        status = "Compile failed: " + outcome.error;
        statusError = true;
        errors = logErrors(logText);
        if(errors.empty()) errors.push_back(outcome.error);
        return;
    }
    applyMeta(buildingSource, outcome.hash, outcome.metaText, false);
}

void scCodeNode::applyMeta(const std::string& source, const std::string& hash, const std::string& text, bool fromPreset){
    Meta parsed;
    std::string error;
    if(!parseMeta(text, parsed, error)){
        status = error;
        statusError = true;
        return;
    }
    meta = parsed;
    metaText = text;
    compiledCode = source;
    compiledChannels = buildingChannels;
    compiledVariables = buildingVariables;
    for(auto& d : meta.defs){
        if(!d.creator) continue;
        compiledChannels = d.creatorChannels;
        compiledVariables.clear();
        for(auto& v : d.variables) compiledVariables.push_back(v.value);
        break;
    }
    compiledHash = hash;
    compiledFolder = (fs::path(cacheRoot()) / hash).string();

    // "block 3 does not parse" -> with the line it starts on
    errors.clear();
    const std::regex blockRef("block (\\d+)");
    for(auto& e : meta.errors){
        std::smatch m;
        std::string annotated = e;
        if(std::regex_search(e, m, blockRef)){
            const int b = std::stoi(m[1].str()) - 1;
            if(b >= 0 && b < (int)blockLines.size())
                annotated = m.prefix().str() + "block " + m[1].str() + " (line " + ofToString(blockLines[b]) + ")" + m.suffix().str();
        }
        errors.push_back(annotated);
    }
    if(!logText.empty() && !meta.errors.empty()){
        for(auto& e : logErrors(logText)) if(!contains(errors, e)) errors.push_back(e);
    }

    if(meta.defs.empty()){
        status = "The code builds no SynthDef";
        statusError = true;
    }else{
        // Keep the choice if it is still there, else the first one that
        // makes sound into the graph
        bool found = false;
        for(auto& d : meta.defs) if(d.name == selectedDef) found = true;
        if(!found){
            selectedDef = meta.defs.front().name;
            for(auto& d : meta.defs){
                if(!busControls(d.outputs).empty()){ selectedDef = d.name; break; }
            }
        }
        status = ofToString(meta.defs.size()) + (meta.defs.size() == 1 ? " SynthDef" : " SynthDefs")
            + (errors.empty() ? "" : ", with errors");
        statusError = false;
    }

    const bool cached = fs::exists(fs::path(compiledFolder) / "meta.json");
    // Synths only from SynthDefs that exist: from a preset's description
    // alone, the compile below has to finish first
    defsBuilt = cached;
    if(cached) requireFolder();
    applyStructure();
    rebuildPending = true;
    // From a preset on a machine that never compiled this code: the shape is
    // already right, the SynthDefs come when the build finishes
    if(fromPreset && !cached) compile(source);
}

void scCodeNode::requireFolder(){
    for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(compiledFolder);
}

bool scCodeNode::defsLoading(ofxSCServer* server) const {
    // Only the server the synth goes to: a load pending on another one
    // used to hold this node back as well
    for(auto* sm : servers){
        if(!sm || (server && sm->getServer() != server)) continue;
        if(sm->areRequiredSynthdefsLoading()) return true;
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
// Shape of the node
// ════════════════════════════════════════════════════════════════════════════

const scCodeNode::DefDesc* scCodeNode::currentDef() const {
    for(auto& d : meta.defs) if(d.name == selectedDef) return &d;
    return nullptr;
}

const scCodeNode::ControlDesc* scCodeNode::findControl(const std::string& name) const {
    const DefDesc* def = currentDef();
    if(!def) return nullptr;
    for(auto& c : def->controls) if(c.name == name) return &c;
    return nullptr;
}

bool scCodeNode::voiceMode() const {
    const DefDesc* def = currentDef();
    if(!def) return false;
    if(mode == ModeVoices) return true;
    if(mode == ModePersistent) return false;
    return def->canFreeSynth && !def->functionPlay;
}

std::vector<std::string> scCodeNode::busControls(const std::vector<BusDesc>& list) const {
    std::vector<std::string> out;
    for(auto& b : list){
        if(b.rate != "audio" || b.control.empty()) continue;
        if(!contains(kInputTypes, b.type) && !contains(kOutputTypes, b.type)) continue;
        if(!contains(out, b.control)) out.push_back(b.control);
    }
    return out;
}

bool scCodeNode::isHiddenControl(const std::string& name) const {
    const DefDesc* def = currentDef();
    if(!def) return true;
    if(contains(busControls(def->inputs), name) || contains(busControls(def->outputs), name)) return true;
    if(name == "gate" && voiceMode()) return true;
    // OceanodeParameter.ar adds <name>_sel and <name>_ar (an audio-rate
    // modulation input of the regular SynthDef node): the slider is <name>
    for(const std::string suffix : {"_sel", "_ar"}){
        if(name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0){
            const std::string base = name.substr(0, name.size() - suffix.size());
            for(auto& c : def->controls) if(c.name == base) return true;
        }
    }
    return false;
}

std::string scCodeNode::uniqueName(const std::string& wanted, const std::string& keep) const {
    auto& group = const_cast<scCodeNode*>(this)->getParameterGroup();
    static const std::vector<std::string> reserved = {"Show", "Gate", "Trig", "Voices", "SC Code"};
    std::string name = wanted.empty() ? "Param" : wanted;
    auto taken = [&](const std::string& n){
        if(n == keep) return false;
        return group.contains(n) || contains(reserved, n);
    };
    if(!taken(name)) return name;
    for(int i = 2; ; i++){
        const std::string candidate = name + " " + ofToString(i);
        if(!taken(candidate)) return candidate;
    }
}

void scCodeNode::applyStructure(){
    const DefDesc* def = currentDef();
    const std::vector<std::string> wantIn = def ? busControls(def->inputs) : std::vector<std::string>();
    std::vector<std::string> wantOut;
    if(def) for(auto& c : busControls(def->outputs)) if(!contains(wantIn, c)) wantOut.push_back(c);
    const bool voices = voiceMode();

    // Parameter inlets sit after the audio inputs in availableInputs; scNode
    // removes an input by its index there, so they go first and come back
    // at the end
    for(auto& c : controls){
        availableInputs.erase(std::remove(availableInputs.begin(), availableInputs.end(), c->audioInput), availableInputs.end());
    }

    // ── Audio ports, kept (with their connections) while their control stays
    suppressInputRefresh = true;
    for(int i = (int)inputs.size() - 1; i >= 0; i--){
        if(i < (int)inputControls.size() && contains(wantIn, inputControls[i])) continue;
        scNode::removeInput(i);
        if(i < (int)inputControls.size()) inputControls.erase(inputControls.begin() + i);
    }
    for(auto& c : wantIn){
        if(contains(inputControls, c)) continue;
        scNode::addInput(uniqueName(displayName(c)));
        inputControls.push_back(c);
    }
    suppressInputRefresh = false;

    for(int i = (int)outputs.size() - 1; i >= 0; i--){
        if(i < (int)outputControls.size() && contains(wantOut, outputControls[i])) continue;
        scNode::removeOutput(i);
        if(i < (int)outputControls.size()) outputControls.erase(outputControls.begin() + i);
    }
    for(auto& c : wantOut){
        if(contains(outputControls, c)) continue;
        outputs.emplace_back(uniqueName(displayName(c)), nodePort((int)outputs.size(), this));
        addOutputParameter(outputs.back());
        outputControls.push_back(c);
    }
    // serverManager numbers a node's outputs by position (0, 1, 2...): a port
    // whose index no longer matches, after another was removed, would be
    // looked up under the wrong number and leave its destination on bus -1.
    // Setting it again also hands the corrected port to what it feeds.
    for(int i = 0; i < (int)outputs.size(); i++){
        if(outputs[i]->getIndex() != i) outputs[i] = nodePort(i, this);
    }

    // ── Control parameters, kept (value and connections) by control name
    std::vector<ControlDesc> want;
    if(def) for(auto& c : def->controls) if(!isHiddenControl(c.name)) want.push_back(c);
    for(auto it = controls.begin(); it != controls.end();){
        const bool keep = std::any_of(want.begin(), want.end(), [&](const ControlDesc& c){ return c.name == (*it)->desc.name; });
        if(keep){ ++it; continue; }
        removeParameter((*it)->param.getName());
        it = controls.erase(it);
    }
    std::vector<std::unique_ptr<ControlParam>> ordered;
    for(auto& c : want){
        auto existing = std::find_if(controls.begin(), controls.end(),
                                     [&](const std::unique_ptr<ControlParam>& p){ return p->desc.name == c.name; });
        float min = c.min, max = c.max;
        if(!c.hasSpec) guessRange(c, min, max);
        {
            std::string units = c.units;
            if(!units.empty() && units[0] == 'a') units = units.substr(1);
            if(units.rfind("d:", 0) == 0 || units.rfind("df:", 0) == 0){
                const auto options = ofSplitString(units.substr(units.find(':') + 1), ":", true);
                min = 0;
                max = std::max(1, (int)options.size() - 1);
            }
        }
        if(min > max) std::swap(min, max);
        if(min == max) max = min + 1;
        if(existing != controls.end()){
            auto cp = std::move(*existing);
            controls.erase(existing);
            cp->desc = c;
            cp->array = c.defaults.size() > 1;
            cp->trigger = c.rate == "trigger" || c.name.rfind("t_", 0) == 0;
            cp->param.setMin({min});
            cp->param.setMax({max});
            std::string name = cp->param.getName();
            parameterChangedMinMax.notify(name);
            cp->lastSent.clear();
            ordered.push_back(std::move(cp));
            continue;
        }
        auto cp = std::make_unique<ControlParam>();
        cp->desc = c;
        cp->array = c.defaults.size() > 1;
        cp->trigger = c.rate == "trigger" || c.name.rfind("t_", 0) == 0;
        // One value (a slider) when every element starts the same, as the
        // regular SynthDef node does with OceanodeParameter's default!n: it
        // is sent to every channel, and a connected vector sets them one by
        // one. Only defaults that differ per element start as a vector.
        const bool uniform = std::all_of(c.defaults.begin(), c.defaults.end(),
                                         [&](float v){ return v == c.defaults.front(); });
        const std::vector<float> start = uniform ? std::vector<float>{c.defaults.front()} : c.defaults;
        cp->param.set(uniqueName(displayName(c.name)), start, {min}, {max});
        auto registered = addParameter(cp->param);
        ControlParam* raw = cp.get();
        // An audio output plugged into the inlet: the graph re-routes
        // (the shared input, not `raw`: a parameter can outlive its record
        // while its connections are being taken down)
        registered->addReceiveFunc<nodePort>([this, input = cp->audioInput](const nodePort& port){
            *input = port;
            for(auto& output : outputs) output = output;
        });
        registered->addDisconnectFunc([this, input = cp->audioInput](){
            *input = nodePort();
            for(auto& output : outputs) output = output;
        });
        cp->listener = cp->param.newListener([this, raw](std::vector<float>&){ sendControl(*raw, false); });
        ordered.push_back(std::move(cp));
    }
    controls = std::move(ordered);
    for(int i = 0; i < (int)controls.size(); i++) controls[i]->index = i;
    for(auto& c : controls){
        c->audioRate = false;
        if(def) for(auto& other : def->controls) if(other.name == c->desc.name + "_ar") c->audioRate = true;
        availableInputs.push_back(c->audioInput);
    }

    setCreatorParameters(def && def->creator ? def : nullptr);
    setVoiceParameters(voices);
    reorderParameters();
}

void scCodeNode::setCreatorParameters(const DefDesc* def){
    if(!def){
        if(creatorParamsRegistered){
            removeParameter(numChannels.getName());
            for(auto& v : variableParams) removeParameter(v->getName());
            variableParams.clear();
            creatorParamsRegistered = false;
        }
        return;
    }
    // Shown with what the SynthDef was built for, without asking for a rebuild
    if(!creatorParamsRegistered){
        addParameter(numChannels);
        creatorParamsRegistered = true;
    }
    numChannels.setWithoutEventNotifications(def->creatorChannels);
    // Variables: kept (with their connections) while the names match
    bool same = variableParams.size() == def->variables.size();
    for(size_t i = 0; same && i < def->variables.size(); i++)
        same = variableParams[i]->getName() == def->variables[i].name;
    if(!same){
        for(auto& v : variableParams) removeParameter(v->getName());
        variableParams.clear();
        for(auto& var : def->variables){
            auto p = std::make_unique<ofParameter<int>>();
            p->set(uniqueName(var.name), var.value, 1, var.dim);
            addParameter(*p);
            listeners.push(p->newListener([this](int&){
                creatorSettingsDirty = true;
                creatorSettingsTime = ofGetElapsedTimef();
            }));
            variableParams.push_back(std::move(p));
        }
    }
    for(size_t i = 0; i < variableParams.size() && i < def->variables.size(); i++){
        variableParams[i]->setMax(def->variables[i].dim);
        variableParams[i]->setWithoutEventNotifications(def->variables[i].value);
    }
}

std::vector<int> scCodeNode::currentVariableValues() const {
    std::vector<int> values;
    for(auto& v : variableParams) values.push_back(v->get());
    return values;
}

void scCodeNode::setVoiceParameters(bool on){
    if(on == voiceParamsRegistered) return;
    if(on){
        addParameter(maxVoices);
        addParameter(gate);
        addParameter(trig);
    }else{
        removeParameter(maxVoices.getName());
        removeParameter(gate.getName());
        removeParameter(trig.getName());
        lastGate.clear();
    }
    voiceParamsRegistered = on;
}

void scCodeNode::reorderParameters(){
    auto& group = getParameterGroup();
    std::vector<std::string> order;
    auto append = [&](const std::string& name){
        if(group.contains(name) && !contains(order, name)) order.push_back(name);
    };
    append(showWindow.getName());
    if(creatorParamsRegistered){
        append(numChannels.getName());
        for(auto& v : variableParams) append(v->getName());
    }
    if(voiceParamsRegistered){
        append(maxVoices.getName());
        append(gate.getName());
        append(trig.getName());
    }
    for(auto& input : inputs) append(input.getName());
    for(auto& c : controls) append(c->param.getName());
    for(auto& output : outputs) append(output.getName());
    for(int i = 0; i < (int)group.size(); i++){
        const std::string name = group.get(i).getName();
        if(name != statusRegion.getName()) append(name);
    }
    append(statusRegion.getName());
    group.reorder(order);
    parameterGroupChanged.notify(this);
}

void scCodeNode::selectDef(const std::string& name){
    if(name == selectedDef) return;
    selectedDef = name;
    applyStructure();
    rebuildSynths(true);
}

void scCodeNode::setMode(int m){
    m = ofClamp(m, 0, 2);
    if(m == mode) return;
    mode = m;
    applyStructure();
    rebuildSynths(true);
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

bool scCodeNode::sendsNow(ServerState& st) const {
    return ofGetElapsedTimef() >= st.groupReadyTime;
}

float scCodeNode::laneValue(const std::vector<float>& v, int lane, float fallback) const {
    if(v.empty()) return fallback;
    return v[std::min(lane, (int)v.size() - 1)];
}

void scCodeNode::setOnSynth(ofxSCSynth* synth, const ControlParam& c, const std::vector<float>& value){
    if(!synth) return;
    if(c.array){
        std::vector<float> out(c.desc.defaults.size());
        for(size_t i = 0; i < out.size(); i++) out[i] = laneValue(value, (int)i, c.desc.defaults[i]);
        synth->set(c.desc.name, out);
    }else{
        synth->set(c.desc.name, (double)laneValue(value, 0, c.desc.defaults[0]));
    }
}

int scCodeNode::inputBusFor(ServerState& st, const std::string& control) const {
    for(size_t i = 0; i < inputs.size() && i < inputControls.size(); i++){
        if(inputControls[i] != control) continue;
        scNode* source = inputs[i]->getNodeRef();
        if(source){
            auto bus = st.inputBusByNode.find(source);
            if(bus != st.inputBusByNode.end()) return bus->second;
        }
        return st.silentBus;
    }
    return st.silentBus;
}

int scCodeNode::outputBusFor(ofxSCServer* server, ServerState& st, const std::string& control){
    for(size_t i = 0; i < outputs.size() && i < outputControls.size(); i++){
        if(outputControls[i] != control) continue;
        auto bus = st.outputBusByIndex.find(outputs[i]->getIndex());
        if(bus != st.outputBusByIndex.end()) return bus->second;
    }
    // A port the graph has not given a bus yet must not write to bus 0, the
    // hardware output: a private bus nobody reads
    if(!st.scratch) st.scratch = new ofxSCBus(RATE_AUDIO, kScratchChannels, server);
    return st.scratch->index;
}

void scCodeNode::sendBuses(ofxSCServer* server, ServerState& st, ofxSCSynth* synth){
    if(!synth) return;
    for(auto& c : outputControls) synth->set(c, outputBusFor(server, st, c));
    for(auto& c : inputControls){
        const int bus = inputBusFor(st, c);
        if(bus >= 0) synth->set(c, bus);
    }
}

void scCodeNode::setGroupControl(ofxSCServer* server, ServerState& st, const std::string& name, float value){
    if(!st.groupCreated || !st.group) return;
    ofxOscMessage m;
    m.setAddress("/n_set");
    m.addIntArg(st.group->nodeID);
    m.addStringArg(name);
    m.addFloatArg(value);
    server->sendMsg(m);
}

void scCodeNode::sendGroupMessage(ofxSCServer* server, ServerState& st, ofxOscMessage m){
    if(!st.groupCreated || !st.group) return;
    server->sendMsg(m);
}

void scCodeNode::routeControl(ofxSCServer* server, ServerState& st, ControlParam& c, int bus){
    const std::string& name = c.desc.name;
    const int n = std::max(1, (int)c.desc.defaults.size());
    const bool wasRouted = st.controlBus.count(name) > 0;
    if(bus >= 0) st.controlBus[name] = bus;
    else if(!wasRouted) return;
    else st.controlBus.erase(name);

    // The persistent synth and, through the group, every voice
    auto mapan = [&](ofxSCSynth* synth, int groupID, const std::string& control, int target){
        if(synth) synth->mapan(control, target, n);
        if(groupID >= 0){
            ofxOscMessage m;
            m.setAddress("/n_mapan");
            m.addIntArg(groupID);
            m.addStringArg(control);
            m.addIntArg(target);
            m.addIntArg(n);
            sendGroupMessage(server, st, m);
        }
    };
    const int groupID = (st.groupCreated && st.group && !st.voices.empty()) ? st.group->nodeID : -1;
    if(c.audioRate){
        // OceanodeParameter.ar: <name>_sel picks the audio input <name>_ar
        const int sel = bus >= 0 ? 1 : 0;
        if(st.synth) st.synth->set(name + "_sel", sel);
        if(groupID >= 0) setGroupControl(server, st, name + "_sel", sel);
        mapan(st.synth, groupID, name + "_ar", bus >= 0 ? bus : std::max(0, st.silentBus));
    }else{
        // Any other control: mapped to the bus itself; -1 unmaps it, and its
        // own value goes back
        mapan(st.synth, groupID, name, bus);
        if(bus < 0){
            if(st.synth) setOnSynth(st.synth, c, c.param.get());
            for(auto& v : st.voices) if(!v.stolen && v.synth && !v.synth->hasEnded()) setOnSynth(v.synth, c, c.param.get());
        }
    }
}

void scCodeNode::applyRoutes(ServerState& st, ofxSCSynth* synth){
    if(!synth) return;
    for(auto& c : controls){
        auto bus = st.controlBus.find(c->desc.name);
        if(bus == st.controlBus.end()) continue;
        const int n = std::max(1, (int)c->desc.defaults.size());
        if(c->audioRate){
            synth->set(c->desc.name + "_sel", 1);
            synth->mapan(c->desc.name + "_ar", bus->second, n);
        }else{
            synth->mapan(c->desc.name, bus->second, n);
        }
    }
}

void scCodeNode::createPersistent(ofxSCServer* server, ServerState& st){
    const DefDesc* def = currentDef();
    if(!def || !st.groupCreated) return;
    auto* fresh = new ofxSCSynth(def->defName, server);
    sendBuses(server, st, fresh);
    for(auto& c : controls) setOnSynth(fresh, *c, c->param.get());
    applyRoutes(st, fresh);
    const bool wasLatency = server->getBLatency();
    // Behind anything still timetagged for this group (its /g_new, the synth
    // being replaced)
    if(!sendsNow(st) || (st.synth && !st.synth->isCreated())) server->setBLatency(true);
    // Replace in place only a synth the server confirmed. One whose /s_new
    // failed (its SynthDef not loaded yet) does not exist there: replacing it
    // failed too, every time, and the group stayed empty for good.
    if(st.synth && st.synth->isCreated()){
        fresh->createAndRun(4, st.synth->nodeID, true);
    }else{
        if(st.synth) st.synth->free();   // in case it is only late
        fresh->createAndRun(0, st.group->nodeID, true);
    }
    server->setBLatency(wasLatency);
    delete st.synth;
    st.synth = fresh;
    st.synthSentAt = ofGetElapsedTimef();
    for(auto& c : controls) c->lastSent = c->param.get();
}

void scCodeNode::retryUnconfirmedSynths(){
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states){
        if(!st.synth || st.builtVoices || st.synth->isCreated() || st.synth->hasEnded()) continue;
        if(now - st.synthSentAt < 2.0f || defsLoading(server)) continue;
        ofLogWarning("scCodeNode") << "The server never started " << st.synth->getName() << "; sending it again";
        createPersistent(server, st);
    }
}

void scCodeNode::freeVoices(ServerState& st, bool sendFree){
    for(auto& v : st.voices){
        if(sendFree && v.synth && !v.synth->hasEnded()) v.synth->free();
        delete v.synth;
    }
    st.voices.clear();
}

void scCodeNode::freeState(ServerState& st){
    if(st.group && st.groupCreated) st.group->free();   // frees the synth and voices with it
    freeVoices(st, false);
    delete st.synth;
    st.synth = nullptr;
    delete st.group;
    st.group = nullptr;
    st.groupCreated = false;
    if(st.scratch){ st.scratch->free(); delete st.scratch; st.scratch = nullptr; }
    st.builtDef.clear();
}

void scCodeNode::rebuildSynths(bool force){
    for(auto& [server, st] : states) rebuildSynths(server, force);
}

void scCodeNode::rebuildSynths(ofxSCServer* server, bool force){
    auto it = states.find(server);
    if(it == states.end() || !it->second.groupCreated) return;
    auto& st = it->second;
    const DefDesc* def = currentDef();
    const bool voices = voiceMode();
    if(!force && def && st.builtDef == def->defName && st.builtVoices == voices) return;
    if(!def){
        // The code builds nothing playable any more: silence what ran before
        freeVoices(st, true);
        if(st.synth){ st.synth->free(); delete st.synth; st.synth = nullptr; }
        st.builtDef.clear();
        return;
    }
    if(defsLoading(server) || !defsBuilt){
        rebuildPending = true;
        return;
    }
    freeVoices(st, true);
    if(voices){
        if(st.synth){ st.synth->free(); delete st.synth; st.synth = nullptr; }
    }else{
        createPersistent(server, st);
    }
    st.builtDef = def->defName;
    st.builtVoices = voices;
}

void scCodeNode::sendControl(ControlParam& c, bool force){
    const auto& value = c.param.get();
    const bool changed = force || c.trigger || value != c.lastSent;
    for(auto& [server, st] : states){
        if(st.synth && changed) setOnSynth(st.synth, c, value);
        for(auto& voice : st.voices){
            if(voice.stolen || !voice.synth || voice.synth->hasEnded()) continue;
            if(c.array){
                if(changed) setOnSynth(voice.synth, c, value);
                continue;
            }
            const float x = laneValue(value, voice.lane, c.desc.defaults[0]);
            if(c.index < (int)voice.sent.size() && !force && !c.trigger && voice.sent[c.index] == x) continue;
            voice.synth->set(c.desc.name, (double)x);
            if(c.index < (int)voice.sent.size()) voice.sent[c.index] = x;
        }
    }
    c.lastSent = value;
}

// ════════════════════════════════════════════════════════════════════════════
// Graph
// ════════════════════════════════════════════════════════════════════════════

void scCodeNode::buildSynth(ofxSCServer* server){
    if(!server) return;
    auto it = states.find(server);
    if(it != states.end()) freeState(it->second);
    states[server].group = new ofxSCGroup(server);
}

void scCodeNode::createSynth(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end() || !it->second.group) return;
    auto& st = it->second;
    st.group->create(0, 1);
    st.groupCreated = true;
    st.groupReadyTime = ofGetElapsedTimef() + (server->getBLatency() ? server->getLatency() + 0.05f : 0.0f);
    if(!getActive()) st.group->run(false);
    st.builtDef.clear();
    rebuildSynths(server, true);
}

void scCodeNode::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto it = states.find(server);
    if(it == states.end() || !it->second.groupCreated) return;
    auto& st = it->second;
    sendBuses(server, st, st.synth);
    for(auto& c : outputControls) setGroupControl(server, st, c, outputBusFor(server, st, c));
    for(auto& c : inputControls){ const int bus = inputBusFor(st, c); if(bus >= 0) setGroupControl(server, st, c, bus); }
    st.group->moveBefore(nodeID);
}

void scCodeNode::free(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return;
    freeState(it->second);
    states.erase(it);
}

void scCodeNode::setOutputBus(ofxSCServer* server, int index, int bus){
    auto& st = states[server];
    st.outputBusByIndex[index] = bus;
    for(size_t i = 0; i < outputs.size() && i < outputControls.size(); i++){
        if(outputs[i]->getIndex() != index) continue;
        const std::string& control = outputControls[i];
        st.outputBus[control] = bus;
        if(st.synth){
            const ControlDesc* c = findControl(control);
            // An .ir bus control only takes effect when the synth is made
            if(c && c->rate == "scalar" && st.synth->isCreated()) createPersistent(server, st);
            else st.synth->set(control, bus);
        }
        setGroupControl(server, st, control, bus);
    }
}

void scCodeNode::setInputBus(ofxSCServer* server, scNode* node, int bus){
    auto& st = states[server];
    st.inputBusByNode[node] = bus;
    for(auto& c : controls){
        if(c->audioInput->getNodeRef() == node) routeControl(server, st, *c, bus);
    }
    for(size_t i = 0; i < inputs.size() && i < inputControls.size(); i++){
        if(inputs[i]->getNodeRef() != node) continue;
        if(st.synth) st.synth->set(inputControls[i], bus);
        setGroupControl(server, st, inputControls[i], bus);
    }
}

void scCodeNode::resetInputBusses(ofxSCServer* server, int targetBus){
    auto& st = states[server];
    st.inputBusByNode.clear();
    st.silentBus = targetBus;
    for(auto& c : controls){
        if(st.controlBus.count(c->desc.name)) routeControl(server, st, *c, -1);
    }
    for(auto& c : inputControls){
        if(st.synth) st.synth->set(c, targetBus);
        setGroupControl(server, st, c, targetBus);
    }
}

int scCodeNode::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = states.find(server);
    if(it == states.end()) return -1;
    auto bus = it->second.outputBusByIndex.find(index);
    return bus == it->second.outputBusByIndex.end() ? -1 : bus->second;
}

int scCodeNode::getLastSynthID(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end() || !it->second.groupCreated || !it->second.group) return -1;
    return it->second.group->nodeID;
}

void scCodeNode::resendParametersForNRT(){
    for(auto& [server, st] : states) sendBuses(server, st, st.synth);
    for(auto& c : controls) sendControl(*c, true);
}

void scCodeNode::activate(){
    for(auto& [server, st] : states) if(st.groupCreated) st.group->run(true);
}

void scCodeNode::deactivate(){
    for(auto& [server, st] : states) if(st.groupCreated) st.group->run(false);
}

// ════════════════════════════════════════════════════════════════════════════
// Voices
// ════════════════════════════════════════════════════════════════════════════

int scCodeNode::liveVoiceCount() const {
    // The first server's count: every server plays the same voices
    if(states.empty()) return 0;
    int n = 0;
    for(auto& v : states.begin()->second.voices) if(!v.stolen && v.synth && !v.synth->hasEnded()) n++;
    return n;
}

void scCodeNode::onGate(std::vector<float>& values){
    const size_t lanes = std::min<size_t>(std::max(values.size(), lastGate.size()), MAX_LANES);
    lastGate.resize(lanes, 0.0f);
    for(size_t lane = 0; lane < lanes; lane++){
        const float v = lane < values.size() ? values[lane] : 0.0f;
        const bool was = lastGate[lane] > 0, now = v > 0;
        lastGate[lane] = v;
        if(now && !was) spawn((int)lane, v);
        else if(!now && was) release((int)lane);
    }
}

void scCodeNode::spawn(int lane, float gateValue, float autoRelease){
    const DefDesc* def = currentDef();
    if(!def || !getActive() || !defsBuilt || defsLoading()) return;
    const int cap = ofClamp(maxVoices.get(), 1, MAX_VOICES);
    const float now = ofGetElapsedTimef();
    const uint64_t id = nextVoiceId++;
    for(auto& [server, st] : states){
        if(!st.groupCreated || st.builtDef != def->defName || !st.builtVoices) continue;
        // Over the cap: steal the oldest, released ones first
        int live = 0;
        for(auto& v : st.voices) if(!v.stolen && !v.synth->hasEnded()) live++;
        while(live >= cap){
            Voice* victim = nullptr;
            for(auto& v : st.voices){
                if(v.stolen || v.synth->hasEnded()) continue;
                if(!victim || (v.released && !victim->released) ||
                   (v.released == victim->released && v.started < victim->started)) victim = &v;
            }
            if(!victim) break;
            if(def->hasGate && victim->synth->isCreated()) victim->synth->set("gate", (double)kStealRelease);
            else victim->synth->free();
            victim->stolen = true;
            live--;
        }

        Voice v;
        v.synth = new ofxSCSynth(def->defName, server);
        v.lane = lane;
        v.id = id;
        v.started = now;
        sendBuses(server, st, v.synth);
        v.sent.assign(controls.size(), 0.0f);
        for(auto& c : controls){
            if(c->array){ setOnSynth(v.synth, *c, c->param.get()); continue; }
            const float x = laneValue(c->param.get(), lane, c->desc.defaults[0]);
            v.synth->set(c->desc.name, (double)x);
            v.sent[c->index] = x;
        }
        applyRoutes(st, v.synth);
        if(def->hasGate) v.synth->set("gate", (double)gateValue);
        const bool wasLatency = server->getBLatency();
        if(!sendsNow(st)) server->setBLatency(true);
        v.synth->createAndRun(1, st.group->nodeID, true);   // tail: voices sum in start order
        server->setBLatency(wasLatency);
        st.voices.push_back(v);
    }
    if(autoRelease >= 0 && def->hasGate) pendingReleases.push_back({id, now + autoRelease});
}

void scCodeNode::release(int lane){
    const DefDesc* def = currentDef();
    if(!def || !def->hasGate) return;
    for(auto& [server, st] : states){
        for(auto& v : st.voices){
            if(v.lane != lane || v.released || v.stolen || v.synth->hasEnded()) continue;
            v.synth->set("gate", 0.0);   // stored until /n_go if not running yet
            v.released = true;
        }
    }
}

void scCodeNode::reapVoices(){
    const float now = ofGetElapsedTimef();
    for(auto it = pendingReleases.begin(); it != pendingReleases.end();){
        if(now < it->time){ ++it; continue; }
        for(auto& [server, st] : states){
            for(auto& v : st.voices){
                if(v.id != it->id || v.released || v.stolen || v.synth->hasEnded()) continue;
                v.synth->set("gate", 0.0);
                v.released = true;
            }
        }
        it = pendingReleases.erase(it);
    }
    for(auto& [server, st] : states){
        for(auto it = st.voices.begin(); it != st.voices.end();){
            const bool ended = it->synth->hasEnded();
            const bool neverStarted = !it->synth->isCreated() && !ended && now - it->started > kVoiceStartTimeout;
            if(ended || neverStarted){
                delete it->synth;
                it = st.voices.erase(it);
            }else ++it;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════

void scCodeNode::update(ofEventArgs&){
    // A new node compiles its example; a loaded one already has its code
    if(firstUpdate){
        firstUpdate = false;
        if(compiledCode.empty() && !buildTask.valid() && !ofxOceanodeShared::isPresetLoading()) compile(editorText);
    }

    if(pendingOpenDialog){
        pendingOpenDialog = false;
        auto r = ofSystemLoadDialog("Open SuperCollider code", false, ofToDataPath("Supercollider/codeScripts", true));
        if(r.bSuccess) loadScriptFile(r.filePath);
    }
    if(pendingSaveDialog){
        pendingSaveDialog = false;
        auto r = ofSystemSaveDialog((saveName.empty() ? std::string("code") : saveName) + ".scd", "Export SuperCollider code");
        if(r.bSuccess) saveScriptFile(r.filePath);
    }
    if(!pendingScriptPath.empty()){
        const std::string path = pendingScriptPath;
        pendingScriptPath.clear();
        loadScriptFile(path);
    }
    if(editorFocusCompile){
        editorFocusCompile = false;
        compile(editorText);
    }

    ai.update();

    if(buildTask.valid() && buildTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        finishCompile(buildTask.get());
        if(hasQueuedSource){
            hasQueuedSource = false;
            compile(queuedSource);
        }
    }

    if(conversionTask.valid() && conversionTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        auto result = conversionTask.get();
        if(!result.error.empty()){
            promoteStatus = "AI: " + result.error;
            promoteStatusError = true;
        }else{
            promoteSource = stripFences(result.text);
            std::smatch m;
            if(std::regex_search(promoteSource, m, std::regex("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(\\s*\"([^\"]+)\"")))
                promoteName = m[1].str();
            if(std::regex_search(promoteSource, m, std::regex("category:\\s*\"([^\"]*)\"")))
                promoteCategory = m[1].str();
            promoteStatus = "Converted. Read it over, then Save, compile & install.";
            promoteStatusError = false;
        }
    }
    if(installTask.valid() && installTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        finishInstall(installTask.get());
    }

    // N Chan / a variable changed: rebuild once it has settled (not on every
    // step of a drag)
    if(creatorSettingsDirty && ofGetElapsedTimef() - creatorSettingsTime > 0.4f && !ImGui::IsMouseDown(0)){
        creatorSettingsDirty = false;
        if(creatorParamsRegistered && !compiledCode.empty() &&
           (numChannels.get() != compiledChannels || currentVariableValues() != compiledVariables)){
            compile(compiledCode);
        }
    }

    if(rebuildPending){
        rebuildPending = false;
        rebuildSynths();   // each server goes ahead once its own loads are done
    }
    retryUnconfirmedSynths();
    // A promoted synth: nodes made from it switch to the new build once
    // the server has loaded it
    if(!pendingReload.empty() && !defsLoading()){
        const int count = scSynthdef::reloadAll(pendingReload);
        if(count > 0) promoteStatus += "\nReloaded " + ofToString(count) + (count == 1 ? " node" : " nodes") + " already using it.";
        pendingReload.clear();
    }
    reapVoices();
}

void scCodeNode::draw(ofEventArgs&){
    if(showWindow) drawWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scCodeNode::presetSave(ofJson& json){
    json["scCode"] = {
        {"editor", editorText},
        {"code", compiledCode},
        {"meta", metaText},
        {"def", selectedDef},
        {"mode", mode},
        {"channels", compiledChannels},
        {"file", sourceFile},
        {"variables", compiledVariables}
    };
    ai.save(json);
}

void scCodeNode::loadBeforeConnections(ofJson& json){
    // Ports and parameters must exist before connections are restored and
    // values are set: rebuilt from the saved description, no compile needed
    if(!json.contains("scCode")) return;
    const auto& s = json["scCode"];
    firstUpdate = false;
    editorText = s.value("editor", editorText);
    selectedDef = s.value("def", "");
    mode = ofClamp(s.value("mode", (int)ModeAuto), 0, 2);
    const std::string code = s.value("code", "");
    const std::string savedMeta = s.value("meta", "");
    if(code.empty()) return;
    blockLines.clear();
    for(auto& block : splitBlocks(code)) blockLines.push_back(block.first);
    sourceFile = s.value("file", "");
    buildingChannels = std::max(1, s.value("channels", 1));
    buildingVariables = s.value("variables", std::vector<int>());
    compiledChannels = buildingChannels;
    // The cache's own description wins over the preset's: it names the
    // SynthDefs that are actually on disk. A preset saved under another cache
    // key (an older version of this node, say) carries other names, and the
    // node asked the server for a SynthDef that was never loaded.
    const std::string key = compileKey(code, buildingChannels, buildingVariables);
    const fs::path cachedMeta = fs::path(cacheRoot()) / key / "meta.json";
    if(fs::exists(cachedMeta)) applyMeta(code, key, readFile(cachedMeta.string()), true);
    else if(!savedMeta.empty()) applyMeta(code, key, savedMeta, true);   // shape now, SynthDefs once compiled
    else compile(code);
}

void scCodeNode::presetRecallAfterSettingParameters(ofJson& json){
    ai.load(json);
    // Presets from before parameters started as one value hold n equal
    // values: back to a single slider unless a vector is plugged in
    for(auto& c : controls){
        const auto& v = c->param.get();
        if(v.size() < 2 || !std::all_of(v.begin(), v.end(), [&](float x){ return x == v.front(); })) continue;
        if(!std::all_of(c->desc.defaults.begin(), c->desc.defaults.end(), [&](float x){ return x == c->desc.defaults.front(); })) continue;
        if(getOceanodeParameter(c->param).hasInConnection()) continue;
        c->param = std::vector<float>{v.front()};
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Scripts
// ════════════════════════════════════════════════════════════════════════════

std::string scCodeNode::scriptsFolder(){
    return ofToDataPath("Supercollider/codeScripts", true);
}

void scCodeNode::refreshScriptList(){
    scriptFiles.clear();
    const fs::path folder = scriptsFolder();
    try {
        fs::create_directories(folder);
        // The bundled examples are copied in once, when the collection is
        // empty, so every script lives in one folder the user owns
        bool empty = true;
        for(auto& f : fs::recursive_directory_iterator(folder)) if(f.path().extension() == ".scd"){ empty = false; break; }
        const std::string runner = findRunnerFolder();
        const fs::path examples = fs::path(runner) / "examples";
        if(empty && !runner.empty() && fs::exists(examples)){
            for(auto& f : fs::recursive_directory_iterator(examples)){
                if(!f.is_regular_file() || f.path().extension() != ".scd") continue;
                const fs::path target = folder / "examples" / fs::relative(f.path(), examples);
                fs::create_directories(target.parent_path());
                fs::copy_file(f.path(), target, fs::copy_options::skip_existing);
            }
        }
        // Subfolders organise the collection: shown as "folder/name"
        for(auto& f : fs::recursive_directory_iterator(folder))
            if(f.is_regular_file() && f.path().extension() == ".scd") scriptFiles.push_back(f.path().string());
    } catch(const std::exception& e) {
        ofLogError("scCodeNode") << "Reading " << folder << ": " << e.what();
    }
    std::sort(scriptFiles.begin(), scriptFiles.end());
    selectedScript = ofClamp(selectedScript, 0, std::max(0, (int)scriptFiles.size() - 1));
    scriptListDirty = false;
}

std::string scCodeNode::scriptLabel(const std::string& path){
    std::error_code ec;
    fs::path relative = fs::relative(path, scriptsFolder(), ec);
    // Outside the collection (a library source, an opened file): its name only
    if(ec || relative.empty() || relative.begin()->string() == "..") relative = fs::path(path).filename();
    relative.replace_extension();
    return relative.generic_string();
}

void scCodeNode::saveToCollection(){
    // Letters, digits, - _ and / for subfolders; nothing that leaves the folder
    std::string name;
    for(char ch : ofTrim(saveName)) name += (std::isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '/' || ch == ' ') ? ch : '_';
    while(!name.empty() && name.front() == '/') name.erase(name.begin());
    if(name.empty() || name.find("..") != std::string::npos){
        status = "Give the script a name";
        statusError = true;
        return;
    }
    const fs::path file = fs::path(scriptsFolder()) / (name + ".scd");
    try { fs::create_directories(file.parent_path()); } catch(...) {}
    saveScriptFile(file.string());
    saveName = name;
}

void scCodeNode::loadScriptFile(const std::string& path){
    if(!fs::exists(path)){
        status = "Cannot open " + path;
        statusError = true;
        return;
    }
    editorText = readFile(path);
    saveName = scriptLabel(path);
    sourceFile = fs::absolute(path).string();
    compile(editorText);
}

void scCodeNode::saveScriptFile(const std::string& path){
    std::ofstream file(path);
    if(!file){
        status = "Cannot save " + path;
        statusError = true;
        return;
    }
    file << editorText;
    status = "Saved " + ofFilePath::getFileName(path);
    statusError = false;
    scriptListDirty = true;
}

// ════════════════════════════════════════════════════════════════════════════
// GUI
// ════════════════════════════════════════════════════════════════════════════

int scCodeNode::resizeCallback(ImGuiInputTextCallbackData* data){
    if(data->EventFlag == ImGuiInputTextFlags_CallbackResize){
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(data->BufTextLen);
        data->Buf = text->data();
    }
    return 0;
}

int scCodeNode::editorCallback(ImGuiInputTextCallbackData* data){
    auto* self = static_cast<scCodeNode*>(data->UserData);
    if(data->EventFlag == ImGuiInputTextFlags_CallbackResize){
        self->editorText.resize(data->BufTextLen);
        data->Buf = self->editorText.data();
    }else if(data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter){
        // Cmd+Return / Shift+Return compiles, as evaluating in the SC IDE
        const ImGuiIO& io = ImGui::GetIO();
        if(data->EventChar == '\n' && (io.KeySuper || io.KeyShift)){
            self->editorFocusCompile = true;
            return 1;
        }
    }
    return 0;
}

void scCodeNode::drawStatusRegion(){
    const DefDesc* def = currentDef();
    std::string line;
    if(buildTask.valid()) line = "compiling...";
    else if(def && rebuildPending){
        line = def->name + "  waiting for the server";
    }
    else if(def){
        line = def->name + (voiceMode() ? "  voices" : "");
        if(voiceMode()) line += " " + ofToString(liveVoiceCount());
    }
    else line = statusError ? "error" : "no SynthDef";
    if(editorText != compiledCode && !compiledCode.empty()) line += "  (edited)";
    ImGui::PushStyleColor(ImGuiCol_Text, statusError ? IM_COL32(255, 110, 100, 255) : IM_COL32(150, 200, 150, 255));
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();
}

void scCodeNode::drawToolbar(){
    const bool edited = editorText != compiledCode;
    if(edited) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 110, 60, 255));
    if(ImGui::Button(buildTask.valid() ? "Compiling..." : "Compile")) compile(editorText);
    if(edited) ImGui::PopStyleColor();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Run the code without a server and keep its SynthDefs (Cmd+Return in the editor)");
    ImGui::SameLine();
    if(ImGui::Button("Open")) pendingOpenDialog = true;
    ImGui::SameLine();
    if(ImGui::Button("Save")){
        if(saveName.empty()) saveName = selectedDef.empty() ? "code" : selectedDef;
        ImGui::OpenPopup("Save script");
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Save into data/Supercollider/codeScripts (the dropdown's collection)");
    if(ImGui::BeginPopup("Save script")){
        ImGui::TextDisabled("data/Supercollider/codeScripts/");
        ImGui::SetNextItemWidth(260);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##savename", saveName.data(), saveName.capacity() + 1,
            ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue, &scCodeNode::resizeCallback, &saveName);
        ImGui::SameLine();
        ImGui::TextDisabled(".scd");
        const bool exists = fs::exists(fs::path(scriptsFolder()) / (ofTrim(saveName) + ".scd"));
        if(exists) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Replaces the existing script");
        ImGui::TextDisabled("Use a/b to save into a subfolder");
        if(ImGui::Button(exists ? "Replace" : "Save") || enter){
            saveToCollection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("Export...")){
            pendingSaveDialog = true;
            ImGui::CloseCurrentPopup();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Save a copy anywhere with the system dialog");
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if(scriptListDirty) refreshScriptList();
    if(!scriptFiles.empty()){
        ImGui::SetNextItemWidth(180);
        const std::string current = scriptLabel(scriptFiles[selectedScript]);
        if(ImGui::BeginCombo("##scripts", current.c_str())){
            for(int i = 0; i < (int)scriptFiles.size(); i++){
                const std::string name = scriptLabel(scriptFiles[i]);
                if(ImGui::Selectable((name + "##" + ofToString(i)).c_str(), i == selectedScript)){
                    selectedScript = i;
                    pendingScriptPath = scriptFiles[i];
                }
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Scripts in data/Supercollider/codeScripts");
        if(ImGui::IsItemClicked()) scriptListDirty = true;   // files added from outside appear
        ImGui::SameLine();
    }
    if(ImGui::Button("Library...")){
        library = scanLibrary();
        libraryFilter.clear();
        libraryOpenRequest = true;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Open one of Oceanode's own SynthDef sources (~synthCreator) to refine it");
    ImGui::SameLine();
    ai.drawButton(ImVec2(34, 0));
    ImGui::SameLine();
    const bool canPromote = currentDef() != nullptr;
    if(!canPromote) ImGui::BeginDisabled();
    if(ImGui::Button("Promote...")) openPromote();
    if(!canPromote) ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Turn this SynthDef into a regular Oceanode node (~synthCreator source, compiled and installed)");
}

void scCodeNode::fixHardwareOut(){
    const DefDesc* def = currentDef();
    const bool hasOut = def && std::any_of(def->controls.begin(), def->controls.end(),
                                           [](const ControlDesc& c){ return c.name == "out"; });
    // Out.ar(0, sig) -> Out.ar(\out.kr(0), sig), or the existing `out` argument
    const std::regex hardware("\\b(Out|OffsetOut|ReplaceOut|XOut)\\.ar\\(\\s*(\\d+)\\s*,");
    std::string result;
    auto begin = std::sregex_iterator(editorText.begin(), editorText.end(), hardware);
    size_t last = 0;
    int count = 0;
    for(auto it = begin; it != std::sregex_iterator(); ++it){
        const auto& m = *it;
        result += editorText.substr(last, m.position() - last);
        result += m[1].str() + ".ar(" + (hasOut ? std::string("out") : "\\out.kr(" + m[2].str() + ")") + ",";
        last = m.position() + m.length();
        count++;
    }
    result += editorText.substr(last);
    if(count == 0) return;
    editorText = result;
    compile(editorText);
}

void scCodeNode::drawEditor(ImVec2 size){
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(16, 20, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(205, 225, 190, 255));
    ImGui::InputTextMultiline("##sccode", editorText.data(), editorText.capacity() + 1, size,
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter,
        &scCodeNode::editorCallback, this);
    ImGui::PopStyleColor(2);
}

void scCodeNode::drawWindow(){
    const std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "SC Code " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open)){
        ImGui::PushID(this);
        drawToolbar();

        // ── SynthDef and mode ────────────────────────────────────────────────
        const DefDesc* def = currentDef();
        ImGui::SetNextItemWidth(200);
        if(ImGui::BeginCombo("SynthDef", def ? def->name.c_str() : "-")){
            for(auto& d : meta.defs){
                if(ImGui::Selectable(d.name.c_str(), d.name == selectedDef)) selectDef(d.name);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        int m = mode;
        if(ImGui::Combo("Mode", &m, kModeNames, 3)) setMode(m);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto: voices for a SynthDef that frees itself, else one persistent synth");
        def = currentDef();
        if(def){
            ImGui::SameLine();
            std::string info = voiceMode() ? (def->hasGate ? "voices, gated" : "voices, one-shot") : "persistent";
            int outChannels = 0;
            for(auto& b : def->outputs) if(b.rate == "audio" && !b.control.empty()) outChannels = std::max(outChannels, b.channels);
            if(outChannels) info += "  ·  " + ofToString(outChannels) + " ch out";
            if(voiceMode()) info += "  ·  " + ofToString(liveVoiceCount()) + " playing";
            ImGui::TextDisabled("%s", info.c_str());
            if(def->creator){
                ImGui::TextDisabled("Oceanode source  ·  %s  ·  %s", def->category.c_str(), def->description.c_str());
            }

            // Writes to a fixed bus: it would sound outside the graph
            std::vector<int> fixed;
            for(auto& b : def->outputs) if(b.rate == "audio" && b.fixed && contains(kOutputTypes, b.type)) fixed.push_back(b.number);
            if(!fixed.empty()){
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 200, 90, 255));
                ImGui::TextWrapped("Writes straight to hardware bus %d, outside the Oceanode graph.", fixed.front());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if(ImGui::SmallButton("Fix")) fixHardwareOut();
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Out.ar(0, ...) -> Out.ar(\\out.kr(0), ...) in the code, then compile");
            }
            if(voiceMode() && !def->canFreeSynth){
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 200, 90, 255));
                ImGui::TextWrapped("This SynthDef never frees itself: each voice plays until it is stolen.");
                ImGui::PopStyleColor();
            }
        }

        // ── Errors ───────────────────────────────────────────────────────────
        for(auto& e : errors){
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
            ImGui::TextWrapped("%s", e.c_str());
            ImGui::PopStyleColor();
        }

        // ── Editor ───────────────────────────────────────────────────────────
        const float footer = ImGui::GetFrameHeightWithSpacing() + (showLog ? 160.0f : 0.0f);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        drawEditor(ImVec2(avail.x, std::max(120.0f, avail.y - footer)));

        // ── Status and log ───────────────────────────────────────────────────
        ImGui::Checkbox("Log", &showLog);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, statusError ? IM_COL32(255, 110, 100, 255) : IM_COL32(140, 200, 140, 255));
        std::string line = status;
        if(editorText != compiledCode && !compiledCode.empty()) line += "   (edited, not compiled)";
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        if(!ai.getStatus().empty()){
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ai.getStatus().c_str());
        }
        if(showLog){
            ImGui::BeginChild("##sclog", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(logText.empty() ? "(no log)" : logText.c_str());
            ImGui::EndChild();
        }

        ai.drawPopup();
        drawPromotePopup();
        drawLibraryPopup();
        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

// ════════════════════════════════════════════════════════════════════════════
// AI
// ════════════════════════════════════════════════════════════════════════════

std::string scCodeNode::buildSystemPrompt(){
    return
R"SYS(You write SuperCollider (sclang) code for the "SC Code" node of Oceanode, a modular environment that hosts SuperCollider synths.

Output ONLY sclang code: no markdown, no code fences, no prose before or after.

How the node uses the code:
- It runs the code WITHOUT a server and keeps every SynthDef the code builds (SynthDef(...).add, .store, .send, {}.play). Patterns, Routines, GUI and server commands are ignored, so do not rely on them.
- The user picks one SynthDef. Audio-rate bus controls become audio ports: `Out.ar(\out.kr(0), sig)` or an `out` argument is the output port, `In.ar(\in.kr(0), 2)` an input port (effects). NEVER write to a fixed bus number such as Out.ar(0, ...).
- Every other control becomes a slider parameter. Give its range in ONE place only, on the control itself: `\cutoff.kr(1200, spec: ControlSpec(20, 20000, \exp))`. Do NOT also add `metadata: (specs: ...)` to the SynthDef: giving a range both ways makes SuperCollider 3.14 fail ("Message 'setFrom' not understood"). Standard names (freq, amp, pan) already have ranges.
- Pitched feedback loops (waveguides, Karplus-Strong): LocalIn/LocalOut add one control block of delay (ControlDur.ir, 64 samples), and a loop that subtracts its feedback (x - loopIn) sounds an octave lower. Subtract ControlDur.ir from the delay time (and halve the period for an inverting loop), clip the pitch range so the delay stays positive, or prefer CombL / Pluck / DWG* UGens, which have no block delay. Check the result would sound at the requested pitch.
- Write code for SuperCollider 3.14 using only core UGens and sc3-plugins. Check that every variable is declared at the top of its function (sclang requires all `var` declarations before any other statement).
- A SynthDef that frees itself (doneAction: 2 on an envelope, DetectSilence) is played as polyphonic voices: each rising Gate lane starts one, with that lane's parameter values; if it has a `gate` argument, the falling edge sets gate 0 (use Env.adsr / Env.asr with doneAction: 2 for sustained notes, Env.perc for one-shots).
- A SynthDef that does not free itself runs as one persistent synth (effects, drones). Effects must not free themselves.
- Keep amplitudes sane (around 0.1-0.5 peak) and avoid unbounded feedback.
- Wrap the code in one ( ... ) block. Several SynthDefs are fine.)SYS";
}

void scCodeNode::setupAIAssistant(){
    ofxOceanodeAIAssistant::Spec spec;
    spec.title = "Write SuperCollider code with Requesty";
    spec.subtitle = [this](){
        const DefDesc* def = currentDef();
        return def ? "Current SynthDef: " + def->name + (voiceMode() ? " (voices)" : " (persistent)") : std::string("No SynthDef compiled yet");
    };
    spec.generateHint = "Describe the synth or effect you want (or paste code to adapt in the request).";
    spec.fixHint = "Describe the issue, or leave empty to fix the compile errors and adapt the code for Oceanode.";
    spec.pendingHint = "The result replaces the code in the editor and is compiled.";
    spec.readyMessage = "AI code compiled";
    spec.systemPrompt = &scCodeNode::buildSystemPrompt;
    spec.buildPrompt = [this](const std::string& request, bool fixMode){
        if(!fixMode) return "Write SuperCollider code for the SC Code node.\n\n" + request;
        std::string prompt = "Fix this SuperCollider code and adapt it for the SC Code node.\n\nCurrent code:\n" + editorText + "\n";
        if(!errors.empty()){
            prompt += "\nCompile errors:\n";
            for(auto& e : errors) prompt += "- " + e + "\n";
        }
        const DefDesc* def = currentDef();
        if(def){
            for(auto& b : def->outputs) if(b.fixed)
                prompt += "\nSynthDef " + def->name + " writes to the fixed bus " + ofToString(b.number) + ": route it through an out control instead.\n";
        }
        prompt += "\n" + (request.empty() ? std::string("Fix every problem.") : "Issue: " + request);
        return prompt;
    };
    spec.apply = [this](const std::string& code) -> std::string {
        editorText = code + "\n";
        compile(editorText);
        return "";
    };
    ai.setup(spec);
}

// ════════════════════════════════════════════════════════════════════════════
// Promote
// ════════════════════════════════════════════════════════════════════════════

std::string scCodeNode::promoteSystemPrompt(){
    return
R"SYS(You convert a tested SuperCollider SynthDef into an Oceanode SynthDef source file, the format of Oceanode's own library.

Output ONLY the .scd file contents: no markdown, no code fences, no prose.

The format, exactly:
(
~synthCreator.value("Name", {|n|
	var ...;
	// parameters
	...
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "One line describing the sound", category: "Source/Something");
)

Rules:
- n is the number of channels. Every parameter is a vector of n values and the synth makes n independent channels (multichannel expansion). The output has n channels: do not mix them down, do not Pan2/Splay to 2 channels; a stereo original becomes n channels (e.g. pan the source of each channel, or drop the panning).
- Parameters: OceanodeParameter.ar(\name, default, n, min, max, "vf") (audio-rate vector) or .kr(...) when it only changes slowly; "vf" = float vector, "vi" = integer vector. OceanodeParameterLag.ar(\name, default, n, min, max, "vf", lagTime, true) for smoothed ones. OceanodeParameterDropdown.kr(\name, default, n, "A:B:C") for a choice.
- Effects read their input with In.ar(OceanodeInput.kr(\in), n).
- Oceanode synths are PERSISTENT: they never free themselves. Replace doneAction: 2 / DetectSilence with doneAction: 0. A one-shot becomes a trigger: gate = OceanodeParameter.ar(\gate, 0, n, 0, 1, "vi"); EnvGen.ar(Env.perc(...), gate) retriggers on each rising edge. A sustained instrument uses the same gate as the EnvGen gate of an ADSR (no doneAction).
- Pitch: prefer a MIDI \pitch parameter (24..96) followed by .midicps over a raw frequency; levels: an \amp or \level parameter 0..1.
- Use the defaults you are given (the values the user is hearing) as the parameter defaults, within sensible min/max ranges.
- Parameter names: lowercase, no spaces. Name: CamelCase identifier. Category: one of the given folders, e.g. "Source/Percussion", "Source/Special", "Effect/Filter", "Effect/Time".
- Keep the sound of the original. Add tanh or a limiter only if the original can blow up.

Example (instrument):
(
~synthCreator.value("Kickdrumb", {|n|
	var body, bodyfreq, bodyamp, gate, pitch, pdec, adec, tune, amp;
	gate = OceanodeParameter.ar(\gate, 0, n, 0, 1, "vi");
	pitch = OceanodeParameterLag.ar(\pitch, 36, n, 24, 60, "vf", 1/30, true).midicps;
	pdec = OceanodeParameter.ar(\pitchdecay, 4, n, 0.1, 8, "vf");
	adec = OceanodeParameter.ar(\ampdecay, 4, n, 0.1, 8, "vf");
	tune = OceanodeParameter.kr(\hitune, 300, n, 100, 5000, "vf");
	amp = OceanodeParameter.ar(\amp, 1, n, 0, 1, "vf");
	bodyfreq = (EnvGen.ar(Env.perc(0.01, pdec, 1, -4), gate) * tune) + pitch;
	bodyamp = EnvGen.ar(Env.perc(0.01, adec, 1, -4), gate);
	body = LFTri.ar(bodyfreq, 0, bodyamp).tanh * amp;
	Out.ar(OceanodeOutput.kr(\out), body);
}, description: "Kick drum with triangle oscillator and pitch envelope", category: "Source/Percussion");
)

Example (effect):
(
~synthCreator.value("DJFilter", {|n|
	var input, wet, sig, cutoff, mix;
	input = In.ar(OceanodeInput.kr(\in), n);
	cutoff = OceanodeParameterLag.ar(\cutoff, 0.5, n, 0, 1, "vf", 0.2, true);
	mix = OceanodeParameterLag.ar(\mix, 0.5, n, 0, 1, "vf", 0.1, true);
	wet = LPF.ar(HPF.ar(input, cutoff.linexp(0.5, 1, 20, 20000)), cutoff.linexp(0, 0.5, 20, 20000));
	sig = XFade2.ar(input, wet, mix * 2 - 1);
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Crossfader filter with HPF and LPF", category: "Effect/Filter");
))SYS";
}

std::string scCodeNode::promotePrompt() const {
    const DefDesc* def = currentDef();
    std::string prompt = "Convert the SynthDef \"" + (def ? def->name : std::string()) + "\" from this code into an Oceanode source.\n";
    prompt += "Name: " + promoteName + "\nCategory: " + promoteCategory + "\n";
    if(def){
        prompt += std::string("It was played as ") + (voiceMode() ? (def->hasGate ? "gated voices (it frees itself after release)" : "one-shot voices (it frees itself)") : "a persistent synth") + ".\n";
        prompt += "Values the user is hearing now (use them as defaults):\n";
        for(auto& c : controls){
            prompt += "- " + c->desc.name + " = ";
            const auto& v = c->param.get();
            for(size_t i = 0; i < v.size() && i < 8; i++) prompt += (i ? ", " : "") + ofToString(v[i]);
            prompt += "  (slider " + ofToString(c->param.getMin().empty() ? 0.0f : c->param.getMin()[0]) + ".." +
                      ofToString(c->param.getMax().empty() ? 1.0f : c->param.getMax()[0]) + ")\n";
        }
    }
    if(!promoteNotes.empty()) prompt += "\nAlso: " + promoteNotes + "\n";
    prompt += "\nCode:\n" + compiledCode;
    return prompt;
}

void scCodeNode::openPromote(){
    const DefDesc* def = currentDef();
    if(!def) return;
    if(def->creator){
        // Already an Oceanode source: it goes back as it is (no AI), over
        // its original file when the library has one
        promoteSource = compiledCode;
        promoteName = def->name;
        promoteCategory = def->category;
        const auto found = scanLibrary();
        auto it = found.find(def->name);
        promoteStatus = it != found.end()
            ? "Already an Oceanode source. Save, compile & install writes it over synthdefs/" + it->second +
              " and rebuilds it (tick Overwrite)."
            : "Already an Oceanode source: Save, compile & install adds it to the library.";
        promoteStatusError = false;
        promoteChannels = std::max(promoteChannels, compiledChannels);
        promoteOpenRequest = true;
        return;
    }
    if(promoteName.empty()){
        std::string name;
        bool upper = true;
        for(char ch : def->name){
            if(!std::isalnum((unsigned char)ch)){ upper = true; continue; }
            name += upper ? (char)std::toupper((unsigned char)ch) : ch;
            upper = false;
        }
        if(name.empty() || std::isdigit((unsigned char)name[0])) name = "Code" + name;
        promoteName = name;
    }
    if(promoteCategory.empty()) promoteCategory = busControls(def->inputs).empty() ? "Source/Special" : "Effect/Modulation";
    promoteOpenRequest = true;
}

void scCodeNode::requestConversion(){
    try {
        conversionTask = ai.generateText(promoteSystemPrompt(), promotePrompt(), 8192);
        promoteStatus = "Converting with " + ai.getModel() + "...";
        promoteStatusError = false;
    } catch(const std::exception& e) {
        promoteStatus = e.what();
        promoteStatusError = true;
    }
}

void scCodeNode::startInstall(){
    std::smatch m;
    if(!std::regex_search(promoteSource, m, std::regex("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(\\s*\"([A-Za-z][A-Za-z0-9_]*)\""))){
        promoteStatus = "The source must call ~synthCreator.value(\"Name\", ...) with a plain identifier as name";
        promoteStatusError = true;
        return;
    }
    const std::string name = m[1].str();
    std::string category = std::regex_search(promoteSource, m, std::regex("category:\\s*\"([^\"]*)\"")) ? m[1].str() : promoteCategory;
    category = ofTrim(category);
    if(category.empty() || category.find("..") != std::string::npos || category.front() == '/'){
        promoteStatus = "Give the source a category such as \"Source/Special\"";
        promoteStatusError = true;
        return;
    }
    const std::string synthdefRoot = ofToDataPath(SYNTHDEF_DIRECTORY, true);
    // A source that already defines this synth is replaced where it is
    // (library files are not always named after their synth)
    const auto found = scanLibrary();
    auto original = found.find(name);
    const std::string relative = original != found.end() ? original->second : (fs::path(category) / (name + ".scd")).string();
    if(!promoteOverwrite){
        // Any existing build or source with this name
        bool exists = original != found.end() || fs::exists(fs::path(addonSynthdefsFolder()) / relative);
        try {
            for(auto it = fs::recursive_directory_iterator(synthdefRoot); !exists && it != fs::recursive_directory_iterator(); ++it)
                if(it->path().filename() == name + ".txarcmeta") exists = true;
        } catch(...) {}
        if(exists){
            promoteStatus = name + " already exists. Rename it in the source, or tick Overwrite.";
            promoteStatusError = true;
            return;
        }
    }
    const std::string runner = findRunnerFolder();
    auto job = scSynthdefCompiler::get().prepareScript("promote", {});
    if(runner.empty() || job.sclang.empty()){
        promoteStatus = runner.empty() ? "synthdefs/OceanodeCode was not found" : "No sclang to compile with";
        promoteStatusError = true;
        return;
    }
    // The addon's sources are the source of truth; a deployed copy in the
    // app's data folder is kept in step so on-demand compiles find it
    std::vector<std::string> sourceFolders = {addonSynthdefsFolder()};
    const std::string deployed = scSynthdefCompiler::get().getSourcesFolder();
    if(!deployed.empty() && fs::weakly_canonical(deployed) != fs::weakly_canonical(addonSynthdefsFolder()))
        sourceFolders.push_back(deployed);
    const std::string work = ofToDataPath("Supercollider/tmp/promote", true);
    installTask = std::async(std::launch::async, &scCodeNode::runInstall, job,
                             (fs::path(runner) / "promote.scd").string(), sourceFolders, relative, synthdefRoot,
                             work, name, category, promoteSource, ofClamp(promoteChannels, 1, 16));
    promoteStatus = "Compiling " + name + " (1.." + ofToString(promoteChannels) + " ch)...";
    promoteStatusError = false;
}

scCodeNode::PromoteOutcome scCodeNode::runInstall(scSynthdefCompiler::ScriptJob job, std::string runner,
                                                  std::vector<std::string> sourceFolders, std::string sourceRelative,
                                                  std::string synthdefRoot, std::string work, std::string name,
                                                  std::string category, std::string source, int maxChannels){
    PromoteOutcome out;
    out.name = name;
    out.category = category;
    const fs::path build = fs::path(work) / name;
    const fs::path sourceFile = fs::path(work) / (name + ".scd");
    try {
        fs::remove_all(build);
        fs::create_directories(build);
        std::ofstream(sourceFile) << source;
    } catch(const std::exception& e) {
        out.error = e.what();
        return out;
    }
    job.args = {runner, build.string(), sourceFile.string(), ofToString(maxChannels)};
    out.logPath = (fs::path(work) / (name + ".log")).string();
    job.logPath = out.logPath;
    job.timeoutSeconds = 180;
    const std::string error = scSynthdefCompiler::runScript(job);
    const fs::path built = build / name;
    if(!error.empty() || !fs::exists(built / (name + ".txarcmeta"))){
        out.error = error.empty() ? "sclang wrote no " + name + ".txarcmeta (see " + out.logPath + ")" : error;
        return out;
    }
    try {
        // Compiled builds next to the others of their category
        const fs::path installed = fs::path(synthdefRoot) / category / name;
        fs::create_directories(installed);
        // Builds of the previous version (any channel count / variables) are
        // stale now: removed, the missing ones are compiled on demand
        const std::regex staleBuild(name + "\\d+(_\\d+)*\\.scsyndef");
        for(auto& f : fs::directory_iterator(installed))
            if(std::regex_match(f.path().filename().string(), staleBuild)) fs::remove(f.path());
        for(auto& f : fs::directory_iterator(built)){
            // Only <Name>.txarcmeta describes the node: one per channel count
            // would register each build as a node type of its own
            if(f.path().extension() == ".txarcmeta" && f.path().stem() != name) continue;
            fs::copy_file(f.path(), installed / f.path().filename(), fs::copy_options::overwrite_existing);
        }
        out.installedFolder = installed.string();
        // The source, in every source tree
        for(auto& root : sourceFolders){
            const fs::path target = fs::path(root) / sourceRelative;
            fs::create_directories(target.parent_path());
            fs::copy_file(sourceFile, target, fs::copy_options::overwrite_existing);
        }
        fs::remove_all(build);
    } catch(const std::exception& e) {
        out.error = std::string("could not install it: ") + e.what();
        return out;
    }
    out.ok = true;
    return out;
}

void scCodeNode::finishInstall(PromoteOutcome outcome){
    if(!outcome.ok){
        const std::string log = fs::exists(outcome.logPath) ? readFile(outcome.logPath) : "";
        auto lines = logErrors(log);
        promoteStatus = outcome.error + (lines.empty() ? "" : "\n" + lines.front());
        promoteStatusError = true;
        return;
    }
    for(auto* sm : servers) if(sm) sm->loadSynthdefFolder(outcome.installedFolder);
    scSynthdefCompiler::get().invalidateSourceIndex();
    // Registers the new node type (the "Load Defs" menu does the same)
    for(auto* sm : servers) if(sm){ sm->reloadSynthdefModels(); break; }
    promoteStatus = "Installed: SuperCollider/" + outcome.category + " > " + outcome.name + "*. Builds 1.." +
                    ofToString(promoteChannels) + " ch are fresh; other counts compile on demand.";
    pendingReload = outcome.name;
    promoteStatusError = false;
}

void scCodeNode::drawPromotePopup(){
    constexpr const char* id = "Promote to Oceanode SynthDef";
    if(promoteOpenRequest){
        promoteOpenRequest = false;
        ImGui::OpenPopup(id);
    }
    ImGui::SetNextWindowSize(ImVec2(720, 640), ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal(id, nullptr)) return;

    ImGui::TextWrapped("The AI rewrites the tested SynthDef in Oceanode's own format (~synthCreator, n channels, "
                       "OceanodeParameters, the current values as defaults). Then it is saved in the SynthDef "
                       "sources, compiled, loaded and added to the node menu.");
    ImGui::Spacing();
    auto inputText = [](const char* label, std::string& text, float width){
        ImGui::SetNextItemWidth(width);
        ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                         &scCodeNode::resizeCallback, &text);
    };
    inputText("Name", promoteName, 200);
    ImGui::SameLine();
    inputText("Category", promoteCategory, 220);
    inputText("Notes for the AI", promoteNotes, -1);

    const bool converting = conversionTask.valid();
    const bool installing = installTask.valid();
    if(converting || installing) ImGui::BeginDisabled();
    if(ImGui::Button(promoteSource.empty() ? "Convert with AI" : "Convert again")) requestConversion();
    if(converting || installing) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("model: %s", ai.getModel().c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + 40;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(16, 20, 28, 255));
    ImGui::InputTextMultiline("##promoteSource", promoteSource.data(), promoteSource.capacity() + 1,
                              ImVec2(avail.x, std::max(160.0f, avail.y - footer)),
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
                              &scCodeNode::resizeCallback, &promoteSource);
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Channel counts to build", &promoteChannels, 1, 16);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Builds 1..N now; any other count is compiled on demand later");
    ImGui::SameLine();
    ImGui::Checkbox("Overwrite", &promoteOverwrite);

    const bool canInstall = !promoteSource.empty() && !converting && !installing;
    if(!canInstall) ImGui::BeginDisabled();
    if(ImGui::Button("Save, compile & install")) startInstall();
    if(!canInstall) ImGui::EndDisabled();
    ImGui::SameLine();
    if(ImGui::Button("Close")) ImGui::CloseCurrentPopup();

    if(!promoteStatus.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, promoteStatusError ? IM_COL32(255, 110, 100, 255) : IM_COL32(140, 200, 140, 255));
        ImGui::TextWrapped("%s", promoteStatus.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

// ════════════════════════════════════════════════════════════════════════════
// Library
// ════════════════════════════════════════════════════════════════════════════

std::map<std::string, std::string> scCodeNode::scanLibrary(){
    std::map<std::string, std::string> found;
    const fs::path root = addonSynthdefsFolder();
    const std::regex creator("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(\\s*\"([^\"]+)\"");
    try {
        for(auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it){
            const std::string top = fs::relative(it->path(), root).begin()->string();
            // The prelude, this node's own files and the hand-built defaults hold no sources
            if(it->is_directory() && (top == "Oceanode" || top == "OceanodeCode" || top == "Defaults" || top == "WIP")){
                it.disable_recursion_pending();
                continue;
            }
            if(!it->is_regular_file() || it->path().extension() != ".scd") continue;
            const std::string text = readFile(it->path().string());
            for(std::sregex_iterator m(text.begin(), text.end(), creator), end; m != end; ++m)
                found.emplace((*m)[1].str(), fs::relative(it->path(), root).string());
        }
    } catch(const std::exception& e) {
        ofLogError("scCodeNode") << "Reading " << root << ": " << e.what();
    }
    return found;
}

void scCodeNode::drawLibraryPopup(){
    constexpr const char* id = "Oceanode SynthDef library";
    if(libraryOpenRequest){
        libraryOpenRequest = false;
        ImGui::OpenPopup(id);
    }
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal(id, nullptr)) return;
    ImGui::TextDisabled("%d sources in synthdefs/  ·  opens the file in the editor", (int)library.size());
    ImGui::SetNextItemWidth(-1);
    if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##libfilter", "search name or folder", libraryFilter.data(), libraryFilter.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize, &scCodeNode::resizeCallback, &libraryFilter);
    const std::string filter = lower(ofTrim(libraryFilter));
    std::string chosen;
    if(ImGui::BeginChild("##liblist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true)){
        for(auto& [name, relative] : library){
            if(!filter.empty() && lower(name).find(filter) == std::string::npos && lower(relative).find(filter) == std::string::npos) continue;
            if(ImGui::Selectable((name + "##" + relative).c_str())) chosen = relative;
            ImGui::SameLine(220);
            ImGui::TextDisabled("%s", relative.c_str());
        }
    }
    ImGui::EndChild();
    if(ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    if(!chosen.empty()){
        pendingScriptPath = (fs::path(addonSynthdefsFolder()) / chosen).string();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
