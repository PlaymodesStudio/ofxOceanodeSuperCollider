#include "scPolyComb.h"
#include "ofxOceanodeShared.h"
#include <cmath>

static bool scalarSlider(const char* label, ofParameter<vector<float>>& p, float min, float max);
static bool scalarDiscreteSlider(const char* label, ofParameter<vector<float>>& p, int min, int max);
static bool scalarIntSlider(const char* label, ofParameter<int>& p, int min, int max);
static bool scalarEnumCombo(const char* label, ofParameter<vector<float>>& p, const char* const items[], int itemCount);
static ImU32 desaturatedPadColor(ImU32 baseColor, float amount = 0.75f, float brightness = 0.22f);
static float reflectUnitCPP(float value);
static float customPowCPP(float value, float pow);
static float biPowCPP(float value, float biPow);
static float computeMorphOscillatorCPP(float ph, float roundness, float skew, float pulseWidth, bool pulseCentered);
static float applySampleHoldBeatPhaseCPP(float beatTime, float beatDuration, float sampleHoldDivisions);
static float hashUnitCPP(float x);
static void drawLfoWavePreview(const char* id, float width, float height, ImU32 color, int li,
                               float beatDuration, float roundness, float skew,
                               float pulseWidth, bool pulseCentered, float pow, float biPow, float sampleHoldDivisions);

namespace {
float getBroadcastValue(const vector<float>& values, int index, float fallback) {
    if (values.empty()) return fallback;
    if (values.size() == 1) return values[0];
    if (index >= 0 && index < (int)values.size()) return values[index];
    return values.back();
}

void broadcastVectorParam(ofParameter<vector<float>>& p, float value) {
    auto v = p.get();
    if (v.empty()) v.assign(1, value);
    else std::fill(v.begin(), v.end(), value);
    p.set(v);
}

vector<float> expandToVoices(const vector<float>& values, int voiceCount, float fallback = 0.0f) {
    if (voiceCount <= 0) return {};
    if (values.empty()) return vector<float>(voiceCount, fallback);
    if ((int)values.size() == voiceCount) return values;
    vector<float> expanded = values;
    if ((int)expanded.size() > voiceCount) {
        expanded.resize(voiceCount);
    } else {
        expanded.resize(voiceCount, values.back());
    }
    return expanded;
}

template <typename T>
void jsonSaveValue(ofJson& j, const string& key, const T& value) {
    j[key] = value;
}

template <typename T>
void jsonLoadValue(const ofJson& j, const string& key, ofParameter<T>& param) {
    if (j.count(key)) param.set(j[key].get<T>());
}
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

scPolyComb::scPolyComb() : scNode("polyComb") {}

scPolyComb::~scPolyComb() {
    listeners.unsubscribeAll();
    for (auto& p : synthInstances) {
        if (p.second) { p.second->free(); delete p.second; }
    }
    synthInstances.clear();
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void scPolyComb::setup() {

    // ── Core ─────────────────────────────────────────────────────────────────
    addParameter(nVoices.set("Voices", 4, 1, MAX_VOICES));
    addParameter(masterLevel.set("Master", 0.8f, 0.f, 1.f));
    addParameter(resetLFOs.set("Reset"));

    addParameter(pitch.set("Pitch",          {60.f}, {0.f},  {127.f}));
    addParameter(oscAmp.set("Osc Amp",       {0.5f}, {0.f},  {1.f}));
    sawLevel.set("Saw",         {1.f},  {0.f},  {1.f});
    triLevel.set("Tri",         {0.f},  {0.f},  {1.f});
    sineLevel.set("Sine",       {0.f},  {0.f},  {1.f});
    pulseLevel.set("Pulse",     {0.f},  {0.f},  {1.f});
    pulseWidth.set("PW",        {0.5f}, {0.f},  {1.f});
    superSawLevel.set("SuperSaw",  {0.f}, {0.f},{1.f});
    superSawDetune.set("SS Detune",{0.5f},{0.f},{1.f});
    whiteNoiseLevel.set("White",{0.f},  {0.f},  {1.f});
    pinkNoiseLevel.set("Pink",  {0.f},  {0.f},  {1.f});
    dustLevel.set("Dust",       {0.f},  {0.f},  {1.f});
    impulseLevel.set("Impulse", {0.f},  {0.f},  {1.f});

    pitchMod.set("PitchMod",      {0.f},  {0.f},  {1.f});
    pitchModRange.set("PMRange",  {0.f},  {-24.f},{24.f});
    pitchDetune.set("PitchDetune",{0.f},  {0.f},  {24.f});
    pitchTranspose.set("Transpose",{0.f}, {-24.f},{24.f});
    pitchScramble.set("Scramble", false);
    vibFreq.set("VibFreq",        {5.f},  {0.f},  {20.f});
    vibAmp.set("VibAmp",          {0.f},  {0.f},  {2.f});

    fmRatio.set("FMRatio",  {1.f},  {0.5f}, {8.f});
    fmAmp.set("FMAmp",      {0.f},  {0.f},  {1.f});
    fmMod.set("FMMod",      {1.f},  {0.f},  {1.f});

    filterType.set("FiltType",  {0.f},  {0.f},  {3.f});
    cutoffMin.set("CutMin",     {30.f}, {0.f},  {135.f});
    cutoffMax.set("CutMax",     {100.f},{0.f},  {135.f});
    cutoffMod.set("CutMod",     {1.f},  {0.f},  {1.f});
    resonance.set("Resonance",  {0.5f}, {0.f},  {1.f});

    addParameter(combPitch.set("CombPitch",  {60.f}, {0.f},  {127.f}));
    combPitchMod.set("CombPM",  {0.f},  {0.f},  {1.f});
    combPMRange.set("CombPMR",  {0.f},  {-24.f},{24.f});
    combDetune.set("CombDetune",{0.f},  {0.f},  {24.f});
    combTranspose.set("CombTranspose",{0.f}, {-24.f}, {24.f});
    combFeed.set("CombFeed",    {0.f},  {-0.999f},{0.999f});
    combMix.set("CombMix",      {0.f},  {0.f},  {1.f});

    addParameter(ringModPitch.set("RMPitch", {72.f}, {12.f}, {120.f}));
    ringModDetune.set("RMDetune", {0.f}, {0.f}, {24.f});
    ringModMix.set("RMMix",     {0.f},  {0.f},  {1.f});

    postFType.set("PFType",    {0.f},  {0.f},  {3.f});
    postCutMin.set("PCutMin",  {30.f}, {0.f},  {135.f});
    postCutMax.set("PCutMax",  {110.f},{0.f},  {135.f});
    postCutMod.set("PCutMod",  {1.f},  {0.f},  {1.f});
    postRes.set("PRes",        {0.5f}, {0.f},  {1.f});

    minTremHz.set("TremMin",   {1.f},  {0.01f},{20.f});
    maxTremHz.set("TremMax",   {8.f},  {0.01f},{20.f});
    tremHzMod.set("TremMod",   {0.f},  {0.f},  {1.f});
    tremAmp.set("TremAmp",     {0.f},  {0.f},  {1.f});

    ssLevel.set("SSLevel",  {1.f},  {0.f},  {20.f});
    ssMix.set("SSMix",      {0.f},  {0.f},  {1.f});

    // ── LFOs ─────────────────────────────────────────────────────────────────
    int initN = nVoices.get();
    for (int li = 0; li < NUM_LFOS; li++) {
        string ln = "LFO" + ofToString(li+1);
        lfoFreq[li].set(ln + " Freq",     {1.f},  {0.125f},{64.f});
        lfoFreqDetune[li].set(ln + " FreqDetune", {0.f}, {0.f}, {8.f});
        lfoInitPhase[li].set(ln + " InitPhase", 0.f, 0.f, 1.f);
        lfoPulseWidth[li].set(ln + " PulseWidth", {0.5f}, {0.f}, {1.f});
        lfoPulseCentered[li].set(ln + " CenteredPW", true);
        lfoRound[li].set(ln + " Round",   {0.5f}, {0.f},  {1.f});
        lfoSkew[li].set(ln + " Skew",     {0.f},  {-1.f}, {1.f});
        lfoPow[li].set(ln + " Pow",       {0.f},  {-1.f}, {1.f});
        lfoBiPow[li].set(ln + " BiPow",   {0.f},  {-1.f}, {1.f});
        lfoSampleHold[li].set(ln + " S&H", {0.f}, {0.f}, {32.f});
        lfoNWaves[li].set( ln + " NWav",  1.f,   0.f,  (float)initN);
        lfoNorm[li].set(   ln + " Norm",  false);
        lfoInvertP[li].set(ln + " Inv",   0.f,   0.f,  1.f);
        lfoSymP[li].set(   ln + " Sym",   0,     0,    initN/2);
        lfoShuffleP[li].set(ln + " Shuf", 0.f,   0.f,  1.f);
        lfoRandP[li].set(  ln + " Rand",  0.f,   0.f,  1.f);
        lfoOffsetP[li].set(ln + " Off",   0.f,  -(float)initN/2, (float)initN/2);
        lfoQuantP[li].set( ln + " Qnt",   initN, 1,    initN);
        lfoCombP[li].set(  ln + " Cmb",   0.f,   0.f,  1.f);
        lfoModuloP[li].set(ln + " Mod",   initN, 1,    initN);
        // Initialise baseIndexer
        lfoIndexers[li].numWaves_Param    = 1.f;
        lfoIndexers[li].normalize_Param   = false;
        lfoIndexers[li].indexInvert_Param = 0.f;
        lfoIndexers[li].symmetry_Param    = 0;
        lfoIndexers[li].wrapShuffle_Param = false;
        lfoIndexers[li].indexShuffle_Param= 0.f;
        lfoIndexers[li].indexRand_Param   = 0.f;
        lfoIndexers[li].indexOffset_Param = 0.f;
        lfoIndexers[li].indexQuant_Param  = initN;
        lfoIndexers[li].combination_Param = 0.f;
        lfoIndexers[li].modulo_Param      = initN;
        lfoIndexers[li].discrete_Param    = false;
        lfoIndexers[li].indexCountChanged(initN);
        // lfoPhOff: internal — computed from indexer, sent to SC (not on canvas)
        lfoPhOff[li].set(ln + " PhOff_", vector<float>(initN, 0.f),
                         vector<float>(1, 0.f), vector<float>(1, 1.f));
    }

    for (int li = 0; li < NUM_LFOS; li++) {
        addParameter(extLFO[li].set("ExtLFO" + ofToString(li + 1), {0.f}, {0.f}, {1.f}));
    }

    // ── LFO Outputs (KR bus → Oceanode output ports) ──────────────────────────
    for (int li = 0; li < NUM_LFOS; li++) {
        string ln = "LFO" + ofToString(li+1);
        lfoDisplayData[li].assign(MAX_VOICES, 0.f);
        addOutputParameter(lfoOut[li].set(ln + " Out",
            vector<float>(nVoices.get(), 0.f),
            vector<float>(1, 0.f),
            vector<float>(1, 1.f)));
    }

    modMatrix.set("ModMatrix",
        vector<float>(NUM_MOD_SOURCES * NUM_MOD_TARGETS, 0.f),
        vector<float>(NUM_MOD_SOURCES * NUM_MOD_TARGETS, -1.f),
        vector<float>(NUM_MOD_SOURCES * NUM_MOD_TARGETS, 1.f));

    // ── Custom GUI toggle ─────────────────────────────────────────────────────
    addParameter(showWindow.set("Show GUI", false));
    addInspectorParameter(windowWidth.set("Win W",  700.f, 300.f, 1400.f));
    addInspectorParameter(windowHeight.set("Win H", 500.f, 200.f, 900.f));

    // ── Output port ──────────────────────────────────────────────────────────
    scNode::addOutput("Out");

    // ─── Listeners ────────────────────────────────────────────────────────────

    listeners.push(nVoices.newListener([this](int& n) {
        if (n != oldNVoices) {
            resizeVoiceParams(n);
            updatePitchScrambleOrder();
            for (int li = 0; li < NUM_LFOS; li++) {
                lfoIndexers[li].indexCountChanged(n);
                lfoNWaves[li].setMax((float)n);
                lfoNWaves[li] = ofClamp(lfoNWaves[li].get(), 0.f, (float)n);
                lfoSymP[li].setMax(n / 2);
                lfoSymP[li] = ofClamp((int)lfoSymP[li].get(), 0, n / 2);
                lfoOffsetP[li].setMin(-(float)n / 2);
                lfoOffsetP[li].setMax( (float)n / 2);
                lfoOffsetP[li] = ofClamp(lfoOffsetP[li].get(), -(float)n/2, (float)n/2);
                lfoQuantP[li].setMax(n);
                lfoQuantP[li] = ofClamp((int)lfoQuantP[li].get(), 1, n);
                lfoModuloP[li].setMax(n);
                lfoModuloP[li] = ofClamp((int)lfoModuloP[li].get(), 1, n);
            }
            updateLFOPhaseOffsets();
            rebuildSynths();
            oldNVoices = n;
        }
    }));

    // Master
    listeners.push(masterLevel.newListener([this](float& v) {
        sendScalarParam("masterlevel", v);
    }));
    listeners.push(resetLFOs.newListener([this](void) {
        lfoResetCounter++;
        lfoPreviewResetTime = ofGetElapsedTimef();
        for (auto& pair : synthInstances) {
            if (pair.second) pair.second->set("lforeset", (float)lfoResetCounter);
        }
    }));

    // Oscillators
    listeners.push(pitch.newListener([this](vector<float>&)             {
        auto processed = getProcessedPitchValues();
        sendFloatParam("pitch", processed);
    }));
    listeners.push(oscAmp.newListener([this](vector<float>& v)          { sendFloatParam("oscamp", v); }));
    listeners.push(sawLevel.newListener([this](vector<float>& v)        { sendFloatParam("sawlevel", v); }));
    listeners.push(triLevel.newListener([this](vector<float>& v)        { sendFloatParam("trilevel", v); }));
    listeners.push(sineLevel.newListener([this](vector<float>& v)       { sendFloatParam("sinelevel", v); }));
    listeners.push(pulseLevel.newListener([this](vector<float>& v)      { sendFloatParam("pulselevel", v); }));
    listeners.push(pulseWidth.newListener([this](vector<float>& v)      { sendFloatParam("pulsewidth", v); }));
    listeners.push(superSawLevel.newListener([this](vector<float>& v)   { sendFloatParam("supersawlevel", v); }));
    listeners.push(superSawDetune.newListener([this](vector<float>& v)  { sendFloatParam("supersawdetune", v); }));
    listeners.push(whiteNoiseLevel.newListener([this](vector<float>& v) { sendFloatParam("whitenoiselevel", v); }));
    listeners.push(pinkNoiseLevel.newListener([this](vector<float>& v)  { sendFloatParam("pinknoiselevel", v); }));
    listeners.push(dustLevel.newListener([this](vector<float>& v)       { sendFloatParam("dustlevel", v); }));
    listeners.push(impulseLevel.newListener([this](vector<float>& v)    { sendFloatParam("impulselevel", v); }));

    // Pitch
    listeners.push(pitchMod.newListener([this](vector<float>& v)        { sendFloatParam("pitchmod", v); }));
    listeners.push(pitchModRange.newListener([this](vector<float>& v)   { sendFloatParam("pitchmodrange", v); }));
    listeners.push(pitchDetune.newListener([this](vector<float>& v)     { sendFloatParam("pitchdetune", v); }));
    listeners.push(pitchTranspose.newListener([this](vector<float>& v)  { sendFloatParam("pitchtranspose", v); }));
    listeners.push(pitchScramble.newListener([this](bool&) {
        updatePitchScrambleOrder();
        auto processed = getProcessedPitchValues();
        sendFloatParam("pitch", processed);
    }));
    listeners.push(vibFreq.newListener([this](vector<float>& v)         { sendFloatParam("vibfreq", v); }));
    listeners.push(vibAmp.newListener([this](vector<float>& v)          { sendFloatParam("vibamp", v); }));

    // FM
    listeners.push(fmRatio.newListener([this](vector<float>& v)         { sendFloatParam("fmratio", v); }));
    listeners.push(fmAmp.newListener([this](vector<float>& v)           { sendFloatParam("fmamp", v); }));
    listeners.push(fmMod.newListener([this](vector<float>& v)           { sendFloatParam("fmmod", v); }));

    // Filter
    listeners.push(filterType.newListener([this](vector<float>& v)      { sendFloatParam("filtertype", v); }));
    listeners.push(cutoffMin.newListener([this](vector<float>& v)       { sendFloatParam("cutoffmin", v); }));
    listeners.push(cutoffMax.newListener([this](vector<float>& v)       { sendFloatParam("cutoffmax", v); }));
    listeners.push(cutoffMod.newListener([this](vector<float>& v)       { sendFloatParam("cutoffmod", v); }));
    listeners.push(resonance.newListener([this](vector<float>& v)       { sendFloatParam("resonance", v); }));

    // Comb
    listeners.push(combPitch.newListener([this](vector<float>& v)       { sendFloatParam("combpitch", v); }));
    listeners.push(combPitchMod.newListener([this](vector<float>& v)    { sendFloatParam("combpitchmod", v); }));
    listeners.push(combPMRange.newListener([this](vector<float>& v)     { sendFloatParam("combpmrange", v); }));
    listeners.push(combDetune.newListener([this](vector<float>& v)      { sendFloatParam("combdetune", v); }));
    listeners.push(combTranspose.newListener([this](vector<float>& v)   { sendFloatParam("combtranspose", v); }));
    listeners.push(combFeed.newListener([this](vector<float>& v)        { sendFloatParam("combfeed", v); }));
    listeners.push(combMix.newListener([this](vector<float>& v)         { sendFloatParam("combmix", v); }));

    // Ring mod
    listeners.push(ringModPitch.newListener([this](vector<float>& v)    { sendFloatParam("ringmodpitch", v); }));
    listeners.push(ringModDetune.newListener([this](vector<float>& v)   { sendFloatParam("ringmoddetune", v); }));
    listeners.push(ringModMix.newListener([this](vector<float>& v)      { sendFloatParam("ringmodmix", v); }));

    // Post filter
    listeners.push(postFType.newListener([this](vector<float>& v)       { sendFloatParam("postftype", v); }));
    listeners.push(postCutMin.newListener([this](vector<float>& v)      { sendFloatParam("postcutmin", v); }));
    listeners.push(postCutMax.newListener([this](vector<float>& v)      { sendFloatParam("postcutmax", v); }));
    listeners.push(postCutMod.newListener([this](vector<float>& v)      { sendFloatParam("postcutmod", v); }));
    listeners.push(postRes.newListener([this](vector<float>& v)         { sendFloatParam("postres", v); }));

    // Tremolo
    listeners.push(minTremHz.newListener([this](vector<float>& v)       { sendFloatParam("mintremhz", v); }));
    listeners.push(maxTremHz.newListener([this](vector<float>& v)       { sendFloatParam("maxtremhz", v); }));
    listeners.push(tremHzMod.newListener([this](vector<float>& v)       { sendFloatParam("tremhzmod", v); }));
    listeners.push(tremAmp.newListener([this](vector<float>& v)         { sendFloatParam("trecamp", v); }));

    // Sine shaper
    listeners.push(ssLevel.newListener([this](vector<float>& v)         { sendFloatParam("sslevel", v); }));
    listeners.push(ssMix.newListener([this](vector<float>& v)           { sendFloatParam("ssmix", v); }));

    // LFOs — audio params
    for (int li = 0; li < NUM_LFOS; li++) {
        string ln = "lfo" + ofToString(li);
        listeners.push(lfoFreq[li].newListener([this, ln](vector<float>& v) mutable {
            auto hzValues = convertBeatDurationsToHz(v);
            sendFloatParam(ln + "freq", hzValues);
        }));
        listeners.push(lfoFreqDetune[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "freqdetune", v);
        }));
        listeners.push(lfoInitPhase[li].newListener([this, ln](float& v) mutable {
            sendScalarParam(ln + "initphase", v);
        }));
        listeners.push(lfoRound[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "round", v);
        }));
        listeners.push(lfoSkew[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "skew", v);
        }));
        listeners.push(lfoPulseWidth[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "pulsewidth", v);
        }));
        listeners.push(lfoPulseCentered[li].newListener([this, ln](bool& v) mutable {
            sendScalarParam(ln + "pulsecentered", v ? 1.0f : 0.0f);
        }));
        listeners.push(lfoPow[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "pow", v);
        }));
        listeners.push(lfoBiPow[li].newListener([this, ln](vector<float>& v) mutable {
            sendFloatParam(ln + "bipow", v);
        }));
        listeners.push(lfoSampleHold[li].newListener([this, ln](vector<float>& v) mutable {
            auto hzValues = convertBeatDivisionsToHz(v);
            sendFloatParam(ln + "samplehold", hzValues);
        }));
        listeners.push(extLFO[li].newListener([this, li](vector<float>& v) mutable {
            sendFloatParam("extlfo" + ofToString(li), v);
        }));
        // lfoPhOff is computed — no direct listener needed
    }
    // LFO phase indexer controls
    for (int li = 0; li < NUM_LFOS; li++) {
        listeners.push(lfoNWaves[li].newListener([this, li](float& f) {
            lfoIndexers[li].numWaves_Param = f;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoNorm[li].newListener([this, li](bool& b) {
            lfoIndexers[li].normalize_Param = b;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoInvertP[li].newListener([this, li](float& f) {
            lfoIndexers[li].indexInvert_Param = f;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoSymP[li].newListener([this, li](int& i) {
            lfoIndexers[li].symmetry_Param = i;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoShuffleP[li].newListener([this, li](float& f) {
            lfoIndexers[li].indexShuffle_Param = f;
            lfoIndexers[li].indexShuffleChanged(f);
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoRandP[li].newListener([this, li](float& f) {
            lfoIndexers[li].indexRand_Param = f;
            lfoIndexers[li].indexRandChanged(f);
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoOffsetP[li].newListener([this, li](float& f) {
            lfoIndexers[li].indexOffset_Param = f;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoQuantP[li].newListener([this, li](int& i) {
            lfoIndexers[li].indexQuant_Param = i;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoCombP[li].newListener([this, li](float& f) {
            lfoIndexers[li].combination_Param = f;
            updateLFOPhaseOffsets();
        }));
        listeners.push(lfoModuloP[li].newListener([this, li](int& i) {
            lfoIndexers[li].modulo_Param = i;
            updateLFOPhaseOffsets();
        }));
    }

    // ModMatrix
    listeners.push(modMatrix.newListener([this](vector<float>& v) {
        if ((int)v.size() != NUM_MOD_SOURCES * NUM_MOD_TARGETS) {
            v.resize(NUM_MOD_SOURCES * NUM_MOD_TARGETS, 0.f);
            modMatrix.set(v);
            return;
        }
        for (auto& p : synthInstances)
            if (p.second) p.second->set("mod_matrix", v);
    }));

    oldNVoices = nVoices.get();
    resizeVoiceParams(nVoices.get());
    updatePitchScrambleOrder();
    updateLFOPhaseOffsets();
}

// ─── Update ──────────────────────────────────────────────────────────────────

void scPolyComb::update(ofEventArgs&) {
    // Read LFO KR buses from the first server that has valid buses.
    // The custom GUI display mirrors this exact KR data so the visuals
    // match the node outputs instead of using a parallel C++ preview path.
    for (auto& p : synthInstances) {
        ofxSCServer* server = p.first;
        if (!p.second) continue;
        if (!lfoBuses.count(server)) continue;
        auto& buses = lfoBuses[server];
        int n = nVoices.get();
        for (int li = 0; li < NUM_LFOS; li++) {
            if (!buses[li]) continue;
            const vector<float>& raw = buses[li]->readValues;
            lfoDisplayData[li].resize(n, 0.f);
            for (int vi = 0; vi < n; vi++)
                lfoDisplayData[li][vi] = (vi < (int)raw.size()) ? raw[vi] : 0.f;
            lfoOut[li].set(lfoDisplayData[li]);
            buses[li]->requestValues();
        }
        break;  // Only read from one server — all run the same params
    }
}

void scPolyComb::draw(ofEventArgs&) {
    if (showWindow.get()) drawPolyCombWindow();
}

void scPolyComb::setBpm(float bpm) {
    currentBpm = bpm;
    for (auto& entry : synthInstances) {
        if (entry.second) sendAllParams(entry.second);
    }
}

// ─── Synth Lifecycle (reference: scFM7Drone.cpp) ─────────────────────────────

void scPolyComb::buildSynth(ofxSCServer* server) {
    if (!server) return;
    if (synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
    }
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

void scPolyComb::createSynth(ofxSCServer* server) {
    if (!server) return;
    if (!synthInstances[server])
        synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);

    freeLFOBuses(server);
    allocateLFOBuses(server);

    auto* s = synthInstances[server];
    sendAllParams(s);
    sendLFOBusIndices(server, s);

    if (outputBuses.count(server) && outputBuses[server].count(0))
        s->set("out", outputBuses[server][0]);

    s->createAndRun(0, 1, getActive());

    for (int li = 0; li < NUM_LFOS; li++)
        if (lfoBuses[server][li]) lfoBuses[server][li]->requestValues();
}

void scPolyComb::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if (!server) return;
    if (!synthInstances.count(server) || !synthInstances[server]) return;
    auto* s = synthInstances[server];
    sendAllParams(s);
    sendLFOBusIndices(server, s);
    if (outputBuses.count(server) && outputBuses[server].count(0))
        s->set("out", outputBuses[server][0]);
    s->moveBefore(nodeID);
}

void scPolyComb::free(ofxSCServer* server) {
    if (!server) return;
    freeLFOBuses(server);
    if (synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
        synthInstances.erase(server);
    }
    outputBuses.erase(server);
}

void scPolyComb::activate() {
    for (auto& p : synthInstances)
        if (p.second) p.second->run(true);
}

void scPolyComb::deactivate() {
    for (auto& p : synthInstances)
        if (p.second) p.second->run(false);
}

int scPolyComb::getLastSynthID(ofxSCServer* server) {
    if (synthInstances.count(server) && synthInstances[server])
        return synthInstances[server]->nodeID;
    return -1;
}

void scPolyComb::setOutputBus(ofxSCServer* server, int index, int bus) {
    if (!server) return;
    outputBuses[server][index] = bus;
    if (synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("out", bus);
}

string scPolyComb::getSynthDefName() const {
    return "polyComb" + ofToString(nVoices.get());
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

void scPolyComb::sendFloatParam(const string& name, vector<float>& values) {
    int n = nVoices.get();
    for (auto& p : synthInstances) {
        if (!p.second) continue;
        if ((int)values.size() == 1) {
            p.second->setMultiple(name, values[0], n);
        } else {
            p.second->set(name, expandToVoices(values, n));
        }
    }
}

void scPolyComb::sendScalarParam(const string& name, float value) {
    int n = nVoices.get();
    for (auto& p : synthInstances) {
        if (!p.second) continue;
        p.second->setMultiple(name, value, n);
    }
}

void scPolyComb::sendAllParams(ofxSCSynth* s) {
    if (!s) return;
    int n = nVoices.get();

    auto send = [&](const string& name, vector<float>& v) {
        if ((int)v.size() == 1) s->setMultiple(name, v[0], n);
        else s->set(name, expandToVoices(v, n));
    };

    s->setMultiple("masterlevel", masterLevel.get(), n);
    s->set("lforeset", (float)lfoResetCounter);
    auto processedPitch = getProcessedPitchValues();
    send("pitch",            processedPitch);
    send("oscamp",           const_cast<vector<float>&>(oscAmp.get()));
    send("sawlevel",         const_cast<vector<float>&>(sawLevel.get()));
    send("trilevel",         const_cast<vector<float>&>(triLevel.get()));
    send("sinelevel",        const_cast<vector<float>&>(sineLevel.get()));
    send("pulselevel",       const_cast<vector<float>&>(pulseLevel.get()));
    send("pulsewidth",       const_cast<vector<float>&>(pulseWidth.get()));
    send("supersawlevel",    const_cast<vector<float>&>(superSawLevel.get()));
    send("supersawdetune",   const_cast<vector<float>&>(superSawDetune.get()));
    send("whitenoiselevel",  const_cast<vector<float>&>(whiteNoiseLevel.get()));
    send("pinknoiselevel",   const_cast<vector<float>&>(pinkNoiseLevel.get()));
    send("dustlevel",        const_cast<vector<float>&>(dustLevel.get()));
    send("impulselevel",     const_cast<vector<float>&>(impulseLevel.get()));
    send("pitchmod",         const_cast<vector<float>&>(pitchMod.get()));
    send("pitchmodrange",    const_cast<vector<float>&>(pitchModRange.get()));
    send("pitchdetune",      const_cast<vector<float>&>(pitchDetune.get()));
    send("pitchtranspose",   const_cast<vector<float>&>(pitchTranspose.get()));
    send("vibfreq",          const_cast<vector<float>&>(vibFreq.get()));
    send("vibamp",           const_cast<vector<float>&>(vibAmp.get()));
    send("fmratio",          const_cast<vector<float>&>(fmRatio.get()));
    send("fmamp",            const_cast<vector<float>&>(fmAmp.get()));
    send("fmmod",            const_cast<vector<float>&>(fmMod.get()));
    send("filtertype",       const_cast<vector<float>&>(filterType.get()));
    send("cutoffmin",        const_cast<vector<float>&>(cutoffMin.get()));
    send("cutoffmax",        const_cast<vector<float>&>(cutoffMax.get()));
    send("cutoffmod",        const_cast<vector<float>&>(cutoffMod.get()));
    send("resonance",        const_cast<vector<float>&>(resonance.get()));
    send("combpitch",        const_cast<vector<float>&>(combPitch.get()));
    send("combpitchmod",     const_cast<vector<float>&>(combPitchMod.get()));
    send("combpmrange",      const_cast<vector<float>&>(combPMRange.get()));
    send("combdetune",       const_cast<vector<float>&>(combDetune.get()));
    send("combtranspose",    const_cast<vector<float>&>(combTranspose.get()));
    send("combfeed",         const_cast<vector<float>&>(combFeed.get()));
    send("combmix",          const_cast<vector<float>&>(combMix.get()));
    send("ringmodpitch",     const_cast<vector<float>&>(ringModPitch.get()));
    send("ringmoddetune",    const_cast<vector<float>&>(ringModDetune.get()));
    send("ringmodmix",       const_cast<vector<float>&>(ringModMix.get()));
    send("postftype",        const_cast<vector<float>&>(postFType.get()));
    send("postcutmin",       const_cast<vector<float>&>(postCutMin.get()));
    send("postcutmax",       const_cast<vector<float>&>(postCutMax.get()));
    send("postcutmod",       const_cast<vector<float>&>(postCutMod.get()));
    send("postres",          const_cast<vector<float>&>(postRes.get()));
    send("mintremhz",        const_cast<vector<float>&>(minTremHz.get()));
    send("maxtremhz",        const_cast<vector<float>&>(maxTremHz.get()));
    send("tremhzmod",        const_cast<vector<float>&>(tremHzMod.get()));
    send("trecamp",          const_cast<vector<float>&>(tremAmp.get()));
    send("sslevel",          const_cast<vector<float>&>(ssLevel.get()));
    send("ssmix",            const_cast<vector<float>&>(ssMix.get()));

    for (int li = 0; li < NUM_LFOS; li++) {
        string ln = "lfo" + ofToString(li);
        auto freqHz = convertBeatDurationsToHz(lfoFreq[li].get());
        send(ln + "freq",     freqHz);
        send(ln + "freqdetune", const_cast<vector<float>&>(lfoFreqDetune[li].get()));
        s->setMultiple((ln + "initphase").c_str(), lfoInitPhase[li].get(), n);
        send(ln + "phoff",    const_cast<vector<float>&>(lfoPhOff[li].get()));
        send(ln + "round",    const_cast<vector<float>&>(lfoRound[li].get()));
        send(ln + "skew",     const_cast<vector<float>&>(lfoSkew[li].get()));
        send(ln + "pulsewidth", const_cast<vector<float>&>(lfoPulseWidth[li].get()));
        s->setMultiple((ln + "pulsecentered").c_str(), lfoPulseCentered[li].get() ? 1.0f : 0.0f, n);
        send(ln + "pow",      const_cast<vector<float>&>(lfoPow[li].get()));
        send(ln + "bipow",    const_cast<vector<float>&>(lfoBiPow[li].get()));
        auto sampleHoldHz = convertBeatDivisionsToHz(lfoSampleHold[li].get());
        send(ln + "samplehold", sampleHoldHz);
        send("extlfo" + ofToString(li), const_cast<vector<float>&>(extLFO[li].get()));
    }

    s->set("mod_matrix", expandToVoices(modMatrix.get(), NUM_MOD_SOURCES * NUM_MOD_TARGETS));
}

vector<float> scPolyComb::getProcessedPitchValues() const {
    auto values = pitch.get();
    if (values.empty()) return values;
    if (!pitchScramble.get() || values.size() <= 1 || pitchScrambleOrder.empty()) return values;

    vector<float> scrambled(values.size(), values.back());
    for (size_t i = 0; i < values.size(); i++) {
        int src = (i < pitchScrambleOrder.size()) ? pitchScrambleOrder[i] : (int)i;
        src = ofClamp(src, 0, (int)values.size() - 1);
        scrambled[i] = values[src];
    }
    return scrambled;
}

void scPolyComb::updatePitchScrambleOrder() {
    int count = nVoices.get();
    pitchScrambleOrder.resize(count);
    std::iota(pitchScrambleOrder.begin(), pitchScrambleOrder.end(), 0);
    if (!pitchScramble.get() || count <= 1) return;

    std::mt19937 rng(0x504f4c59 + count * 31);
    std::shuffle(pitchScrambleOrder.begin(), pitchScrambleOrder.end(), rng);
}

vector<float> scPolyComb::convertBeatDurationsToHz(const vector<float>& beatValues) const {
    vector<float> hzValues = beatValues;
    const float beatsPerSecond = std::max(currentBpm, 1.0f) / 60.0f;
    for (float& value : hzValues) {
        value = beatsPerSecond / std::max(value, 0.001f);
    }
    return hzValues;
}

vector<float> scPolyComb::convertBeatDivisionsToHz(const vector<float>& beatValues) const {
    vector<float> hzValues = beatValues;
    const float beatsPerSecond = std::max(currentBpm, 1.0f) / 60.0f;
    for (float& value : hzValues) {
        value = (value <= 0.0f) ? 0.0f : (value * beatsPerSecond);
    }
    return hzValues;
}

vector<float> scPolyComb::computePreviewLFOValues(int li) const {
    int n = nVoices.get();
    vector<float> preview(n, 0.f);
    if (li < 0 || li >= NUM_LFOS) return preview;

    const auto& phases = lfoPhOff[li].get();
    const auto& beatDurations = lfoFreq[li].get();
    const auto& freqDetunes = lfoFreqDetune[li].get();
    const float initPhase = lfoInitPhase[li].get();
    const auto& rounds = lfoRound[li].get();
    const auto& skews = lfoSkew[li].get();
    const auto& pulseWidths = lfoPulseWidth[li].get();
    const bool pulseCentered = lfoPulseCentered[li].get();
    const auto& pows = lfoPow[li].get();
    const auto& biPows = lfoBiPow[li].get();
    const auto& sampleHolds = lfoSampleHold[li].get();
    const float timeSeconds = ofGetElapsedTimef();
    const float elapsedSinceReset = std::max(0.0f, timeSeconds - lfoPreviewResetTime);
    const float beatsPerSecond = std::max(currentBpm, 1.0f) / 60.0f;
    const float elapsedBeats = elapsedSinceReset * beatsPerSecond;

    for (int vi = 0; vi < n; vi++) {
        float phaseOffset = getBroadcastValue(phases, vi, 0.f);
        float beatDuration = getBroadcastValue(beatDurations, vi, 1.f);
        float freqDetuneHz = getBroadcastValue(freqDetunes, vi, 0.f);
        float roundness = getBroadcastValue(rounds, vi, 0.5f);
        float skew = getBroadcastValue(skews, vi, 0.f);
        float pulseWidth = getBroadcastValue(pulseWidths, vi, 0.5f);
        float pow = getBroadcastValue(pows, vi, 0.f);
        float biPow = getBroadcastValue(biPows, vi, 0.f);
        float sampleHoldDivisions = getBroadcastValue(sampleHolds, vi, 0.f);
        float detunedHz = (beatsPerSecond / std::max(beatDuration, 0.001f)) + (hashUnitCPP(500.0f + li * 29.0f + vi * 7.0f) * freqDetuneHz);
        float detunedBeatDuration = beatsPerSecond / std::max(detunedHz, 0.001f);
        float sampleBeatTime = applySampleHoldBeatPhaseCPP(elapsedBeats, beatDuration, sampleHoldDivisions);
        float phase = std::fmod(sampleBeatTime / std::max(detunedBeatDuration, 0.001f) + initPhase + phaseOffset, 1.0f);
        if (phase < 0.f) phase += 1.f;
        float value = computeMorphOscillatorCPP(phase, roundness, skew, pulseWidth, pulseCentered);
        if (std::abs(pow) > 1.0e-5f) value = reflectUnitCPP(customPowCPP(value, pow));
        if (std::abs(biPow) > 1.0e-5f) value = biPowCPP(value, biPow);
        preview[vi] = ofClamp(value, 0.f, 1.f);
    }
    return preview;
}

void scPolyComb::resizeVoiceParams(int n) {
    // Keep scalar-by-default Oceanode params at size 1 so the node GUI stays scalar.
    // Only resize parameters that are already carrying real per-voice vectors.
    auto resize = [&](ofParameter<vector<float>>& p) {
        auto v = p.get();
        if (v.empty()) return;
        if (v.size() > 1 && (int)v.size() != n) {
            float def = v.back();
            v.resize(n, def);
            p.set(v);
        }
    };

    resize(pitch);        resize(oscAmp);
    resize(sawLevel);     resize(triLevel);  resize(sineLevel);
    resize(pulseLevel);   resize(pulseWidth);resize(superSawLevel);
    resize(superSawDetune);resize(whiteNoiseLevel);resize(pinkNoiseLevel);
    resize(dustLevel);    resize(impulseLevel);
    resize(pitchMod);     resize(pitchModRange); resize(pitchDetune); resize(pitchTranspose); resize(vibFreq); resize(vibAmp);
    resize(fmRatio);      resize(fmAmp);     resize(fmMod);
    resize(filterType);   resize(cutoffMin); resize(cutoffMax);
    resize(cutoffMod);    resize(resonance);
    resize(combPitch);    resize(combPitchMod); resize(combPMRange); resize(combDetune); resize(combTranspose);
    resize(combFeed);     resize(combMix);
    resize(ringModPitch); resize(ringModDetune); resize(ringModMix);
    resize(postFType);    resize(postCutMin); resize(postCutMax);
    resize(postCutMod);   resize(postRes);
    resize(minTremHz);    resize(maxTremHz); resize(tremHzMod); resize(tremAmp);
    resize(ssLevel);      resize(ssMix);
    for (int li = 0; li < NUM_LFOS; li++) {
        resize(lfoFreq[li]); resize(lfoFreqDetune[li]); resize(lfoRound[li]);
        resize(lfoSkew[li]);
        resize(lfoPulseWidth[li]); resize(lfoPow[li]); resize(lfoBiPow[li]);
        resize(lfoSampleHold[li]); resize(extLFO[li]);
    }
}

void scPolyComb::updateLFOPhaseOffsets() {
    for (int li = 0; li < NUM_LFOS; li++) {
        const int n = nVoices.get();
        vector<float> phases;
        bool useSimpleSpread =
            std::abs(lfoNWaves[li].get() - 1.0f) < 0.0001f &&
            !lfoNorm[li].get() &&
            std::abs(lfoInvertP[li].get()) < 0.0001f &&
            lfoSymP[li].get() == 0 &&
            std::abs(lfoShuffleP[li].get()) < 0.0001f &&
            std::abs(lfoRandP[li].get()) < 0.0001f &&
            std::abs(lfoOffsetP[li].get()) < 0.0001f &&
            lfoQuantP[li].get() == n &&
            std::abs(lfoCombP[li].get()) < 0.0001f &&
            lfoModuloP[li].get() == n;

        if (useSimpleSpread) {
            phases.resize(n, 0.f);
            for (int vi = 0; vi < n; vi++) phases[vi] = (float)vi / (float)std::max(n, 1);
        } else {
            lfoIndexers[li].recomputeIndexs();
            phases = lfoIndexers[li].getIndexs();
        }

        lfoPhOff[li].set(phases);
        string ln = "lfo" + ofToString(li);
        sendFloatParam(ln + "phoff", phases);
    }
}

void scPolyComb::rebuildSynths() {
    for (auto& p : synthInstances) {
        if (!p.first) continue;
        if (p.second) { p.second->free(); delete p.second; }
        freeLFOBuses(p.first);
        p.second = new ofxSCSynth(getSynthDefName(), p.first);
        allocateLFOBuses(p.first);
        if (outputBuses.count(p.first) && outputBuses[p.first].count(0))
            p.second->set("out", outputBuses[p.first][0]);
        sendAllParams(p.second);
        sendLFOBusIndices(p.first, p.second);
        p.second->createAndRun(0, 1, getActive());
        for (int li = 0; li < NUM_LFOS; li++)
            if (lfoBuses[p.first][li]) lfoBuses[p.first][li]->requestValues();
    }
}

void scPolyComb::allocateLFOBuses(ofxSCServer* server) {
    if (!server) return;
    auto& buses = lfoBuses[server];
    buses.fill(nullptr);
    int n = nVoices.get();
    for (int li = 0; li < NUM_LFOS; li++) {
        buses[li] = new ofxSCBus(RATE_CONTROL, n, server);
        if (buses[li]->index < 0 || buses[li]->index >= 4096 ||
            !server->controlBusses[buses[li]->index]) {
            buses[li]->free();
            delete buses[li];
            buses[li] = nullptr;
        }
    }
}

void scPolyComb::freeLFOBuses(ofxSCServer* server) {
    if (!server || !lfoBuses.count(server)) return;
    auto& buses = lfoBuses[server];
    for (int li = 0; li < NUM_LFOS; li++) {
        if (buses[li]) { buses[li]->free(); delete buses[li]; buses[li] = nullptr; }
    }
    lfoBuses.erase(server);
}

void scPolyComb::sendLFOBusIndices(ofxSCServer* server, ofxSCSynth* s) {
    if (!server || !s) return;
    for (int li = 0; li < NUM_LFOS; li++) {
        int idx = (lfoBuses.count(server) && lfoBuses[server][li])
                  ? lfoBuses[server][li]->index : 0;
        s->set("lfobus" + ofToString(li), idx);
    }
}

// ─── Preset Serialization ─────────────────────────────────────────────────────

void scPolyComb::presetSave(ofJson& j) {
    ofJson hidden;
    jsonSaveValue(hidden, "oscAmp", oscAmp.get());
    jsonSaveValue(hidden, "sawLevel", sawLevel.get());
    jsonSaveValue(hidden, "triLevel", triLevel.get());
    jsonSaveValue(hidden, "sineLevel", sineLevel.get());
    jsonSaveValue(hidden, "pulseLevel", pulseLevel.get());
    jsonSaveValue(hidden, "pulseWidth", pulseWidth.get());
    jsonSaveValue(hidden, "superSawLevel", superSawLevel.get());
    jsonSaveValue(hidden, "superSawDetune", superSawDetune.get());
    jsonSaveValue(hidden, "whiteNoiseLevel", whiteNoiseLevel.get());
    jsonSaveValue(hidden, "pinkNoiseLevel", pinkNoiseLevel.get());
    jsonSaveValue(hidden, "dustLevel", dustLevel.get());
    jsonSaveValue(hidden, "impulseLevel", impulseLevel.get());
    jsonSaveValue(hidden, "pitchMod", pitchMod.get());
    jsonSaveValue(hidden, "pitchModRange", pitchModRange.get());
    jsonSaveValue(hidden, "pitchDetune", pitchDetune.get());
    jsonSaveValue(hidden, "pitchTranspose", pitchTranspose.get());
    jsonSaveValue(hidden, "pitchScramble", pitchScramble.get());
    jsonSaveValue(hidden, "vibFreq", vibFreq.get());
    jsonSaveValue(hidden, "vibAmp", vibAmp.get());
    jsonSaveValue(hidden, "fmRatio", fmRatio.get());
    jsonSaveValue(hidden, "fmAmp", fmAmp.get());
    jsonSaveValue(hidden, "fmMod", fmMod.get());
    jsonSaveValue(hidden, "filterType", filterType.get());
    jsonSaveValue(hidden, "cutoffMin", cutoffMin.get());
    jsonSaveValue(hidden, "cutoffMax", cutoffMax.get());
    jsonSaveValue(hidden, "cutoffMod", cutoffMod.get());
    jsonSaveValue(hidden, "resonance", resonance.get());
    jsonSaveValue(hidden, "combPitch", combPitch.get());
    jsonSaveValue(hidden, "combPitchMod", combPitchMod.get());
    jsonSaveValue(hidden, "combPMRange", combPMRange.get());
    jsonSaveValue(hidden, "combDetune", combDetune.get());
    jsonSaveValue(hidden, "combTranspose", combTranspose.get());
    jsonSaveValue(hidden, "combFeed", combFeed.get());
    jsonSaveValue(hidden, "combMix", combMix.get());
    jsonSaveValue(hidden, "ringModPitch", ringModPitch.get());
    jsonSaveValue(hidden, "ringModDetune", ringModDetune.get());
    jsonSaveValue(hidden, "ringModMix", ringModMix.get());
    jsonSaveValue(hidden, "postFType", postFType.get());
    jsonSaveValue(hidden, "postCutMin", postCutMin.get());
    jsonSaveValue(hidden, "postCutMax", postCutMax.get());
    jsonSaveValue(hidden, "postCutMod", postCutMod.get());
    jsonSaveValue(hidden, "postRes", postRes.get());
    jsonSaveValue(hidden, "minTremHz", minTremHz.get());
    jsonSaveValue(hidden, "maxTremHz", maxTremHz.get());
    jsonSaveValue(hidden, "tremHzMod", tremHzMod.get());
    jsonSaveValue(hidden, "tremAmp", tremAmp.get());
    jsonSaveValue(hidden, "ssLevel", ssLevel.get());
    jsonSaveValue(hidden, "ssMix", ssMix.get());
    jsonSaveValue(hidden, "modMatrix", modMatrix.get());

    for (int li = 0; li < NUM_LFOS; li++) {
        string idx = ofToString(li);
        jsonSaveValue(hidden, "lfoFreq" + idx, lfoFreq[li].get());
        jsonSaveValue(hidden, "lfoFreqDetune" + idx, lfoFreqDetune[li].get());
        jsonSaveValue(hidden, "lfoInitPhase" + idx, lfoInitPhase[li].get());
        jsonSaveValue(hidden, "lfoRound" + idx, lfoRound[li].get());
        jsonSaveValue(hidden, "lfoSkew" + idx, lfoSkew[li].get());
        jsonSaveValue(hidden, "lfoPulseWidth" + idx, lfoPulseWidth[li].get());
        jsonSaveValue(hidden, "lfoPulseCentered" + idx, lfoPulseCentered[li].get());
        jsonSaveValue(hidden, "lfoPow" + idx, lfoPow[li].get());
        jsonSaveValue(hidden, "lfoBiPow" + idx, lfoBiPow[li].get());
        jsonSaveValue(hidden, "lfoSampleHold" + idx, lfoSampleHold[li].get());
        jsonSaveValue(hidden, "extLFO" + idx, extLFO[li].get());
        jsonSaveValue(hidden, "lfoNWaves" + idx, lfoNWaves[li].get());
        jsonSaveValue(hidden, "lfoNorm" + idx, lfoNorm[li].get());
        jsonSaveValue(hidden, "lfoInvertP" + idx, lfoInvertP[li].get());
        jsonSaveValue(hidden, "lfoSymP" + idx, lfoSymP[li].get());
        jsonSaveValue(hidden, "lfoShuffleP" + idx, lfoShuffleP[li].get());
        jsonSaveValue(hidden, "lfoRandP" + idx, lfoRandP[li].get());
        jsonSaveValue(hidden, "lfoOffsetP" + idx, lfoOffsetP[li].get());
        jsonSaveValue(hidden, "lfoQuantP" + idx, lfoQuantP[li].get());
        jsonSaveValue(hidden, "lfoCombP" + idx, lfoCombP[li].get());
        jsonSaveValue(hidden, "lfoModuloP" + idx, lfoModuloP[li].get());
    }

    j["hiddenParams"] = hidden;
}

void scPolyComb::presetRecallAfterSettingParameters(ofJson& j) {
    if (j.count("hiddenParams")) {
        const ofJson& hidden = j["hiddenParams"];
        jsonLoadValue(hidden, "oscAmp", oscAmp);
        jsonLoadValue(hidden, "sawLevel", sawLevel);
        jsonLoadValue(hidden, "triLevel", triLevel);
        jsonLoadValue(hidden, "sineLevel", sineLevel);
        jsonLoadValue(hidden, "pulseLevel", pulseLevel);
        jsonLoadValue(hidden, "pulseWidth", pulseWidth);
        jsonLoadValue(hidden, "superSawLevel", superSawLevel);
        jsonLoadValue(hidden, "superSawDetune", superSawDetune);
        jsonLoadValue(hidden, "whiteNoiseLevel", whiteNoiseLevel);
        jsonLoadValue(hidden, "pinkNoiseLevel", pinkNoiseLevel);
        jsonLoadValue(hidden, "dustLevel", dustLevel);
        jsonLoadValue(hidden, "impulseLevel", impulseLevel);
        jsonLoadValue(hidden, "pitchMod", pitchMod);
        jsonLoadValue(hidden, "pitchModRange", pitchModRange);
        jsonLoadValue(hidden, "pitchDetune", pitchDetune);
        jsonLoadValue(hidden, "pitchTranspose", pitchTranspose);
        jsonLoadValue(hidden, "pitchScramble", pitchScramble);
        jsonLoadValue(hidden, "vibFreq", vibFreq);
        jsonLoadValue(hidden, "vibAmp", vibAmp);
        jsonLoadValue(hidden, "fmRatio", fmRatio);
        jsonLoadValue(hidden, "fmAmp", fmAmp);
        jsonLoadValue(hidden, "fmMod", fmMod);
        jsonLoadValue(hidden, "filterType", filterType);
        jsonLoadValue(hidden, "cutoffMin", cutoffMin);
        jsonLoadValue(hidden, "cutoffMax", cutoffMax);
        jsonLoadValue(hidden, "cutoffMod", cutoffMod);
        jsonLoadValue(hidden, "resonance", resonance);
        jsonLoadValue(hidden, "combPitch", combPitch);
        jsonLoadValue(hidden, "combPitchMod", combPitchMod);
        jsonLoadValue(hidden, "combPMRange", combPMRange);
        jsonLoadValue(hidden, "combDetune", combDetune);
        jsonLoadValue(hidden, "combTranspose", combTranspose);
        jsonLoadValue(hidden, "combFeed", combFeed);
        jsonLoadValue(hidden, "combMix", combMix);
        jsonLoadValue(hidden, "ringModPitch", ringModPitch);
        jsonLoadValue(hidden, "ringModDetune", ringModDetune);
        jsonLoadValue(hidden, "ringModMix", ringModMix);
        jsonLoadValue(hidden, "postFType", postFType);
        jsonLoadValue(hidden, "postCutMin", postCutMin);
        jsonLoadValue(hidden, "postCutMax", postCutMax);
        jsonLoadValue(hidden, "postCutMod", postCutMod);
        jsonLoadValue(hidden, "postRes", postRes);
        jsonLoadValue(hidden, "minTremHz", minTremHz);
        jsonLoadValue(hidden, "maxTremHz", maxTremHz);
        jsonLoadValue(hidden, "tremHzMod", tremHzMod);
        jsonLoadValue(hidden, "tremAmp", tremAmp);
        jsonLoadValue(hidden, "ssLevel", ssLevel);
        jsonLoadValue(hidden, "ssMix", ssMix);
        jsonLoadValue(hidden, "modMatrix", modMatrix);
        if ((int)modMatrix.get().size() != NUM_MOD_SOURCES * NUM_MOD_TARGETS) {
            auto resized = modMatrix.get();
            resized.resize(NUM_MOD_SOURCES * NUM_MOD_TARGETS, 0.f);
            modMatrix.set(resized);
        }

        for (int li = 0; li < NUM_LFOS; li++) {
            string idx = ofToString(li);
            jsonLoadValue(hidden, "lfoFreq" + idx, lfoFreq[li]);
            jsonLoadValue(hidden, "lfoFreqDetune" + idx, lfoFreqDetune[li]);
            jsonLoadValue(hidden, "lfoInitPhase" + idx, lfoInitPhase[li]);
            jsonLoadValue(hidden, "lfoRound" + idx, lfoRound[li]);
            jsonLoadValue(hidden, "lfoSkew" + idx, lfoSkew[li]);
            jsonLoadValue(hidden, "lfoPulseWidth" + idx, lfoPulseWidth[li]);
            jsonLoadValue(hidden, "lfoPulseCentered" + idx, lfoPulseCentered[li]);
            jsonLoadValue(hidden, "lfoPow" + idx, lfoPow[li]);
            jsonLoadValue(hidden, "lfoBiPow" + idx, lfoBiPow[li]);
            jsonLoadValue(hidden, "lfoSampleHold" + idx, lfoSampleHold[li]);
            jsonLoadValue(hidden, "extLFO" + idx, extLFO[li]);
            jsonLoadValue(hidden, "lfoNWaves" + idx, lfoNWaves[li]);
            jsonLoadValue(hidden, "lfoNorm" + idx, lfoNorm[li]);
            jsonLoadValue(hidden, "lfoInvertP" + idx, lfoInvertP[li]);
            jsonLoadValue(hidden, "lfoSymP" + idx, lfoSymP[li]);
            jsonLoadValue(hidden, "lfoShuffleP" + idx, lfoShuffleP[li]);
            jsonLoadValue(hidden, "lfoRandP" + idx, lfoRandP[li]);
            jsonLoadValue(hidden, "lfoOffsetP" + idx, lfoOffsetP[li]);
            jsonLoadValue(hidden, "lfoQuantP" + idx, lfoQuantP[li]);
            jsonLoadValue(hidden, "lfoCombP" + idx, lfoCombP[li]);
            jsonLoadValue(hidden, "lfoModuloP" + idx, lfoModuloP[li]);
        }
    }

    for (auto& p : synthInstances)
        if (p.second) sendAllParams(p.second);
}

// ─── Custom GUI ───────────────────────────────────────────────────────────────

void scPolyComb::drawPolyCombWindow() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float w = windowWidth.get() * zoom;
    float h = windowHeight.get() * zoom;

    string title = "polyComb##PCW";
    bool open = showWindow.get();

    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &open,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        if (!open) showWindow = false;
        return;
    }
    if (!open) { ImGui::End(); showWindow = false; return; }

    float avail_w = ImGui::GetContentRegionAvail().x;
    float avail_h = ImGui::GetContentRegionAvail().y;

    // Left panel: mod matrix column + global sections column; right panel: LFO bank
    float leftW  = avail_w * 0.55f;
    float rightW = avail_w * 0.45f - 4.f;

    ImGui::BeginChild("##PCLeft", ImVec2(leftW, avail_h), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::Text("polyComb - %d voices", nVoices.get());
    ImGui::Separator();

    ImGui::Columns(2, "##leftColumns", false);
    float matrixColumnW = ImGui::GetContentRegionAvail().x;
    ImGui::Text("Mod Matrix (LFO / Ext → targets)");
    float matH = std::max(180.0f * zoom, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - 4.0f);
    drawModMatrixGrid(matrixColumnW - 4.f, matH);

    ImGui::NextColumn();
    ImGui::Text("Global");
    float masterValue = masterLevel.get();
    if (ImGui::SliderFloat("Master##global", &masterValue, 0.f, 1.f)) masterLevel.set(masterValue);
    ImGui::Separator();

    float globalW = ImGui::GetContentRegionAvail().x;
    float columnW = globalW * 0.5f;
    ImGui::Columns(2, "##globalSections", false);
    if (ImGui::CollapsingHeader("Pitch"))       drawPitchSection(columnW);
    if (ImGui::CollapsingHeader("Oscillators")) drawOscSection(columnW);
    if (ImGui::CollapsingHeader("Filter"))      drawFilterSection(columnW, false);
    if (ImGui::CollapsingHeader("RingMod"))     drawRingModSection(columnW);

    ImGui::NextColumn();
    if (ImGui::CollapsingHeader("FM"))          drawFMSection(columnW);
    if (ImGui::CollapsingHeader("Comb"))        drawCombSection(columnW);
    if (ImGui::CollapsingHeader("PostFilter"))  drawFilterSection(columnW, true);
    if (ImGui::CollapsingHeader("Tremolo"))     drawTremoloSection(columnW);
    if (ImGui::CollapsingHeader("SineShaper"))  drawSineShaperSection(columnW);
    ImGui::Columns(1);
    ImGui::Columns(1);

    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##PCRight", ImVec2(rightW, avail_h), false);

    drawLFOBank(rightW - 4.f, avail_h);

    ImGui::EndChild();
    ImGui::End();
}

// ─── Section draw helpers ─────────────────────────────────────────────────────

// Helper: scalar slider that broadcasts to all voices in a per-voice vector param
static bool scalarSlider(const char* label, ofParameter<vector<float>>& p, float min, float max) {
    auto v = p.get();
    float val = v.empty() ? min : v[0];
    if (ImGui::SliderFloat(label, &val, min, max)) {
        broadcastVectorParam(p, val);
        return true;
    }
    return false;
}

static bool scalarDiscreteSlider(const char* label, ofParameter<vector<float>>& p, int min, int max) {
    auto v = p.get();
    int val = v.empty() ? min : ofClamp((int)std::round(v[0]), min, max);
    if (ImGui::SliderInt(label, &val, min, max)) {
        broadcastVectorParam(p, (float)val);
        return true;
    }
    return false;
}

static bool scalarIntSlider(const char* label, ofParameter<int>& p, int min, int max) {
    int value = p.get();
    if (ImGui::SliderInt(label, &value, min, max)) {
        p.set(value);
        return true;
    }
    return false;
}

static bool scalarEnumCombo(const char* label, ofParameter<vector<float>>& p, const char* const items[], int itemCount) {
    auto v = p.get();
    int value = v.empty() ? 0 : ofClamp((int)std::round(v[0]), 0, std::max(itemCount - 1, 0));
    if (ImGui::BeginCombo(label, items[value])) {
        for (int i = 0; i < itemCount; i++) {
            bool selected = (value == i);
            if (ImGui::Selectable(items[i], selected)) {
                broadcastVectorParam(p, (float)i);
                value = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
        return true;
    }
    return false;
}

static ImU32 desaturatedPadColor(ImU32 baseColor, float amount, float brightness) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(baseColor);
    float gray = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
    c.x = gray + (c.x - gray) * (1.0f - amount);
    c.y = gray + (c.y - gray) * (1.0f - amount);
    c.z = gray + (c.z - gray) * (1.0f - amount);
    float maxComp = std::max(c.x, std::max(c.y, c.z));
    float scale = (maxComp > 1.0e-5f) ? (brightness / maxComp) : 1.0f;
    c.x *= scale;
    c.y *= scale;
    c.z *= scale;
    c.w = 0.95f;
    return ImGui::ColorConvertFloat4ToU32(c);
}

static void drawLfoWavePreview(const char* id, float width, float height, ImU32 color, int li,
                               float beatDuration, float roundness, float skew,
                               float pulseWidth, bool pulseCentered, float pow, float biPow, float sampleHoldDivisions) {
    (void)li;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size(width, height);
    ImGui::InvisibleButton(id, size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = origin;
    ImVec2 max(origin.x + width, origin.y + height);
    dl->AddRectFilled(min, max, IM_COL32(18, 18, 18, 220), 4.0f);
    dl->AddRect(min, max, IM_COL32(60, 60, 60, 180), 4.0f);

    const float midY = origin.y + height * 0.5f;
    dl->AddLine(ImVec2(origin.x, midY), ImVec2(origin.x + width, midY), IM_COL32(70, 70, 70, 120));

    std::array<ImVec2, 64> pts;
    for (int i = 0; i < (int)pts.size(); i++) {
        float t = (float)i / (float)(pts.size() - 1);
        float beatTime = t * std::max(beatDuration, 0.001f);
        float sampledBeatTime = applySampleHoldBeatPhaseCPP(beatTime, beatDuration, sampleHoldDivisions);
        float phase = sampledBeatTime / std::max(beatDuration, 0.001f);
        float cyclePhase = std::fmod(phase, 1.0f);
        if (cyclePhase < 0.f) cyclePhase += 1.f;
        float yValue = computeMorphOscillatorCPP(cyclePhase, roundness, skew, pulseWidth, pulseCentered);
        if (std::abs(pow) > 1.0e-5f) yValue = reflectUnitCPP(customPowCPP(yValue, pow));
        if (std::abs(biPow) > 1.0e-5f) yValue = biPowCPP(yValue, biPow);
        float x = origin.x + t * width;
        float y = origin.y + (1.0f - yValue) * height;
        pts[i] = ImVec2(x, y);
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), color, false, 2.0f);
}

float reflectUnitCPP(float value) {
    return value + ((value < 0.f) * (0.f - value)) + ((value > 1.f) * (1.f - value));
}

float customPowCPP(float value, float pow) {
    float k1 = 2.0f * pow * 0.99999f;
    float k2 = k1 / ((-pow * 0.999999f) + 1.0f);
    float k3 = k2 * std::abs(value) + 1.0f;
    return value * (k2 + 1.0f) / std::max(k3, 1.0e-6f);
}

float biPowCPP(float value, float biPow) {
    float bipolar = (value * 2.0f) - 1.0f;
    bipolar = customPowCPP(bipolar, biPow);
    return reflectUnitCPP((bipolar + 1.0f) * 0.5f);
}

float computeMorphOscillatorCPP(float ph, float roundness, float skew, float pulseWidth, bool pulseCentered) {
    float linPhase = std::fmod(ph, 1.0f);
    if (linPhase < 0.f) linPhase += 1.f;

    pulseWidth = ofClamp(pulseWidth, 0.f, 1.f);
    if (pulseWidth < 0.5f) {
        if (pulseCentered) {
            linPhase = ofMap(linPhase, 0.5f - pulseWidth, 0.5f + pulseWidth, 0.f, 1.f, true);
        } else {
            linPhase = ofMap(linPhase, 0.f, std::max(pulseWidth * 2.0f, 1.0e-7f), 0.f, 1.f, true);
        }
        if (skew < 0.f && linPhase >= 1.f - 1.0e-6f) linPhase = 0.f;
    } else if (pulseWidth >= 1.f - 1.0e-6f) {
        linPhase = ofMap(skew, -1.f, 1.f, 1.f, 0.f, true);
    } else {
        float midpoint = ofMap(skew, -1.f, 1.f, 1.f, 0.f, true);
        if (linPhase < midpoint) {
            float srcMax = ofMap(1.f - pulseWidth, 0.f, 0.5f, 0.f, midpoint, true);
            linPhase = ofMap(linPhase, 0.f, std::max(srcMax, 1.0e-7f), 0.f, midpoint, true);
        } else {
            float srcMin = ofMap(pulseWidth, 0.5f, 1.f, midpoint, 1.f, true);
            linPhase = ofMap(linPhase, std::min(srcMin, 1.f - 1.0e-7f), 1.f, midpoint, 1.f, true);
        }
    }

    float skewedLinPhase = linPhase;
    if (skew < 0.f) {
        float split = 0.5f + (std::abs(skew) * 0.5f);
        if (linPhase < split)
            skewedLinPhase = ofMap(linPhase, 0.f, split, 0.f, 0.5f, true);
        else
            skewedLinPhase = ofMap(linPhase, split, 1.f, 0.5f, 1.f, true);
    } else if (skew > 0.f) {
        float split = (1.f - std::abs(skew)) * 0.5f;
        if (linPhase > split)
            skewedLinPhase = ofMap(linPhase, split, 1.f, 0.5f, 1.f, true);
        else
            skewedLinPhase = ofMap(linPhase, 0.f, split, 0.f, 0.5f, true);
    }

    linPhase = skewedLinPhase;

    float w = linPhase * TWO_PI;
    float val = 0.f;
    if (roundness <= 0.f) {
        val = 1.f - std::abs((linPhase * -2.f) + 1.f);
    } else if (roundness >= 1.f) {
        val = (linPhase < 0.25f || linPhase >= 0.75f) ? 0.f : 1.f;
    } else if (std::abs(roundness - 0.5f) <= 1.0e-6f) {
        val = ofMap(std::cos(w + PI), -1.f, 1.f, 0.f, 1.f, true);
    } else if (roundness < 0.5f) {
        float triVal = 1.f - std::abs((linPhase * -2.f) + 1.f);
        float cosVal = ofMap(std::cos(w + PI), -1.f, 1.f, 0.f, 1.f, true);
        val = ofLerp(triVal, cosVal, roundness * 2.f);
    } else {
        float cosVal = std::cos(w + PI);
        cosVal = customPowCPP(cosVal, (roundness - 0.5f) * 2.f);
        val = ofMap(cosVal, -1.f, 1.f, 0.f, 1.f, true);
    }

    return ofClamp(val, 0.f, 1.f);
}

float applySampleHoldBeatPhaseCPP(float beatTime, float beatDuration, float sampleHoldDivisions) {
    (void)beatDuration;
    if (sampleHoldDivisions <= 0.f) return beatTime;
    float samplesPerBeat = std::max(sampleHoldDivisions, 1.0e-6f);
    float sampleIndex = std::floor(std::max(beatTime, 0.0f) * samplesPerBeat);
    return sampleIndex / samplesPerBeat;
}

float hashUnitCPP(float x) {
    float v = std::sin(x * 12.9898f + 78.233f) * 43758.5453f;
    return v - std::floor(v);
}

void scPolyComb::drawOscSection(float w) {
    scalarSlider("Saw##osc",  sawLevel,   0.f, 1.f);
    scalarSlider("Tri##osc",  triLevel,   0.f, 1.f);
    scalarSlider("Sine##osc", sineLevel,  0.f, 1.f);
    scalarSlider("Pulse##osc",pulseLevel, 0.f, 1.f);
    scalarSlider("PW##osc",   pulseWidth, 0.f, 1.f);
    scalarSlider("SuperSaw##osc",  superSawLevel,  0.f, 1.f);
    scalarSlider("SS Detune##osc", superSawDetune, 0.f, 1.f);
    scalarSlider("White##osc", whiteNoiseLevel, 0.f, 1.f);
    scalarSlider("Pink##osc",  pinkNoiseLevel,  0.f, 1.f);
    scalarSlider("Dust##osc",  dustLevel,       0.f, 1.f);
    scalarSlider("Impulse##osc",impulseLevel,   0.f, 1.f);
    scalarSlider("Amp##osc",   oscAmp,          0.f, 1.f);
}

void scPolyComb::drawPitchSection(float w) {
    (void)w;
    scalarSlider("PitchMod##pitch", pitchMod, 0.f, 1.f);
    scalarSlider("PMRange##pitch", pitchModRange, -24.f, 24.f);
    scalarSlider("Transpose##pitch", pitchTranspose, -24.f, 24.f);
    scalarSlider("Detune##pitch", pitchDetune, 0.f, 24.f);
    bool scramble = pitchScramble.get();
    if (ImGui::Checkbox("Scramble##pitch", &scramble)) pitchScramble.set(scramble);
    scalarSlider("VibFreq##pitch", vibFreq, 0.f, 20.f);
    scalarSlider("VibAmp##pitch", vibAmp, 0.f, 2.f);
}

void scPolyComb::drawFMSection(float w) {
    scalarSlider("Ratio##fm", fmRatio, 0.5f, 8.f);
    scalarSlider("Amp##fm",   fmAmp,   0.f,  1.f);
    scalarSlider("Mod##fm",   fmMod,   0.f,  1.f);
}

void scPolyComb::drawFilterSection(float w, bool isPost) {
    (void)w;
    static const char* filterItems[] = {"LPF", "HPF", "BPF", "BRF"};
    const char* sfx = isPost ? "##pf" : "##mf";
    if (isPost) {
        scalarEnumCombo(("Type" + string(sfx)).c_str(), postFType, filterItems, 4);
        scalarSlider(("CutMin" + string(sfx)).c_str(), postCutMin, 0.f, 135.f);
        scalarSlider(("CutMax" + string(sfx)).c_str(), postCutMax, 0.f, 135.f);
        scalarSlider(("CutMod" + string(sfx)).c_str(), postCutMod, 0.f, 1.f);
        scalarSlider(("Res"    + string(sfx)).c_str(), postRes,    0.f, 1.f);
    } else {
        scalarEnumCombo(("Type" + string(sfx)).c_str(), filterType, filterItems, 4);
        scalarSlider(("CutMin" + string(sfx)).c_str(), cutoffMin,  0.f, 135.f);
        scalarSlider(("CutMax" + string(sfx)).c_str(), cutoffMax,  0.f, 135.f);
        scalarSlider(("CutMod" + string(sfx)).c_str(), cutoffMod,  0.f, 1.f);
        scalarSlider(("Res"    + string(sfx)).c_str(), resonance,  0.f, 1.f);
    }
}

void scPolyComb::drawCombSection(float w) {
    scalarSlider("Pitch##comb",   combPitch,    0.f, 127.f);
    scalarSlider("PitchMod##comb",combPitchMod, 0.f, 1.f);
    scalarSlider("PMRange##comb", combPMRange, -24.f, 24.f);
    scalarSlider("Transpose##comb", combTranspose, -24.f, 24.f);
    scalarSlider("Detune##comb",  combDetune,   0.f, 24.f);
    scalarSlider("Feed##comb",    combFeed,    -0.999f, 0.999f);
    scalarSlider("Mix##comb",     combMix,      0.f, 1.f);
}

void scPolyComb::drawRingModSection(float w) {
    scalarSlider("Pitch##rm", ringModPitch, 12.f, 120.f);
    scalarSlider("Detune##rm", ringModDetune, 0.f, 24.f);
    scalarSlider("Mix##rm",   ringModMix,    0.f, 1.f);
}

void scPolyComb::drawTremoloSection(float w) {
    scalarSlider("MinHz##tr", minTremHz, 0.01f, 20.f);
    scalarSlider("MaxHz##tr", maxTremHz, 0.01f, 20.f);
    scalarSlider("Mod##tr",   tremHzMod, 0.f,   1.f);
    scalarSlider("Amp##tr",   tremAmp,   0.f,   1.f);
}

void scPolyComb::drawSineShaperSection(float w) {
    scalarSlider("Level##ss", ssLevel, 0.f, 20.f);
    scalarSlider("Mix##ss",   ssMix,   0.f, 1.f);
}

// ─── LFO Bank (live per-voice multislider) ────────────────────────────────────

void scPolyComb::drawLFOBank(float w, float h) {
    (void)h;
    float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int n = nVoices.get();

    static const ImU32 lfoColors[] = {
        IM_COL32(100, 200, 255, 230),
        IM_COL32(255, 160,  80, 230),
        IM_COL32(120, 255, 120, 230),
        IM_COL32(255, 100, 180, 230),
    };

    for (int li = 0; li < NUM_LFOS; li++) {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float rowH = 66.0f * zoom;
        ImVec2 rowMax(origin.x + w, origin.y + rowH);
        const float labelW = 44.0f * zoom;
        const float valueW = 80.0f * zoom;
        const float waveW = std::min(120.0f * zoom, w * 0.28f);

        ImU32 padColor = desaturatedPadColor(lfoColors[li]);
        ImU32 headerColor = desaturatedPadColor(lfoColors[li], 0.82f, 0.28f);
        dl->AddRectFilled(origin, rowMax, padColor, 6.0f);
        dl->AddRect(origin, rowMax, IM_COL32(70, 70, 70, 180));

        char buf[16];
        snprintf(buf, sizeof(buf), "LFO%d", li + 1);
        dl->AddText(ImVec2(origin.x + 6.0f, origin.y + rowH * 0.5f - 6.0f),
                    IM_COL32(180, 180, 180, 220), buf);

        float beats0 = getBroadcastValue(lfoFreq[li].get(), 0, 1.0f);
        char freqBuf[32];
        snprintf(freqBuf, sizeof(freqBuf), "%.2f beats", beats0);
        dl->AddText(ImVec2(rowMax.x - valueW, origin.y + 4.0f),
                    IM_COL32(140, 140, 140, 180), freqBuf);

        float sliderAreaX = origin.x + labelW;
        float sliderAreaW = (rowMax.x - valueW - waveW - 8.0f * zoom) - sliderAreaX;
        float slotW = (n > 0) ? sliderAreaW / (float)n : sliderAreaW;
        float slotTop = origin.y + 4.0f;
        float slotBottom = origin.y + rowH - 6.0f;
        float slotH = slotBottom - slotTop;

        for (int vi = 0; vi < n && vi < MAX_VOICES; vi++) {
            float val = (vi < (int)lfoDisplayData[li].size()) ? lfoDisplayData[li][vi] : 0.f;
            val = ofClamp(val, 0.f, 1.f);

            float sx = sliderAreaX + vi * slotW;
            float sw = slotW - 1.0f;
            float barH = val * slotH;

            dl->AddRectFilled(ImVec2(sx, slotTop), ImVec2(sx + sw, slotBottom),
                              IM_COL32(18, 18, 18, 200));
            if (barH > 0.f) {
                dl->AddRectFilled(ImVec2(sx, slotBottom - barH), ImVec2(sx + sw, slotBottom), lfoColors[li]);
            }
            dl->AddRect(ImVec2(sx, slotTop), ImVec2(sx + sw, slotBottom),
                        IM_COL32(45, 45, 45, 150));
        }

        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - valueW - waveW, origin.y + 6.0f));
        float roundness = getBroadcastValue(lfoRound[li].get(), 0, 0.5f);
        float skew = getBroadcastValue(lfoSkew[li].get(), 0, 0.0f);
        float pulseWidth = getBroadcastValue(lfoPulseWidth[li].get(), 0, 0.5f);
        bool pulseCentered = lfoPulseCentered[li].get();
        float pow = getBroadcastValue(lfoPow[li].get(), 0, 0.0f);
        float biPow = getBroadcastValue(lfoBiPow[li].get(), 0, 0.0f);
        float sampleHoldDivisions = getBroadcastValue(lfoSampleHold[li].get(), 0, 0.0f);
        drawLfoWavePreview(("##lfowave" + ofToString(li)).c_str(), waveW - 6.0f * zoom, rowH - 12.0f * zoom,
                           lfoColors[li], li, beats0, roundness, skew, pulseWidth, pulseCentered, pow, biPow, sampleHoldDivisions);

        ImGui::SetCursorScreenPos(ImVec2(origin.x, rowMax.y + 3.0f * zoom));
        ImGui::PushID(li);
        string paramsHeader = "LFO" + ofToString(li + 1) + " Params";
        ImGui::PushStyleColor(ImGuiCol_Header, headerColor);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, desaturatedPadColor(lfoColors[li], 0.72f, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, desaturatedPadColor(lfoColors[li], 0.68f, 0.36f));
        if (ImGui::CollapsingHeader(paramsHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Columns(2, "##lfoGrid", false);
            ImGui::TextUnformatted("Oscillator");
            scalarSlider("Freq (beats)##lf", lfoFreq[li], 0.125f, 64.f);
            scalarSlider("FreqDetune##lf", lfoFreqDetune[li], 0.f, 8.f);
            float initPhase = lfoInitPhase[li].get();
            if (ImGui::SliderFloat("Init Phase##lf", &initPhase, 0.f, 1.f)) lfoInitPhase[li].set(initPhase);
            scalarSlider("PulseWidth##lf", lfoPulseWidth[li], 0.f, 1.f);
            if (ImGui::Checkbox("Centered PW##lf", &pulseCentered)) lfoPulseCentered[li].set(pulseCentered);
            scalarSlider("Round##lf", lfoRound[li], 0.f, 1.f);
            scalarSlider("Skew##lf", lfoSkew[li], -1.f, 1.f);
            scalarSlider("Pow##lf", lfoPow[li], -1.f, 1.f);
            scalarSlider("BiPow##lf", lfoBiPow[li], -1.f, 1.f);
            scalarSlider("S&H / beat##lf", lfoSampleHold[li], 0.f, 32.f);

            ImGui::NextColumn();
            ImGui::TextUnformatted("Phase Offset");
            drawLFOIndexerControls(li);
            ImGui::Columns(1);
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, 6.0f * zoom));
    }
}

void scPolyComb::drawLFOIndexerControls(int li) {
    float nWaves = lfoNWaves[li].get();
    if (ImGui::SliderFloat("Num Waves", &nWaves, 0.f, lfoNWaves[li].getMax())) lfoNWaves[li].set(nWaves);

    bool norm = lfoNorm[li].get();
    if (ImGui::Checkbox("Normalize", &norm)) lfoNorm[li].set(norm);

    float invert = lfoInvertP[li].get();
    if (ImGui::SliderFloat("Invert", &invert, 0.f, 1.f)) lfoInvertP[li].set(invert);

    scalarIntSlider("Symmetry", lfoSymP[li], 0, lfoSymP[li].getMax());

    float shuffle = lfoShuffleP[li].get();
    if (ImGui::SliderFloat("Shuffle", &shuffle, 0.f, 1.f)) lfoShuffleP[li].set(shuffle);

    float random = lfoRandP[li].get();
    if (ImGui::SliderFloat("Random", &random, 0.f, 1.f)) lfoRandP[li].set(random);

    float offset = lfoOffsetP[li].get();
    if (ImGui::SliderFloat("Offset", &offset, lfoOffsetP[li].getMin(), lfoOffsetP[li].getMax())) lfoOffsetP[li].set(offset);

    scalarIntSlider("Quant", lfoQuantP[li], lfoQuantP[li].getMin(), lfoQuantP[li].getMax());

    float comb = lfoCombP[li].get();
    if (ImGui::SliderFloat("Combination", &comb, 0.f, 1.f)) lfoCombP[li].set(comb);

    scalarIntSlider("Modulo", lfoModuloP[li], lfoModuloP[li].getMin(), lfoModuloP[li].getMax());
}

// ─── ModMatrix (8×8 interactive grid) ────────────────────────────────────────

void scPolyComb::drawModMatrixGrid(float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    static const char* sourceNames[] = {
        "LFO1", "LFO2", "LFO3", "LFO4", "Ext1", "Ext2", "Ext3", "Ext4"
    };
    static const char* targetNames[] = {
        "Pitch", "FMAmp", "CutMod", "CombPM", "TremHz", "RingMix", "SSMix", "OscAmp"
    };

    float labelH   = 18.f * zoom;
    float labelW   = 40.f * zoom;
    float gridW    = w - labelW;
    float gridH    = h - labelH;
    float cellW    = gridW / (float)NUM_MOD_TARGETS;
    float cellH    = gridH / (float)NUM_MOD_SOURCES;

    // Column headers (target names)
    for (int ti = 0; ti < NUM_MOD_TARGETS; ti++) {
        ImVec2 p(origin.x + labelW + ti * cellW, origin.y);
        dl->AddText(p, IM_COL32(160,160,160,200), targetNames[ti]);
    }

    // Row headers (LFO names) + cells
    ImGui::InvisibleButton("##modmat", ImVec2(w, gridH + labelH));
    bool active     = ImGui::IsItemActive();
    ImVec2 mouse    = ImGui::GetIO().MousePos;
    bool  mouseDown = ImGui::IsMouseDown(0);
    bool  rightClick= ImGui::IsMouseClicked(1);
    if (!mouseDown) activeMatrixCell = -1;

    vector<float> mat = modMatrix.get();
    if ((int)mat.size() != NUM_MOD_SOURCES * NUM_MOD_TARGETS) {
        mat.resize(NUM_MOD_SOURCES * NUM_MOD_TARGETS, 0.f);
    }
    bool changed = false;

    for (int li = 0; li < NUM_MOD_SOURCES; li++) {
        // Row label
        ImVec2 rp(origin.x, origin.y + labelH + li * cellH + cellH * 0.3f);
        dl->AddText(rp, IM_COL32(180,180,180,200), sourceNames[li]);

        for (int ti = 0; ti < NUM_MOD_TARGETS; ti++) {
            int idx = li * NUM_MOD_TARGETS + ti;
            float val = mat[idx];

            ImVec2 cp(origin.x + labelW + ti * cellW,       origin.y + labelH + li * cellH);
            ImVec2 cm(cp.x + cellW - 2.f, cp.y + cellH - 2.f);
            bool hovered = (mouse.x >= cp.x && mouse.x < cm.x &&
                            mouse.y >= cp.y && mouse.y < cm.y);

            if (active && mouseDown) {
                if (activeMatrixCell < 0 && hovered) activeMatrixCell = idx;
                if (activeMatrixCell == idx) {
                    float delta = ImGui::GetIO().MouseDelta.y * -0.008f;
                    if (ImGui::GetIO().KeyShift) delta *= 0.1f;
                    float nv = ofClamp(val + delta, -1.f, 1.f);
                    if (nv != val) { mat[idx] = nv; changed = true; }
                }
            }
            if (hovered && rightClick) { mat[idx] = 0.f; changed = true; }

            // Background
            dl->AddRectFilled(cp, cm, IM_COL32(35, 35, 35, 240));

            // Fill bar from the center line so polarity is visually unambiguous.
            float absV  = fabsf(val);
            float halfH = (cellH - 2.f) * 0.5f;
            float fillH = halfH * absV;
            ImU32 col   = (val >= 0.f) ? IM_COL32(60,140,220,200) : IM_COL32(220,80,60,200);

            // Centre mark
            float midY = cp.y + cellH * 0.5f;
            if (val >= 0.f)
                dl->AddRectFilled(ImVec2(cp.x, midY - fillH), ImVec2(cm.x, midY), col);
            else
                dl->AddRectFilled(ImVec2(cp.x, midY), ImVec2(cm.x, midY + fillH), col);
            dl->AddLine(ImVec2(cp.x, midY), ImVec2(cm.x, midY), IM_COL32(80,80,80,120));
            dl->AddRect(cp, cm, IM_COL32(70,70,70,120));

            if (hovered) ImGui::SetTooltip("%.2f", val);
        }
    }

    if (changed) modMatrix.set(mat);
}
