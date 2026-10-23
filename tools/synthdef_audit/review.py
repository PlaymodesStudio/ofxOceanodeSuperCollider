"""Curated conclusions. Numbers describe logical creator entries, not binary variants."""
GROUPS=[]
def g(id,phase,target,names,reason,preserve,kind='Mode',risk='Medium'):
    GROUPS.append(dict(id=id,phase=phase,target=target,names=names.split('|'),reason=reason,preserve=preserve,kind=kind,risk=risk))
g('A01',1,'BandpassBank (PanAz)','MonoBandPassBank1080|MonoBandPassBank1200_|MonoBandPassBank256|MonoBandPassBank128|MonoBandPassBank64|MonoBandPassBank32|MonoBandPassBank21|MonoBandPassBank6','Bodies differ only in the literal band count; 32 and 64 are identical, both set numBands = 64.','Keep two BPF stages, audio-rate arrays, mono input summing, PanAz, 1/sqrt(bands) and dry path. Map old 32 to 64, not 32.','Size variants','Low')
g('A02',1,'BandpassBank (PanX)','MonoBandPassBank128PanX|MonoBandPassBank256PanX','Same generator apart from band count. Distinct from A01: one BPF, control-rate arrays, PanX and other defaults.','Retain separate PanX profile; do not substitute the two-stage PanAz response.','Size variants','Low')
g('A03',1,'AdditiveBank','Additive32_|Additive64_|Additive128_|Additive256_|Additive512_|Additive1080_|Additive1200_','Seven bodies differ only in partial count.','Compile partial count as a structural variable. Keep Rand oscillator phase ordering, per-size 1/partials normalization, and mono/Pan2/PanAz branches.','Size variants','Low')
g('A04',1,'RatioResonatorBank','CustomResonator_8_|CustomResonator_16_|CustomResonator_32_|CustomResonator_64_','8/16/32 are count-only copies; 64 additionally curves send and levels.','Preserve shared ratio arrays, audio-rate ratios and amplitudes, control-rate decay and send-affected dry path. Add legacy gain-law mode for 64.','Size + mode','Medium')
g('A05',1,'FrequencyResonatorBank','CustomResonator_128|CustomResonator256_|CustomResonator512_','128 uses per-channel frequency arrays; 256/512 add per-partial panning and differ only in size.','Preserve array layouts and control rates. Keep 128 unpanned and 256/512 Pan2/PanAz channel selection exactly; do not silently fix their routing.','Size + mode','Medium')
g('A06',1,'BasicOscillator (pitch)','CheapSaw|CheapTri|CheapPulse|CheapSin|CheapImpulse','Five copies of pitch-to-Hz, FM input, levels and output boilerplate with a waveform substitution.','Keep exact Saw/LFTri/Pulse/SinOsc/Impulse kernels, phase defaults, linear levels, and wrapped pulse-width mapping.','Runtime mode','Low')
g('A07',1,'BasicOscillator (Hz)','SimpleImpulse|SimplePulse|SimpleSaw|SimpleSine','Simple oscillator wrappers expose Hz/amplitude/phase rather than the Cheap family pitch interface.','Retain each waveform kernel and control rate: SimpleSaw uses a resettable raw Phasor ramp, not Saw.ar; SimpleSine exposes phase offset. Preserve reset timing, phase conventions, defaults and gain law. Keep this separate from A06 initially.','Runtime mode','Medium')
g('A08',1,'BasicFilter','LPF|HPF','Matching parameter and mix wrapper around LPF or HPF.','Preserve cutoff units, lag, XFade2 equal-power law and existing defaults. A type switch must preserve state or recreate node deliberately.','Runtime mode','Low')
g('A09',1,'ResonantFilter','RLPF|RHPF','Matching resonant low/high-pass wrappers.','Keep pitch conversion, reciprocal-Q conventions, control rate and equal-power mix. RBPF has a different two-stage core.','Runtime mode','Low')
g('A10',1,'FourPoleFilter','RLPF4Pole|RHPF4Pole','Same wrapper with BLowPass4 versus BHiPass4.','Keep FM in Hz, all existing controls/defaults and original four-pole kernels; do not replace with LadderMultimode.','Runtime mode','Low')
g('A11',1,'Gain','Gain|LinearGain','Both multiply the input by a smoothed control; only mapping and control range differ.','Keep Gain formula 0.000031623*exp(10.36*x) and LinearGain range 0..8. A normalized legacy control is not interchangeable with dB.','Runtime mode','Low')
g('A12',1,'ClipSaturator','HardClip|Tanh','Same DC removal, drive, ceiling, level, mix and final limiter wrapper.','Keep hard clipping versus tanh equations, clip scaling, dry tap and final limiter latency exactly.','Runtime mode','Low')
g('A13',1,'RationalSaturator','SoftSaturator|ParamShaper','Same k=2*a/(1-a), same rational transfer function. ParamShaper adds post-mix gain and changes defaults/lag.','Profile defaults and mix smoothing must survive; SoftSaturator maps to postgain=1. Distort is not a drop-in replacement.','Runtime mode','Low')
g('A14',1,'DetunedOscillator','SuperSaw|SuperPulse','Identical detune curve, seven oscillator layout and balance laws; LFSaw is replaced by LFPulse with width.','Keep random phase draws in order, detune polynomial, mix curve, filter/output stage and phase conventions.','Runtime mode','Medium')
g('A15',1,'FeedbackFlanger','Flanger|FlangerPitch','Same DelayN feedback network; delay is seconds or reciprocal MIDI frequency and allocation is 0.02 vs 0.2 seconds.','Keep delay bounds and block feedback latency. Converting pitch controls must retain full low-pitch behavior.','Runtime mode','Medium')
g('A16',1,'OversampledOscillator','SawOS|SquareOS|SinOs|TriOs','Shared pitch/FM/phase/levels wrapper around different 4x oversampled kernels.','Preserve kernel, phase mapping, width support, 4x oversampling and audio-rate inputs. Do not replace with non-oversampled waves.','Runtime mode','Medium')
g('B01',2,'Compressor','Compressor|CompressorB|CompressorC','Three distinct detector/gain designs fit named modes, not a single replacement algorithm.','A: linear threshold with below/above slopes. B: independent Amplitude detector, dB threshold/ratio/knee and curved send. C: linked max detector, HPF, log ballistics, lookahead, saturator and DC filtering.','Algorithm profiles','High')
g('B02',2,'Kick','KickDrum|Kickdrumb|Kickdrumc|KickBestia','KickBestia and Kickdrumb share triangle/tanh core but different envelopes; the other two use PM/pulse and sine/noise architectures.','Keep four complete algorithms and trigger semantics. Envelope start levels, pitch trajectories, velocity and output levels require per-profile adapters.','Algorithm profiles','High')
g('B03',2,'Phaser','Phaser|PhaserB|PhaserC','Delay-allpass, AnalogPhaser and six FOS stages are different effects under suffix names.','Preserve topology, stage count, coefficients and LocalIn latency. Use descriptive modes; never claim B/C are supersets of A.','Algorithm profiles','High')
g('B04',2,'Chorus','Chorus|ChorusB','A uses ten random modulated DelayL taps; B uses six phase-offset sinusoidal DelayC taps with nonlinear feedback.','Keep modulation distribution, tap gain, interpolator, randomness and feedback paths; no direct parameter equivalence.','Algorithm profiles','High')
g('B05',2,'SpectralCircularBuffer','CircularBufferFFT|CircularBufferFFT_B','A computes one diffused FFT reader and multiplies its gain by active-reader count; B runs separate FFT buffers/chains per reader.','Modes must retain coherent gain scaling versus independent phase diffusion, buffer allocation, reader envelopes and CPU behavior.','Algorithm profiles','High')
g('B06',2,'SpectralSamplePlayer','FFTStretch|FFTStretchRate','Closely related BufFFT engines with 4 vs 2 overlaps and different rate/buffer interfaces.','Keep overlap/window/trigger settings, PV_Diffuser condition, transpose versus signed-rate mapping and multibuffer versus shared-buffer semantics. Preserve missing-buffer safeguards.','Algorithm profiles','High')
g('B07',2,'Pulsar','Pulsar|Pulsar_B','A uses saw-derived pulsaret/window math; B uses triggered grain/phase processing and additional modulation.','Two named algorithms with distinct Hz/pitch controls. Preserve overlap/window shape and phase/reset behavior.','Algorithm profiles','High')
g('B08',2,'VectorPhaseOscillator','VectorSynth|VectorSynthB','Both are phase-distortion/vector generators, but phase mapping, harmonic interpolation and feedback differ.','Keep original transfer functions, even/odd selection, feedback latency and B formant/antialias toggles. B is not a compatible upgrade.','Algorithm profiles','High')
g('B09',2,'WaveguideFlute','Flute|FluteB','B changes delay limits and adds a comb-resonated excitation path and crossfade.','Retain delay compensation, breath/noise scaling, nonlinear feedback, path gain and filter/limiter ordering per algorithm. Flute2Compat needs separate migration evidence.','Algorithm profiles','High')
g('B10',2,'ShimmerReverb','ShimmerReverb|ShimmerReverb_Feed','A injects three pitch-shift voices into a comb/allpass reverb; B shifts within a LocalIn feedback network.','Keep feedforward vs feedback architecture, number of shifters, ratios, delay modulation, tail response and gain.','Algorithm profiles','High')
g('B11',2,'MultibandDistortion','MultibandDistortion|MultibandHardClip|MultibandSineShaper','Shared three-band processing with different nonlinear curves, crossover subtraction and DC preprocessing.','Profiles must preserve crossover equations: sine mid-band filters residual input, unlike the other pair. Preserve clip controls and dry path.','Algorithm profiles','High')
g('B12',2,'SpectralCombine','FFTVocoder|FFTVocoderClassic|FFTVocoderMax','Shared FFT shell; bin math differs: magnitude/phase exchange, magnitude multiplication, max-magnitude phase selection.','Preserve all three bin formulas, input ownership, FFT setup and default levels (Classic is initially silent). Do not substitute a conventional vocoder.','Runtime bin mode','Medium')
g('B13',2,'FilterbankDelay','MonoSpectralDelay512|SpectralDelay512','Both are 512-band BPF/CombC banks, not FFT spectral delay. One sums input to mono and pans bands; the other processes channels independently.','Keep mono/per-channel routing, PanAz, send-affected dry signal, delay milliseconds and tan(decay*pi/2) mapping.','Algorithm profiles','High')
g('B14',2,'FilteredEcho','Echo|PitchDelay','Same filter-in-feedback shell; controls differ and PitchDelay adds soft clipping inside feedback.','Keep seconds versus MIDI-period delay, cutoff bounds, filtertype ar/kr, nonlinear feedback and block latency.','Algorithm profiles','High')
g('B15',2,'AnalogWavefolder','BuchlaFoldOs|SergeFoldOs','Same wrapper, separate native oversampled folder models.','Keep Buchla/Serge UGen choice, 4x oversampling and distinct drive ranges 0..10 versus 0..1. Runtime Select runs both unless implemented inside a generic UGen.','Algorithm profiles','Medium')
g('B16',2,'Bandpass','BandpassFilter|RBPF','Both cascade two BPF stages. RBPF adds dry/wet but omits the 0.005 minimum reciprocal-Q guard.','Keep compatibility guard mode and old mix/gain contracts. Fixing q=1 behavior is a separate sound-changing task.','Runtime mode','Medium')

CATEGORY_DESC={
'Effect/Distortion':'Nonlinear transfer functions, clipping, folding and saturation. TransientShaper belongs with dynamics.',
'Effect/Dynamics':'Level-dependent gain, compression and gating; also currently contains simple gain and signal hygiene.',
'Effect/Eq':'Frequency balance and harmonic enhancement; excitation is nonlinear processing rather than equalization.',
'Effect/Filter':'Frequency-selective processors, including banks and several distinct resonant/ladder designs.',
'Effect/Modulation':'Audio amplitude/frequency modulation effects, distinct from control-signal generators.',
'Effect/Pitch':'Pitch/time-grain shifting, frequency translation and octave processing.',
'Effect/Resonators':'Externally excited resonant models and programmable modal banks.',
'Effect/Spectral':'FFT-bin processing plus some time-domain filter-bank processors currently described as spectral.',
'Effect/Time/Blur':'Time smearing currently declared separately from the source Delay folder.',
'Effect/Time/Delay':'Echo, feedback delays and granular buffer readers.',
'Effect/Time/Freeze':'Hold or freeze incoming audio using buffer/grain processing.',
'Effect/Time/Looping':'Live circular buffers, repetition, capture and playback.',
'Effect/Time/Phase':'Chorus, flanger and phaser effects; phase inversion is polarity routing.',
'Effect/Time/Reverb':'Reverberation networks, diffusion, shimmer and granular ambience.',
'Effect/Spatial/Widening':'Interchannel decorrelation for widening; currently stored under Spatial/Panning.',
'InOut':'External audio input.', 'Mixing':'Signal summing and crossfades.',
'Modulation/Envelope':'Event, gated and shape-controlled amplitude/control envelopes.',
'Modulation/LFO':'Periodic control oscillators and morphing waves.',
'Modulation/Ramps':'Phasors, ramps and transport-linked phase.',
'Modulation/Random':'Probability, choice and random-value generation.',
'Modulation/Sequencer':'Step/value sequencing, quantizers and transport state.',
'Modulation/Trigger':'Clock impulses, duration, gate timing and event selection.',
'Modulation/Utility':'Sample/hold utility for control values.',
'Optimized/Bloobs':'Legacy-compatible performance profiles for 78-channel Bloobs patches; optimization is a tag, not a musical role.',
'Routing':'Channel selection, mapping, duplication, matrix routing and band splitting.',
'Source/Additive':'Additive oscillator banks and a legacy Forms generator.',
'Source/Analog':'Basic and detuned oscillators; algorithm choices include both bandlimited and raw waveforms.',
'Source/Chaotic':'Chaotic oscillator or trajectory generation.',
'Source/FM':'Frequency/phase modulation and an additive DynKlang bank.',
'Source/Feedback':'Self-excited feedback oscillator networks and drones.',
'Source/Formants':'Formant-oriented oscillator generators.',
'Source/Noise':'Noise processes, clicks and stochastic event sources.',
'Source/Oversampling':'Oversampled oscillator wrappers; oversampling should become a build/quality tag.',
'Source/Percussion':'Triggered drum voices with several synthesis methods.',
'Source/Percussion/808':'Drum-machine voice models; 808 is a model tag under percussion.',
'Source/PhysicalModelling':'Waveguides, modal instruments and other acoustic/electric instrument approximations.',
'Source/Pulsar':'Pulsaret, pulse-train and wavelet generators.',
'Source/Radio':'Radio-style source generator.',
'Source/Sampler':'Buffered sample playback, stretching, granular reading and loop players.',
'Source/Special':'Mixed synthesis methods and named voices; replace this catch-all with algorithm categories.',
'Source/Stochastic':'Stochastic/dynamic random and noise-field oscillators.',
'Source/Sync':'Hard-sync oscillators with different waveform and bandlimiting methods.',
'Source/Vector':'Vector/phase-distortion oscillators.',
'Source/Wavetable':'Buffer/table-driven oscillators.',
'Spatial/MidSide':'Mid/side encoding and decoding.',
'Spatial/Panning':'Spatial distribution, balance and panning laws, including spectral-band placement.',
'Spatial/Perceptual':'Binaural, Haas and rotating-speaker effects.',
'Synthesis':'FM7 creator entries stored in runtime defaults; move browser metadata to Source/FM.',
'Utilities/Analysis':'Envelope, transient, spectrum and sample-rate information.',
'Utilities/Conversion':'Control/signal conversion and constant signal generation.',
'Utilities/Feedback':'Bus-based feedback plumbing; these are routing infrastructure.',
'Utilities/Math':'Arithmetic, logical, statistics, mapping and quantization operators.',
'Utilities/Other':'Currently only phasor wrap-event detection; should be under control timing.',
'Utilities/Voicing':'Impulse counting; this is event processing rather than voice allocation.',
'Compatibility/OceanAudio':'Six metadata wrappers used to load older OceanAudio graphs. These remain auto-compile eligible.',
}

SPECIAL={
'Phaser':'Effect/Phaser','PhaserB':'Effect/Phaser','PhaserC':'Effect/Phaser','DynKlang':'Source/Additive','PluckResonator':'Effect/Resonator','TransientShaper':'Effect/Dynamics','HarmonicExciter':'Effect/Nonlinear',
'Gain':'Routing/Gain','LinearGain':'Routing/Gain','Sanitize':'Utility/SignalHygiene','DCCorrection':'Utility/SignalHygiene','PhaseInvert':'Routing/Polarity',
'RingmodSidechain':'Effect/Modulation','SampleRate':'Analysis/System','SampleAndHold':'Control/Value','ImpulseCounter':'Control/Event','PhasorCycleTrigger':'Control/Event',
'Transport':'Control/Transport','transportPhasor':'Control/Transport','TransportPulse':'Control/Transport','ImpulseQuantizer':'Control/Quantize','GateQuantizer':'Control/Quantize','ValueQuantizer':'Control/Quantize',
'ChannelDecorrelator':'Spatial/Width','MidSide':'Routing/Encoding','InFeedback':'Routing/Feedback','FeedbackNode':'Routing/Feedback',
'SuperFM7':'Source/FM','SuperFMDrone':'Source/FM','BloobsNoise':'Source/Noise','BloobsBandpass':'Effect/Filter','BloobsStereoPan':'Spatial/Pan',
'BinauralHRTF':'Spatial/Binaural','BinauralHaas':'Spatial/Width','Leslie':'Spatial/Rotation',
'OctaveDown':'Effect/Pitch','MonoSpectralDelay512':'Effect/FilterbankDelay','SpectralDelay512':'Effect/FilterbankDelay',
'AliasingSynth':'Source/Digital','BubbleA':'Source/Percussion','BubbleB':'Source/PhysicalModel','BuzzBass':'Source/Voice','Frumbo':'Source/Voice','GutterSynth':'Source/Voice','IkedaBass':'Source/Voice','InterferenceRhythm':'Source/Rhythm','Lissajous':'Source/Chaotic','Recursive':'Source/Feedback','SigmoidOSC':'Source/PhaseDistortion','TB303':'Source/Voice',
'Input':'IO/Input','AudioMerge':'Routing/Mix',
}
PREFIX={'Effect/Distortion':'Effect/Nonlinear','Effect/Eq':'Effect/EQ','Effect/Resonators':'Effect/Resonator','Effect/Time/Blur':'Effect/Blur','Effect/Time/Delay':'Effect/Delay','Effect/Time/Freeze':'Effect/Freeze','Effect/Time/Looping':'Effect/Loop','Effect/Time/Phase':'Effect/ModulatedDelay','Effect/Time/Reverb':'Effect/Reverb','InOut':'IO/Input','Mixing':'Routing/Mix','Modulation/Envelope':'Control/Envelope','Modulation/LFO':'Control/Oscillator','Modulation/Ramps':'Control/Phase','Modulation/Random':'Control/Random','Modulation/Sequencer':'Control/Sequence','Modulation/Trigger':'Control/Event','Modulation/Utility':'Control/Value','Source/Analog':'Source/Oscillator','Source/Oversampling':'Source/Oscillator','Source/PhysicalModelling':'Source/PhysicalModel','Source/Vector':'Source/PhaseDistortion','Source/Special':'Source/Voice','Spatial/Panning':'Spatial/Pan','Spatial/MidSide':'Routing/Encoding','Spatial/Perceptual':'Spatial/Binaural','Utilities/Analysis':'Analysis/Audio','Utilities/Conversion':'Utility/Conversion','Utilities/Feedback':'Routing/Feedback','Utilities/Math':'Utility/Math','Utilities/Other':'Control/Event','Utilities/Voicing':'Control/Event','Source/Wavetable':'Source/Wavetable'}
def proposed(e):
    if e['status'] in ['Build tools','Code sandbox','Experiment']:return {'Build tools':'Development/Compiler','Code sandbox':'Development/Examples','Experiment':'Development/Experimental'}[e['status']]
    if e['status']=='Runtime' and not e['body_hash']:return 'Runtime/'+e['path'].split('/')[1]
    if e['status']=='Compatibility':return 'Compatibility/Adapters' if e['body_hash'] else 'Archive/Legacy'
    return SPECIAL.get(e['name'],PREFIX.get(e['category'],e['category']))
