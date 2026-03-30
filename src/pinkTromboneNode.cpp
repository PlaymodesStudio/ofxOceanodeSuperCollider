//
//  pinkTromboneNode.cpp
//  ofxOceanodeSuperCollider
//

#include "pinkTromboneNode.h"
#include "pinkTrombone/noise.hpp"   // pt_noise_seed()

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

pinkTromboneNode::pinkTromboneNode()
    : scNode("Pink Trombone")
{
    // Zero the struct so pointer members are null when setup() is never called
    // (registerModel constructs+destructs a temporary without calling setup())
    memset(&tractProps, 0, sizeof(tractProps));

#if PT_HAS_SHM
    instanceId = nextInstanceId.fetch_add(1, std::memory_order_relaxed);
#endif
}

pinkTromboneNode::~pinkTromboneNode() {
    cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// setup
// ─────────────────────────────────────────────────────────────────────────────

void pinkTromboneNode::setup() {
    // Register output port in the SC graph
    addOutput("Out");

    // ── Parameters ───────────────────────────────────────────────────────────
    addParameter(frequency.set        ("Frequency",          140.f,  20.f, 2000.f));
    addParameter(tenseness.set        ("Tenseness",          0.6f,   0.f,  1.f));
    addParameter(tongueIndex.set      ("Tongue Index",       12.9f,  6.f,  28.f));
    addParameter(tongueDiameter.set   ("Tongue Diameter",    2.43f,  0.f,  3.5f));
    addParameter(constrictionIndex.set("Constriction Index", -1.f,  -1.f,  44.f));
    addParameter(constrictionDiam.set ("Constriction Dia",   3.5f,  -2.f,  3.5f));
    addParameter(fricativeIntensity.set("Fricative",         0.f,    0.f,  1.f));
    addParameter(gain.set             ("Gain",               1.f,    0.f,  2.f));
    // Off by default: audio goes to SC only. Enable if you also want local speaker output.
    addParameter(monitor.set          ("Monitor",            false));
    addParameter(numChannels.set      ("N Chan",             1,      1,    2));

    // ── Mirror parameters into atomics on change ──────────────────────────────
    listeners.push(frequency.newListener([this](float &v) {
        aFreq.store(v, std::memory_order_relaxed);
    }));
    listeners.push(tenseness.newListener([this](float &v) {
        aTenseness.store(v, std::memory_order_relaxed);
    }));
    listeners.push(tongueIndex.newListener([this](float &v) {
        aTongueIndex.store(v, std::memory_order_relaxed);
    }));
    listeners.push(tongueDiameter.newListener([this](float &v) {
        aTongueDiameter.store(v, std::memory_order_relaxed);
    }));
    listeners.push(constrictionIndex.newListener([this](float &v) {
        aConstrictionIndex.store(v, std::memory_order_relaxed);
    }));
    listeners.push(constrictionDiam.newListener([this](float &v) {
        aConstrictionDiam.store(v, std::memory_order_relaxed);
    }));
    listeners.push(fricativeIntensity.newListener([this](float &v) {
        aFricative.store(v, std::memory_order_relaxed);
    }));
    listeners.push(gain.newListener([this](float &v) {
        aGain.store(v, std::memory_order_relaxed);
    }));
    listeners.push(monitor.newListener([this](bool &v) {
        aMonitor.store(v, std::memory_order_relaxed);
    }));
    listeners.push(numChannels.newListener([this](int &) {
        // Rebuild synths when channel count changes
        for (auto &pair : synthInstances) {
            if (pair.second) {
                pair.second->free();
                delete pair.second;
                pair.second = nullptr;
            }
        }
        synthInstances.clear();
    }));

    // ── Seed noise and init engine ────────────────────────────────────────────
    pt_noise_seed(42u);
    initEngine();

#if PT_HAS_SHM
    setupSHM();
#endif

    setupAudioUnit();
}

// ─────────────────────────────────────────────────────────────────────────────
// update  (main thread — nothing audio-critical here)
// ─────────────────────────────────────────────────────────────────────────────

void pinkTromboneNode::update(ofEventArgs &/*args*/) {
    // Tongue index bounds come from the tract; expose them after engine is ready.
    if (engineReady) {
        long lo = tract->tongueIndexLowerBound();
        long hi = tract->tongueIndexUpperBound();
        if (tongueIndex.getMin() != (float)lo || tongueIndex.getMax() != (float)hi) {
            tongueIndex.setMin((float)lo);
            tongueIndex.setMax((float)hi);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// initEngine  — allocate and wire up Pink Trombone objects
// ─────────────────────────────────────────────────────────────────────────────

void pinkTromboneNode::initEngine() {
    memset(&tractProps, 0, sizeof(tractProps));
    initializeTractProps(&tractProps, (int)NUM_CONSTRICTIONS);

    glottis  = std::make_unique<Glottis>(PT_SR);
    tract    = std::make_unique<Tract>((sample_t)PT_SR,
                                       (sample_t)PT_BLOCK_SIZE / (sample_t)PT_SR,
                                       &tractProps);
    noiseGen = std::make_unique<WhiteNoise>(88200L); // 2 seconds of noise buffer
    // release store: audio thread's acquire load will see all objects fully constructed
    engineReady.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// CoreAudio
// ─────────────────────────────────────────────────────────────────────────────

bool pinkTromboneNode::setupAudioUnit() {
    AudioComponentDescription desc{};
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    audioComponent = AudioComponentFindNext(nullptr, &desc);
    if (!audioComponent) {
        ofLogError("pinkTromboneNode") << "AudioComponentFindNext failed";
        return false;
    }

    if (AudioComponentInstanceNew(audioComponent, &audioUnit) != noErr) {
        ofLogError("pinkTromboneNode") << "AudioComponentInstanceNew failed";
        return false;
    }

    // Non-interleaved float32 stereo at 44 100 Hz
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = PT_SR;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = 2;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = 4;
    asbd.mBytesPerPacket   = 4;

    AudioUnitSetProperty(audioUnit,
                         kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0,
                         &asbd, sizeof(asbd));

    AURenderCallbackStruct cb{};
    cb.inputProc       = audioRenderCallback;
    cb.inputProcRefCon = this;
    AudioUnitSetProperty(audioUnit,
                         kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0,
                         &cb, sizeof(cb));

    if (AudioUnitInitialize(audioUnit) != noErr) {
        ofLogError("pinkTromboneNode") << "AudioUnitInitialize failed";
        return false;
    }
    if (AudioOutputUnitStart(audioUnit) != noErr) {
        ofLogError("pinkTromboneNode") << "AudioOutputUnitStart failed";
        return false;
    }

#if PT_HAS_SHM
    ofLogNotice("pinkTromboneNode") << "CoreAudio output started (id=" << instanceId << ")";
#else
    ofLogNotice("pinkTromboneNode") << "CoreAudio output started";
#endif
    return true;
}

void pinkTromboneNode::cleanupAudioUnit() {
    if (audioUnit) {
        AudioOutputUnitStop(audioUnit);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        audioUnit = nullptr;
    }
}

// ── Audio render callback (real-time thread) ──────────────────────────────────

OSStatus pinkTromboneNode::audioRenderCallback(
    void*                        inRefCon,
    AudioUnitRenderActionFlags*  /*ioActionFlags*/,
    const AudioTimeStamp*        /*inTimeStamp*/,
    UInt32                       /*inBusNumber*/,
    UInt32                       inNumberFrames,
    AudioBufferList*             ioData)
{
    auto* self = static_cast<pinkTromboneNode*>(inRefCon);
    // acquire load pairs with the release store in initEngine() —
    // guarantees glottis/tract/noiseGen are fully visible before we use them
    if (!self->engineReady.load(std::memory_order_acquire)) {
        for (UInt32 b = 0; b < ioData->mNumberBuffers; b++)
            memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
        return noErr;
    }

    const float freq    = self->aFreq.load(std::memory_order_relaxed);
    const float tens    = self->aTenseness.load(std::memory_order_relaxed);
    const float tIdx    = self->aTongueIndex.load(std::memory_order_relaxed);
    const float tDia    = self->aTongueDiameter.load(std::memory_order_relaxed);
    const float cIdx    = self->aConstrictionIndex.load(std::memory_order_relaxed);
    const float cDia    = self->aConstrictionDiam.load(std::memory_order_relaxed);
    const float fric    = self->aFricative.load(std::memory_order_relaxed);
    const float gainVal = self->aGain.load(std::memory_order_relaxed);

    self->glottis->setTargetFrequency(freq);
    self->glottis->setTargetTenseness(tens);
    self->tract->setRestDiameter(tIdx, tDia);
    if (cIdx >= 0.f)
        self->tract->setConstriction(cIdx, cDia, fric);
    else
        self->tract->setConstriction(-1.f, 3.5f, 0.f);

    float* outL = (ioData->mNumberBuffers > 0)
                  ? static_cast<float*>(ioData->mBuffers[0].mData) : nullptr;
    float* outR = (ioData->mNumberBuffers > 1)
                  ? static_cast<float*>(ioData->mBuffers[1].mData) : nullptr;

    // 2048 frames × 2 ch × 4 bytes = 16 KB — safe on the render thread stack.
    // Hardware block size is ≤ 1024 in normal use; 2048 is a conservative ceiling.
    static constexpr uint32_t kMaxFrames = 2048;
    float shmBuf[kMaxFrames * 2];
    const uint32_t framesToProcess = (inNumberFrames > kMaxFrames) ? kMaxFrames : inNumberFrames;

    for (UInt32 i = 0; i < framesToProcess; i++) {
        const float lambda1 = (float)self->sampleInBlock / PT_BLOCK_SIZE;
        const float lambda2 = (self->sampleInBlock + 0.5f) / PT_BLOCK_SIZE;

        float noise = self->noiseGen->runStep();

        float g1  = self->glottis->runStep(lambda1, noise);
        float nm1 = self->glottis->getNoiseModulator();
        self->tract->runStep(g1, noise, lambda1, nm1);
        float vocalOut = self->tract->lipOutput + self->tract->noseOutput;

        float g2  = self->glottis->runStep(lambda2, noise);
        float nm2 = self->glottis->getNoiseModulator();
        self->tract->runStep(g2, noise, lambda2, nm2);
        vocalOut += self->tract->lipOutput + self->tract->noseOutput;

        // 0.125 = original Pink Trombone output gain
        vocalOut *= 0.125f * gainVal;

        if (++self->sampleInBlock >= PT_BLOCK_SIZE) {
            self->glottis->finishBlock();
            self->tract->finishBlock();
            self->sampleInBlock = 0;
        }

        // Local speaker monitoring (off by default — avoids double-signal when SC is active)
        const float localSample = self->aMonitor.load(std::memory_order_relaxed) ? vocalOut : 0.f;
        if (outL) outL[i] = localSample;
        if (outR) outR[i] = localSample;

        // Interleave for SHM (mono → stereo: same signal on both channels)
        shmBuf[i * 2]     = vocalOut;
        shmBuf[i * 2 + 1] = vocalOut;
    }

#if PT_HAS_SHM
    if (self->aSHMActive.load(std::memory_order_relaxed))
        self->writeSHM(shmBuf, framesToProcess);
#endif

    return noErr;
}

// ─────────────────────────────────────────────────────────────────────────────
// SHM bridge  (identical layout to radioStationVLC)
// ─────────────────────────────────────────────────────────────────────────────

#if PT_HAS_SHM

bool pinkTromboneNode::setupSHM() {
    if (shmBase) {
        static_cast<OceanodeRadioSHMHeader*>(shmBase)->active = 0;
        munmap(shmBase, ORAD_TOTAL_BYTES);
        shmBase = nullptr;
    }
    if (shmFd >= 0) { close(shmFd); shmFd = -1; }

    char shmName[OCEANODE_RADIO_SHM_NAME_MAX];
    snprintf(shmName, sizeof(shmName), "%s_%d", OCEANODE_RADIO_SHM_BASE, instanceId);

    shmFd = shm_open(shmName, O_CREAT | O_EXCL | O_RDWR, 0644);
    bool isNew = (shmFd >= 0);
    if (!isNew && errno == EEXIST)
        shmFd = shm_open(shmName, O_RDWR, 0644);

    if (shmFd < 0) {
        ofLogError("pinkTromboneNode") << "shm_open failed: " << strerror(errno);
        return false;
    }
    if (isNew && ftruncate(shmFd, (off_t)ORAD_TOTAL_BYTES) != 0) {
        ofLogError("pinkTromboneNode") << "ftruncate failed: " << strerror(errno);
        close(shmFd); shmFd = -1;
        shm_unlink(shmName);
        return false;
    }

    shmBase = mmap(nullptr, ORAD_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (shmBase == MAP_FAILED) {
        ofLogError("pinkTromboneNode") << "mmap failed: " << strerror(errno);
        close(shmFd); shmFd = -1; shmBase = nullptr;
        return false;
    }

    auto* hdr         = static_cast<OceanodeRadioSHMHeader*>(shmBase);
    hdr->magic        = OCEANODE_RADIO_SHM_MAGIC;
    hdr->version      = OCEANODE_RADIO_SHM_VERSION;
    hdr->sampleRate   = PT_SR;
    hdr->channels     = ORAD_CHANNELS;
    hdr->bufferFrames = ORAD_RING_FRAMES;
    hdr->generation++;
    hdr->active    = 0;
    hdr->scNodeID  = 0;
    __atomic_store_n(&hdr->writePos, 0ULL, __ATOMIC_RELEASE);
    memset(oradRingBuffer(shmBase), 0, ORAD_RING_BYTES);

    ofLogNotice("pinkTromboneNode") << "SHM ready: " << shmName
                                    << " (" << (ORAD_TOTAL_BYTES / 1024 / 1024) << " MB)";
    return true;
}

void pinkTromboneNode::cleanupSHM(bool withUnlink) {
    if (shmBase) {
        static_cast<OceanodeRadioSHMHeader*>(shmBase)->active = 0;
        munmap(shmBase, ORAD_TOTAL_BYTES);
        shmBase = nullptr;
    }
    if (shmFd >= 0) { close(shmFd); shmFd = -1; }
    if (withUnlink) {
        char shmName[OCEANODE_RADIO_SHM_NAME_MAX];
        snprintf(shmName, sizeof(shmName), "%s_%d", OCEANODE_RADIO_SHM_BASE, instanceId);
        shm_unlink(shmName);
    }
}

void pinkTromboneNode::writeSHM(const float* interleavedBuf, uint32_t frames) noexcept {
    if (!shmBase || frames == 0) return;

    auto*    hdr      = static_cast<OceanodeRadioSHMHeader*>(shmBase);
    float*   ring     = oradRingBuffer(shmBase);
    uint64_t wp       = __atomic_load_n(&hdr->writePos, __ATOMIC_RELAXED);
    uint32_t startIdx = (uint32_t)(wp & ORAD_RING_MASK);

    if (startIdx + frames <= ORAD_RING_FRAMES) {
        memcpy(ring + startIdx * ORAD_CHANNELS, interleavedBuf,
               frames * ORAD_CHANNELS * sizeof(float));
    } else {
        uint32_t first  = ORAD_RING_FRAMES - startIdx;
        uint32_t second = frames - first;
        memcpy(ring + startIdx * ORAD_CHANNELS, interleavedBuf,
               first * ORAD_CHANNELS * sizeof(float));
        memcpy(ring, interleavedBuf + first * ORAD_CHANNELS,
               second * ORAD_CHANNELS * sizeof(float));
    }
    __atomic_store_n(&hdr->writePos, wp + frames, __ATOMIC_RELEASE);
}

#endif // PT_HAS_SHM

// ─────────────────────────────────────────────────────────────────────────────
// scNode graph-management interface
// ─────────────────────────────────────────────────────────────────────────────

std::string pinkTromboneNode::getSynthDefName() const {
    // Reuse radioStationVLC's SynthDef: reads from SHM by instanceId, routes to bus.
    return "RadioStation" + ofToString(numChannels.get());
}

void pinkTromboneNode::buildSynth(ofxSCServer* server) {
    if (!server) return;
    if (synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
    }
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

void pinkTromboneNode::createSynth(ofxSCServer* server) {
    if (!server) return;
    if (!synthInstances.count(server) || !synthInstances[server])
        buildSynth(server);

    ofxSCSynth* synth = synthInstances[server];
#if PT_HAS_SHM
    synth->set("instanceId", (float)instanceId);
#endif
    synth->set("amp", std::vector<float>(numChannels.get(), gain.get()));
    if (outputBuses.count(server) && outputBuses[server].count(0))
        synth->set("out", outputBuses[server][0]);

    synth->createAndRun(0, 1, getActive());

#if PT_HAS_SHM
    if (!shmBase) setupSHM();
    if (shmBase) {
        static_cast<OceanodeRadioSHMHeader*>(shmBase)->scNodeID =
            static_cast<uint32_t>(synth->nodeID);
    }
#endif

    aSHMActive.store(true, std::memory_order_relaxed);
#if PT_HAS_SHM
    if (shmBase)
        static_cast<OceanodeRadioSHMHeader*>(shmBase)->active = 1;
#endif
}

void pinkTromboneNode::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if (!server || !synthInstances.count(server) || !synthInstances[server]) return;
    ofxSCSynth* synth = synthInstances[server];
    synth->set("amp", std::vector<float>(numChannels.get(), gain.get()));
    if (outputBuses.count(server) && outputBuses[server].count(0))
        synth->set("out", outputBuses[server][0]);
    synth->moveBefore(nodeID);
}

void pinkTromboneNode::free(ofxSCServer* server) {
    if (synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
        synthInstances.erase(server);
    }
}

void pinkTromboneNode::setOutputBus(ofxSCServer* server, int index, int bus) {
    if (!server) return;
    outputBuses[server][index] = bus;
    if (synthInstances.count(server) && synthInstances[server]) {
        // "out" is always the first (and only) output bus on the RadioStation synthdef
        if (index == 0)
            synthInstances[server]->set("out", bus);
    }
}

// Pink Trombone is a pure source node — it has no SC inputs from other nodes.
void pinkTromboneNode::setInputBus(ofxSCServer* /*server*/, scNode* /*node*/, int /*bus*/) {}
void pinkTromboneNode::resetInputBusses(ofxSCServer* /*server*/, int /*targetBus*/) {}

int pinkTromboneNode::getOutputBusIndex(ofxSCServer* server, int index) {
    if (outputBuses.count(server) && outputBuses[server].count(index))
        return outputBuses[server][index];
    return -1;
}

int pinkTromboneNode::getLastSynthID(ofxSCServer* server) {
    if (synthInstances.count(server) && synthInstances[server])
        return synthInstances[server]->nodeID;
    return -1;
}

void pinkTromboneNode::activate() {
    aSHMActive.store(true, std::memory_order_relaxed);
#if PT_HAS_SHM
    if (shmBase) static_cast<OceanodeRadioSHMHeader*>(shmBase)->active = 1;
#endif
    for (auto &pair : synthInstances)
        if (pair.second) pair.second->run(true);
}

void pinkTromboneNode::deactivate() {
    aSHMActive.store(false, std::memory_order_relaxed);
#if PT_HAS_SHM
    if (shmBase) static_cast<OceanodeRadioSHMHeader*>(shmBase)->active = 0;
#endif
    for (auto &pair : synthInstances)
        if (pair.second) pair.second->run(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// cleanup
// ─────────────────────────────────────────────────────────────────────────────

void pinkTromboneNode::cleanup() {
    aSHMActive.store(false, std::memory_order_relaxed);

    // Stop the audio unit first — AudioUnitUninitialize() blocks until the
    // last render callback has returned, so it is safe to free engine state below.
    cleanupAudioUnit();

    // Tear down engine objects BEFORE freeing tractProps:
    // Tract stores a raw t_tractProps* and may touch those arrays in its dtor.
    // C++ would otherwise destroy tractProps first (last-declared = first-destroyed)
    // leaving Tract with dangling pointers.
    if (engineReady.load(std::memory_order_relaxed)) {
        engineReady.store(false, std::memory_order_release);
        glottis.reset();
        noiseGen.reset();
        tract.reset();   // Tract dtor runs here — tractProps arrays still alive
        if (tractProps.tractDiameter) { ::free(tractProps.tractDiameter); tractProps.tractDiameter = nullptr; }
        if (tractProps.noseDiameter)  { ::free(tractProps.noseDiameter);  tractProps.noseDiameter  = nullptr; }
    }

    for (auto &pair : synthInstances)
        if (pair.second) { pair.second->free(); delete pair.second; }
    synthInstances.clear();

#if PT_HAS_SHM
    cleanupSHM(true);
#endif
}
