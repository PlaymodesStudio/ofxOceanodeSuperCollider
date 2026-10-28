//
//  scDrumKitBoxData.cpp
//  ofxOceanodeSuperCollider
//
//  DrumKitBox's data: every control of a piece (Spec), the Models and their
//  Styles (templates: a label -> value per control they set; anything they
//  leave out goes back to its default), and the factory kits.
//

#include "scDrumKitBox.h"
#include "../shared/scSnapshots.h"
#include <set>
#include <filesystem>

using S = scDrumKitBox;

const std::vector<scDrumKitBox::Spec>& scDrumKitBox::specs() {
    static const std::vector<Spec> all = {
        // ── Played by ──
        {"play", "Channel", "channel", 'i', 0, 0, MaxChannels, DEST_NONE, "%d", false,
         "The channel of Gate / Gate Audio (and Pitch) this pad plays.\nAuto: its row, so the timeline's row n (or a sequencer's lane n) plays pad n.\nTwo pads on one channel play together (layers)."},
        {"play", "Choke", "choke", 'i', 0, 0, ChokeGroups, DEST_NONE, "%d", false,
         "Pads in the same group cut each other (a closed hat stops the open hat)."},
        {"play", "Model", "model", 'i', 0, 0, M_COUNT - 1, DEST_NONE, "%d", false,
         "What this pad is: picking one loads its sound (layers, envelopes, voice).\nEvery layer stays editable afterwards."},
        {"play", "Style", "style", 'i', 0, 0, 7, DEST_NONE, "%d", false, "A variation of the Model: picking one loads it."},
        // ── The piece ──
        {"piece", "Tune", "tune", 'f', 0, -24, 24, DEST_TRIG, "%+.2f st", false,
         "Every tonal layer, in semitones, on top of the hit's Pitch (60 = no change)."},
        {"piece", "Decay", "decayscale", 'f', 1, 0.25f, 4, DEST_SOUND, "x%.2f", true, "Every layer's times at once (envelopes, sweeps)."},
        {"piece", "Velocity", "velamp", 'f', 1, 0, 1, DEST_SOUND, "%.2f", false,
         "How much the hit's level (the Gate's value) sets its loudness.\n0: every hit at full level."},
        {"play", "Poly", "poly", 'b', 0, 0, 1, DEST_LAYER_ON, "", false,
         "Off (mono): a new hit takes over, the previous one fades in 3 ms, like a drum machine.\nOn (poly): every hit rings on (up to four at once), fast rolls overlap.\nEach layer then runs four voices: more CPU."},
        // ── Layers on / off ──
        {"body", "Body On", "lbody", 'b', 1, 0, 1, DEST_LAYER_ON, "", false, "The tonal body (off: no CPU)"},
        {"click", "Click On", "lclick", 'b', 0, 0, 1, DEST_LAYER_ON, "", false, "The transient (off: no CPU)"},
        {"noise", "Noise On", "lnoise", 'b', 0, 0, 1, DEST_LAYER_ON, "", false, "The noise layer (off: no CPU)"},
        {"metal", "Metal On", "lmetal", 'b', 0, 0, 1, DEST_LAYER_ON, "", false, "The metallic layer (off: no CPU)"},
        {"sample", "Sample On", "lsample", 'b', 0, 0, 1, DEST_LAYER_ON, "", false, "The sample layer (off: no CPU)"},
        // ── Body ──
        {"body", "Body Level", "bodylevel", 'f', 0, -60, 12, DEST_SOUND, "%+.1f dB", false, nullptr},
        {"body", "Body Freq", "bodyfreq", 'f', 52, 20, 2000, DEST_SOUND, "%.1f Hz", true, "The body's resting frequency (the end of the sweep)"},
        {"body", "Body Shape", "bodyshape", 'f', 0, 0, 2, DEST_SOUND, "%.2f", false, "0 sine, 1 triangle, 2 square (morphs between)"},
        {"body", "Body Phase", "bodyphase", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false,
         "Where the waveform starts at every hit (0.25: a cosine, a harder click)"},
        {"body", "Sweep", "sweep", 'f', 30, -48, 72, DEST_SOUND, "%+.1f st", false, "The pitch drop: the hit starts this many semitones above Freq"},
        {"body", "Sweep Time", "sweeptime", 'f', 0.045f, 0.0005f, 1, DEST_SOUND, "%.4f s", true, "How fast the sweep falls to Freq"},
        {"body", "Sweep Curve", "sweepcurve", 'f', -5, -12, 12, DEST_SOUND, "%.1f", false, "Negative: falls fast first (punchy); 0: linear"},
        {"body", "Bend", "bend", 'f', 0, -24, 24, DEST_SOUND, "%+.2f st", false, "A pitch glide over the whole hit (808 droop: -1..-3, zaps: up)"},
        {"body", "Bend Curve", "bendcurve", 'f', 0, -12, 12, DEST_SOUND, "%.1f", false, nullptr},
        {"body", "Body Attack", "bodyatk", 'f', 0.0005f, 0.00005f, 0.5f, DEST_SOUND, "%.4f s", true, nullptr},
        {"body", "Body Hold", "bodyhold", 'f', 0, 0, 2, DEST_SOUND, "%.3f s", false, "Full level before the decay starts"},
        {"body", "Body Decay", "bodydecay", 'f', 0.45f, 0.005f, 8, DEST_SOUND, "%.3f s", true, nullptr},
        {"body", "Body Curve", "bodycurve", 'f', -4, -12, 4, DEST_SOUND, "%.1f", false, "The decay's shape: negative falls fast first, 0 linear"},
        {"bodyx", "FM Amount", "fmamt", 'f', 0, 0, 8, DEST_SOUND, "%.2f", false, "A sine modulating the body's phase: metallic / woody attack"},
        {"bodyx", "FM Ratio", "fmratio", 'f', 1.5f, 0.25f, 8, DEST_SOUND, "%.3f", true, "The modulator's frequency over the body's"},
        {"bodyx", "FM Decay", "fmdecay", 'f', 0.03f, 0.001f, 2, DEST_SOUND, "%.3f s", true, "How fast the FM fades (short: only the attack)"},
        {"bodyx", "Mode 2 Level", "m2level", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "A second partial (a membrane's overtone: snares, toms)"},
        {"bodyx", "Mode 2 Ratio", "m2ratio", 'f', 1.59f, 1, 4, DEST_SOUND, "%.3f", false, "1.59: a drum membrane's first overtone"},
        {"bodyx", "Mode 2 Decay", "m2decay", 'f', 0.5f, 0.05f, 2, DEST_SOUND, "x%.2f", false, "Its decay against the body's"},
        {"bodyx", "Body Drive", "bodydrive", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "Saturates the body itself (before the voice's Character)"},
        // ── Click ──
        {"click", "Click Level", "clicklevel", 'f', -12, -60, 12, DEST_SOUND, "%+.1f dB", false, nullptr},
        {"click", "Click Type", "clicktype", 'i', 0, 0, 3, DEST_SOUND, "%d", false,
         "Pulse: a sharp tick. Noise: a short burst. Blip: a falling sine. Ping: a resonator."},
        {"click", "Click Tone", "clicktone", 'f', 2500, 100, 16000, DEST_SOUND, "%.0f Hz", true, nullptr},
        {"click", "Click Decay", "clickdecay", 'f', 0.004f, 0.0002f, 0.2f, DEST_SOUND, "%.4f s", true, nullptr},
        {"click", "Click Reso", "clickreso", 'f', 0.3f, 0, 1, DEST_SOUND, "%.2f", false, nullptr},
        {"click", "Click Key Track", "clicktrack", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "Tone follows the hit's pitch"},
        // ── Noise ──
        {"noise", "Noise Level", "noiselevel", 'f', 0, -60, 12, DEST_SOUND, "%+.1f dB", false, nullptr},
        {"noise", "Noise Color", "noisecolor", 'f', 0, 0, 2, DEST_SOUND, "%.2f", false, "0 white, 1 pink, 2 brown"},
        {"noise", "Noise Width", "noisewidth", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "Left and right different noise (stereo)"},
        {"noise", "Bursts", "bursts", 'i', 1, 1, 8, DEST_SOUND, "%d", false,
         "1: one noise hit. More: that many short bursts (a clap), the envelope after the last"},
        {"noise", "Burst Spacing", "burstspace", 'f', 0.011f, 0.002f, 0.08f, DEST_SOUND, "%.4f s", true, "Time between the bursts"},
        {"noise", "Noise Filter", "noisefilter", 'i', 1, 0, 3, DEST_SOUND, "%d", false, nullptr},
        {"noise", "Noise Cutoff", "noisecut", 'f', 6000, 20, 20000, DEST_SOUND, "%.0f Hz", true, nullptr},
        {"noise", "Noise Reso", "noisereso", 'f', 0.2f, 0, 1, DEST_SOUND, "%.2f", false, nullptr},
        {"noise", "Noise Env Amount", "noisefenv", 'f', 0, -8, 8, DEST_SOUND, "%+.2f oct", false, "The filter's sweep at every hit, in octaves"},
        {"noise", "Noise Env Decay", "noisefdecay", 'f', 0.08f, 0.001f, 2, DEST_SOUND, "%.3f s", true, nullptr},
        {"noise", "Noise Key Track", "noisetrack", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "The cutoff follows the hit's pitch"},
        {"noise", "Noise Low Cut", "noisehp", 'f', 20, 20, 12000, DEST_SOUND, "%.0f Hz", true, nullptr},
        {"noise", "Noise Attack", "noiseatk", 'f', 0.0005f, 0.00005f, 1, DEST_SOUND, "%.4f s", true, nullptr},
        {"noise", "Noise Hold", "noisehold", 'f', 0, 0, 2, DEST_SOUND, "%.3f s", false, nullptr},
        {"noise", "Noise Decay", "noisedecay", 'f', 0.2f, 0.005f, 8, DEST_SOUND, "%.3f s", true, nullptr},
        {"noise", "Noise Curve", "noisecurve", 'f', -5, -12, 4, DEST_SOUND, "%.1f", false, nullptr},
        // ── Metal ──
        {"metal", "Metal Level", "metallevel", 'f', 0, -60, 12, DEST_SOUND, "%+.1f dB", false, nullptr},
        {"metal", "Metal Mode", "metalmode", 'i', 0, 0, 2, DEST_SOUND, "%d", false,
         "Square: six square waves (808 hats, cymbals). Ring: pairs multiplied (bells).\nFM: three sine pairs (index Metal FM)."},
        {"metal", "Metal Freq", "metalfreq", 'f', 420, 40, 2000, DEST_SOUND, "%.1f Hz", true, "The lowest oscillator"},
        {"metal", "Inharmonic", "inharm", 'f', 1, 0, 3, DEST_SOUND, "%.2f", false, "1: the 808's ratios. 0: all at Freq. 2: twice as far apart"},
        {"metal", "Metal FM", "metalfm", 'f', 2, 0, 10, DEST_SOUND, "%.2f", false, "FM mode's index"},
        {"metal", "Metal Tone", "metaltone", 'f', 8000, 200, 20000, DEST_SOUND, "%.0f Hz", true, "The band-pass's centre"},
        {"metal", "Metal Reso", "metalreso", 'f', 0.3f, 0, 1, DEST_SOUND, "%.2f", false, nullptr},
        {"metal", "Metal Color", "metalcolor", 'f', 0.3f, 0, 1, DEST_SOUND, "%.2f", false, "Blends in the unfiltered oscillators"},
        {"metal", "Metal Low Cut", "metalhp", 'f', 3000, 20, 16000, DEST_SOUND, "%.0f Hz", true, nullptr},
        {"metal", "Metal Key Track", "metaltrack", 'f', 1, 0, 1, DEST_SOUND, "%.2f", false, "The oscillators follow the hit's pitch"},
        {"metal", "Metal Attack", "metalatk", 'f', 0.0005f, 0.00005f, 1, DEST_SOUND, "%.4f s", true, nullptr},
        {"metal", "Metal Hold", "metalhold", 'f', 0, 0, 2, DEST_SOUND, "%.3f s", false, nullptr},
        {"metal", "Metal Decay", "metaldecay", 'f', 0.08f, 0.005f, 8, DEST_SOUND, "%.3f s", true, nullptr},
        {"metal", "Metal Curve", "metalcurve", 'f', -6, -12, 4, DEST_SOUND, "%.1f", false, nullptr},
        // ── Sample ──
        {"sample", "Sample Level", "smplevel", 'f', 0, -60, 12, DEST_SOUND, "%+.1f dB", false, nullptr},
        {"sample", "Sample Start", "smpstart", 'f', 0, 0, 1, DEST_SOUND, "%.3f", false, "Where in the file the hit starts"},
        {"sample", "Sample Length", "smplen", 'f', 1, 0.001f, 1, DEST_SOUND, "%.3f", true, "How much of the file, from Start"},
        {"sample", "Sample Tune", "smptune", 'f', 0, -24, 24, DEST_SOUND, "%+.2f st", false, nullptr},
        {"sample", "Sample Key Track", "smptrack", 'f', 1, 0, 1, DEST_SOUND, "%.2f", false, "The sample follows the hit's pitch (and Tune)"},
        {"sample", "Sample Reverse", "smpreverse", 'b', 0, 0, 1, DEST_SOUND, "", false, nullptr},
        {"sample", "Sample Attack", "smpatk", 'f', 0.0002f, 0.00005f, 1, DEST_SOUND, "%.4f s", true, nullptr},
        {"sample", "Sample Hold", "smphold", 'f', 0, 0, 2, DEST_SOUND, "%.3f s", false, nullptr},
        {"sample", "Sample Decay", "smpdecay", 'f', 10, 0.005f, 10, DEST_SOUND, "%.3f s", true, "10 s: the whole region (no envelope)"},
        {"sample", "Sample Curve", "smpcurve", 'f', -4, -12, 4, DEST_SOUND, "%.1f", false, nullptr},
        // ── Voice: filter ──
        {"filter", "Filter On", "filteron", 'b', 0, 0, 1, DEST_SOUND, "", false, nullptr},
        {"filter", "Filter Type", "filtertype", 'i', 0, 0, 3, DEST_SOUND, "%d", false, nullptr},
        {"filter", "Cutoff", "cutoff", 'f', 18000, 20, 20000, DEST_SOUND, "%.0f Hz", true, nullptr},
        {"filter", "Resonance", "resonance", 'f', 0.1f, 0, 1, DEST_SOUND, "%.2f", false, nullptr},
        {"filter", "Env Amount", "fenvamt", 'f', 0, -8, 8, DEST_SOUND, "%+.2f oct", false, "The cutoff's sweep at every hit, in octaves"},
        {"filter", "Env Decay", "fenvdecay", 'f', 0.15f, 0.001f, 4, DEST_SOUND, "%.3f s", true, nullptr},
        {"filter", "Key Track", "keytrack", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "The cutoff follows the hit's pitch"},
        {"filter", "Step Filter Reso", "sfreso", 'f', 0.2f, 0, 1, DEST_SOUND, "%.2f", false,
         "Resonance of the per-hit LP / HP filter (the Filter input: the Drum Grid's Filter strip)"},
        {"filter", "Vel Tone", "veltone", 'f', 0, 0, 1, DEST_SOUND, "%.2f", false, "Soft hits darker (up to 3 octaves at the softest)"},
        // ── Voice: character ──
        {"char", "Character On", "charon", 'b', 0, 0, 1, DEST_SOUND, "", false, nullptr},
        {"char", "Drive Type", "drivetype", 'i', 0, 0, 3, DEST_SOUND, "%d", false, "Soft: tanh. Hard: clipping. Fold: wavefolding. Tube: asymmetric (even harmonics)"},
        {"char", "Drive", "drive", 'f', 6, 0, 48, DEST_SOUND, "%.1f dB", false, nullptr},
        {"char", "Drive Mix", "drivemix", 'f', 1, 0, 1, DEST_SOUND, "%.2f", false, nullptr},
        {"char", "Bits", "bits", 'f', 24, 1, 24, DEST_SOUND, "%.1f", false, "Bit depth (24: off)"},
        {"char", "Sample Rate", "srate", 'f', 48000, 100, 48000, DEST_SOUND, "%.0f Hz", true, "Downsampling (48000: off)"},
        // ── Voice: transient ──
        {"trans", "Transient On", "transon", 'b', 0, 0, 1, DEST_SOUND, "", false, nullptr},
        {"trans", "Punch", "punch", 'f', 0, -1, 1, DEST_SOUND, "%+.2f", false, "Boosts (or softens) the attack"},
        {"trans", "Sustain", "sustain", 'f', 0, -1, 1, DEST_SOUND, "%+.2f", false, "Boosts (or tightens) what follows the attack"},
        // ── Voice: output ──
        {"out", "Gated", "gated", 'b', 0, 0, 1, DEST_SOUND, "", false,
         "The hit lasts while its gate is open: closing it fades the pad in Release\n(open hats held by the step's length, gated basses)."},
        {"out", "Release", "release", 'f', 0.08f, 0.001f, 4, DEST_SOUND, "%.3f s", true, nullptr},
        {"out", "Pan", "pan", 'f', 0, -1, 1, DEST_SOUND, "%+.2f", false, nullptr},
        {"out", "Width", "width", 'f', 1, 0, 2, DEST_SOUND, "%.2f", false, "The layers' stereo (0: mono)"},
        // ── Mixer ──
        {"mix", "Level", "level", 'f', 1, 0, 2, DEST_STRIP, "%.2f", false, nullptr},
        {"mix", "Mute", "mute", 'b', 0, 0, 1, DEST_STRIP, "", false, nullptr},
        {"mix", "Solo", "solo", 'b', 0, 0, 1, DEST_STRIP, "", false, nullptr},
        {"duck", "Duck From", "duckfrom", 'i', 0, 0, MaxPads, DEST_NONE, "%d", false,
         "Side-chain: this pad ducks whenever that pad plays (pumping under the kick)"},
        {"duck", "Duck Amount", "duckamt", 'f', 0, 0, 1, DEST_STRIP, "%.2f", false, nullptr},
        {"duck", "Duck Attack", "duckatk", 'f', 0.003f, 0.0005f, 0.1f, DEST_STRIP, "%.4f s", true, nullptr},
        {"duck", "Duck Release", "duckrel", 'f', 0.18f, 0.01f, 2, DEST_STRIP, "%.3f s", true, nullptr},
        // ── Effects (the Box family's) ──
        {"eq", "EQ On", "on", 'b', 0, 0, 1, DEST_FX + FX_EQ, "", false, nullptr},
        {"eq", "EQ Low", "b1gain", 'f', 0, -24, 24, DEST_FX + FX_EQ, "%+.1f dB", false, nullptr},
        {"eq", "EQ Low Freq", "b1freq", 'f', 80, 20, 20000, DEST_FX + FX_EQ, "%.0f Hz", true, nullptr},
        {"eq", "EQ Low Slope", "b1shape", 'f', 1, 0.1f, 4, DEST_FX + FX_EQ, "%.2f", false, nullptr},
        {"eq", "EQ Lo Mid", "b2gain", 'f', 0, -24, 24, DEST_FX + FX_EQ, "%+.1f dB", false, nullptr},
        {"eq", "EQ Lo Mid Freq", "b2freq", 'f', 250, 20, 20000, DEST_FX + FX_EQ, "%.0f Hz", true, nullptr},
        {"eq", "EQ Lo Mid Q", "b2shape", 'f', 1, 0.1f, 20, DEST_FX + FX_EQ, "%.2f", false, nullptr},
        {"eq", "EQ Mid", "b3gain", 'f', 0, -24, 24, DEST_FX + FX_EQ, "%+.1f dB", false, nullptr},
        {"eq", "EQ Mid Freq", "b3freq", 'f', 1000, 20, 20000, DEST_FX + FX_EQ, "%.0f Hz", true, nullptr},
        {"eq", "EQ Mid Q", "b3shape", 'f', 1, 0.1f, 20, DEST_FX + FX_EQ, "%.2f", false, nullptr},
        {"eq", "EQ Hi Mid", "b4gain", 'f', 0, -24, 24, DEST_FX + FX_EQ, "%+.1f dB", false, nullptr},
        {"eq", "EQ Hi Mid Freq", "b4freq", 'f', 4000, 20, 20000, DEST_FX + FX_EQ, "%.0f Hz", true, nullptr},
        {"eq", "EQ Hi Mid Q", "b4shape", 'f', 1, 0.1f, 20, DEST_FX + FX_EQ, "%.2f", false, nullptr},
        {"eq", "EQ High", "b5gain", 'f', 0, -24, 24, DEST_FX + FX_EQ, "%+.1f dB", false, nullptr},
        {"eq", "EQ High Freq", "b5freq", 'f', 12000, 20, 20000, DEST_FX + FX_EQ, "%.0f Hz", true, nullptr},
        {"eq", "EQ High Slope", "b5shape", 'f', 1, 0.1f, 4, DEST_FX + FX_EQ, "%.2f", false, nullptr},
        {"comp", "Compressor On", "on", 'b', 0, 0, 1, DEST_FX + FX_COMP, "", false, nullptr},
        {"comp", "Comp Threshold", "threshold", 'f', -18, -60, 0, DEST_FX + FX_COMP, "%.1f dB", false, nullptr},
        {"comp", "Comp Ratio", "ratio", 'f', 4, 1, 20, DEST_FX + FX_COMP, "%.1f : 1", true, nullptr},
        {"comp", "Comp Attack", "attack", 'f', 10, 0.1f, 200, DEST_FX + FX_COMP, "%.1f ms", true,
         "Slower than the hit's attack lets the transient through (punch)"},
        {"comp", "Comp Release", "release", 'f', 120, 1, 2000, DEST_FX + FX_COMP, "%.0f ms", true, nullptr},
        {"comp", "Comp Knee", "knee", 'f', 6, 0, 24, DEST_FX + FX_COMP, "%.1f dB", false, nullptr},
        {"comp", "Comp Makeup", "makeup", 'f', 0, -12, 24, DEST_FX + FX_COMP, "%+.1f dB", false, nullptr},
        {"comp", "Comp Mix", "mix", 'f', 1, 0, 1, DEST_FX + FX_COMP, "%.2f", false, "Parallel (New York) compression below 1"},
        {"sat", "Saturator On", "on", 'b', 0, 0, 1, DEST_FX + FX_SAT, "", false, nullptr},
        {"sat", "Sat Drive", "drive", 'f', 6, 0, 36, DEST_FX + FX_SAT, "%.1f dB", false, nullptr},
        {"sat", "Sat Bias", "bias", 'f', 0, 0, 1, DEST_FX + FX_SAT, "%.2f", false, "Even harmonics (warmth)"},
        {"sat", "Sat Tone", "tone", 'f', 20000, 500, 20000, DEST_FX + FX_SAT, "%.0f Hz", true, nullptr},
        {"sat", "Sat Output", "output", 'f', 0, -24, 12, DEST_FX + FX_SAT, "%+.1f dB", false, nullptr},
        {"sat", "Sat Mix", "mix", 'f', 1, 0, 1, DEST_FX + FX_SAT, "%.2f", false, nullptr},
        {"echo", "Echo On", "on", 'b', 0, 0, 1, DEST_FX + FX_ECHO, "", false, nullptr},
        {"echo", "Echo Time", "", 'f', 0.75f, 0.0625f, 4, DEST_FX + FX_ECHO, "%.3f beats", true,
         "In beats: 0.75 = a dotted 8th, 0.5 = an 8th, 0.333 = an 8th triplet"},
        {"echo", "Echo Feedback", "feed", 'f', 0.4f, 0, 0.98f, DEST_FX + FX_ECHO, "%.2f", false, nullptr},
        {"echo", "Echo Filter", "filtertype", 'i', 0, 0, 3, DEST_FX + FX_ECHO, "%d", false, nullptr},
        {"echo", "Echo Cutoff", "cutoff", 'f', 96, 12, 130, DEST_FX + FX_ECHO, "%.1f", false, "MIDI note (96: 2 kHz)"},
        {"echo", "Echo Resonance", "resonance", 'f', 0.1f, 0, 0.9f, DEST_FX + FX_ECHO, "%.2f", false, nullptr},
        {"echo", "Echo Ping-Pong", "pingpong", 'f', 0.5f, 0, 1, DEST_FX + FX_ECHO, "%.2f", false, nullptr},
        {"echo", "Echo Pitch", "pitch", 'f', 0, -12, 12, DEST_FX + FX_ECHO, "%+.2f st", false,
         "Each repeat shifted by this many semitones: repeats spiral up or down (0: off)"},
        {"echo", "Echo Drift", "drift", 'f', 0, 0, 1, DEST_FX + FX_ECHO, "%.2f", false,
         "The delay time wanders like a worn tape: the repeats' pitch drifts (each channel its own way)"},
        {"echo", "Echo Drift Rate", "driftrate", 'f', 0.4f, 0.02f, 12, DEST_FX + FX_ECHO, "%.2f Hz", true, nullptr},
        {"echo", "Echo Mix", "mix", 'f', 0.3f, 0, 1, DEST_FX + FX_ECHO, "%.2f", false, nullptr},
        {"grain", "Grains On", "on", 'b', 0, 0, 1, DEST_FX + FX_GRAIN, "", false, nullptr},
        {"grain", "Grain Time", "", 'f', 0.5f, 0.0625f, 4, DEST_FX + FX_GRAIN, "%.3f beats", true,
         "How far back the grains read (in beats, up to 3.5 s)"},
        {"grain", "Grain Jitter", "jitter", 'f', 0.05f, 0, 0.4f, DEST_FX + FX_GRAIN, "%.3f s", false, "Each grain reads up to this much further back, at random"},
        {"grain", "Grain Density", "density", 'f', 12, 0.5f, 80, DEST_FX + FX_GRAIN, "%.1f /s", true, "Grains per second"},
        {"grain", "Grain Periodic", "periodic", 'f', 0, 0, 1, DEST_FX + FX_GRAIN, "%.2f", false, "0: grains at random times (a cloud) .. 1: regular (a stutter)"},
        {"grain", "Grain Size", "size", 'f', 0.08f, 0.005f, 0.5f, DEST_FX + FX_GRAIN, "%.3f s", true, nullptr},
        {"grain", "Grain Size Jitter", "sizejit", 'f', 0.3f, 0, 1, DEST_FX + FX_GRAIN, "%.2f", false, nullptr},
        {"grain", "Grain Pitch", "pitch", 'f', 0, -24, 24, DEST_FX + FX_GRAIN, "%+.2f st", false, nullptr},
        {"grain", "Grain Pitch Jitter", "pitchjit", 'f', 0.2f, 0, 12, DEST_FX + FX_GRAIN, "%.2f st", false, "Each grain detuned at random by up to this much"},
        {"grain", "Grain Reverse", "reverse", 'f', 0, 0, 1, DEST_FX + FX_GRAIN, "%.2f", false, "The chance a grain plays backwards"},
        {"grain", "Grain Feedback", "feed", 'f', 0.3f, 0, 0.95f, DEST_FX + FX_GRAIN, "%.2f", false, nullptr},
        {"grain", "Grain Tone", "tone", 'f', 110, 30, 130, DEST_FX + FX_GRAIN, "%.1f", false, "The feedback's low-pass (MIDI note: 110 = 4.7 kHz)"},
        {"grain", "Grain Spread", "spread", 'f', 0.5f, 0, 1, DEST_FX + FX_GRAIN, "%.2f", false, "Grains wander across the channels"},
        {"grain", "Grain Mix", "mix", 'f', 0.35f, 0, 1, DEST_FX + FX_GRAIN, "%.2f", false, nullptr},
        {"reverb", "Reverb On", "on", 'b', 0, 0, 1, DEST_FX + FX_REVERB, "", false, nullptr},
        {"reverb", "Rev Size", "size", 'f', 30, 0, 60, DEST_FX + FX_REVERB, "%.1f", false, nullptr},
        {"reverb", "Rev Decay", "decay", 'f', 1.6f, 0.05f, 30, DEST_FX + FX_REVERB, "%.2f s", true, nullptr},
        {"reverb", "Rev Predelay", "predelay", 'f', 15, 0, 500, DEST_FX + FX_REVERB, "%.0f ms", false, nullptr},
        {"reverb", "Rev Lowpass", "lowpass", 'f', 9000, 200, 20000, DEST_FX + FX_REVERB, "%.0f Hz", true, nullptr},
        {"reverb", "Rev Hi Damp", "highdamp", 'f', 6, 0, 60, DEST_FX + FX_REVERB, "%.1f dB", false, nullptr},
        {"reverb", "Rev Position", "position", 'f', 0.8f, 0, 1, DEST_FX + FX_REVERB, "%.2f", false, "0 = early reflections, 1 = the late tail"},
        {"reverb", "Rev Spread", "spread", 'f', 0.15f, -1, 1, DEST_FX + FX_REVERB, "%+.2f", false, nullptr},
        {"reverb", "Rev Mix", "mix", 'f', 0.25f, 0, 1, DEST_FX + FX_REVERB, "%.2f", false, nullptr},
        {"stereo", "Stereo On", "on", 'b', 0, 0, 1, DEST_FX + FX_WIDTH, "", false, nullptr},
        {"stereo", "Stereo Width", "width", 'f', 1, 0, 2, DEST_FX + FX_WIDTH, "%.2f", false, nullptr},
        {"stereo", "Diffuse", "diffuse", 'f', 0, 0, 1, DEST_FX + FX_WIDTH, "%.2f", false, nullptr},
        {"stereo", "Bass Mono", "bassMono", 'f', 0, 0, 1, DEST_FX + FX_WIDTH, "%.2f", false, nullptr},
        {"stereo", "Bass Freq", "bassFreq", 'f', 150, 20, 1000, DEST_FX + FX_WIDTH, "%.0f Hz", true, nullptr},
    };
    return all;
}

const std::vector<std::string>& scDrumKitBox::modelNames() {
    static const std::vector<std::string> names = {"Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Tom", "Rim",
                                                   "Cowbell", "Cymbal", "Perc", "808 Sub", "FX", "Sample"};
    return names;
}

namespace {
// Each model: its sound ("base", over every control's default) and its
// styles (over the base). Keys are the controls' labels.
const ofJson& templates() {
    static const ofJson t = ofJson::parse(R"JSON([
 {"base": {"Body On": 1, "Click On": 1, "Body Freq": 52, "Sweep": 30, "Sweep Time": 0.045, "Sweep Curve": -5,
           "Body Decay": 0.45, "Body Curve": -4, "Body Shape": 0.1, "Body Drive": 0.1,
           "Click Type": 0, "Click Tone": 2500, "Click Decay": 0.004, "Click Level": -12},
  "styles": [["909", {}],
   ["808 Boom", {"Body Freq": 45, "Sweep": 14, "Sweep Time": 0.03, "Body Decay": 1.4, "Body Curve": -2, "Bend": -1.5,
                 "Body Shape": 0, "Body Drive": 0, "Click Type": 3, "Click Tone": 1400, "Click Level": -24}],
   ["Techno Rumble", {"Body Freq": 47, "Sweep": 28, "Body Decay": 0.7, "Body Curve": -3, "Body Drive": 0.35,
                      "Noise On": 1, "Noise Color": 2, "Noise Filter": 0, "Noise Cutoff": 180, "Noise Reso": 0.4,
                      "Noise Decay": 0.6, "Noise Curve": -3, "Noise Level": -14,
                      "Character On": 1, "Drive Type": 0, "Drive": 8}],
   ["Hardcore", {"Body Freq": 55, "Sweep": 40, "Sweep Time": 0.06, "Body Decay": 0.5, "Body Drive": 0.9, "Body Shape": 1,
                 "Character On": 1, "Drive Type": 1, "Drive": 14, "Click Level": -6}],
   ["Punchy House", {"Body Freq": 56, "Sweep": 24, "Sweep Time": 0.035, "Body Decay": 0.35,
                     "Transient On": 1, "Punch": 0.5, "Body Level": -3, "Click Type": 1, "Click Tone": 4000, "Click Level": -16}],
   ["Minimal Tick", {"Body Freq": 62, "Sweep": 18, "Body Decay": 0.16, "Body Curve": -6,
                     "Click Type": 2, "Click Tone": 3500, "Click Level": -8}],
   ["Industrial", {"Body Freq": 50, "Sweep": 36, "Body Decay": 0.7, "FM Amount": 1.2, "FM Ratio": 1.41, "FM Decay": 0.06,
                   "Character On": 1, "Drive Type": 2, "Drive": 10, "Bits": 10}],
   ["Deep Sine", {"Body Freq": 43, "Sweep": 20, "Sweep Time": 0.05, "Body Decay": 0.9, "Body Curve": -2.5,
                  "Body Shape": 0, "Body Drive": 0, "Click Level": -30}]]},
 {"base": {"Body On": 1, "Noise On": 1, "Click On": 1, "Body Freq": 185, "Sweep": 10, "Sweep Time": 0.02, "Body Decay": 0.11,
           "Body Shape": 0.6, "Mode 2 Level": 0.5, "Mode 2 Ratio": 1.59, "Body Level": -5,
           "Noise Filter": 2, "Noise Cutoff": 1800, "Noise Reso": 0.1, "Noise Decay": 0.22, "Noise Curve": -5,
           "Noise Level": -7, "Noise Width": 0.4, "Click Type": 1, "Click Tone": 5000, "Click Level": -16},
  "styles": [["909", {}],
   ["808", {"Body Freq": 180, "Mode 2 Ratio": 1.85, "Mode 2 Level": 0.8, "Body Decay": 0.15, "Body Shape": 0, "Body Level": -5,
            "Noise Cutoff": 4000, "Noise Decay": 0.16, "Noise Level": -6, "Click On": 0}],
   ["Trap Snap", {"Body Freq": 220, "Body Decay": 0.07, "Noise Filter": 1, "Noise Cutoff": 3500, "Noise Reso": 0.3,
                  "Noise Decay": 0.12, "Noise Level": -12, "Body Level": -6, "Click Type": 0, "Click Tone": 6000, "Click Level": -16, "Transient On": 1, "Punch": 0.3}],
   ["Gated", {"Noise Filter": 0, "Noise Cutoff": 9000, "Noise Hold": 0.09, "Noise Decay": 0.04, "Noise Curve": -1,
              "Noise Level": 0, "Body Level": -9}],
   ["Breakbeat", {"Body Freq": 200, "Body Decay": 0.16, "Noise Filter": 1, "Noise Cutoff": 2500, "Noise Reso": 0.05,
                  "Noise Decay": 0.25, "Noise Color": 1, "Character On": 1, "Drive Type": 3, "Drive": 6,
                  "Bits": 12, "Sample Rate": 22050}],
   ["Electro", {"Body Freq": 240, "Sweep": 24, "Sweep Time": 0.03, "Body Shape": 1.4, "Body Decay": 0.09,
                "Noise Filter": 1, "Noise Cutoff": 5000, "Noise Env Amount": -2, "Noise Env Decay": 0.1}]]},
 {"base": {"Body On": 0, "Noise On": 1, "Noise Filter": 1, "Noise Cutoff": 1200, "Noise Reso": 0.35, "Bursts": 4,
           "Burst Spacing": 0.011, "Noise Decay": 0.22, "Noise Curve": -5, "Noise Width": 0.5, "Noise Level": 0,
           "Noise Env Amount": 0.5, "Noise Env Decay": 0.03},
  "styles": [["909", {}],
   ["808", {"Bursts": 3, "Burst Spacing": 0.009, "Noise Cutoff": 1000, "Noise Decay": 0.35}],
   ["Tight", {"Bursts": 2, "Burst Spacing": 0.007, "Noise Decay": 0.1, "Noise Filter": 2, "Noise Cutoff": 1400}],
   ["Big Room", {"Bursts": 5, "Burst Spacing": 0.014, "Noise Decay": 0.5, "Noise Width": 1, "Noise Cutoff": 1500}],
   ["Snap", {"Bursts": 2, "Burst Spacing": 0.004, "Noise Cutoff": 2800, "Noise Reso": 0.5, "Noise Decay": 0.06,
             "Click On": 1, "Click Type": 1, "Click Tone": 3000, "Click Level": -8}]]},
 {"base": {"Body On": 0, "Metal On": 1, "Noise On": 1, "Choke": 1, "Metal Mode": 0, "Metal Freq": 410, "Inharmonic": 1,
           "Metal Tone": 10000, "Metal Reso": 0.3, "Metal Low Cut": 7000, "Metal Decay": 0.05, "Metal Curve": -6,
           "Metal Color": 0.2, "Noise Filter": 2, "Noise Cutoff": 9000, "Noise Decay": 0.04, "Noise Level": -14,
           "Noise Width": 0.3},
  "styles": [["808", {}],
   ["909", {"Noise Cutoff": 7500, "Noise Decay": 0.06, "Noise Level": -3, "Metal Level": -8}],
   ["Tick", {"Metal Decay": 0.025, "Metal Low Cut": 9000, "Noise Decay": 0.02}],
   ["Dusty", {"Metal Mode": 2, "Metal FM": 3, "Character On": 1, "Bits": 10, "Sample Rate": 18000, "Drive": 3}],
   ["Shaker", {"Metal On": 0, "Noise Filter": 1, "Noise Cutoff": 7000, "Noise Reso": 0.2, "Noise Attack": 0.015,
               "Noise Decay": 0.06, "Noise Level": -4}]]},
 {"base": {"Body On": 0, "Metal On": 1, "Noise On": 1, "Choke": 1, "Metal Mode": 0, "Metal Freq": 410, "Inharmonic": 1,
           "Metal Tone": 10000, "Metal Reso": 0.3, "Metal Low Cut": 7000, "Metal Decay": 0.4, "Metal Curve": -3,
           "Metal Color": 0.2, "Noise Filter": 2, "Noise Cutoff": 9000, "Noise Decay": 0.3, "Noise Level": -14,
           "Noise Width": 0.3},
  "styles": [["808", {}],
   ["909", {"Noise Cutoff": 7500, "Noise Decay": 0.35, "Noise Level": -4, "Metal Level": -7}],
   ["Long", {"Metal Decay": 0.9, "Noise Decay": 0.7}],
   ["Sizzle", {"Metal Mode": 1, "Metal Freq": 600, "Inharmonic": 1.6}],
   ["Gated Open", {"Gated": 1, "Release": 0.03, "Metal Decay": 1.2, "Noise Decay": 1.0}]]},
 {"base": {"Body On": 1, "Click On": 1, "Body Freq": 110, "Sweep": 7, "Sweep Time": 0.06, "Body Decay": 0.35,
           "Body Shape": 0, "Mode 2 Level": 0.25, "Mode 2 Ratio": 1.5, "Click Type": 3, "Click Tone": 2500, "Click Level": -18},
  "styles": [["Mid", {}],
   ["Low", {"Body Freq": 75, "Body Decay": 0.5}],
   ["High", {"Body Freq": 170, "Body Decay": 0.25}],
   ["Electro", {"Body Shape": 1, "Sweep": 24, "Sweep Time": 0.12, "Sweep Curve": -3}],
   ["Tribal", {"Noise On": 1, "Noise Filter": 1, "Noise Cutoff": 600, "Noise Decay": 0.2, "Noise Level": -12, "Mode 2 Level": 0.5}]]},
 {"base": {"Body On": 1, "Click On": 1, "Noise On": 1, "Body Freq": 460, "Body Decay": 0.03, "Body Shape": 1, "Sweep": 4,
           "Sweep Time": 0.01, "Click Type": 3, "Click Tone": 1700, "Click Level": -6, "Noise Filter": 1,
           "Noise Cutoff": 3000, "Noise Decay": 0.02, "Noise Level": -12},
  "styles": [["808", {}],
   ["909", {"Body Freq": 520, "Mode 2 Level": 0.6, "Mode 2 Ratio": 2.3}],
   ["Stick", {"Body Freq": 800, "Body Decay": 0.015, "Click Tone": 3500}]]},
 {"base": {"Body On": 0, "Metal On": 1, "Metal Mode": 0, "Metal Freq": 540, "Inharmonic": 0.33, "Metal Tone": 2640,
           "Metal Reso": 0.6, "Metal Color": 0.1, "Metal Low Cut": 400, "Metal Decay": 0.25, "Metal Curve": -4,
           "Metal Attack": 0.001},
  "styles": [["808", {}],
   ["Bell", {"Metal Mode": 1, "Metal Freq": 800, "Inharmonic": 2.1, "Metal Tone": 4000, "Metal Decay": 0.9}],
   ["Agogo", {"Metal Mode": 2, "Metal FM": 1.5, "Metal Freq": 700, "Inharmonic": 0.6, "Metal Decay": 0.18}]]},
 {"base": {"Body On": 0, "Metal On": 1, "Noise On": 1, "Metal Mode": 0, "Metal Freq": 300, "Inharmonic": 1.3,
           "Metal Tone": 7000, "Metal Low Cut": 5000, "Metal Decay": 1.6, "Metal Curve": -3, "Metal Color": 0.4,
           "Noise Filter": 2, "Noise Cutoff": 8000, "Noise Decay": 1.2, "Noise Level": -12, "Noise Width": 0.8},
  "styles": [["Crash", {}],
   ["Ride", {"Metal Freq": 380, "Metal Decay": 2.5, "Metal Tone": 5000, "Metal Reso": 0.5, "Noise Level": -20}],
   ["China", {"Metal Mode": 1, "Inharmonic": 1.9, "Metal Decay": 1.2}],
   ["Reverse Swell", {"Metal Attack": 0.6, "Metal Decay": 0.08, "Noise Attack": 0.6, "Noise Decay": 0.06}]]},
 {"base": {"Body On": 1, "Body Freq": 330, "Body Shape": 0, "FM Amount": 2.5, "FM Ratio": 2.7, "FM Decay": 0.05,
           "Body Decay": 0.15, "Sweep": 3, "Sweep Time": 0.01},
  "styles": [["FM Blip", {}],
   ["Wood", {"Body Freq": 750, "FM Amount": 0.8, "FM Ratio": 3.4, "Body Decay": 0.06, "Click On": 1, "Click Type": 3,
             "Click Tone": 2200, "Click Level": -10}],
   ["Glass", {"Body Freq": 1400, "FM Amount": 1.2, "FM Ratio": 1.41, "Body Decay": 0.5, "Body Curve": -3}],
   ["Laser", {"Body Freq": 200, "Sweep": 48, "Sweep Time": 0.15, "Sweep Curve": -3, "Body Shape": 2, "FM Amount": 0}],
   ["Conga", {"Body Freq": 220, "FM Amount": 0.3, "FM Ratio": 1.5, "Sweep": 5, "Body Decay": 0.25,
              "Mode 2 Level": 0.3, "Mode 2 Ratio": 1.8}]]},
 {"base": {"Body On": 1, "Click On": 0, "Body Freq": 41.2, "Sweep": 12, "Sweep Time": 0.02, "Body Decay": 2.5,
           "Body Curve": -1.5, "Body Shape": 0, "Body Drive": 0.2},
  "styles": [["808 Long", {}],
   ["Short", {"Body Decay": 0.5}],
   ["Distorted", {"Body Drive": 0.7, "Character On": 1, "Drive Type": 0, "Drive": 10}],
   ["Square Growl", {"Body Shape": 1.5, "Filter On": 1, "Filter Type": 1, "Cutoff": 600, "Resonance": 0.3,
                     "Env Amount": 2, "Env Decay": 0.2}],
   ["Gated Bass", {"Gated": 1, "Release": 0.04, "Body Decay": 4, "Body Curve": 0}]]},
 {"base": {"Body On": 1, "Body Freq": 80, "Sweep": 48, "Sweep Time": 0.12, "Sweep Curve": -2, "Body Shape": 2,
           "FM Amount": 1.5, "Body Decay": 0.3},
  "styles": [["Zap", {}],
   ["Noise Riser", {"Body On": 0, "Noise On": 1, "Noise Filter": 1, "Noise Cutoff": 400, "Noise Env Amount": 5,
                    "Noise Env Decay": 0.8, "Noise Attack": 0.5, "Noise Decay": 0.15, "Noise Reso": 0.5}],
   ["Downlifter", {"Body On": 0, "Noise On": 1, "Noise Filter": 0, "Noise Cutoff": 12000, "Noise Env Amount": -6,
                   "Noise Env Decay": 1.2, "Noise Decay": 1.5, "Noise Curve": -2, "Noise Level": -5}],
   ["Crushed Hit", {"Character On": 1, "Bits": 4, "Sample Rate": 4000, "Drive": 6}],
   ["Impulse", {"Body On": 0, "Click On": 1, "Click Type": 0, "Click Tone": 200, "Click Decay": 0.002, "Click Level": -10}]]},
 {"base": {"Body On": 0, "Sample On": 1, "Sample Decay": 10},
  "styles": [["One-shot", {}],
   ["Tight", {"Sample Decay": 0.15, "Sample Curve": -6}],
   ["Under the Body", {"Body On": 1, "Sample Level": -6}]]}
])JSON");
    return t;
}

// The sections a Model sets (the rest: mixer, effects, channel stay)
bool soundSection(const std::string& s) {
    static const std::set<std::string> sound = {"piece", "body", "bodyx", "click", "noise", "metal", "sample",
                                                "filter", "char", "trans", "out"};
    return sound.count(s) > 0;
}
}

std::vector<std::string> scDrumKitBox::styleNames(int model) {
    std::vector<std::string> out;
    const auto& t = templates();
    if(model < 0 || model >= (int)t.size()) return out;
    for(const auto& s : t[model]["styles"]) out.push_back(s[0].get<std::string>());
    return out;
}

// label -> value for every control a Model / Style decides (its sound
// sections, plus Choke)
static ofJson modelValues(int model, int style) {
    ofJson v = ofJson::object();
    for(const auto& s : scDrumKitBox::specs())
        if(soundSection(s.section)) v[s.label] = s.def;
    v["Choke"] = 0;
    const auto& t = templates();
    if(model < 0 || model >= (int)t.size()) return v;
    for(auto it = t[model]["base"].begin(); it != t[model]["base"].end(); ++it) v[it.key()] = it.value();
    const auto& styles = t[model]["styles"];
    if(style >= 0 && style < (int)styles.size())
        for(auto it = styles[style][1].begin(); it != styles[style][1].end(); ++it) v[it.key()] = it.value();
    return v;
}

void scDrumKitBox::applyModel(Pad& p, int model, int style) {
    const ofJson v = modelValues(model, style);
    const std::string oldModelName = p.I("Model").get() >= 0 && p.I("Model").get() < (int)modelNames().size()
        ? modelNames()[p.I("Model").get()] : "";
    restoring = true;
    for(auto it = v.begin(); it != v.end(); ++it) {
        const std::string label = it.key();
        const float x = it.value().get<float>();
        if(auto f = p.f.find(label); f != p.f.end()) f->second = x;
        else if(auto i = p.i.find(label); i != p.i.end()) i->second = (int)std::lround(x);
        else if(auto b = p.b.find(label); b != p.b.end()) b->second = x > 0.5f;
    }
    p.I("Model") = model;
    p.I("Style") = style;
    restoring = false;
    p.sound.clear();
    dropdowns.set(p.I("Style").getEscapedName(), styleNames(model));
    // a pad still named after its model (or its row): the new model's name
    if(p.name.empty() || p.name == oldModelName || p.name == "Pad " + ofToString(p.index + 1))
        p.name = modelNames()[std::clamp(model, 0, (int)modelNames().size() - 1)];
    syncPad(p);
    sendChokeMasks();
}

// ── Factory kits (the snapshots' factory collection) ────────────────────────

std::vector<ofJson> scDrumKitBox::factorySnapshots() const {
    // each pad: [model, style, {label: value, ...} extras]
    static const char* src = R"JSON([
 {"name": "Init", "pads": [[0, 0], [1, 0], [3, 0], [4, 0]]},
 {"name": "909 Kit", "pads": [[0, 0], [1, 0], [2, 0], [3, 1], [4, 1], [5, 0], [6, 1], [8, 1]]},
 {"name": "808 Kit", "pads": [[0, 1], [1, 1], [2, 1], [3, 0], [4, 0], [5, 1], [7, 0], [6, 0]]},
 {"name": "Techno Warehouse", "pads": [
    [0, 2], [2, 3, {"Reverb On": 1, "Rev Mix": 0.3, "Rev Decay": 2.2, "Duck From": 1, "Duck Amount": 0.4}],
    [3, 2, {"Duck From": 1, "Duck Amount": 0.5}], [4, 1, {"Duck From": 1, "Duck Amount": 0.6}],
    [9, 1, {"Echo On": 1, "Echo Mix": 0.25, "Echo Time": 0.75}], [8, 1, {"Duck From": 1, "Duck Amount": 0.5}]]},
 {"name": "Hardcore", "pads": [[0, 3, {"Compressor On": 1, "Comp Threshold": -20, "Comp Ratio": 6}], [1, 4], [2, 4], [3, 3], [11, 0]]},
 {"name": "Minimal", "pads": [[0, 5], [6, 2], [3, 2], [9, 0], [9, 2, {"Reverb On": 1, "Rev Mix": 0.35}], [2, 2]]},
 {"name": "Trap", "pads": [[10, 0], [1, 2, {"Reverb On": 1, "Rev Mix": 0.15}], [2, 4], [3, 0], [4, 0], [9, 3]]},
 {"name": "Lo-fi", "pads": [[0, 0, {"Character On": 1, "Bits": 9, "Sample Rate": 16000, "Drive": 6}], [1, 4], [3, 3], [3, 4],
    [5, 1, {"Character On": 1, "Bits": 10, "Sample Rate": 20000}]]},
 {"name": "Electro", "pads": [[0, 6], [1, 5], [2, 0], [3, 1], [5, 3], [9, 3], [7, 0]]}
])JSON";
    ofJson pad = initialPad;
    pad.erase("path");
    std::vector<ofJson> out;
    for(const auto& kit : ofJson::parse(src)) {
        ofJson arr = ofJson::array();
        for(const auto& entry : kit["pads"]) {
            const int model = entry[0].get<int>(), style = entry[1].get<int>();
            ofJson ps = pad;
            ofJson v = modelValues(model, style);
            if(entry.size() > 2) for(auto it = entry[2].begin(); it != entry[2].end(); ++it) v[it.key()] = it.value();
            v["Model"] = model;
            v["Style"] = style;
            for(auto it = v.begin(); it != v.end(); ++it) {
                const float x = it.value().get<float>();
                std::string text = ofToString(x);
                for(const auto& s : specs()) if(it.key() == s.label && s.type != 'f') text = ofToString((int)std::lround(x));
                ps["controls"][scSnapshots::escapeKey(it.key())] = text;
            }
            ps["name"] = modelNames()[model];
            arr.push_back(ps);
        }
        out.push_back({{"name", kit["name"]}, {"pads", arr}});
    }
    return out;
}

// ── The model libraries ─────────────────────────────────────────────────────

std::string scDrumKitBox::libraryPath(int model) const {
    const std::string name = modelNames()[std::clamp(model, 0, (int)modelNames().size() - 1)];
    return ofToDataPath("nodeSnapshots/DrumKitBox/Sounds/" + name + ".json", true);
}

std::vector<scDrumKitBox::Sound>& scDrumKitBox::sounds(int model) {
    auto it = library.find(model);
    if(it != library.end()) return it->second;
    auto& list = library[model];
    const std::string path = libraryPath(model);
    if(std::filesystem::exists(path)) {
        try {
            const ofJson j = ofLoadJson(path);
            if(j.contains("sounds") && j["sounds"].is_array())
                for(const auto& e : j["sounds"])
                    if(e.contains("name") && e["name"].is_string() && e.contains("pad") && e["pad"].is_object())
                        list.push_back({e["name"].get<std::string>(), e["pad"]});
        } catch(const std::exception& e) { ofLogWarning("scDrumKitBox") << path << ": " << e.what(); }
    }
    return list;
}

void scDrumKitBox::saveLibrary(int model) {
    ofJson arr = ofJson::array();
    for(const auto& s : sounds(model)) arr.push_back({{"name", s.name}, {"pad", s.pad}});
    const std::string path = libraryPath(model);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    ofSavePrettyJson(path, {{"model", modelNames()[std::clamp(model, 0, (int)modelNames().size() - 1)]}, {"sounds", arr}});
    // other DrumKitBoxes read it again
    library.erase(model);
}

void scDrumKitBox::storeSound(Pad& p, const std::string& name) {
    if(name.empty()) return;
    const int model = p.I("Model").get();
    ofJson st = padState(p.index);
    st.erase("name");
    st.erase("sound");
    auto& list = sounds(model);
    auto it = std::find_if(list.begin(), list.end(), [&](const Sound& s) { return s.name == name; });
    if(it != list.end()) it->pad = st; else list.push_back({name, st});
    saveLibrary(model);
    p.sound = name;
}

// The pad's sound: layers, voice, effects, modulation and sample. Its
// channel, ducking and mixing stay.
void scDrumKitBox::recallSound(Pad& p, const Sound& s) {
    ofJson st = s.pad;
    if(st.contains("controls") && st["controls"].is_object())
        for(const char* keep : {"Channel", "Duck From", "Mute", "Solo", "Level"}) st["controls"].erase(scSnapshots::escapeKey(keep));
    st.erase("name");
    const std::string modelName = modelNames()[std::clamp(p.I("Model").get(), 0, (int)modelNames().size() - 1)];
    const bool rename = p.name == modelName || (!p.sound.empty() && p.name == p.sound) || p.name == "Pad " + ofToString(p.index + 1);
    applyPadState(p.index, st);
    const std::string path = st.value("path", std::string());
    if(!path.empty() && path != p.sample->path) loadSample(p, path);
    p.sound = s.name;
    if(rename) p.name = s.name;
    syncPad(p);
    sendChokeMasks();
    sendDucks();
}

void scDrumKitBox::deleteSound(int model, const std::string& name) {
    auto& list = sounds(model);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Sound& s) { return s.name == name; }), list.end());
    saveLibrary(model);
    for(auto& p : pads) if(p->I("Model").get() == model && p->sound == name) p->sound.clear();
}

void scDrumKitBox::stepSound(Pad& p, int dir) {
    const int model = p.I("Model").get();
    const int styles = (int)styleNames(model).size();
    const auto list = sounds(model);   // a copy: recalling may reload the library
    const int n = styles + (int)list.size();
    if(n == 0) return;
    int cur = std::clamp(p.I("Style").get(), 0, std::max(0, styles - 1));
    for(int k = 0; k < (int)list.size(); k++) if(list[k].name == p.sound) cur = styles + k;
    const int next = ((cur + dir) % n + n) % n;
    if(next < styles) applyModel(p, model, next);
    else recallSound(p, list[next - styles]);
}
