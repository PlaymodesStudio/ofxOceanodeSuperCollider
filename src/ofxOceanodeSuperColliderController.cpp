//
//  ofxOceanodeSuperColliderController.cpp
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#include "ofxOceanodeSuperColliderController.h"
#include "scStart.h"
#include "ofxSCServer.h"
#include "imgui.h"
#include "serverManager.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#if OFXOCEANODESC_HAS_TIMELINE
#include "ofxOceanodeTime.h"
#include <array>
#include <cstring>
#endif // OFXOCEANODESC_HAS_TIMELINE

ofxOceanodeSuperColliderController::ofxOceanodeSuperColliderController() : ofxOceanodeBaseController("SuperCollider"){
    volume = 1;
    delay = 0;
    stereomix = true;
    stereomixSize = 4;
    audioDevice = 0;
    audioInputDevice = 0;
    sampleRate = 0;
    selectedSampleRate = scPreferences().hardwareSampleRate;
    selectedAudioDeviceName = "nil";
    selectedAudioInputDeviceName = "nil";
	mute = false;
	
	reloadAudioDevices();
}

ofxOceanodeSuperColliderController::~ofxOceanodeSuperColliderController(){
#if OFXOCEANODESC_HAS_TIMELINE
    if(nrtCaptureActive) completeNRTCapture(true);
    if(nrtRenderThread.joinable()) nrtRenderThread.join();
#endif // OFXOCEANODESC_HAS_TIMELINE
}

#if OFXOCEANODESC_HAS_TIMELINE
void ofxOceanodeSuperColliderController::update(){
    joinFinishedNRTThread();
    updateNRTArming();
    if(!nrtCaptureActive) return;

    const double currentTime = ofxOceanodeTime::getInstance()->getGlobalTimeState().time;
    if(!nrtManualStop && currentTime >= std::max(0.01f, nrtDuration)){
        completeNRTCapture(false);
    }
}

void ofxOceanodeSuperColliderController::joinFinishedNRTThread(){
    if(nrtRenderThread.joinable() && !nrtRendering.load()){
        nrtRenderThread.join();
        nrtStatus = nrtRenderResult == 0 ? "NRT render complete" : "NRT render failed";
    }
}

void ofxOceanodeSuperColliderController::startNRTRender(){
    beginNRTRecording(nrtServer, nrtOutputChannels, nrtOutputPath, false);
}

std::vector<ofxOceanodeSuperColliderController::NRTSource>
ofxOceanodeSuperColliderController::buildNRTSources(serverManager* manager) const{
    using Kind = NRTSource::Kind;
    std::vector<NRTSource> sources;
    sources.push_back({Kind::Master, "Master", "", -1});
    if(manager == nullptr) return sources;

    const auto stems = manager->getNRTStems();
    if(stems.empty()) return sources;

    sources.push_back({Kind::AllStems, "All stems", "", -1});

    // One group per mixer next, in the order the mixers appear in the graph,
    // so the common case -- record this mixer's tracks -- stays near the top.
    std::vector<std::string> mixers;
    for(const auto& stem : stems){
        if(std::find(mixers.begin(), mixers.end(), stem.mixerName) != mixers.end()) continue;
        mixers.push_back(stem.mixerName);
    }
    for(const auto& mixer : mixers){
        sources.push_back({Kind::Mixer, mixer + " (all)", mixer, -1});
    }

    // Then every stem on its own, labelled by the mixer it belongs to.
    for(std::size_t i = 0; i < stems.size(); i++){
        sources.push_back({Kind::Stem, stems[i].mixerName + " / " + stems[i].name,
                           stems[i].mixerName, (int)i});
    }
    return sources;
}

std::vector<ofxOceanodeSuperColliderController::NRTSource>
ofxOceanodeSuperColliderController::getNRTSources(int serverIndex) const{
    if(serverIndex < 0 || serverIndex >= (int)outputServers.size()) return buildNRTSources(nullptr);
    return buildNRTSources(outputServers[serverIndex]);
}

std::vector<std::string> ofxOceanodeSuperColliderController::getNRTSourceNames(int serverIndex) const{
    std::vector<std::string> names;
    for(const auto& source : getNRTSources(serverIndex)) names.push_back(source.label);
    return names;
}

bool ofxOceanodeSuperColliderController::armNRTRecording(int serverIndex, int outputChannels, const std::string& outputPath){
    if(nrtCaptureActive || nrtRendering.load() || outputServers.empty()) return false;
    if(nrtArmState != NRTArmState::Disarmed) return isNRTArmed();
    joinFinishedNRTThread();

    if(serverIndex < 0 || serverIndex >= (int)outputServers.size()) serverIndex = 0;
    nrtServer = serverIndex;
    serverManager* manager = outputServers[nrtServer];
    if(manager == nullptr || manager->getServer() == nullptr){
        nrtStatus = "Selected server is not available";
        return false;
    }

    // Any rebuild we cause ourselves is expected; a rebuild from anywhere else
    // means the patch changed and whatever we armed is stale.
    nrtGraphListeners.unsubscribeAll();
    nrtSelfRebuild = true;
    const bool started = manager->beginNRTCapture();
    nrtSelfRebuild = false;
    if(!started){
        nrtStatus = "Server must be booted and initialized before arming";
        return false;
    }

    const int maxChannels = std::max(1, manager->preferences.numOutputBusChannels);
    nrtCaptureOutputChannels = std::max(1, std::min(outputChannels, maxChannels));
    nrtCaptureOutputPath = outputPath.empty() ? nrtOutputPath : outputPath;

    nrtGraphListeners.push(manager->graphComputed.newListener([this](){
        if(nrtSelfRebuild) return;
        if(nrtArmState == NRTArmState::Disarmed || nrtCaptureActive) return;
        ofLogWarning("ofxOceanodeSuperColliderController")
            << "NRT: the patch changed after arming; disarming";
        disarmNRTRecording();
    }));

    // Ask the server to report when the setup has actually finished. Loading
    // the SynthDef tree alone takes seconds, and the parameter state that
    // follows it is the part that has to land at score time zero.
    manager->getServer()->requestNRTSync();
    // Arming can take many seconds. Whatever the patch plays while it waits
    // belongs to before the recording, not to score time zero.
    manager->getServer()->setNRTEventsSuppressed(true);

    nrtSettleDeadline = ofGetElapsedTimeMillis() + 60000;
    nrtArmState = NRTArmState::Settling;
    nrtStatus = "Arming: loading SynthDefs";
    return true;
}

void ofxOceanodeSuperColliderController::updateNRTArming(){
    if(nrtArmState != NRTArmState::Settling || nrtCaptureActive) return;
    if(nrtServer < 0 || nrtServer >= (int)outputServers.size()) return;
    serverManager* manager = outputServers[nrtServer];
    if(manager == nullptr || manager->getServer() == nullptr) return;

    const bool timedOut = ofGetElapsedTimeMillis() > nrtSettleDeadline;
    const bool syncPending = manager->getServer()->isNRTSyncPending();

    // The only thing that has to finish before arming is the server's own
    // work. /sync is answered once every asynchronous command issued before it
    // has completed, which here means the SynthDef tree has finished loading
    // and the synths exist.
    //
    // Waiting for the capture to also fall quiet does not work: a patch
    // resends some parameters every frame whether or not they changed, so the
    // score never stops growing and arming would never finish. It is not
    // needed either -- the complete parameter state is written explicitly at
    // score time zero when recording starts.
    if(syncPending && !timedOut){
        nrtStatus = "Arming: loading SynthDefs";
        return;
    }
    if(syncPending){
        ofLogWarning("ofxOceanodeSuperColliderController")
            << "NRT: the server never answered /sync; arming anyway";
    }

    // From here until recording starts, every frame would otherwise stack more
    // messages onto score time zero.
    manager->getServer()->setNRTCaptureSuspended(true);
    nrtArmState = NRTArmState::Armed;
    nrtStatus = "Armed: " + ofToString((int)manager->getNRTEventCount()) + " events at time zero";
    ofLogNotice("ofxOceanodeSuperColliderController") << "NRT: " << nrtStatus;
}

void ofxOceanodeSuperColliderController::disarmNRTRecording(){
    if(nrtArmState == NRTArmState::Disarmed) return;
    nrtGraphListeners.unsubscribeAll();
    if(nrtServer >= 0 && nrtServer < (int)outputServers.size()){
        serverManager* manager = outputServers[nrtServer];
        if(manager != nullptr){
            if(manager->getServer() != nullptr){
                manager->getServer()->setNRTEventsSuppressed(false);
                manager->getServer()->setNRTCaptureSuspended(false);
                manager->getServer()->setNRTTimeProviderEnabled(false);
            }
            // Puts the graph back on the realtime server.
            nrtSelfRebuild = true;
            manager->endNRTCapture(-1.0);
            nrtSelfRebuild = false;
        }
    }
    nrtArmState = NRTArmState::Disarmed;
    nrtStatus = "Disarmed";
}

bool ofxOceanodeSuperColliderController::beginNRTRecording(int serverIndex, int outputChannels, const std::string& outputPath, bool manualStop){
    if(nrtCaptureActive || nrtRendering.load() || outputServers.empty()) return false;

    if(nrtArmState != NRTArmState::Armed){
        // Recording without arming still works, but the graph is settling while
        // the clock already runs, so the opening of the render arrives late.
        ofLogWarning("ofxOceanodeSuperColliderController")
            << "NRT: recording was started before arming finished; the first"
            << " moments of the render may be missing their initial state";
        if(nrtArmState == NRTArmState::Disarmed &&
           !armNRTRecording(serverIndex, outputChannels, outputPath)) return false;
    }

    serverManager* manager = outputServers[nrtServer];
    if(manager == nullptr || manager->getServer() == nullptr){
        nrtStatus = "Selected server is not available";
        return false;
    }
    nrtManualStop = manualStop;

    // Everything captured from here belongs on the transport's clock; what came
    // before it is the graph's opening state and is already at time zero.
    manager->getServer()->setNRTEventsSuppressed(false);
    manager->getServer()->setNRTCaptureSuspended(false);

    // Write the patch's complete state before the clock starts. Parameters are
    // only sent when they change, so one that nobody has touched since the
    // preset loaded has never been sent at all -- the render would run on the
    // SynthDef's default until its first change. A trigger left at its default
    // fires a note at the top of the render that was never played.
    manager->resendAllParametersForNRT();

    manager->getServer()->setNRTTimeProvider([](){
        return ofxOceanodeTime::getInstance()->getGlobalTimeState().time;
    });
    manager->getServer()->setNRTTimeProviderEnabled(true);

    const std::string absoluteOutputPath = ofToDataPath(nrtCaptureOutputPath, true);
    ofDirectory::createDirectory(ofFilePath::getEnclosingDirectory(absoluteOutputPath), true, false);
    ofxOceanodeTime::getInstance()->setFrameMode(true);
    ofxOceanodeTime::getInstance()->resetTransportToStart();
    ofxOceanodeTime::getInstance()->setIsPlaying(true);
    nrtCaptureActive = true;
    nrtArmState = NRTArmState::Disarmed;
    nrtGraphListeners.unsubscribeAll();
    nrtStatus = "Capturing frame-stepped OSC score";
    return true;
}

bool ofxOceanodeSuperColliderController::endNRTRecording(bool cancelled){
    if(!nrtCaptureActive || !nrtManualStop) return false;

    // An externally controlled recording uses the actual frame-stepped
    // transport time, so the WAV duration matches the texture sequence.
    const double duration = std::max(0.01, ofxOceanodeTime::getInstance()->getGlobalTimeState().time);
    completeNRTCapture(cancelled, duration);
    return true;
}

void ofxOceanodeSuperColliderController::completeNRTCapture(bool cancelled, double durationOverride){
    if(!nrtCaptureActive) return;
    if(nrtServer < 0 || nrtServer >= (int)outputServers.size()) nrtServer = 0;
    serverManager* manager = outputServers[nrtServer];
    const double duration = durationOverride > 0.0 ? durationOverride : std::max(0.01f, nrtDuration);
    const int outputChannels = std::max(1, nrtCaptureOutputChannels);
    const std::string outputPath = ofToDataPath(nrtCaptureOutputPath.empty() ? nrtOutputPath : nrtCaptureOutputPath, true);
    const std::string scorePath = outputPath + ".osc";

    if(manager != nullptr && manager->getServer() != nullptr){
        manager->getServer()->setNRTTimeProviderEnabled(false);
        manager->endNRTCapture(cancelled ? -1.0 : duration);
    }
    ofxOceanodeTime::getInstance()->setIsPlaying(false);
    ofxOceanodeTime::getInstance()->setFrameMode(false);
    nrtCaptureActive = false;
    nrtManualStop = false;
    nrtCaptureOutputPath.clear();

    if(cancelled){
        nrtStatus = "NRT capture cancelled";
        return;
    }

    if(manager == nullptr || !manager->writeNRTScore(scorePath, duration)){
        nrtStatus = "Could not write NRT score";
        return;
    }

    // One job per file. Each is the same capture; a stem only differs by which
    // bus the file-writing synth reads, so no second capture is needed.
    struct renderJob { std::string score, output, label; };
    std::vector<renderJob> jobs;

    const auto stems = manager->getNRTStems();
    const std::string base = ofFilePath::removeExt(outputPath);
    const std::string ext = "." + ofFilePath::getFileExt(outputPath);

    auto addStem = [&](const serverManager::NRTStem& stem, const std::string& path){
        const std::string stemScore = path + ".osc";
        if(!manager->writeNRTStemScore(stemScore, duration, stem.bus)){
            ofLogWarning("ofxOceanodeSuperColliderController")
                << "NRT: could not write the score for stem " << stem.name;
            return;
        }
        jobs.push_back({stemScore, path, stem.name});
    };

    // Resolve the choice by label. This list is built from the graph as it
    // stands after arming rebuilt it, which is not necessarily in the same
    // order as the list the dropdown was filled from, so a position is not a
    // stable way to name a stem.
    const auto sources = buildNRTSources(manager);
    const NRTSource* selected = nullptr;
    if(!nrtSourceLabel.empty() && !sources.empty() && nrtSourceLabel != sources.front().label){
        for(const auto& source : sources){
            if(source.label != nrtSourceLabel) continue;
            selected = &source;
            break;
        }
        if(selected == nullptr){
            ofLogWarning("ofxOceanodeSuperColliderController")
                << "NRT: the selected source \"" << nrtSourceLabel
                << "\" is not in the graph any more; rendering the master instead";
        }
    }
    const bool withMaster = nrtRecordStems;

    // The master uses the requested path exactly. Besides making the Filename
    // parameter truthful, this lets downstream recorder nodes consume it
    // directly. Stems retain their suffixes so a multi-file render still has
    // unique targets beside the master.
    const std::string masterPath = outputPath;
    if(selected == nullptr || withMaster) jobs.push_back({scorePath, masterPath, "master"});

    if(selected != nullptr){
        switch(selected->kind){
            case NRTSource::Kind::AllStems:
                for(const auto& stem : stems) addStem(stem, base + "_" + stem.name + ext);
                break;
            case NRTSource::Kind::Mixer:
                for(const auto& stem : stems){
                    if(stem.mixerName != selected->mixerName) continue;
                    addStem(stem, base + "_" + stem.name + ext);
                }
                break;
            case NRTSource::Kind::Stem:
                if(selected->stemIndex >= 0 && selected->stemIndex < (int)stems.size()){
                    const auto& stem = stems[selected->stemIndex];
                    addStem(stem, base + "_" + stem.name + ext);
                }
                break;
            case NRTSource::Kind::Master:
                break;
        }
    }

    if(jobs.empty()){
        nrtStatus = "Nothing to render";
        return;
    }

    // What the progress bar reads while the render runs. scsynth reports no
    // progress of its own, so the only live signal is the output files
    // growing: every job renders the same span at the same rate and width, so
    // one expected size covers them all.
    nrtRenderOutputs.clear();
    for(const auto& job : jobs) nrtRenderOutputs.push_back(job.output);
    nrtRenderJobsDone = 0;

    // Empty each target before any worker starts. A file left by an earlier
    // render is exactly the size a finished one will be, so progress measured
    // from file size would count it as complete the moment the render begins
    // -- with four stale stems and one real job the bar jumps straight to 80%
    // and then barely moves. Truncating also means a failed render leaves an
    // empty file rather than the previous take wearing the new take's name.
    for(const auto& path : nrtRenderOutputs){
        std::ofstream truncate(path, std::ios::binary | std::ios::trunc);
    }
    const long long frames = (long long)std::llround((double)duration *
                                                     (double)manager->preferences.hardwareSampleRate);
    // 44-byte canonical WAVE header, 32-bit float samples.
    nrtRenderExpectedBytes = 44 + frames * (long long)std::max(1, outputChannels) * 4;

    nrtRendering = true;
    // ofClamp() is float-only; keep this integral so std::min deduces.
    const int allowedParallel = std::max(1, std::min(8, nrtMaxParallelRenders));
    const int workerCount = std::max(1, std::min((int)jobs.size(), allowedParallel));
    nrtStatus = "Rendering " + ofToString((int)jobs.size()) + " file(s) with scsynth -N";
    const bool removeDC = nrtRemoveDC;
    nrtRenderThread = std::thread([this, manager, jobs, outputChannels, workerCount, removeDC](){
        nrtRenderResult = 0;
        const uint64_t startedAt = ofGetElapsedTimeMillis();
        // Every job is an independent scsynth process over the same score, so
        // they can run side by side; sequentially, a set of stems costs one
        // full pass over the patch per file.
        std::atomic<std::size_t> nextJob{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for(int worker = 0; worker < workerCount; worker++){
            workers.emplace_back([this, manager, &jobs, outputChannels, &nextJob, removeDC](){
                for(;;){
                    const std::size_t i = nextJob.fetch_add(1);
                    if(i >= jobs.size()) return;
                    const int result = manager->renderNRT(jobs[i].score, jobs[i].output, outputChannels);
                    if(result != 0){
                        nrtRenderResult = result;
                        ofLogError("ofxOceanodeSuperColliderController")
                            << "NRT: render failed for " << jobs[i].label;
                    }else if(removeDC){
                        removeDCOffsetInPlace(jobs[i].output);
                    }
                    nrtRenderJobsDone.fetch_add(1);
                }
            });
        }
        for(auto& worker : workers) worker.join();

        // Each job logs its own duration. If the total is close to their sum
        // the jobs did not overlap, whatever the worker count says; if it is
        // close to the longest one, they did.
        const double seconds = (ofGetElapsedTimeMillis() - startedAt) / 1000.0;
        ofLogNotice("ofxOceanodeSuperColliderController")
            << "NRT: rendered " << jobs.size() << " file(s) in " << seconds
            << " s, up to " << workerCount << " at a time";
        nrtRendering = false;
    });
}

bool ofxOceanodeSuperColliderController::removeDCOffsetInPlace(const std::string& wavPath,
                                                               double cornerHz){
    std::fstream file(wavPath, std::ios::in | std::ios::out | std::ios::binary);
    if(!file.is_open()){
        ofLogWarning("ofxOceanodeSuperColliderController") << "DC removal: cannot open " << wavPath;
        return false;
    }

    auto readU32 = [&file](std::uint32_t& value){
        unsigned char raw[4];
        if(!file.read(reinterpret_cast<char*>(raw), 4)) return false;
        value = (std::uint32_t)raw[0] | ((std::uint32_t)raw[1] << 8)
              | ((std::uint32_t)raw[2] << 16) | ((std::uint32_t)raw[3] << 24);
        return true;
    };
    auto readU16 = [&file](std::uint16_t& value){
        unsigned char raw[2];
        if(!file.read(reinterpret_cast<char*>(raw), 2)) return false;
        value = (std::uint16_t)((std::uint32_t)raw[0] | ((std::uint32_t)raw[1] << 8));
        return true;
    };

    char riff[4], wave[4];
    std::uint32_t riffSize = 0;
    if(!file.read(riff, 4) || !readU32(riffSize) || !file.read(wave, 4)) return false;
    if(std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0){
        ofLogWarning("ofxOceanodeSuperColliderController") << "DC removal: not a RIFF/WAVE file: " << wavPath;
        return false;
    }

    // Walk the chunks: scsynth writes fmt before data, but never assume it.
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t sampleRate = 0;
    std::streamoff dataStart = 0;
    std::uint64_t dataBytes = 0;
    for(;;){
        char id[4];
        std::uint32_t size = 0;
        if(!file.read(id, 4) || !readU32(size)) break;
        const std::streamoff body = file.tellg();
        if(std::memcmp(id, "fmt ", 4) == 0 && size >= 16){
            std::uint16_t ignored16 = 0;
            std::uint32_t ignored32 = 0;
            if(!readU16(format) || !readU16(channels) || !readU32(sampleRate) ||
               !readU32(ignored32) || !readU16(ignored16) || !readU16(bits)) return false;
        }else if(std::memcmp(id, "data", 4) == 0){
            dataStart = body;
            dataBytes = size;
            break;
        }
        file.clear();
        file.seekg(body + (std::streamoff)size + (std::streamoff)(size & 1), std::ios::beg);
    }

    // WAVE_FORMAT_IEEE_FLOAT, which is what scsynth writes for "WAVE float".
    if(dataStart == 0 || channels == 0 || format != 3 || bits != 32){
        ofLogWarning("ofxOceanodeSuperColliderController")
            << "DC removal: skipping " << wavPath << " (format " << format
            << ", " << bits << " bit, " << channels << " ch)";
        return false;
    }

    const std::uint64_t frames = dataBytes / ((std::uint64_t)channels * 4ull);
    if(frames == 0) return true;

    // y[n] = x[n] - x[n-1] + coef * y[n-1], per channel, exactly LeakDC -- but
    // with the coefficient solved from the corner frequency and this file's
    // own rate, so a 48k or 96k render gets the same filter, not a different
    // one. The clamp keeps a silly corner from making the filter unstable or
    // a no-op.
    const double rate = sampleRate > 0 ? (double)sampleRate : 44100.0;
    const double twoPi = 6.283185307179586;
    const double coef = std::min(0.9999999,
                                 std::max(0.9, 1.0 - twoPi * std::max(0.0, cornerHz) / rate));
    std::vector<double> lastIn((std::size_t)channels, 0.0);
    std::vector<double> lastOut((std::size_t)channels, 0.0);
    // Taking an offset out lifts the waveform off centre, so a file that
    // already sat near full scale can end up past it. The float file holds it
    // fine, but converting to 16- or 24-bit later would clip, so say so.
    double peak = 0.0;

    const std::size_t blockFrames = 16384;
    std::vector<float> block(blockFrames * (std::size_t)channels);
    std::uint64_t done = 0;
    while(done < frames){
        const std::size_t count = (std::size_t)std::min<std::uint64_t>(blockFrames, frames - done);
        const std::streamoff offset = dataStart + (std::streamoff)(done * (std::uint64_t)channels * 4ull);
        const std::streamsize bytes = (std::streamsize)(count * (std::size_t)channels * sizeof(float));

        file.clear();
        file.seekg(offset, std::ios::beg);
        if(!file.read(reinterpret_cast<char*>(block.data()), bytes)) return false;

        for(std::size_t frame = 0; frame < count; frame++){
            for(std::size_t channel = 0; channel < (std::size_t)channels; channel++){
                const std::size_t i = frame * (std::size_t)channels + channel;
                const double in = (double)block[i];
                const double out = in - lastIn[channel] + coef * lastOut[channel];
                lastIn[channel] = in;
                lastOut[channel] = out;
                block[i] = (float)out;
                peak = std::max(peak, std::abs(out));
            }
        }

        file.clear();
        file.seekp(offset, std::ios::beg);
        if(!file.write(reinterpret_cast<const char*>(block.data()), bytes)) return false;
        done += count;
    }

    file.flush();
    if(peak > 1.0){
        ofLogWarning("ofxOceanodeSuperColliderController")
            << "DC removal: " << wavPath << " now peaks at " << peak
            << " (" << (20.0 * std::log10(peak)) << " dBFS). The float file keeps it,"
            << " but converting to fixed point will clip.";
    }
    return file.good();
}

float ofxOceanodeSuperColliderController::getNRTRenderProgress() const{
    if(!nrtRendering.load()) return -1.0f;
    const int count = (int)nrtRenderOutputs.size();
    if(count <= 0) return -1.0f;
    const long long expected = nrtRenderExpectedBytes.load();
    if(expected <= 0) return ofClamp((float)nrtRenderJobsDone.load() / (float)count, 0.0f, 1.0f);

    // Summing every file's own fraction works whether the jobs run one after
    // another or side by side; a file not started yet simply contributes zero.
    float total = 0.0f;
    for(const auto& path : nrtRenderOutputs){
        ofFile file(path);
        if(!file.exists()) continue;
        total += ofClamp((float)((double)file.getSize() / (double)expected), 0.0f, 1.0f);
    }
    return ofClamp(total / (float)count, 0.0f, 1.0f);
}

float ofxOceanodeSuperColliderController::getNRTCaptureProgress() const{
    // An externally stopped capture has no known end, so there is nothing
    // honest to show; the bar falls back to an indeterminate sweep.
    if(!nrtCaptureActive || nrtManualStop) return -1.0f;
    const float total = std::max(0.01f, nrtDuration);
    const double now = ofxOceanodeTime::getInstance()->getGlobalTimeState().time;
    return ofClamp((float)(now / (double)total), 0.0f, 1.0f);
}

#endif // OFXOCEANODESC_HAS_TIMELINE

void ofxOceanodeSuperColliderController::createServers(){
    ofDirectory dir;
    dir.open(ofToDataPath("Supercollider/Config/Server"));
    if(dir.exists()){
        dir.sort();
        for(auto f : dir.getFiles()){
            outputServers.push_back(new serverManager());
            loadConfig(f.getAbsolutePath(), outputServers.back()->preferences);
            outputServers.back()->setAudioDevices(audioDeviceNames);
            if(outputServers.size() == 1){
                selectedAudioDeviceName = outputServers.back()->preferences.deviceName;
                selectedAudioInputDeviceName = outputServers.back()->preferences.inputDeviceName;
                selectedSampleRate = outputServers.back()->preferences.hardwareSampleRate;
                syncAudioDeviceSelection();
                syncAudioInputDeviceSelection();
                syncSampleRateSelection();
            }
        }
    }else{
        dir.createDirectory("Supercollider/Config/Server");
		ofSystemAlertDialog("Supercollider server dir not found!\nCheck ./data/SuperCollider/Config ");
    }
    //If no config found, create just one server with default settings
    if(outputServers.size() == 0){
        outputServers.push_back(new serverManager());
        outputServers.back()->setAudioDevices(audioDeviceNames);
    }
    applyAudioDeviceToServers(false);
}

void ofxOceanodeSuperColliderController::setup(){
	
	// load preferences from JSON
	ofJson json = ofLoadJson("Supercollider/Config/Controller/ControllerPreferences.json");
	if(!json.empty())
	{
		delay = json["delay"];
		volume = json["volume"];
		stereomixSize = json["stereomixsize"];
		stereomix = json["stereomix"];
        selectedAudioDeviceName = json.value<std::string>("deviceName", selectedAudioDeviceName);
        selectedAudioInputDeviceName = json.value<std::string>("inputDeviceName", selectedAudioInputDeviceName);
        selectedSampleRate = json.value<int>("sampleRate", selectedSampleRate);
        syncAudioDeviceSelection();
        syncAudioInputDeviceSelection();
        syncSampleRateSelection();
	}
	// apply preferences to output servers
    applyAudioDeviceToServers(false);
    for(auto s : outputServers)
	{
		s->setup();
		s->setDelay(delay);
		s->setVolume(volume);
		s->setStereoMix(stereomix);
		s->setStereoMixSize(stereomixSize);
	}
	
}

void ofxOceanodeSuperColliderController::draw(){
    if(ImGui::Button("Boot Servers")){
        for(auto s : outputServers) s->boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        for(auto s : outputServers) s->kill();
    }
    
    ImGui::SameLine();
    
    if(ImGui::Button("Load Defs")){
        for(auto s : outputServers) s->loadDefs();
    }

#if OFXOCEANODESC_HAS_TIMELINE
    ImGui::Separator();
    ImGui::TextUnformatted("Non-realtime WAV rendering");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputFloat("Duration (s)", &nrtDuration, 1.0f, 10.0f, "%.2f");
    nrtDuration = std::max(0.01f, nrtDuration);

    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("WAV channels", &nrtOutputChannels);
    int maxNrtChannels = 128;
    if(nrtServer >= 0 && nrtServer < (int)outputServers.size() && outputServers[nrtServer] != nullptr){
        maxNrtChannels = std::max(1, outputServers[nrtServer]->preferences.numOutputBusChannels);
    }
    nrtOutputChannels = std::max(1, std::min(nrtOutputChannels, maxNrtChannels));

    std::array<char, 512> outputBuffer{};
    std::strncpy(outputBuffer.data(), nrtOutputPath.c_str(), outputBuffer.size() - 1);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if(ImGui::InputText("Output WAV", outputBuffer.data(), outputBuffer.size())){
        nrtOutputPath = outputBuffer.data();
    }

    if(outputServers.size() > 1){
        std::vector<std::string> serverLabels;
        for(std::size_t i = 0; i < outputServers.size(); i++) serverLabels.push_back("Server " + ofToString(i));
        auto getter = [](void* data, int index, const char** out){
            auto& labels = *static_cast<std::vector<std::string>*>(data);
            if(index < 0 || index >= (int)labels.size()) return false;
            *out = labels[index].c_str();
            return true;
        };
        ImGui::Combo("NRT Server", &nrtServer, getter, &serverLabels, (int)serverLabels.size());
    }

    if(!nrtCaptureActive && !nrtRendering.load()){
        if(ImGui::Button("Render NRT WAV")) startNRTRender();
    }else if(nrtCaptureActive){
        if(ImGui::Button("Cancel NRT Capture")) completeNRTCapture(true);
    }else{
        ImGui::TextUnformatted("Rendering...");
    }
    if(!nrtStatus.empty()) ImGui::TextWrapped("%s", nrtStatus.c_str());
#endif // OFXOCEANODESC_HAS_TIMELINE
    
    ImGui::Separator();

    auto vector_getter = [](void* vec, int idx, const char** out_text)
    {
        auto& vector = *static_cast<std::vector<std::string>*>(vec);
        if (idx < 0 || idx >= static_cast<int>(vector.size())) { return false; }
        *out_text = vector.at(idx).c_str();
        return true;
    };

    if(ImGui::Button("Refresh Audio Devices")){
        reloadAudioDevices();
        syncAudioDeviceSelection();
        syncAudioInputDeviceSelection();
        for(auto s : outputServers) s->setAudioDevices(audioDeviceNames);
    }

    ImGui::TextUnformatted("Output Device");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if(ImGui::Combo("##Output Device", &audioDevice, vector_getter, static_cast<void*>(&audioDeviceNames), audioDeviceNames.size())){
        const std::string newAudioDeviceName = getAudioDeviceNameFromSelection();
        if(newAudioDeviceName != selectedAudioDeviceName){
            ofLogNotice("ofxOceanodeSuperColliderController") << "Switching audio output device to "
                << (newAudioDeviceName == "nil" ? "Default" : newAudioDeviceName);
            selectedAudioDeviceName = newAudioDeviceName;
            syncSampleRateSelection();
            applyAudioDeviceToServers(true);
        }
    }

    ImGui::TextUnformatted("Input Device");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if(ImGui::Combo("##Input Device", &audioInputDevice, vector_getter, static_cast<void*>(&inputDeviceNames), inputDeviceNames.size())){
        const std::string newInputDeviceName = getAudioInputDeviceNameFromSelection();
        if(newInputDeviceName != selectedAudioInputDeviceName){
            ofLogNotice("ofxOceanodeSuperColliderController") << "Switching audio input device to "
                << (newInputDeviceName == "nil" ? "Default" : newInputDeviceName);
            selectedAudioInputDeviceName = newInputDeviceName;
            syncSampleRateSelection();
            applyAudioDeviceToServers(true);
        }
    }

    ImGui::TextUnformatted("Sample Rate");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if(ImGui::Combo("##Sample Rate", &sampleRate, vector_getter, static_cast<void*>(&sampleRateNames), sampleRateNames.size())){
        const int newSampleRate = getSampleRateFromSelection();
        if(newSampleRate > 0 && newSampleRate != selectedSampleRate){
            ofLogNotice("ofxOceanodeSuperColliderController") << "Switching sample rate to " << newSampleRate;
            selectedSampleRate = newSampleRate;
            applyAudioDeviceToServers(true);
        }
    }

    ImGui::Separator();
    
    if(ImGui::SliderFloat("Master Volume", &volume, 0, 1)){
        for(auto &n : outputServers){
            n->setVolume(volume);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Master Mute", &mute)){
        for(auto &n : outputServers){
            if(mute) n->setVolume(0);
            else n->setVolume(volume);
        }
    }
    
    if(ImGui::SliderInt("Master Delay", &delay, 0, 5000)){
        for(auto &n : outputServers){
            n->setDelay(delay);
        }
    }
    
    if(ImGui::Checkbox("StereoMix", &stereomix)){
        for(auto &n : outputServers){
            n->setStereoMix(stereomix);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::SliderInt("StereoMix Size", &stereomixSize, 2, 100)){
        for(auto &n : outputServers){
            n->setStereoMixSize(stereomixSize);
        }
    }
	if(ImGui::Button("[Save SC Controller Settings]"))
	{
		saveControllerConfig("Supercollider/Config/Controller/ControllerPreferences.json");
	}

    ImGui::Separator();
    
    for(int i = 0; i < outputServers.size(); i++){
        if(ImGui::TreeNode(("Server " + ofToString(i)).c_str())){
            outputServers[i]->draw();
            ImGui::TreePop();
        }
    }
    
    if(ImGui::Button("[Save Server Settings]")){
        for(int i = 0; i < outputServers.size(); i++){
            saveConfig("Supercollider/Config/Server/ServerPreferences_" + ofToString(i) + ".json", outputServers[i]->preferences);
        }
    }
}

void ofxOceanodeSuperColliderController::killServers(){
    for(auto s : outputServers) s->kill();
}


void ofxOceanodeSuperColliderController::reloadAudioDevices(){
    auto devices = ofSoundStreamListDevices();
    
    audioDeviceNames = {"Default"};
    audioDeviceSuperColliderNames = {"nil"};
    audioDeviceInputChannels = {0};
    audioDeviceOutputChannels = {0};
    audioDeviceSampleRates = {{}};
    for(auto &d : devices){
        audioDeviceNames.push_back(d.name);
        audioDeviceSuperColliderNames.push_back(getSuperColliderDeviceName(d.name));
        audioDeviceInputChannels.push_back((int)d.inputChannels);
        audioDeviceOutputChannels.push_back((int)d.outputChannels);
        vector<int> rates;
        for(auto rate : d.sampleRates){
            rates.push_back((int)rate);
        }
        std::sort(rates.begin(), rates.end());
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        audioDeviceSampleRates.push_back(rates);
    }

    // Input dropdown only lists devices that actually have input channels
    inputDeviceNames = {"Default"};
    inputDeviceSuperColliderNames = {"nil"};
    inputDeviceFullIndices = {0};
    for(int i = 1; i < (int)audioDeviceNames.size(); i++){
        if(audioDeviceInputChannels[i] > 0){
            inputDeviceNames.push_back(audioDeviceNames[i]);
            inputDeviceSuperColliderNames.push_back(audioDeviceSuperColliderNames[i]);
            inputDeviceFullIndices.push_back(i);
        }
    }
    syncSampleRateSelection();
}


void ofxOceanodeSuperColliderController::saveConfig(std::string filepath, scPreferences prefs){
    ofJson json;
    json["local"] = prefs.local;
    json["loadOnPreset"] = prefs.loadOnPreset;
    json["udpPort"] = prefs.udpPort;
    json["bindAddress"] = prefs.bindAddress;
    json["numControlBusChannels"] = prefs.numControlBusChannels;
    json["numAudioBusChannels"] = prefs.numAudioBusChannels;
    json["numInputBusChannels"] = prefs.numInputBusChannels;
    json["numOutputBusChannels"] = prefs.numOutputBusChannels;
    json["blockSize"] = prefs.blockSize;
    json["hardwareBufferSize"] = prefs.hardwareBufferSize;
    json["hardwareSampleRate"] = prefs.hardwareSampleRate;
    json["numBuffers"] = prefs.numBuffers;
    json["maxNodes"] = prefs.maxNodes;
    json["maxSynthDefs"] = prefs.maxSynthDefs;
    json["memSize"] = prefs.memSize;
    json["numWireBufs"] = prefs.numWireBufs;
    json["numRGens"] = prefs.numRGens;
    json["maxLogins"] = prefs.maxLogins;
    json["safetyClipThreshold"] = prefs.safetyClipThreshold;
    json["deviceName"] = prefs.deviceName;
    json["inputDeviceName"] = prefs.inputDeviceName;
    json["verbosity"] = prefs.verbosity;
    json["ugensPlugins"] = prefs.ugensPlugins;
    
    ofSavePrettyJson(filepath, json);
}

void ofxOceanodeSuperColliderController::saveControllerConfig(std::string filepath){
	ofJson json;

	json["delay"] = delay;
	json["volume"] = volume;
	json["stereomixsize"] = stereomixSize;
	json["stereomix"] = stereomix;
    json["deviceName"] = selectedAudioDeviceName;
    json["inputDeviceName"] = selectedAudioInputDeviceName;
    json["sampleRate"] = selectedSampleRate;

	ofSavePrettyJson(filepath, json);
}


void ofxOceanodeSuperColliderController::loadConfig(std::string filepath, scPreferences &prefs){
    ofJson json = ofLoadJson(filepath);
    if(!json.empty()){
        prefs.local = json["local"];
        prefs.loadOnPreset = json.value<bool>("loadOnPreset", false);
        prefs.udpPort = json["udpPort"];
        prefs.bindAddress = json["bindAddress"];
        prefs.numControlBusChannels = json["numControlBusChannels"];
        prefs.numAudioBusChannels = json["numAudioBusChannels"];
        prefs.numInputBusChannels = json["numInputBusChannels"];
        prefs.numOutputBusChannels = json["numOutputBusChannels"];
        prefs.blockSize = json["blockSize"];
        prefs.hardwareBufferSize = json["hardwareBufferSize"];
        prefs.hardwareSampleRate = json["hardwareSampleRate"];
        prefs.numBuffers = json["numBuffers"];
        prefs.maxNodes = json["maxNodes"];
        prefs.maxSynthDefs = json["maxSynthDefs"];
        prefs.memSize = json["memSize"];
        prefs.numWireBufs = json["numWireBufs"];
        prefs.numRGens = json["numRGens"];
        prefs.maxLogins = json["maxLogins"];
        prefs.safetyClipThreshold = json["safetyClipThreshold"];
        prefs.deviceName = json.value<std::string>("deviceName", "nil");
        prefs.inputDeviceName = json.value<std::string>("inputDeviceName", "nil");
        prefs.verbosity = json["verbosity"];
        prefs.ugensPlugins = json["ugensPlugins"];
    }
}

void ofxOceanodeSuperColliderController::syncAudioDeviceSelection(){
    if(selectedAudioDeviceName.empty() || selectedAudioDeviceName == "Default"){
        selectedAudioDeviceName = "nil";
    }

    audioDevice = 0;
    if(selectedAudioDeviceName == "nil"){
        return;
    }

    for(int i = 0; i < (int)audioDeviceSuperColliderNames.size(); i++){
        if(audioDeviceSuperColliderNames[i] == selectedAudioDeviceName){
            audioDevice = i;
            return;
        }
    }

    ofLogWarning("ofxOceanodeSuperColliderController") << "Saved audio device \"" << selectedAudioDeviceName
        << "\" is not available on this computer. Falling back to the system default device.";
    selectedAudioDeviceName = "nil";
    audioDevice = 0;
}

void ofxOceanodeSuperColliderController::syncAudioInputDeviceSelection(){
    if(selectedAudioInputDeviceName.empty() || selectedAudioInputDeviceName == "Default"){
        selectedAudioInputDeviceName = "nil";
    }

    audioInputDevice = 0;
    if(selectedAudioInputDeviceName == "nil"){
        return;
    }

    for(int i = 0; i < (int)inputDeviceSuperColliderNames.size(); i++){
        if(inputDeviceSuperColliderNames[i] == selectedAudioInputDeviceName){
            audioInputDevice = i;
            return;
        }
    }

    ofLogWarning("ofxOceanodeSuperColliderController") << "Saved audio input device \"" << selectedAudioInputDeviceName
        << "\" is not available on this computer. Falling back to the system default device.";
    selectedAudioInputDeviceName = "nil";
    audioInputDevice = 0;
}

void ofxOceanodeSuperColliderController::syncSampleRateSelection(){
    if(selectedSampleRate <= 0){
        selectedSampleRate = scPreferences().hardwareSampleRate;
    }

    sampleRateNames.clear();
    sampleRateValues.clear();

    if(audioDevice >= 0 && audioDevice < (int)audioDeviceSampleRates.size()){
        sampleRateValues = audioDeviceSampleRates[audioDevice];
    }

    // scsynth needs input and output devices running at the same rate,
    // so only offer rates both devices support when they differ
    if(audioInputDevice > 0 && audioInputDevice < (int)inputDeviceFullIndices.size()){
        const int fullIndex = inputDeviceFullIndices[audioInputDevice];
        if(fullIndex != audioDevice && fullIndex < (int)audioDeviceSampleRates.size() && !audioDeviceSampleRates[fullIndex].empty()){
            const auto &inputRates = audioDeviceSampleRates[fullIndex];
            if(sampleRateValues.empty()){
                sampleRateValues = inputRates;
            }else{
                vector<int> commonRates;
                for(int rate : sampleRateValues){
                    if(std::find(inputRates.begin(), inputRates.end(), rate) != inputRates.end()){
                        commonRates.push_back(rate);
                    }
                }
                if(!commonRates.empty()){
                    sampleRateValues = commonRates;
                }else{
                    ofLogWarning("ofxOceanodeSuperColliderController")
                        << "Input and output devices share no common sample rate. scsynth may fail to boot.";
                }
            }
        }
    }

    if(sampleRateValues.empty()){
        sampleRateValues.push_back(selectedSampleRate);
    }else if(std::find(sampleRateValues.begin(), sampleRateValues.end(), selectedSampleRate) == sampleRateValues.end()){
        selectedSampleRate = sampleRateValues.front();
    }

    sampleRate = 0;
    for(int i = 0; i < (int)sampleRateValues.size(); i++){
        sampleRateNames.push_back(ofToString(sampleRateValues[i]));
        if(sampleRateValues[i] == selectedSampleRate){
            sampleRate = i;
        }
    }
}

std::string ofxOceanodeSuperColliderController::getSuperColliderDeviceName(const std::string& deviceName) const{
    const auto separator = deviceName.find(": ");
    if(separator == std::string::npos){
        return deviceName;
    }
    return deviceName.substr(separator + 2);
}

bool ofxOceanodeSuperColliderController::hasAvailableAudioDevice(const std::string& deviceName) const{
    if(deviceName.empty() || deviceName == "Default" || deviceName == "nil"){
        return true;
    }

    return std::find(audioDeviceSuperColliderNames.begin(), audioDeviceSuperColliderNames.end(), deviceName)
        != audioDeviceSuperColliderNames.end();
}

std::string ofxOceanodeSuperColliderController::getAudioDeviceNameFromSelection() const{
    if(audioDevice <= 0 || audioDevice >= (int)audioDeviceSuperColliderNames.size()){
        return "nil";
    }
    return audioDeviceSuperColliderNames[audioDevice];
}

bool ofxOceanodeSuperColliderController::hasAvailableAudioInputDevice(const std::string& deviceName) const{
    if(deviceName.empty() || deviceName == "Default" || deviceName == "nil"){
        return true;
    }

    return std::find(inputDeviceSuperColliderNames.begin(), inputDeviceSuperColliderNames.end(), deviceName)
        != inputDeviceSuperColliderNames.end();
}

std::string ofxOceanodeSuperColliderController::getAudioInputDeviceNameFromSelection() const{
    if(audioInputDevice <= 0 || audioInputDevice >= (int)inputDeviceSuperColliderNames.size()){
        return "nil";
    }
    return inputDeviceSuperColliderNames[audioInputDevice];
}

int ofxOceanodeSuperColliderController::getSampleRateFromSelection() const{
    if(sampleRate < 0 || sampleRate >= (int)sampleRateValues.size()){
        return selectedSampleRate;
    }
    return sampleRateValues[sampleRate];
}

void ofxOceanodeSuperColliderController::applyAudioDeviceToServers(bool restartServers){
    const bool requestedDeviceAvailable = hasAvailableAudioDevice(selectedAudioDeviceName);
    const std::string deviceName = requestedDeviceAvailable ? getAudioDeviceNameFromSelection() : "nil";
    if(!requestedDeviceAvailable){
        selectedAudioDeviceName = "nil";
        audioDevice = 0;
        syncSampleRateSelection();
    }

    const bool requestedInputDeviceAvailable = hasAvailableAudioInputDevice(selectedAudioInputDeviceName);
    const std::string inputDeviceName = requestedInputDeviceAvailable ? getAudioInputDeviceNameFromSelection() : "nil";
    if(!requestedInputDeviceAvailable){
        selectedAudioInputDeviceName = "nil";
        audioInputDevice = 0;
        syncSampleRateSelection();
    }

    // Input channel count comes from the explicit input device when one is
    // selected, otherwise from the output device (which also feeds the input
    // side when no input device is set, matching previous behavior)
    int inputChannels = 0;
    if(audioInputDevice > 0 && audioInputDevice < (int)inputDeviceFullIndices.size()){
        const int fullIndex = inputDeviceFullIndices[audioInputDevice];
        if(fullIndex < (int)audioDeviceInputChannels.size()) inputChannels = audioDeviceInputChannels[fullIndex];
    }else if(audioDevice >= 0 && audioDevice < (int)audioDeviceInputChannels.size()){
        inputChannels = audioDeviceInputChannels[audioDevice];
    }
    const int outputChannels = (audioDevice >= 0 && audioDevice < (int)audioDeviceOutputChannels.size()) ? audioDeviceOutputChannels[audioDevice] : 0;
    selectedAudioDeviceName = deviceName;
    selectedAudioInputDeviceName = inputDeviceName;
    selectedSampleRate = getSampleRateFromSelection();

    for(auto s : outputServers){
        s->setAudioDevices(audioDeviceNames);
        s->setAudioDeviceNames(deviceName, inputDeviceName, inputChannels, outputChannels);
        s->setHardwareSampleRate(selectedSampleRate);
    }

    if(restartServers){
        for(auto s : outputServers) s->prepareForRestart();
        for(auto s : outputServers) s->boot();
    }
}
