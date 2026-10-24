# ofxOceanodeSuperCollider

Addon for interfacing [ofxOceanode](https://github.com/PlaymodesStudio/ofxOceanode) and [Supercollider](https://github.com/supercollider/supercollider) via [ofxSupercollider](https://github.com/PlaymodesStudio/ofxSuperCollider)

## PolyComb morphing

Local snapshots and MacroMixer share PolyComb's sound-morph engine, including
temporary voice-count banks and interpolation of generated pitch maps. See the
[consolidated reference](src/docs/polyCombMorphing.md) for behavior, boundaries,
build instructions and the quick/audio/native regression commands.

## Startup updates

On macOS, launching an app with this addon checks the public addon repository
in the background for differences in the installed SynthDef sources, bundled
compiler, UGens, and shared node snapshot banks/presets. It uses the addon
checkout's branch, or `main` when the checkout is unavailable, and also checks
the third-party UGens published in the `sc-extensions` release.

When changes are available, **Update** runs the existing sync and compilation
workflow; **Later** leaves the installed files untouched. Locally edited presets
are preserved, with the repository version saved beside them as ` (repo)`.
Updated UGens offer a server restart after syncing. Up-to-date launches show no
prompt; connection failures are logged without blocking startup. You can also
update manually through **SuperCollider → Sync with SynthDefs Git…**.

Startup checks are enabled by default. Toggle **SuperCollider → Check for SynthDef
updates on startup**, or select **Don't check on future startups** in the update
prompt. The choice is saved immediately and does not affect manual syncing.

After syncing, choosing an uncompiled **N Chan** count on a `~synthCreator`
SynthDef opens the **Compile** prompt. The addon builds that exact channel count
and variable combination from the synced sources, installs it, loads it into
the running servers, and resumes the node. Synced UGen classes are included.
Sources explicitly marked `@oceanode-no-autocompile` and standalone scripts
without `~synthCreator` cannot use this workflow.

Run the compiler integration test on macOS with
`python3 tools/synthdef_compiler_test/run.py` (requires the bundled sclang).

## Folded audio waveforms

Folding an audio node shows a live waveform in each ScBus output's display
area. The compact preview shows the first channel over the latest 50 ms, using
64 samples and a fixed ±1 display range. Macro/Router/Portal ScBus displays can
reuse the same capture when their folded display exposes that output.

Only visible displays request capture. A bus shared by multiple displays uses
one analyzer and one 64-value control bus, polled through batched OSC at 20 Hz.
Expanding a node, hiding its display, or moving it outside the viewport releases
its capture within two frames. Drawing uses a small polyline with no texture or
FBO. Capture is suspended during NRT rendering.

The preview SynthDef is bundled in `Defaults/Analysis/foldedWaveform`; existing
app data folders can load it from this addon checkout without a manual copy.
Run `python3 tools/folded_waveform_test/audio.py` and
`python3 tools/folded_waveform_test/lifecycle.py` for regression checks.

## Keyboard piano

Focus the PolyComb, AdditiveBox, SteamPipeBox, PulsarBox, GrainBox, AliasingBox,
PhysicalBox, SampleBox or AnalogBox editor to play it from the computer keyboard.
`zxcvbnm,.` plays white keys starting with **Z = C4 (MIDI 60)**.
`qwertyuiop` plays white keys one octave higher, starting with **Q = C5**.
The matching black keys are `s d` (C♯, D♯), `g h j` (F♯, G♯, A♯),
and `l ñ` (the next C♯, D♯); `a f k` are silent gaps.
On keyboards without ñ, use the semicolon key in that position.
**Up / Down** shifts new notes by one octave; held notes retain their original
pitch. The current starting octave appears in the editor.

The pitched Box editors include a piano footer showing active Pitch/Gate notes,
including external parameter input and computer-keyboard notes. Click a piano key
to play, or drag across keys. Enable **Hold** to latch clicked notes; click them
again to switch them off. Disable Hold to release latched notes, or use **Release**
to stop local notes. Hiding/collapsing the editor also releases them.

Hold several keys for chords, up to the instrument's configured voice count.
When voices run out, the newest notes take priority. Releasing a key closes
its gate; leaving the editor, hiding/collapsing it or entering text releases
keyboard notes. Ctrl, Cmd, Alt and Shift combinations are reserved for shortcuts.
GrainBox plays the selected engine's playheads and respects its MIDI/relative
pitch mode. SampleBox plays the selected pad's channel with last-note priority;
pads sharing that channel receive the same notes. Its Gate Mode determines
whether a release stops the sample or a trigger plays the full region.

Keyboard notes write the existing Pitch and Gate (Trigger/Gate in SampleBox)
parameters. Auto gate mode becomes gated after playing. Explicit Drone mode and
audio-rate input cables retain their normal precedence over these values.

Run the headless input regression tests with `python3 tools/keyboard_piano_test/run.py`.

## SynthDef On/Off

SynthDef nodes (`scSynthdef` and legacy `scOldSynthdef`) have an `On`
boolean, enabled by default. An unlabeled checkbox appears just to the right
of the collapse arrow in the node header, with a connectable inlet that also works when collapsed.
Connect a Router B to expose it inside a macro; the value saves with presets.
The header display requires an ofxOceanode version with
`OFXOCEANODE_HAS_HEADER_TOGGLE`; older versions show `On` as a normal parameter.

Turning `On` off pauses the running synth to stop its DSP processing while
retaining its memory and state. Its first audio input feeds every output;
other inputs are ignored for forwarding. With no first input connected, the
outputs carry silence. Consecutive Off nodes are resolved through the chain.
The visible cables remain intact, and turning `On` back on restores processing.
Macro activation respects the switch. No `.scd` changes are needed.

Pausing freezes delay/reverb state and cuts the current tail; resuming can
restore stored audio. Switching currently changes routing directly, without
a crossfade. Custom C++ instrument/effect nodes are unchanged.

Run the routing regression tests with `sh tools/audio_routing_test/run.sh`.

ScBus inputs accept one incoming cable, with the usual cable removal and rewiring.
Use `SC AudioMerge*` (Mixing) to sum signals channel by channel at unity gain.
`N Chan` controls the output width and `nInputs` (1..32) creates individually
connectable `In1` … `In32` pins. There is no normalization or limiter. Reducing
`nInputs` removes the excess pins and their cables; retained inputs keep their
connections. Missing channel/input builds compile on demand like other SynthDefs.
Run `python3 tools/audio_merge_test/audio.py` for the offline summing and
unplug/replug regression. `python3 tools/audio_merge_test/lifecycle.py` checks
preset pins before wiring and the missing-build approval path. `tools/migrate_audio_merge.py <bin/data>` previews
preset/macro migrations; `--apply` writes them with backups.

## Core transport and optional timeline integration

This addon requires the current ofxOceanode core. Shared transport, frame
stepping, timestamped parameter scheduling and instrument metadata are core
APIs and are always compiled. Transport sequencers, Sync To Transport options,
NRT WAV rendering and SC NRT Recorder work without the timeline addon.

Only clip-specific features need **ofxOceanodeTimeline**: Timeline Wave Track,
parameter publishing menus, track-colour indicators and timeline binding
retargeting. Add the timeline addon to the application's `addons.make`,
regenerate the project and call `ofxOceanodeTimeline::setup(oceanode)` after
`oceanode.setup()`, before loading presets.

The timeline addon exports `OFX_OCEANODE_HAS_TIMELINE=1` for the whole build.
Header discovery does not enable it. `OFXOCEANODESC_HAS_TIMELINE=0` can disable
SC's timeline integration explicitly; manually enabling it requires linking
the timeline addon. Compiled timeline UI also checks runtime attachment.

The backend scheduling adapter always registers targets with core. Timeline
lanes can produce scheduled events when installed, and other scheduling
producers can use the same interface without linking timeline code.

## FMBox

`fmBox` uses Aziz Ege Gönül's DX7-Supercollider implementation, with the original
16,384-preset `.afx` bank and an in-editor bank browser with searchable names
and source banks from the upstream catalogue (names are marked unverified).
The dockable Box editor
keeps its editable FM routing matrix, operator/pitch envelopes, snapshots and
morphing, right-click publishing/modulation/timeline controls, saturation, EQ,
echo and reverb. The node face remains compact. Existing SuperFM nodes remain
available. See [`synthdefs/Defaults/FMBox/README.md`](synthdefs/Defaults/FMBox/README.md)
for presets, credits and verification.

## AliasingBox

`aliasingBox` adds the Reaktor-inspired table instrument, 38 imported factory
snapshots, shared dynamic LFOs/envelopes and routing, timeline instrument
inputs, and a dockable editor with live table and waveform displays. See
[`synthdefs/Defaults/AliasingBox/README.md`](synthdefs/Defaults/AliasingBox/README.md)
for compilation, preset recovery and fidelity notes.

## SynthDef collection

<!-- synthdef-tree:start -->
<!-- Generated by tools/update_synthdef_tree.py: edit the sources, not this list. -->

The SuperCollider sources live in [`synthdefs/`](synthdefs/) (479 generic synths). Generic synths are built for any channel count with `~synthCreator`; `Defaults/` holds the synths of the server and of nodes with a C++ counterpart; `Compatibility/` keeps older definitions; `WIP/` holds experiments not yet in Oceanode. See [`synthdefs/README.md`](synthdefs/README.md).

<details>
<summary><b>Effect</b> (199)</summary>

```
Effect/
├── Distortion/
│   ├── AsymParSat — Asymmetric parallel saturation distortion effect
│   ├── Bias — Bias effect with bottom clipping
│   ├── BuchlaFoldOs — A wavefolder in the shape of the classic Buchla 259 wavefolder.
│   ├── ChebyShaper — Chebyshev polynomial waveshaper with 32 controllable harmonics
│   ├── Decimator — Bit crusher and sample rate reducer effect
│   ├── Distort — Multi-algorithm distortion effect with various distortion types
│   ├── Fattener — Adds warmth, distortion and fatness to audio signals
│   ├── HardClip — Hard clipping distortion effect with drive and clip controls
│   ├── HarmonicExciter — Harmonic exciter for enhancing high frequency content
│   ├── MultibandDistortion — Multiband distortion effect with tanh algorithm
│   ├── MultibandHardClip — Multiband hard clipping distortion effect with separate controls for each band
│   ├── MultibandSineShaper — Multiband sine shaper distortion effect
│   ├── NewscoolDistortion — Asymmetric waveshaping distortion using rational function processing
│   ├── ParabolicSaturator — Reaktor parabolic saturator with a ceiling of plus or minus 2
│   ├── ParamShaper — Parametric waveshaping distortion with smooth control
│   ├── Roar — Dynamic distortion effect with feedback and filtering
│   ├── SaturatorPro — Anti aliased tanh saturation with bias for even harmonics auto gain tone and parallel mix
│   ├── SergeFoldOs — A wavefolder in the shape of the classic Serge wavefolder.
│   ├── ShaperOS — Performs waveshaping with slew rate limiting for glitch reduction
│   ├── SigmoidShaper — Sigmoid waveshaping distortion with steepness and inflection controls
│   ├── SignalDestroyer — Chain of extreme distortions applied in series with separate mix controls
│   ├── SineShaper — Sine waveshaper with constant power crossfade
│   ├── SoftSaturator — Soft saturation distortion with smooth control
│   ├── SubharmonicShaper — True subharmonic generator using frequency division and gating
│   ├── Tanh — Hyperbolic tangent distortion effect with smooth saturation
│   ├── TapeSat — Tape machine saturation with emphasis head bump gap loss shared wow and flutter hiss and auto gain
│   ├── TubeSat — Asymmetric tube style saturation with dynamic bias blocking sag anti aliasing and auto gain
│   ├── WaveFold — Wavefolder distortion for creating harmonically rich sounds
│   ├── WaveShaper — Buffer-based waveshaping distortion using transfer functions
│   └── ZeroClipping — Distortion with threshold clipping
├── Dynamics/
│   ├── Compressor — Dynamic range compressor and expander with threshold and slope controls
│   ├── CompressorB — Alternative compressor with ratio-based compression
│   ├── CompressorC — Dynamic range compressor with threshold ratio and makeup gain
│   ├── CompressorPro — Log domain soft knee compressor with lookahead sidechain HPF stereo link and auto makeup
│   ├── Cuntpressor — Frequency aware compressor with peak tracking
│   ├── DCCorrection — DC Correction
│   ├── DynamicCompressor — Dynamic range compressor with adjustable threshold ratio knee attack and release times
│   ├── Gain — Gain control with exponential curve for natural level control
│   ├── Limiter — Hard limiter with DC offset removal
│   ├── LinearGain — Linear gain
│   ├── Maximizer — Dynamic processor inspired by the Ozone 3 Maximizer
│   ├── MultiBandCompressor — Five band compressor with independent controls for each frequency range
│   ├── NoiseGate — Noise gate that silences signals below threshold
│   ├── Normalizer — Normalize signal amplitude with dry/wet mix
│   ├── OTT — OTT-style multiband upward/downward compressor
│   ├── Sanitize — Replaces infinities NaNs and subnormal numbers with 0
│   ├── SaturatingCompressor — Linked compressor with full-width soft knee, automatic makeup and polynomial saturation
│   ├── Sidechain — Sidechain compression using a control signal
│   └── TransientShaper — Transient shaper for controlling attack and sustain portions of sounds
├── Eq/
│   └── EQ5 — 5 Band EQ
├── Filter/
│   ├── AutoWah — Envelope-controlled filter effect
│   ├── BandPassBank1024 — 1024-band bandpass filter bank
│   ├── BandpassFilter — 2 pole bandpass filter
│   ├── ColorComb — Feedback comb with a low-pass in the loop (NI Reaktor Resonator voice)
│   ├── Comb — Comb filter with pitch and decay controls
│   ├── CombFeed — High quality feedforward and feedback comb filter with interpolation and FM input
│   ├── CompensatedSVF — Reaktor Core compensated two pole state variable filter; type HP BP LP
│   ├── DJFilter — Crossfader filter with HPF and LPF
│   ├── Filter4Pole — 4-pole filter with lowpass and highpass modes
│   ├── HPF — Simple high pass filter
│   ├── LadderFilter — Ladder filter with overdrive and multiple filter types
│   ├── LadderMultimode — Reaktor Multi LP 4-Pole filter: HP2 BP2 LP2 BP4 BL4 LP4 HP4 Notch
│   ├── LPF — Simple low pass filter
│   ├── MonoBandPassBank1080 — 1080-band bandpass filter with spectral panning
│   ├── MonoBandPassBank1200_ — 1200-band bandpass filter with spectral panning
│   ├── MonoBandPassBank128 — 128-band bandpass filter with spectral panning
│   ├── MonoBandPassBank128PanX — 128-band bandpass filter with spectral panning
│   ├── MonoBandPassBank21 — 21-band bandpass filter with spectral panning
│   ├── MonoBandPassBank256 — 256-band bandpass filter with spectral panning
│   ├── MonoBandPassBank256PanX — 256-band bandpass filter with spectral panning
│   ├── MonoBandPassBank6 — 6-band bandpass filter with spectral panning
│   ├── MonoBandPassBank64 — 64-band bandpass filter with spectral panning
│   ├── MoogFilter — Moog filter with multiple filter modes and saturation control
│   ├── MoogVCF — Collection of different Moog filter implementations
│   ├── MultimodeFilter — Multimode filter with various filter types
│   ├── Notch — Notch filter with cascaded BRF and Butterworth implementations
│   ├── OnePole — One-pole low-pass or high-pass filter (6 dB per octave)
│   ├── PeakFilter — Parametric EQ filter with frequency Q and gain controls
│   ├── RBPF — Resonant BPF
│   ├── RHPF — Resonant HPF
│   ├── RHPF4Pole — 4-pole HPF
│   ├── RLPF — Resonant LPF
│   ├── RLPF4Pole — 4-pole LPF
│   ├── SatFMFilter — Serial HP LP FM filter with saturation
│   ├── SlinkFilter — Spectral filtering effect with modulated bandpass filterbank
│   ├── VadimFilter — Vadim filter with multiple filter types
│   └── VowelFilter — Vocal formant filter with 6 vowel types
├── Modulation/
│   ├── AmpMod — Amplitude modulator with multiple waveforms and controls
│   ├── RingMod — Ring modulator with frequency and strength control
│   ├── RingmodSidechain — Ring modulation sidechain ducking effect that carves out frequency content at audio rate
│   ├── Tremolo — Tremolo effect with morphable LFO featuring pulse width control
│   └── Vibrato — Vibrato effect with adjustable rate, depth, phase offset, and phase reset
├── Pitch/
│   ├── FreqShifter — Frequency shifter with phase control
│   ├── GrainPitchShifter — Granular pitch shifter with independent grain control
│   ├── OctaveDown — Octave down effect using phase inversion
│   └── PitchShifter — Pitch shifter with grain size and dispersion controls
├── Resonators/
│   ├── BellResonator — Bell resonator effect that applies inharmonic bell-like resonances to input audio
│   ├── BrushedCymbalResonator — Brushed cymbal resonator effect that applies multiple resonant frequencies to input audio
│   ├── CustomResonator256_ — Resonator effect with 256 tunable resonators applied to input audio
│   ├── CustomResonator512_ — Resonator effect with 512 tunable resonators applied to input audio
│   ├── CustomResonator_128 — Resonator effect with 128 tunable resonators applied to input audio
│   ├── CustomResonator_16_ — Resonator effect with 16 tunable resonance ratios applied to input audio
│   ├── CustomResonator_32_ — Resonator effect with 32 tunable resonance ratios applied to input audio
│   ├── CustomResonator_64_ — Resonator effect with 64 tunable resonance ratios applied to input audio
│   ├── CustomResonator_8_ — Resonator effect with 8 tunable resonance ratios applied to input audio
│   ├── CymbalResonator — Cymbal resonator effect that applies multiple resonant filters to input audio
│   ├── GamelanResonator — Gamelan resonator effect that applies gamelan-like resonances to input audio
│   ├── GlassResonator — Glass resonator effect that applies glass-like resonances to input audio
│   ├── GongResonator — Javanese gong resonator effect that applies gong-like resonances to input audio
│   ├── HandpanResonator — Handpan resonator effect that applies handpan-like resonances to input audio
│   ├── KalimbaResonator — Kalimba resonator effect that applies kalimba-like resonances to input audio
│   ├── MarimbaResonator — Marimba resonator effect that applies marimba-like resonances to input audio
│   ├── MembraneResonator — Membrane resonator effect that applies circular membrane resonance to input audio
│   ├── MetalResonator — Metal resonator effect that applies metallic modal resonance to input audio
│   ├── MiRings — Mutable Instruments Rings resonator — all parameters exposed: modal/string/FM models, polyphony, internal exciter, easter egg
│   ├── PlateResonator — Plate resonator effect that applies metal plate resonances to input audio
│   ├── PluckResonator — Physical model of a plucked string excited by external audio input
│   ├── Ringz — Legacy per-channel Ringz resonator
│   ├── SitarResonator — Sitar resonator effect that applies sitar-like resonances to input audio
│   ├── SteelDrumResonator — Steel drum resonator effect that applies steel drum-like resonances to input audio
│   ├── StringResonator — String resonator effect that applies plucked string resonances to input audio
│   ├── TempleBlockResonator — Temple block resonator effect that applies temple block-like resonances to input audio
│   └── WoodBlockResonator — Woodblock resonator effect that applies woodblock-like resonances to input audio
├── Spatial/
│   └── Widening/
│       └── ChannelDecorrelator — Static multichannel allpass decorrelator for channel widening
├── Spectral/
│   ├── AdditiveFreeze — True Sinusoidal Resynthesis (Zero-Grain)
│   ├── FFTEnhancer — Simple spectral enhancer for emphasizing harmonic content
│   ├── FFTVocoder — Phase vocoder cross synthesis between two input signals
│   ├── FFTVocoderClassic — Phase vocoder multiplying magnitudes of both inputs
│   ├── FFTVocoderMax — Phase vocoder taking maximum magnitude from either input with corresponding phase
│   ├── FrequencyBlur — Blurs the frequency content of audio
│   ├── FreqWarp — Spectral morphing effect
│   ├── MonoSpectralDelay512 — 512-band spectral delay with panning and mapped decay times
│   ├── PitchFilter — Filter that passes frequencies at specified pitches array
│   ├── PV_BinShift — Spectral bin shifter with stretch and shift controls
│   ├── PV_Brickwall — Spectral brick wall filter with adjustable cutoff threshold
│   ├── PV_Freeze — Spectral freeze with selectable Loop or Blur modes
│   ├── PV_HarmonicFilter — Spectral filter that passes only frequencies at specified pitches
│   ├── PV_Magabove — Spectral filter that passes frequencies with magnitudes above threshold
│   ├── PV_Morph — Spectral morphing between two input signals
│   ├── PV_RectComb — Spectral comb filter with adjustable teeth width and phase
│   ├── SpectralDelay512 — 512-band spectral delay with multichannel support
│   └── SpectralMorph — Phase vocoder morphing between two spectral sources
└── Time/
    ├── Delay/
    │   ├── Blur — Real-time multichannel allpass diffusion smear
    │   ├── Delay — Simple delay effect with variable delay time
    │   ├── DelayFeed — Feedback delay with pitch shifting and modulation
    │   ├── Echo — Echo effect with feedback and filtering options
    │   ├── EchoBlur — Feedback delay whose repeats become progressively diffused
    │   ├── EchoHold — Mutes input signal after specified delay time when gate is triggered. Immediately unmutes when gate is off.
    │   ├── EchoSpat — Spatializing echo with customizable patterns
    │   ├── EchoSpatExpand — Spatializing echo with variable speaker output
    │   ├── FFTDelay — FFT-based delay with spectral processing
    │   ├── FilteredFeedbackDelay — Four point Core microdelay with sample feedback SVF and parabolic crossfade
    │   ├── GrainCloudDelay — Granular cloud delay effect with variable grain parameters
    │   ├── GrainDelay — Granular delay effect with variable density and grain duration
    │   ├── GrainFeedDelay — Granular cloud delay with feedback filtering and grain control
    │   ├── MultiTapEcho — 4-tap delay with independent per-tap feedback, pan (PanAz), serial HP/LP filter and dry/wet mix
    │   ├── Overdub — Infinite feedback delay with reset control
    │   ├── PingPongEcho — Ping-pong echo effect that alternates between left and right channels
    │   ├── PitchDelay — Delay effect with delay time based on musical pitch
    │   ├── ReverseDelay — Reverse delay: plays back chunks of the input reversed. Chunks last Delayms or follow the Sync triggers and play with probability Chance
    │   ├── TapeDelay — Tape-style delay with shelving filters and feedback
    │   └── TriggeredGrainDelay — Triggered granular delay with complete grain playback
    ├── Freeze/
    │   ├── Freeze — Spectral freeze effect with smooth attack and release transitions
    │   ├── Freezish — Enhanced spectral freeze effect with nuanced control over attack and decay
    │   └── GrainFreeze — Granular freeze effect with position pitch control and smooth crossfading
    ├── Looping/
    │   ├── BeatRepeat — Beat repeater effect with accumulative pitch shifting
    │   ├── CircularBuffer — Circular buffer with one reader per channel
    │   ├── CircularBufferFFT — Circular FFT buffer; numreaders controls coherent-copy gain, not independent readers (use CircularBufferFFT_B for those)
    │   ├── CircularBufferFFT_B — Circular buffer with FFT processing for each channel
    │   ├── Looper — Audio looper with variable speed and crossfade
    │   ├── Stutter — Beat repeater effect with variable pitch control
    │   └── StutterPoly — Beat repeater effect with variable pitch control
    ├── Phase/
    │   ├── Chorus — Classic chorus effect with modulated delay times
    │   ├── ChorusB — Advanced chorus effect with feedback and multiple taps
    │   ├── Flanger — Flanger effect with feedback delay line
    │   ├── FlangerLFO — Flanger effect with variable LFO shape
    │   ├── FlangerPitch — Flanger effect with pitch-controlled delay time
    │   ├── FlangerSync — Synchronized flanger effect with dual LFOs
    │   ├── Phaser — Phaser effect with allpass filter
    │   ├── PhaserB — Advanced phaser effect with analog modeling
    │   └── PhaserC — Phaser effect with FOS filters and feedback
    └── Reverb/
        ├── FDNReverb — Jot FDN reverb — 16 delay lines, Hadamard mixing, RT60 decay, frequency-dependent absorption
        ├── FreeVerb — Simple reverb based on the Freeverb algorithm
        ├── GatedReverb — Gated reverb effect inspired by Phil Collins
        ├── GranularReverb — Granular reverb effect with independent multi-channel processing
        ├── GreyholeReverb — Reverb based on the Greyhole algorithm with pitch shifting
        ├── HoldReverb — Ambient reverb with infinite hold and clear gate
        ├── JPReverb — High quality reverb based on the JP algorithm
        ├── LongReverb — Long reverb with 32 allpass filters for dense reverberation
        ├── MicroSpace — NI Micro Space reverb rebuilt from its Reaktor Core cell
        ├── PlateReverb — Dattorro plate reverb — two cross-coupled tanks with 7-tap stereo output and early reflections
        ├── ReflectorReverb — Early reflections reverb using prime-spaced delays with stereo panning
        ├── RichReverb — Classic spatial reverb with improved diffusion
        ├── SchroederReverb — Schroeder feedback network reverb algorithm with diffusion control
        ├── ShimmerReverb — Harmonic shimmer reverb with pitch shifting
        ├── ShimmerReverb_Feed — Shimmer reverb with feedback path for infinite ascending shimmer
        ├── SimpleReverb — Legacy simple comb/allpass multichannel reverb
        ├── SpaceMaster — R66-style pairwise multichannel diffusion reverb
        └── TankReverb — Tank reverb with 4 stage diffusion network and independent tank controls
```

</details>

<details>
<summary><b>InOut</b> (1)</summary>

```
InOut/
└── Input — Captures audio from system inputs
```

</details>

<details>
<summary><b>Mixing</b> (4)</summary>

```
Mixing/
├── AudioMerge — Sum audio inputs channel by channel at unity gain
├── CrossfadeAB — Crossfade between two inputs
├── Mixer — Two input mixer with per channel level controls and master level
└── PolyMixer — Mixer with 8 inputs with level controls solo mute and balance
```

</details>

<details>
<summary><b>Modulation</b> (40)</summary>

```
Modulation/
├── Envelope/
│   ├── Envelope — Envelope generator with multiple envelope types
│   ├── EventADSR — Event-retriggered ADSR; linear attack and exponential time constants in milliseconds
│   ├── MorphEnv — Flexible morphing envelope generator
│   └── SigmoidEnv — Flexible two segment sigmoid envelope generator
├── LFO/
│   ├── LFO — Low frequency oscillator with multiple waveforms and effects
│   ├── MorphLFO — Morphable LFO with bipow shaping, pulse width control, skew, and quantization
│   └── OceanodeLFO — LFO matching C++ baseOscillator: roundness (tri/cos/square), center-anchored pw, skew [-1,1], customPow
├── Ramps/
│   ├── Phasor — Phasor
│   ├── PhasorHz — Phasor in Hertz
│   ├── PhasorPitch — Phasor in Pitch
│   ├── Ramp — Ramp
│   └── transportPhasor — BeatTransport-synced phasor with FM and PLL
├── Random/
│   ├── Chance — Randomly passes or blocks triggers based on chance probability with seed control
│   ├── Choose — Weighted random selection with immediate reseed output
│   ├── CongruentialRandom — Bipolar 32-bit linear congruential random; triggered, sample-rate, or continuously seeded
│   └── RandomValues — Random number generator with synchronized seeds and bias controls
├── Sequencer/
│   ├── ChannelSequencer — Routes multiple value sequences across channels
│   ├── DurationSequencer — Sequential trigger with individual rhythm per channel
│   ├── GateQuantizer — Quantizes gate starts to a rhythmic grid, preserving amplitude/velocity.
│   ├── ImpulseQuantizer — Quantizes impulses using robust Pulse Counter logic (No Feedback).
│   ├── indexSequencer — Index-based sequencer - selects values from array based on index input
│   ├── Transport — Global transport with audio rate timing - Outputs PPQ counter beats bars sixteenths ticks milliseconds and beat weight
│   ├── ValueQuantizer — Samples and holds input values at a rhythmic grid rate (Robust Timing).
│   ├── VectorSequencer — Vector step sequencer with trigger and value outputs
│   ├── XORSequencer — XOR shift register sequencer outputting pitch or gate values as audio signals
│   └── XORShiftRegister — True XOR shift register sequencer using LocalBuf feedback
├── Trigger/
│   ├── DurationSequential — Sequential trigger with individual duration per channel
│   ├── GateDuration — Gate sustainer: converts impulses to sustained gates with S&H or immediate duration response
│   ├── ImpulseCounter — Counts impulses with optional modulo functionality
│   ├── Metro — Metro trigger generator with synchronized reset and reseed for consistent timing
│   ├── MetroChoose — Metro picking weighted random beat durations: a new one every pulse (with per-value repeats) or one per trigger, looped until the next
│   ├── MetroSequential — Sequential or random metro with consistent per-channel random values
│   ├── MetroSequentialIndex — Metro with Vector Index input (sequencable channel order)
│   ├── MetroSequentialInput — Sequential / random distribution of incoming impulses
│   ├── MetroSequentialPh — Phasor-driven sequential trigger: n impulses per cycle, channel order set by index vector
│   ├── PhasorCycleTrigger — Outputs trigger when input phasor completes a cycle (falling edge)
│   ├── RandomBeatClock — Beat clock with random or fixed integer beat intervals
│   ├── RandomMetro — Triggers at random intervals between minms and minms plus range
│   └── TransportPulse — Musical pulses locked to host Beat Transport; interpolates between position anchors
└── Utility/
    └── SampleAndHold — Sample and Hold with edge/gate modes and last-value output
```

</details>

<details>
<summary><b>OceanodeCode</b> (32)</summary>

```
OceanodeCode/
├── classes/
│   └── OceanodeCodeSandbox.sc
├── examples/
│   ├── bass/
│   │   ├── acid.scd
│   │   ├── fm_bass.scd
│   │   └── reese.scd
│   ├── fx/
│   │   ├── bitcrusher.scd
│   │   ├── pingpong_delay.scd
│   │   ├── resonator.scd
│   │   ├── reverb.scd
│   │   ├── ring_mod.scd
│   │   ├── spectral_freeze.scd
│   │   └── tape_delay.scd
│   ├── keys/
│   │   ├── fm_bell.scd
│   │   ├── fm_epiano.scd
│   │   ├── glass_marimba.scd
│   │   ├── organ.scd
│   │   └── pluck_ks.scd
│   ├── pads/
│   │   ├── additive_shimmer.scd
│   │   ├── choir.scd
│   │   ├── saw_pad.scd
│   │   └── supersaw.scd
│   ├── percussion/
│   │   ├── clap.scd
│   │   ├── fm_perc.scd
│   │   ├── hihat.scd
│   │   ├── kick808.scd
│   │   ├── membrane.scd
│   │   └── snare.scd
│   └── textures/
│       ├── chaos_drone.scd
│       ├── crackle.scd
│       ├── drone_play.scd
│       ├── grain_cloud.scd
│       └── wind.scd
├── captureCode.scd
└── promote.scd
```

</details>

<details>
<summary><b>Optimized</b> (3)</summary>

```
Optimized/
└── Bloobs/
    ├── BloobsBandpass — BLOOBS cascaded 78-voice bandpass
    ├── BloobsNoise — BLOOBS four-choice noise source
    └── BloobsStereoPan — BLOOBS signed stereo pan mixer
```

</details>

<details>
<summary><b>Routing</b> (15)</summary>

```
Routing/
├── 4x4Matrix — 4x4 matrix router with interpolation
├── ChannelConcatenator — Concatenates 8 audio inputs into sequential channel array
├── ChannelDuplicator — Duplicates input channels according to index mapping
├── ChannelExtractor — Extracts one zero-based channel from a multichannel input as mono
├── ChannelIndexInvert — Inverts channel order
├── ChannelMapper — Routes input channels to output channels with configurable mapping
├── ChannelRepeat — Repeat each input channel across multiple output channels
├── ChannelRouter — Routes each input channel to specific output channels n defines max output channels per-channel routing accepts int vectors
├── ChannelSolo — Solos a selected channel with options to fill all outputs or bypass
├── Crossover — Frequency crossover with polarity phase alignment and gain per band
├── MultibandSeparator — Separates audio into low mid and high frequency bands
├── NtoM — Channel count converter with interpolation
├── PhaseInvert — Inverts the phase of the input signal
├── ShiftChannels — Continuously shift audio channels using interpolation
└── SwitchAB — Switch between two inputs (A or B)
```

</details>

<details>
<summary><b>Source</b> (151)</summary>

```
Source/
├── Additive/
│   ├── Additive1080_ — Additive synthesis with 1080 sinusoidal oscillators
│   ├── Additive1200_ — Additive synthesis with 1200 sinusoidal oscillators
│   ├── Additive128_ — Additive synthesis with 128 sinusoidal oscillators
│   ├── Additive256_ — Additive synthesis with 256 sinusoidal oscillators
│   ├── Additive32_ — Additive synthesis with 32 sinusoidal oscillators
│   ├── Additive512_ — Additive synthesis with 512 sinusoidal oscillators
│   ├── Additive64_ — Additive synthesis with 64 sinusoidal oscillators
│   ├── DynKlang — Legacy 16-partial per-channel sine bank
│   └── FormsLegacy — Legacy multichannel FORMS oscillator with Hz plus pitch arrays
├── Analog/
│   ├── Analog — Analog synth with different wave types
│   ├── Blit — Band limited oscillators with different wave shapes
│   ├── CheapImpulse — Cheap Impulse
│   ├── CheapPulse — Cheap Pulse
│   ├── CheapSaw — Cheap Saw
│   ├── CheapSin — Cheap Sin
│   ├── CheapTri — Cheap Tri
│   ├── MorphOSC — Morphable bipolar oscillator with FM input, bipow shaping, pulse width control, shape morphing, skew, quantization, phase offset, and reset
│   ├── SimpleImpulse — Simple Impulse oscillator with frequency and amplitude control
│   ├── SimplePulse — Simple pulse oscillator with frequency and amplitude control
│   ├── SimpleSaw — Sawtooth oscillator with phase reset for hard sync effects
│   ├── SimpleSine — Simple sine oscillator with frequency and amplitude control
│   ├── SinReset — Sinusoidal oscillator with phase control and reset
│   ├── Squine — Squine oscillator with optional hardsync
│   ├── SuperPulse — Rich detuned superpulse with pulse width vibrato and tremolo modulation
│   └── SuperSaw — Rich detuned supersaw with vibrato and tremolo modulation
├── Chaotic/
│   └── Chaotic — Chaotic oscillators including Lorenz Rossler Chua and others
├── Feedback/
│   ├── DualFeedback — Dual feedback oscillator with frequency modulation and filtering
│   ├── FbDrone — Feedback drone with pitch shifting and filtering
│   ├── FbDroneMod — Feedback drone with modulated parameters
│   ├── Feedback — Feedback network with delay and pitch shifting
│   └── FeedbackOsc — Oscillator with phase feedback and multiple wave types
├── FM/
│   ├── FM2op — Basic FM synth with multiple waveforms
│   ├── FMFeedback — Complex 4-operator FM synthesizer with feedback
│   ├── FMGrain — Basic granular synth with frequency modulated sine tones
│   ├── Kontour — Complex phase modulation oscillator with feedback
│   └── PMod — Phase modulation oscillator with carrier and modulator
├── Formants/
│   ├── FMFormant — FM-based formant synthesizer with vowel control
│   └── Formants — Formant synthesizer with vowel control
├── Noise/
│   ├── Click — Click generator triggered by input signal
│   ├── crax — Crackle oscillator rebuilt from decoded Reaktor Core Cells, without delay or envelopes
│   ├── Geiger — Impulse generator with progressive timing disorder
│   └── Noise — Noise generator with different noise types
├── Oversampling/
│   ├── PMOs — Oversampled Phase Modulation oscillator
│   ├── SawOS — Oversampled Sawtooth oscillator with phase control
│   ├── SinOs — Oversampled Sinusoidal oscillator with phase control
│   ├── SquareOS — Oversampled Square oscillator with phase control
│   └── TriOs — Oversampled Triangle oscillator with phase control
├── Percussion/
│   ├── 808/
│   │   ├── Clap808 — 808 style clap sound with attack and decay control
│   │   ├── Claves808 — 808 style claves with sine oscillator
│   │   ├── Cowbell808 — 808 style cowbell with dual pulse oscillators
│   │   ├── Cymbal808 — 808 style cymbal with multiple resonant components
│   │   ├── Hat808 — 808 style closed hihat with pulse oscillator bank
│   │   ├── Kick808 — 808 style kick drum with tone control
│   │   ├── Maracas808 — 808 style maracas with filtered noise
│   │   ├── OpenHihat808 — 808 style open hihat with pulse oscillator bank
│   │   ├── Rimshot808 — 808 style rimshot with triangle pulse and noise components
│   │   ├── Snare808 — 808 style snare with tone control
│   │   └── Tom808 — 808 style tom with continuous tone control from low to high
│   ├── AbstractDrum — Physical modeling percussion with modal synthesis
│   ├── Clap — Clap sound with separate pop and tail components
│   ├── Crash — Noise-based cymbal synthesizer without resonators
│   ├── FMDrum — FM percussion with latched amplitude control
│   ├── FMPerc — Complex FM percussion with feedback network
│   ├── FMPercRnd — Stochastic percussive FM synth — manual or auto-random timbre per hit
│   ├── Hihat — Hihat with filtered noise and adjustable resonance
│   ├── KickBestia — Kick drum with triangle oscillator and harmonic control
│   ├── KickDrum — Kick drum synthesizer with envelope controlled frequency and phase modulation
│   ├── Kickdrumb — Kick drum with triangle oscillator and additive pitch envelope
│   ├── Kickdrumc — Kick drum with complex frequency modulation and click transient
│   ├── RealisticSnare — Realistic snare with transient noise and resonance components
│   └── Snare — Snare drum with pop transient and filtered noise
├── PhysicalModelling/
│   ├── Bells — Legacy Bells13 modal bell bank
│   ├── Bow — Physical model of a bowed string instrument
│   ├── BowedCrystal — Bowed crystal physical model with vibrato and shimmer
│   ├── Brass — Physical model of a brass instrument
│   ├── BrushedCymbal — Brushed cymbal with resonant frequencies and shimmer control
│   ├── Celesta — Realistic celesta using modal synthesis
│   ├── CristalBaschet — Physical model of the Cristal Baschet instrument
│   ├── Cymbal — Physical model of a cymbal with controllable resonators
│   ├── CymbalB — Dispersive physical-model cymbal with feedback diffusion and comb resonator bank
│   ├── CymbalPad — Metallic pad synthesizer based on cymbal resonance
│   ├── DWGClarinet — Physical model of a clarinet instrument
│   ├── DWGFlute — Physical model of a flute instrument
│   ├── Flute — Physical modeling flute synthesizer
│   ├── FluteB — Physical modeling flute synthesizer
│   ├── FrameDrum — Physical model of a frame drum with detailed modal synthesis
│   ├── Gamelan — Gamelan percussion instrument model with adjustable parameters
│   ├── GlassPercussion — Glass percussion physical model with strike position and damping
│   ├── GongAgeng — Javanese large gong model with shimmer and adjustable resonance
│   ├── Handpan — Physical model of a handpan with adjustable tone and resonance characteristics
│   ├── JunkPerc — Complex percussive sounds with tunable randomness and transient behavior
│   ├── Kalimba — Kalimba physical model with adjustable tonal characteristics
│   ├── KarplusPluck — Plucked string physical model with adjustable physics parameters
│   ├── Mallet — Resonant mallet percussion with trigger gate drone and input modes
│   ├── Marimba — Modal synthesis marimba with triggering drone and input modes
│   ├── MDAPiano — MDA Piano model with adjustable touch response and tuning
│   ├── Membrane — Circular membrane physical model with internal or external excitation
│   ├── MetalPercussion — Metal percussion with harmonicity inharmonicity and decay control
│   ├── MetalPlate — Metal plate simulation with adjustable position and decay characteristics
│   ├── OteyPiano — Physically modeled piano with adjustable string and hammer parameters
│   ├── Pluck — Physical model of a plucked string instrument
│   ├── PrayerBell — Tibetan prayer bell model with inharmonic resonances
│   ├── Rhodes — FM-based Fender Rhodes electric piano emulation
│   ├── Sitar — Digital waveguide sitar model with dual string planes and gourd resonance
│   ├── SteelDrum — Physical model of a steel drum with resonant modes
│   ├── StoneModel — Physical model of stone percussion and friction from polished to chaotic
│   ├── Tabla — Realistic tabla physical modeling synthesizer
│   ├── TempleBlock — Physical model of a temple block with inharmonic resonances
│   ├── WoodBlock — Physical model of a woodblock with adjustable material properties
│   └── Woodwind — Physical model of a woodwind instrument with breath and reed controls
├── Pulsar/
│   ├── FMPulsar — FM-based pulsar synthesis with grain control
│   ├── Pulsar — Basic pulsar synthesis oscillator
│   ├── Pulsar_B — Advanced pulsar synthesis with LFO modulation
│   ├── PulseTrain — Multi-waveform pulsar train synthesis with envelope control
│   └── Wavelet — Enhanced wavelet generator with morphable shape (triangle/sine/square), skew for asymmetry, power shaping, inversion, quantization, and amplitude range control
├── Radio/
│   └── RadioStation — VLC internet radio — output connects to any effect chain
├── Sampler/
│   ├── BasicSampler — Sampler with switchable Polyphony. If Poly is off, it behaves monophonically.
│   ├── BreakSampler — Sample player for rhythm breaks with automatic duration adjustment
│   ├── CrossLooper — Buffer looper with crossfade and variable duration
│   ├── FFTStretch — FFT stretch with correct trigger indices; BufIFFT2@2× (Hann) / BufIFFT@4× (Sine)
│   ├── FFTStretchRate — Legacy FFT stretch player with direct per-channel rate control
│   ├── GrainSampler — Basic granular sampler with trigger control
│   ├── OneShotSampler — One-shot sampler with ADSR envelope and latching
│   ├── PaulStretch — Extreme time stretching using the PaulStretch algorithm
│   ├── PhasorSampler — Buffer reader driven by external 0..1 phasor. One channel per voice.
│   ├── SamplerJump — Click-free sampler: audio-rate cross-fade on offset jumps
│   ├── SamplerStretch — Time-stretching sampler with granular capabilities
│   ├── SamplerSync — Sampler with frequency synchronization capabilities
│   └── Vectory — Breakbeat sequencer with pitch shifting and filtering per step
├── Special/
│   ├── AliasingSynth — Aliasing synthesizer based on buffer read/write with modulated phasors
│   ├── BubbleA — Simple bubble sound with rising pitch envelope
│   ├── BubbleB — Complex bubble sound using TwoTube physical model
│   ├── BuzzBass — Buzzy bass synth with noise and distortion
│   ├── Frumbo — Complex synth with FM pitch and filter envelopes and Squine oscillator
│   ├── GutterSynth — Complex physical modeling synth with multiple resonators and distortion methods
│   ├── IkedaBass — Ikeda-style bass synth using phase modulation technique
│   ├── InterferenceRhythm — N independent sinusoids creating interference rhythm patterns
│   ├── Lissajous — Lissajous oscillator with multiple waveforms and waveshaping
│   ├── Recursive — Selfreferential feedback network of oscillators creating complex timbres
│   ├── SigmoidOSC — Flexible two segment sigmoid oscillator
│   └── TB303 — TB-303 bass synthesizer emulation with acid filter envelope
├── Stochastic/
│   ├── Gendy — GENDY1 stochastic synthesizer with dynamic waveform generation
│   └── Perlin — Perlin noise oscillator with 3D movement through noise space and click-free looping
├── Sync/
│   ├── HardSync — Hard sync oscillator with sawtooth slave
│   ├── HardSyncBLPulse — Band-limited hard sync pulse oscillator with pulse width control
│   ├── HardSyncBLSaw — Band-limited hard sync sawtooth oscillator
│   ├── HardSyncPulse — Hard sync oscillator with pulse waves and filtering
│   └── SyncSaw — Sync sawtooth oscillator with base and sync frequency control
├── Vector/
│   ├── VectorSynth — Vector synthesis oscillator with phase modulation
│   └── VectorSynthB — Advanced vector synthesis with formants and antialiasing
└── Wavetable/
    └── WTSynth — Advanced wavetable oscillator with 8 buffer interpolation
```

</details>

<details>
<summary><b>Spatial</b> (18)</summary>

```
Spatial/
├── MidSide/
│   └── MidSide — Separates multichannel signal into mid and side components
├── Panning/
│   ├── 360Panner — Multi-source mono-input 360-degree panner with stereo psychoacoustic and multichannel circular modes
│   ├── Balance — Multichannel balance with equal power
│   ├── Campaneitor — Legacy Campaneitor channel folding and PanX spatializer
│   ├── DBAP — Distance-based amplitude panning
│   ├── MultibandPanaz — Multiband spectral panner with bands equal to channels
│   ├── PanAz
│   ├── PanAzDoppler — Spatial panner with doppler effect
│   ├── PanAzDopplerComplex — Advanced spatial panner with doppler and reflections
│   ├── Panner
│   ├── spectralPanner32 — 32-band per-input-channel spectral panner with shared per-band pan controls
│   ├── StereoPan2 — Signed stereo panner for a mono source
│   ├── XPanAz — Cross panning with front and back distribution
│   ├── XPanAzLegacy — Legacy XPanAz 78-to-speaker mirror panner
│   └── XYPanner — XY multi-source panner with distance, Doppler, and psychoacoustic stereo/multichannel cues
└── Perceptual/
    ├── BinauralHaas — Mono to stereo binaural processor with Haas effect. It understands each input channel as a mono input and outputs all processed mono inputs as a stereo output
    ├── BinauralHRTF — Structural HRTF binaural renderer for n virtual sources or speakers with head shadow ITD pinna torso distance and room
    └── Leslie — Rotating speaker cabinet effect with modulated delays and panning
```

</details>

<details>
<summary><b>Utilities</b> (16)</summary>

```
Utilities/
├── Analysis/
│   ├── EnvelopeFollower — Generates control signal from input amplitude
│   ├── SampleRate — Outputs the current sample rate as an audio signal
│   ├── SpectralAnalyzer — Divides the spectrum into 100 frequency bands
│   └── TransientDetect — Detects rising edges/transients in audio and outputs impulses
├── Conversion/
│   ├── Conversions — Utility for common audio and music conversions including bar divisions
│   └── Sig — Converts numeric parameter value to audio signal
├── Feedback/
│   ├── FeedbackNode — Explicit block-delayed bridge for modular feedback loops between SynthDefs
│   └── InFeedback — Feedback unitari: suma l'entrada amb InFeedback.ar controlat per gain
├── Math/
│   ├── Arithmetic — Performs arithmetic operations between two audio signals
│   ├── ChannelStatistics — Global operations across all input channels
│   ├── ClipMinMax — Clips input signal between min and max values
│   ├── LogicOps — Applies logic operations to audio signals
│   ├── Mapper — Maps input values from one range to another with optional integer quantization
│   ├── Quantizer — Quantizes input signal to nearest values in qlist (all channels use same qlist)
│   ├── Trigonometry — Apply various trigonometric and mathematical operations to audio signals
│   └── UnaryOps — Applies unary operations to audio signals
├── Other/
└── Voicing/
```

</details>

<details>
<summary><b>Compatibility</b> (170)</summary>

```
Compatibility/
├── OceanAudio/
│   ├── AdditiveComb — Original stereo additive comb synthesizer
│   ├── DynKlank — Sixteen-resonator bank per input channel
│   ├── FeedbackBCompat — Original eight-tap stereo feedback effect
│   ├── Flute2Compat — Original hybrid physical flute
│   ├── OteyPianoCompat — Physically modeled piano with adjustable string and hammer parameters
│   └── RhythmicMute — Per-channel rhythmic mute
└── Old/
    ├── 808.scd
    ├── abstractDrum.scd
    ├── additive.scd
    ├── additivecomb.scd
    ├── advancedreverb.scd
    ├── aliasingSynth.scd
    ├── ampmod.scd
    ├── asymparsat.scd
    ├── audioinput.scd
    ├── bandpass78.scd
    ├── baschet.scd
    ├── beatrepeat.scd
    ├── beatslicer.scd
    ├── beattrack.scd
    ├── blit.scd
    ├── bowpluck.scd
    ├── brass.scd
    ├── brushedcymbal.scd
    ├── bubble.scd
    ├── buzzbass.scd
    ├── channelshift.scd
    ├── chorus.scd
    ├── circularbuffer.scd
    ├── click.scd
    ├── combfilter.scd
    ├── compressor.scd
    ├── convolution.scd
    ├── crossfadeloopsampler.scd
    ├── cymbal.scd
    ├── cymbalpad.scd
    ├── dbap.scd
    ├── dccorrection.scd
    ├── decimator.scd
    ├── delay.scd
    ├── distort.scd
    ├── doppler.scd
    ├── dwgflute.scd
    ├── dynklank.scd
    ├── echo.scd
    ├── envelope.scd
    ├── eq.scd
    ├── fbdrone.scd
    ├── feedback.scd
    ├── feedosc.scd
    ├── fft.scd
    ├── fftdelay.scd
    ├── fftstretch.scd
    ├── filter.scd
    ├── filter4pole.scd
    ├── flanger.scd
    ├── flute.scd
    ├── fm1op.scd
    ├── FM2op.scd
    ├── fm4op.scd
    ├── fmcomplex.scd
    ├── fmformants.scd
    ├── fmperc.scd
    ├── fmpulsar.scd
    ├── formants.scd
    ├── forms.scd
    ├── frameDrum.scd
    ├── freeverb.scd
    ├── freeze.scd
    ├── freqshifter.scd
    ├── frumbo.scd
    ├── gain.scd
    ├── gamelan.scd
    ├── gong.scd
    ├── grain.scd
    ├── grainclouddelay_ok.scd
    ├── graindelay.scd
    ├── grainfreeze.scd
    ├── grainsampler.scd
    ├── granular_reverb.scd
    ├── guttersynth.scd
    ├── handpan.scd
    ├── hardclip.scd
    ├── hardsync.scd
    ├── holdreverb.scd
    ├── ikedabass.scd
    ├── junkperc.scd
    ├── kalimba.scd
    ├── karplus.scd
    ├── kickbestia.scd
    ├── kickdrum.scd
    ├── kickdrumb.scd
    ├── kickdrumc.scd
    ├── lfo.scd
    ├── limiter.scd
    ├── lissajous.scd
    ├── looper.scd
    ├── mallet.scd
    ├── marimba.scd
    ├── materials_modal.scd
    ├── mdapiano.scd
    ├── membrane.scd
    ├── metal.scd
    ├── metalPlate.scd
    ├── mixer.scd
    ├── monoize.scd
    ├── monoto.scd
    ├── moogfilter.scd
    ├── moogvcf.scd
    ├── morphosc.scd
    ├── multar.scd
    ├── multibanddistortion13.scd
    ├── noise.scd
    ├── noisegate.scd
    ├── notch.scd
    ├── paulstretch.scd
    ├── peakfilter.scd
    ├── percmembrane.scd
    ├── phaseinvert.scd
    ├── phasemod.scd
    ├── phaser.scd
    ├── phasor.scd
    ├── piano.scd
    ├── pitchdelay13.scd
    ├── pitchshift.scd
    ├── pluck.scd
    ├── polymixer.scd
    ├── polymute.scd
    ├── prayerbell.scd
    ├── pulsar.scd
    ├── pulsar2.scd
    ├── pulsar3.scd
    ├── rave.scd
    ├── realisticsnare.scd
    ├── recorder.scd
    ├── recursion.scd
    ├── resonator.scd
    ├── reverb.scd
    ├── ringmod.scd
    ├── rotatetranslate.scd
    ├── sampler.scd
    ├── satfmfilter.scd
    ├── shapers.scd
    ├── sidechain.scd
    ├── sinefeed.scd
    ├── sineshaper.scd
    ├── sitar.scd
    ├── slicer.scd
    ├── softsaturator.scd
    ├── solochannel.scd
    ├── spectralmorph.scd
    ├── steelDrum.scd
    ├── stereomix.scd
    ├── stereopan13.scd
    ├── stutter.scd
    ├── syncsaw.scd
    ├── tankreverb.scd
    ├── tapedelay.scd
    ├── tb303.scd
    ├── transientshaper.scd
    ├── tremolo.scd
    ├── vadimfilter.scd
    ├── vectorsynth.scd
    ├── vibrato.scd
    ├── vocoder.scd
    ├── wavefold.scd
    ├── waveform.scd
    ├── woodblocks.scd
    ├── woodwind.scd
    └── wtsynth.scd
```

</details>

<details>
<summary><b>Defaults</b> (58)</summary>

```
Defaults/
├── AdditiveBox/
│   └── additiveBox.scd
├── AliasingBox/
│   └── aliasingBox.scd
├── AnalogBox/
│   └── analogBox.scd
├── Analysis/
│   ├── a2k.scd
│   ├── beattracker.scd
│   ├── bufferscope.scd
│   ├── fftanalyzer.scd
│   ├── fftanalyzerHD_synthdefs.scd
│   ├── foldedWaveform.scd
│   ├── pitchtracker.scd
│   ├── vumeter.scd
│   ├── wavescope2.scd
│   └── wavescope3.scd
├── ArpeggiatorBox/
│   └── arpeggiatorBox.scd
├── AudioRouting/
├── BeatRepeatPro/
│   └── BeatRepeatPro.scd
├── BufferBrowser/
│   └── bufferBrowserPreview.scd
├── ChannelRouterMatrix/
│   └── channelRouterMatrix.scd
├── ChordBox/
│   └── chordBox.scd
├── Convolution/
│   └── scconvolution.scd
├── Core/
│   ├── bufalloc.scd
│   ├── info.scd
│   └── output.scd
├── DataBox/
│   └── databox.scd
├── DynGen/
│   └── dyngen.scd
├── EchoBox/
│   └── echoBox.scd
├── EuclideanBox/
│   └── euclideanBox.scd
├── FM7/
│   ├── SuperFM7 — 6-Operator FM with Per-Voice ModScale & Trem/Vib
│   └── SuperFMDrone — 6-Operator FM Drone - per-voice op amps/ratios/detunes. No gate/envelope.
├── FMBox/
│   ├── upstream/
│   └── fmBox.scd
├── GrainBox/
│   └── scGrainBoxPoly.scd
├── GraphicEQ/
│   └── graphiceq.scd
├── MacroMixer/
│   └── macromixer.scd
├── MetroBox/
│   └── metroBox.scd
├── MixerPro/
│   └── mixerpro.scd
├── Modulation/
│   ├── modulation.scd
│   ├── modulationRandom.scd
│   └── pulseGate.scd
├── PhysicalBox/
│   └── physicalBox.scd
├── PolyArpeggiator/
│   └── scpolyarpeggiator.scd
├── PolyComb/
│   ├── polyCombExtras.scd
│   ├── polyCombOptions.scd
│   └── polyCombParts.scd
├── PolyMixer/
│   └── polymixer.scd
├── PulsarBox/
│   └── pulsarbox.scd
├── Rave/
│   └── rave.scd
├── Recorder/
│   ├── multirecbuf.scd
│   ├── recbuf.scd
│   └── recbuffhold.scd
├── RhythmBox/
│   ├── scRhythmBoxTracks.scd
│   └── slicePreview.scd
├── SampleBox/
│   └── samplebox.scd
├── SlicerBox/
│   └── slicerBox.scd
├── SpectralFilterBox/
│   └── spectralFilterBox.scd
├── SteamPipeBox/
│   └── steampipeBox.scd
├── VelvetReverb/
│   └── scvelvetreverb.scd
├── VST/
│   ├── vst.scd
│   └── VSTPluginController.sc
├── Wavetable/
│   └── wavetable.scd
└── WaveTrack/
    └── waveTrackPlayer.scd
```

</details>

<details>
<summary><b>WIP</b> (8)</summary>

```
WIP/
├── Core/
│   └── outputVariants.scd
├── CustomBuffer/
│   └── custombufferplayer.scd
├── PhysicalBox/
│   ├── physicalbox.scd
│   ├── physicalbox_core.scd
│   └── reference.scd
├── RawData/
│   └── rawDataAudio.scd
└── Voice/
    ├── DECtalkKlatt.scd
    └── VotraxSC01A.scd
```

</details>

<!-- synthdef-tree:end -->
