//
//  serverManager.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#include "serverManager.h"
#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSuperCollider.h"
#include "scNode.h"
#include "scStart.h"
#include "scOutput.h"
#include "ofxOceanodeShared.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#if !defined(_WIN32)
    #include <cerrno>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
    extern char **environ;
#endif
#include <map>
#include <set>
#include <sstream>
#include <cstring>

std::map<ofxSCServer*, int> serverManager::serverSampleRates;

namespace {
#if defined(_WIN32)
// Only the Windows path still goes through a shell; elsewhere scsynth is
// spawned directly with an argument vector, so nothing needs quoting.
std::string shellQuote(const std::string& value){
    std::string quoted = "'";
    for(char c : value){
        if(c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}
#endif

std::string getScPluginPath(const std::string& scsynthPath){
    const std::string scRoot = scsynthPath.substr(0, scsynthPath.size() - 7);
    return scRoot.find("/SuperCollider.app/") != std::string::npos
        ? scRoot + "plugins"
        : scRoot;
}
}

serverManager::serverManager(){
    initialized = false;
    volume = 1;
    mute = false;
    delay = 0;
    stereomix = false;
    stereomixSize = 2;
    audioDevice = 0;
    dumpOsc = false;
    numRecomputeGraphOnce = 0;
    busFromSilent = nullptr;
    server = nullptr;
    sc = nullptr;
    configuredNumInputBusChannels = -1;
    configuredNumOutputBusChannels = -1;
};

serverManager::~serverManager(){
    serverSampleRates.erase(server);
    for(auto node : nodesList) node->free(server);
    nodesList.clear();
    for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
    busses.clear();
}

void serverManager::setup(){
    if(configuredNumInputBusChannels < 0) configuredNumInputBusChannels = preferences.numInputBusChannels;
    if(configuredNumOutputBusChannels < 0) configuredNumOutputBusChannels = preferences.numOutputBusChannels;

    server = new ofxSCServer(preferences.bindAddress, preferences.udpPort, preferences.udpPort+20, preferences.numInputBusChannels, preferences.numOutputBusChannels, preferences.numAudioBusChannels, preferences.numControlBusChannels, preferences.numBuffers);
    serverSampleRates[server] = preferences.hardwareSampleRate;
    if(preferences.local){
        sc = new scStart(preferences);
    }
    boot();
    
    listeners.push(ofxOceanodeShared::getPresetWillBeLoadedEvent().newListener([this](){
        if(ofxOceanodeShared::getPresetLoadType() == ofxOceanodePresetLoadType_ClipboardPaste) return;
        teardownGraphForPresetLoad();
    }));
    
    listeners.push(ofxOceanodeShared::getPresetHasLoadedEvent().newListener([this](){
        if(preferences.loadOnPreset){
            loadSynthdefsFromPreset(ofxOceanodeShared::getCurrentPresetPath());
        }
        recomputeGraph();
    }));
    
    listeners.push(server->serverBootedEvent.newListener([this](){
        initialized = false;
        initialize();
    }));
    listeners.push(server->serverInitializedEvent.newListener([this](){
        initialized = true;
        nodesList.clear();
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();

        // Server initialization resets the audio bus allocator, so the old
        // reserved silent bus address may now point to a live allocation.
        busFromSilent.reset();
        busFromSilent = std::make_unique<ofxSCBus>(RATE_AUDIO, MAX_NODE_CHANNELS, server);
        recomputeGraph();
        
        setVolume(volume);
        setDelay(delay);
        setStereoMix(stereomix);
        setStereoMixSize(stereomixSize);
    }));
	
	if(busFromSilent == nullptr) busFromSilent = std::make_unique<ofxSCBus>(RATE_AUDIO, MAX_NODE_CHANNELS, server);
}

void serverManager::draw(){
    if(ImGui::Button("Boot Server")){
        boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        if(preferences.local){
            kill();
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Local", &preferences.local)){
        if(!preferences.local){
            if(sc != nullptr){
                sc->killServer();
                delete sc;
                sc = nullptr;
            }
        }else{
            sc = new scStart(preferences);
        }
    }
    
    if(ImGui::Checkbox("Load Synthdefs On Preset", &preferences.loadOnPreset)){
        if(!preferences.loadOnPreset){
            loadDefs();
        }
    }
    
    if(ImGui::Button("Load Defs")){
        loadDefs();
    }
    
    ImGui::Separator();
    
    if(ImGui::SliderFloat("Volume", &volume, 0, 1)){
        setVolume(volume);
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Mute", &mute)){
        if(mute) setVolume(0);
        else setVolume(volume);
    }
    
    if(ImGui::SliderInt("Delay", &delay, 0, 5000)){
        setDelay(delay);
    }
    
    ImGui::Separator();
    
    ImGui::InputInt("Udp Port", &preferences.udpPort);
    
    
    int intaddress[4] = {0, 0, 0, 0};
    vector<string> splitAddress = ofSplitString(preferences.bindAddress, ".");
    for(int i = 0; i < 4; i++){
        intaddress[i] = ofToInt(splitAddress[i]);
    }
    if(ImGui::InputInt4("Bind Address", &intaddress[0])){
        string newAddress = ofToString(intaddress[0]) + "."
                            + ofToString(intaddress[1]) + "."
                            + ofToString(intaddress[2]) + "."
                            + ofToString(intaddress[3]) + ".";
        preferences.bindAddress = newAddress;
    }
    
    ImGui::InputInt("Audio Busses", &preferences.numAudioBusChannels);
    ImGui::InputInt("Control Busses", &preferences.numControlBusChannels);
    ImGui::InputInt("Input Channels", &preferences.numInputBusChannels);
    ImGui::InputInt("Output Channels", &preferences.numOutputBusChannels);
    ImGui::InputInt("Block Size", &preferences.blockSize);
    ImGui::InputInt("Buffer Size", &preferences.hardwareBufferSize);
    ImGui::InputInt("Sampling Rate", &preferences.hardwareSampleRate);
    ImGui::InputInt("Num Buffers", &preferences.numBuffers);
    ImGui::InputInt("Max Nodes", &preferences.maxNodes);
    ImGui::InputInt("Max Synthdefs", &preferences.maxSynthDefs);
    ImGui::InputInt("Mem Size", &preferences.memSize);
    ImGui::InputInt("Num Wire Bufs", &preferences.numWireBufs);
    ImGui::InputInt("Num R Gens", &preferences.numRGens);
    ImGui::InputInt("Max Logins", &preferences.maxLogins);
    ImGui::InputFloat("Safety Clip Th", &preferences.safetyClipThreshold);
    
    ImGui::Text("Output Device: %s", preferences.deviceName == "nil" ? "Default" : preferences.deviceName.c_str());
    ImGui::Text("Input Device: %s", preferences.inputDeviceName == "nil" ? "Default" : preferences.inputDeviceName.c_str());
    
    //Device name;
    bool verb = preferences.verbosity;
    if(ImGui::Checkbox("Verbosity", &verb)){
        if(verb) preferences.verbosity = 1;
        else preferences.verbosity = 0;
    }

    bool localUgens = (preferences.ugensPlugins != "");
    if(ImGui::Checkbox("LocalUgens", &localUgens)){
        if(localUgens)
            preferences.ugensPlugins = ofToDataPath("Supercollider/Ugens", true);
        else
            preferences.ugensPlugins = "";
    }
    
    if(ImGui::Checkbox("Dump Osc", &dumpOsc)){
        ofxOscMessage m;
        m.setAddress("/dumpOSC");
        if(dumpOsc) m.addIntArg(1);
        else m.addIntArg(0);
        server->sendMsg(m);
    }
}

void serverManager::boot(){
    if(preferences.local && sc != nullptr){
        sc->setPreferences(preferences);
        sc->start();
    }
}

void serverManager::initialize(){
    server->notify();
    
    ofxOscMessage m2;
    m2.setAddress("/g_new");
    m2.addIntArg(1);
    m2.addIntArg(0);
    m2.addIntArg(0);
    server->sendMsg(m2);
    
    loadDefs();
    
    server->sendInitializationSyncMessage();
}

void serverManager::kill(){
    if(preferences.local && sc != nullptr){
        ofxOscMessage m;
        m.setAddress("/quit");
        server->sendMsg(m);
        sc->killServer();
    }
}

void serverManager::prepareForRestart(){
    initialized = false;
    if(preferences.local){
        ofxOscMessage m;
        m.setAddress("/quit");
        if(server != nullptr) server->sendMsg(m);
        if(sc == nullptr) sc = new scStart(preferences);
        sc->setPreferences(preferences);
        sc->killServer();
    }
}

void serverManager::setAudioDeviceNames(const std::string& outputDeviceName, const std::string& inputDeviceName, int inputChannels, int outputChannels){
    if(configuredNumInputBusChannels < 0) configuredNumInputBusChannels = preferences.numInputBusChannels;
    if(configuredNumOutputBusChannels < 0) configuredNumOutputBusChannels = preferences.numOutputBusChannels;

    preferences.deviceName = (outputDeviceName.empty() || outputDeviceName == "Default") ? "nil" : outputDeviceName;
    preferences.inputDeviceName = (inputDeviceName.empty() || inputDeviceName == "Default") ? "nil" : inputDeviceName;
    preferences.numInputBusChannels = configuredNumInputBusChannels;
    preferences.numOutputBusChannels = configuredNumOutputBusChannels;

    if(inputChannels > 0 && configuredNumInputBusChannels > inputChannels){
        ofLogNotice("serverManager") << "Clamping SC input channels for "
            << (preferences.inputDeviceName != "nil" ? preferences.inputDeviceName : preferences.deviceName)
            << " from " << configuredNumInputBusChannels << " to " << inputChannels;
        preferences.numInputBusChannels = inputChannels;
    }
    if(preferences.deviceName != "nil" && outputChannels > 0 && configuredNumOutputBusChannels > outputChannels){
        ofLogNotice("serverManager") << "Clamping SC output channels for " << preferences.deviceName
            << " from " << configuredNumOutputBusChannels << " to " << outputChannels;
        preferences.numOutputBusChannels = outputChannels;
    }
}

void serverManager::loadDefs(){
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    if(preferences.loadOnPreset){
        m.addStringArg(ofToDataPath(std::string(SYNTHDEF_DIRECTORY) + "/Defaults", true));
    }else{
        m.addStringArg(ofToDataPath(SYNTHDEF_DIRECTORY, true));
    }
    m.addIntArg(0);
    server->sendMsg(m);

    // DynGen slot SynthDefs (DynGenWrapper_N_S) live in their own subdir.
    // Copy/symlink the CompiledSynthdefs/dyngen/ output from dyngen.scd to
    // [data]/Supercollider/Synthdefs/dyngen/ so they are picked up here.
    std::string dyngenDir = ofToDataPath(std::string(SYNTHDEF_DIRECTORY) + "/dyngen", true);
    if(ofDirectory::doesDirectoryExist(dyngenDir)){
        ofxOscMessage m2;
        m2.setAddress("/d_loadDir");
        m2.addStringArg(dyngenDir);
        server->sendMsg(m2);
    }
}

bool serverManager::beginNRTCapture(){
    if(server == nullptr || !initialized) return false;

    // Must happen first: some nodes hold their real state inside the server
    // rather than in their own parameters, and pulling it back needs a reply.
    // Once capture starts nothing is transmitted, so no reply can arrive.
    prepareNodesForNRTCapture();

    // Capture, but keep transmitting. A patch whose visuals are driven by
    // audio analysis coming back from the server -- an envelope follower
    // polling a control bus, say -- depends on those replies: in capture-only
    // mode the requests never leave, nothing answers, the analysis freezes,
    // and every texture and trigger derived from it freezes with it. The
    // score stays correct either way, because event times come from the
    // frame-stepped transport rather than from when a message went out.
    server->beginNRTCapture(false);
    teardownGraphForPresetLoad();
    server->clearNRTScore();
    server->setNRTTime(0.0);

    ofxOscMessage group;
    group.setAddress("/g_new");
    group.addIntArg(1);
    group.addIntArg(0);
    group.addIntArg(0);
    server->sendMsg(group);

    loadNRTSynthdefs();
    // Samples are read once when a preset loads, so a capture that starts
    // later never sees the command. Replay them here, while the score is
    // still at time zero and before any synth exists to read them.
    const int unrecoverableBuffers = server->replayBuffersForNRT();
    if(unrecoverableBuffers > 0){
        ofLogWarning("serverManager")
            << "NRT capture: " << unrecoverableBuffers
            << " buffer(s) hold audio that was recorded or generated into the"
            << " server rather than read from a file; those will be silent in"
            << " the render";
    }
    recomputeGraph();
    return true;
}

void serverManager::prepareNodesForNRTCapture(){
    if(server == nullptr) return;

    bool anyPending = false;
    for(auto node : nodesList){
        if(node == nullptr) continue;
        node->prepareForNRTCapture();
        anyPending = anyPending || node->isNRTCapturePreparationPending();
    }
    if(!anyPending) return;

    // Pump the server so the replies that complete those pulls arrive. The
    // timeout keeps a plugin that never answers from blocking the render; the
    // capture then simply falls back to the last state the node had cached.
    const uint64_t deadline = ofGetElapsedTimeMillis() + 2000;
    while(ofGetElapsedTimeMillis() < deadline){
        server->process();
        anyPending = false;
        for(auto node : nodesList){
            if(node != nullptr && node->isNRTCapturePreparationPending()){
                anyPending = true;
                break;
            }
        }
        if(!anyPending) return;
        ofSleepMillis(5);
    }
    ofLogWarning("serverManager") << "NRT capture: some nodes did not finish saving their state in time";
}

void serverManager::loadNRTSynthdefs(){
    // The renderer is a separate scsynth process with an empty SynthDef
    // table, so it needs everything the realtime server has -- which it got
    // in two stages: loadDefs() at boot, then the preset's own definitions.
    // Both are replayed here, in that order.
    //
    // The preset pass alone is not enough. It derives SynthDef names from the
    // module names in modules.json, which only maps for the generic
    // scSynthdef-driven nodes (the ones whose names carry a '*'). A
    // hand-written node's SynthDef -- vstStereo for SC VST, a2k1 for SC A2k,
    // polyMixerTrack2 for SC PolyMixerTrack -- has no such mapping and was
    // silently skipped, so those /s_new calls failed with "SynthDef not
    // found", every later command addressed to those nodes failed too, and
    // the render came out silent.
    loadDefs();

    const std::string presetPath = ofxOceanodeShared::getCurrentPresetPath();
    const std::string absolutePresetPath = ofToDataPath(presetPath, true);
    if(!presetPath.empty() && ofDirectory::doesDirectoryExist(absolutePresetPath)){
        loadSynthdefsFromPreset(presetPath, true, false);
    }else{
        ofLogWarning("serverManager") << "NRT capture has no valid current preset;"
                                      << " only the boot-time SynthDefs were loaded";
    }

    // Output is a legacy SynthDef without a .txarcmeta descriptor.
    const std::string outputSynthDef = ofToDataPath(
        std::string(SYNTHDEF_DIRECTORY) + "/Defaults/output.scsyndef", true);
    if(ofFile::doesFileExist(outputSynthDef)){
        ofxOscMessage message;
        message.setAddress("/d_load");
        message.addStringArg(outputSynthDef);
        server->sendMsg(message);
    }
}

void serverManager::endNRTCapture(double endTime){
    if(server == nullptr) return;
    server->endNRTCapture(endTime);

    // The model graph was rebuilt for the score. Rebuild it once more against
    // the realtime server so normal playback continues without stale node
    // handles or bus allocations.
    teardownGraphForPresetLoad();
    recomputeGraph();
}

std::vector<serverManager::NRTStem> serverManager::getNRTStems() const{
    // Walk upstream while the chain is unambiguous. The node feeding a mixer
    // is usually the last effect in a chain -- a panner, say -- which says
    // nothing useful; the node at the head of that chain is the layer, and is
    // what a stem should be called.
    auto originOf = [this](scNode* node) -> scNode* {
        scNode* current = node;
        for(int depth = 0; depth < 64 && current != nullptr; depth++){
            scNode* single = nullptr;
            int count = 0;
            for(const auto& link : nodeLinks){
                if(link.destination != current) continue;
                count++;
                single = link.source;
            }
            // No source: the head of the chain. More than one: a sub-mix,
            // where the chain's identity is the node itself.
            if(count != 1) return current;
            current = single;
        }
        return current;
    };

    // The busses feeding the file-writing synths are the master, which the
    // main render already covers.
    std::set<int> masterBusses;
    for(const auto& link : nodeLinks){
        if(link.destination == nullptr) continue;
        for(auto output : outputs){
            if((scNode*)output == link.destination) masterBusses.insert(link.bus);
        }
    }

    // Mixer labels are what the Source dropdown groups by, so they have to be
    // stable and unique even when two mixers carry the same node name.
    std::vector<NRTStem> stems;
    std::map<std::string, int> used;
    std::map<const scNode*, std::string> mixerNames;
    std::map<std::string, int> usedMixerNames;
    std::set<int> takenBusses;
    for(const auto& link : nodeLinks){
        if(link.destination == nullptr || !link.destination->isNRTStemPoint()) continue;
        if(link.source == nullptr || link.bus < 0) continue;
        if(masterBusses.count(link.bus) != 0) continue;
        // Several sources can share one bus; that bus is a single stem.
        if(!takenBusses.insert(link.bus).second) continue;

        scNode* origin = originOf(link.source);
        std::string name = origin != nullptr ? origin->getParameterGroup().getName()
                                             : link.source->getParameterGroup().getName();
        ofStringReplace(name, "*", "");
        ofStringReplace(name, " ", "_");
        ofStringReplace(name, "/", "_");
        name = ofTrim(name);
        if(name.empty()) name = "stem";

        // Two tracks can share an origin; keep both, distinguished.
        const int seen = used[name]++;
        if(seen > 0) name += "_" + ofToString(seen + 1);

        auto mixerEntry = mixerNames.find(link.destination);
        if(mixerEntry == mixerNames.end()){
            std::string mixerName = link.destination->getParameterGroup().getName();
            ofStringReplace(mixerName, "*", "");
            mixerName = ofTrim(mixerName);
            if(mixerName.empty()) mixerName = "Mixer";
            const int mixerSeen = usedMixerNames[mixerName]++;
            if(mixerSeen > 0) mixerName += " " + ofToString(mixerSeen + 1);
            mixerEntry = mixerNames.emplace(link.destination, mixerName).first;
        }

        stems.push_back({name, link.bus, mixerEntry->second});
    }
    return stems;
}

void serverManager::resendAllParametersForNRT(){
    for(auto node : nodesList){
        if(node != nullptr) node->resendParametersForNRT();
    }
}

bool serverManager::writeNRTScore(const std::string& path, double endTime) const{
    return server != nullptr && server->writeNRTScore(path, endTime);
}

bool serverManager::writeNRTStemScore(const std::string& path, double endTime, int bus) const{
    if(server == nullptr) return false;
    // The id has to come from the score, not from the live scOutput. The
    // server goes on allocating node ids after the capture, so by the time a
    // stem is written the output synth reports an id that does not exist
    // inside the score; scsynth then answers "Node not found" and renders the
    // untouched master, which looks exactly like a stem that failed to
    // isolate. Fail loudly instead of writing a score that silently produces
    // another copy of the mix.
    const int outputNode = server->findNRTNodeID(scOutput::getSynthDefName());
    if(outputNode <= 0){
        ofLogError("serverManager") << "NRT stems: the capture contains no '"
                                    << scOutput::getSynthDefName()
                                    << "' synth to redirect; cannot isolate a stem";
        return false;
    }
    ofxOscMessage redirect;
    redirect.setAddress("/n_set");
    redirect.addIntArg(outputNode);
    redirect.addStringArg("in");
    redirect.addFloatArg((float)bus);
    return server->writeNRTScore(path, endTime, {redirect});
}

std::size_t serverManager::getNRTEventCount() const{
    return server != nullptr ? server->getNRTEventCount() : 0;
}

int serverManager::renderNRT(const std::string& scorePath, const std::string& outputPath, int outputChannels) const{
    if(server == nullptr) return -1;

    std::string scPath = ofToDataPath("Supercollider/Scsynth/bin/scsynth", true);
    if(!ofFile::doesFileExist(scPath)) scPath = "/Applications/SuperCollider.app/Contents/Resources/scsynth";

    const auto &p = preferences;
    outputChannels = std::max(1, std::min(outputChannels, p.numOutputBusChannels));

    std::vector<std::string> args{
        scPath, "-N", scorePath, "_", outputPath,
        ofToString(p.hardwareSampleRate), "WAVE", "float",
        "-o", ofToString(outputChannels),
        "-i", ofToString(p.numInputBusChannels),
        "-a", ofToString(p.numAudioBusChannels),
        "-c", ofToString(p.numControlBusChannels),
        "-b", ofToString(p.numBuffers),
        "-n", ofToString(p.maxNodes),
        "-d", ofToString(p.maxSynthDefs),
        "-m", ofToString(p.memSize),
        "-w", ofToString(p.numWireBufs),
        "-r", ofToString(p.numRGens),
        "-l", ofToString(p.maxLogins),
        "-z", ofToString(p.blockSize),
        "-D", "0"
    };
    if(!p.ugensPlugins.empty()){
        // scsynth does not search its standard plugin locations when -U is
        // supplied. Keep the bundled SuperCollider plugins available in NRT
        // renders, in addition to Oceanode's optional custom UGen folder.
        args.push_back("-U");
        args.push_back(ofToDataPath(p.ugensPlugins, true) + ":" + getScPluginPath(scPath));
    }

    // scsynth prints one progress line per score packet in NRT mode, which
    // would bury the console -- but its failures ("SynthDef not found", bus
    // and node errors) go to the same stream, and those are exactly what is
    // needed when a render comes out silent. Keep the lot in a log beside the
    // audio rather than discarding it.
    const std::string logPath = outputPath + ".log";

    ofLogNotice("serverManager") << "Rendering SuperCollider NRT score: " << outputPath;
    const uint64_t startedAt = ofGetElapsedTimeMillis();
    int result = -1;

#if defined(_WIN32)
    std::ostringstream command;
    for(const auto& arg : args) command << shellQuote(arg) << " ";
    command << " > " << shellQuote(logPath) << " 2>&1";
    result = std::system(command.str().c_str());
#else
    // Deliberately not std::system(): on macOS it is serialised by a global
    // lock inside libc, so several render threads calling it queue up one
    // behind another and parallel rendering quietly becomes sequential --
    // four files finishing exactly one render apart rather than together.
    // posix_spawn has no such lock, and skipping the shell means no quoting
    // and no argument whose spacing can be misread.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for(auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions) != 0){
        ofLogError("serverManager") << "NRT render: cannot prepare the child process";
        return -1;
    }
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, scPath.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if(spawned != 0){
        ofLogError("serverManager") << "NRT render: could not start " << scPath
                                    << " (" << strerror(spawned) << ")";
        return -1;
    }

    int status = 0;
    while(waitpid(pid, &status, 0) < 0){
        if(errno != EINTR){
            ofLogError("serverManager") << "NRT render: lost track of scsynth (" << strerror(errno) << ")";
            return -1;
        }
    }
    result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    const double seconds = (ofGetElapsedTimeMillis() - startedAt) / 1000.0;
    ofLogNotice("serverManager") << "NRT render finished in " << seconds << " s: " << outputPath
                                 << " (real-time memory " << (p.memSize / 1024) << " MB)";

    // Surface anything that looks like a failure, so a silent render explains
    // itself in the console instead of only in the log.
    std::ifstream log(logPath);
    if(log.is_open()){
        int reported = 0;
        for(std::string line; std::getline(log, line); ){
            if(line.find("FAILURE") == std::string::npos &&
               line.find("ERROR") == std::string::npos &&
               line.find("exception") == std::string::npos) continue;
            if(++reported > 20){
                ofLogError("serverManager") << "NRT render: further errors in " << logPath;
                break;
            }
            ofLogError("serverManager") << "NRT render: " << line;
        }
        if(reported == 0 && result != 0){
            ofLogError("serverManager") << "NRT render: scsynth exited with " << result
                                        << "; see " << logPath;
        }
    }
    return result;
}

void serverManager::setVolume(float _volume){
    volume = _volume;
    for(auto &o : outputs) o->setVolume(volume);
    
}

void serverManager::setDelay(int _delay){
    delay = _delay;
    for(auto &o : outputs) o->setDelay(delay);
}

void serverManager::setStereoMix(bool _stereomix){
    stereomix = _stereomix;
    for(auto &o : outputs) o->setStereoMix(stereomix);
}

void serverManager::setStereoMixSize(int _stereomixSize){
    stereomixSize = _stereomixSize;
    for(auto &o : outputs) o->setStereoMixSize(stereomixSize);
}

void serverManager::setHardwareSampleRate(int sampleRate){
    if(sampleRate > 0){
        preferences.hardwareSampleRate = sampleRate;
        if(server != nullptr){
            serverSampleRates[server] = sampleRate;
        }
    }
}

int serverManager::getSampleRateForServer(ofxSCServer* server){
    auto it = serverSampleRates.find(server);
    if(it != serverSampleRates.end() && it->second > 0){
        return it->second;
    }
    return scPreferences().hardwareSampleRate;
}

void serverManager::addOutput(scOutput *output){
    outputs.push_back(output);
    setVolume(volume);
    setDelay(delay);
    setStereoMix(stereomix);
    setStereoMixSize(stereomixSize);
}

void serverManager::removeOutput(scOutput *output){
    outputs.erase(std::remove(outputs.begin(), outputs.end(), output), outputs.end());
    recomputeGraph();
}

void serverManager::teardownGraphForPresetLoad(){
    if(server == nullptr) return;

    outputBussesRefToNode.clear();
    inputBussesRefToNode.clear();
    connections.clear();
    nodesListChanged = true;

    const int silentBusIndex = busFromSilent != nullptr ? busFromSilent->index : 0;
    for(auto node : nodesList){
        if(node != nullptr){
            node->resetInputBusses(server, silentBusIndex);
            node->free(server);
        }
    }
    nodesList.clear();

    for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
    busses.clear();

    if(initialized){
        ofxOscMessage m;
        m.setAddress("/g_freeAll");
        m.addIntArg(1);
        server->sendMsg(m);
    }
}

void serverManager::recomputeGraph(){
    if(ofxOceanodeShared::isPresetLoading()) return;
    if(!initialized) return;
//    ofLog() << "Recompute Graph";
    if(outputs.size() == 0){
        for(auto node : nodesList) node->free(server);
        nodesList.clear();
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();
    }else{
        
//        server->setWaitToSend(true);
        
        
//        for(auto &b : busses) b.free();
        
        outputBussesRefToNode.clear();
        inputBussesRefToNode.clear();
        
        std::vector<scNode*> newNodesList;
        std::map<scNode*, std::pair<int, std::vector<int>>> nodeChilds;
        for(int i = 0; i < outputs.size(); i++){
            if(outputs[i]->isInputConnected()){
                outputs[i]->getInputNode()->appendOrderedNodes(newNodesList, nodeChilds);
            }
        }
        for(int i = 0; i < outputs.size(); i++){
            if(outputs[i]->isInputConnected()){
                newNodesList.push_back(outputs[i]);
            }
        }
        
        std::map<nodePort, std::vector<scNode*>> newConnections;
        
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            (*it)->getConnections(newConnections);
        }
        
        //Skip all proceses if the list is the same and if the nodeList has not changed (due to destroyed nodes)
        if(nodesList == newNodesList && !nodesListChanged && newConnections == connections){
            return;
        }
        
        nodesListChanged = false;
        
        std::vector<scNode*> toCreateNodes;
        std::vector<scNode*> toUpdateNodes;
        
        for(auto &node : newNodesList){
            auto nodeInListIter = std::find(nodesList.begin(), nodesList.end(), node);
            if(nodeInListIter != nodesList.end()){
                toUpdateNodes.push_back(node);
                nodesList.erase(nodeInListIter);
            }else{
                toCreateNodes.push_back(node);
            }
        }
        server->setBLatency(true);
        for(auto b = busses.rbegin(); b != busses.rend(); ++b) b->free();
        busses.clear();
        
        for(auto node : nodesList){
            if(node != nullptr)
                node->free(server);
        }
        nodesList.clear();
        
        for(auto &node : toCreateNodes){
            nodeDestroyedListeners[node] = node->destroyedNode.newListener([this, node](){
                nodesList.erase(std::remove(nodesList.begin(), nodesList.end(), node), nodesList.end());
                nodesListChanged = true;
            });
        }
        
        for(auto &node : nodesList){
            nodeDestroyedListeners.erase(node);
        }
        
            
            for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
                if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
                    (*it)->buildSynth(server);
                }
                (*it)->resetInputBusses(server, busFromSilent->index);
            }
                
        nodeLinks.clear();

        //Create outputBusses for all nodes except scOutput
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            for(int i = 0; i < (*it)->getNumOutputs() ; i++){
                busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server);
                int busindex = busses.back().index;
                (*it)->setOutputBus(server, i, busindex);
                outputBussesRefToNode[(*it)][i] = busindex;
            }
        }
        
            for(auto &c : newConnections){
                for(auto &dest : c.second){
                    int busindex;
                    auto& nodeMap = outputBussesRefToNode[c.first.getNodeRef()];
                    if(nodeMap.count(c.first.getIndex()))
                        busindex = nodeMap[c.first.getIndex()];
                    else
                        busindex = c.first.getBusIndex(server); // fallback for self-managed buses (e.g. mix bus)
                    dest->setInputBus(server, c.first.getNodeRef(), busindex);
                    inputBussesRefToNode[dest].push_back(busindex);
                    nodeLinks.push_back({c.first.getNodeRef(), dest, busindex});
                }
            }
        
        scNode* lastNode = nullptr;
        for (auto it = newNodesList.rbegin(); it != newNodesList.rend(); ++it) {
            if(std::find(toCreateNodes.begin(), toCreateNodes.end(), (*it)) != toCreateNodes.end()){
                (*it)->createSynth(server);
            }
            else{
                if(lastNode == nullptr){
//                    (*it)->moveSynthAfter(server, -1);
                }
                else{
                    (*it)->moveSynthBefore(server, lastNode->getLastSynthID(server));
                }
            }
            lastNode = (*it);
        }
        
        nodesList = newNodesList;
        connections = newConnections;
//            nodesList.insert(nodesList.end(), newNodesList.begin(), newNodesList.end());
//        }
//        server->sendStoredBundle();
//        server->setWaitToSend(false);
        server->setBLatency(false);
    }
    graphComputed.notify();
}

void serverManager::loadSynthdefsFromPreset(const std::string& path, bool forceLoad, bool waitForLoad){
    //TODO: clear all loaded definitions, via /d_free message https://doc.sccode.org/Reference/Server-Command-Reference.html
    
//    std::set<std::string> synthsList;
    std::set<std::string> synthsList;
    
    std::function<void(const std::string&)> checkSynthsInPreset = [this, &checkSynthsInPreset, &synthsList, forceLoad](const std::string& path){
        ofJson json = ofLoadJson(path + "/modules.json");
        
        if(json.empty()){
            return;
        }
        
        for (ofJson::iterator node = json.begin(); node != json.end(); ++node) {
            if(node.key().rfind("SC ", 0) == 0){
                std::string synthdefName = node.key();
                bool version2 = ofStringTimesInString(node.key(), "*") == 1;
                ofStringReplace(synthdefName, "SC ", "");
                ofStringReplace(synthdefName, "*", "");
                
                if(version2){
                    if(forceLoad || alreadyLoadedSynthsList.count(synthdefName) == 0){
                        synthsList.insert(synthdefName);
                    }
                }
//                else{
//                    synthsList.insert(synthdefName);
//                }
            }
            else if(node.key() == "Macro"){
                for (ofJson::iterator nodeID = node.value().begin(); nodeID != node.value().end(); ++nodeID) {
                    int id = ofToInt(nodeID.key());
                    
                    ofJson macroJson = ofLoadJson(path + "/Macro_" + ofToString(id) + ".json");
                    
                    if(macroJson.empty()) continue;
                    
                    if(macroJson["LocalPreset"]){
                        checkSynthsInPreset(path + "/Macro_" + ofToString(id));
                    }else{
                        std::string macroPath = macroJson["MacroPath"];
//                        ofLog() << macroPath;
                        std::vector<std::string> macroPathSplit = ofSplitString(macroPath, "/");
                        std::string recreatedMacroPath = "";
                        for(auto it = macroPathSplit.rbegin(); it !=macroPathSplit.rend(); it++){
                            recreatedMacroPath = *it + "/" + recreatedMacroPath;
                            if(*it == "Macros"){
//                                ofLog() << "Recreated Macro Path: " << recreatedMacroPath;
                                checkSynthsInPreset(recreatedMacroPath);
                                continue;
                            }
                        }
                    }
                }
            }
        }
    };
    
    checkSynthsInPreset(path);

    for(auto &synthdef : synthsList){
        auto folder = synthdefFolders.find(synthdef);
        if(folder == synthdefFolders.end() || folder->second.empty()){
            ofLogWarning("serverManager") << "Could not find SynthDef folder for " << synthdef;
            continue;
        }
        if(!forceLoad) alreadyLoadedSynthsList.insert(synthdef);
        std::string path = folder->second;
        
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(path);
        server->sendMsg(m);
    }
    
    if(waitForLoad) ofSleepMillis(100 * synthsList.size());
}


//int serverManager::getOutputBusForNode(scNode* node){
//    if(outputBussesRefToNode.count(node) == 1){
//        return outputBussesRefToNode[node];
//    }
//    else{
//        return -1;
//    }
//}
