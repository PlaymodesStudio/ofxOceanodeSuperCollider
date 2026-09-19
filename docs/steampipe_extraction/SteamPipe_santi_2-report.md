# Reaktor patch extraction report

- Source: `/Users/santiagovilanova/Documents/ENSEMBLES_REAKTOR/SteamPipe_santi_2.ens`
- Modules: 950
- Connections: 991 / 991 resolved
- Snapshots: 67 (4136 parameter records)

## Module types used

| Kind | Name | Instances | DSP implementation | Description |
|---:|---|---:|---|---|
| 4 | Macro | 90 |  | This is a special type of macro module that allows to select one of the internal macros to be visible in its area. If the Instrument Panel is unlocked, the indices of the available macros can be read from the panel context menu. A Panel Index module has to be placed inside to select the visible macro by a signal or a control. |
| 5 | In | 212 |  |  |
| 6 | Out | 119 |  |  |
| 9 | Instrument | 1 |  |  |
| 20 | Fader | 51 |  |  |
| 22 | Button | 2 |  | Monophonic Single-Trigger gate source. The status of the output signal is switched by MIDI Note On and Note Off events. While the gate is on, more Note On's will not cause retriggering. The amplitude is controlled by the On velocity. |
| 24 | Switch | 4 |  | This module establishes a switchable connection between other modules. Multiple signals can be connected to the Switch Module's input ports, but which of these signals it forwards, is chosen on the instrument panel by the switch's panel representation. |
| 25 | XY | 3 |  | The XY control has two functions: It displays audio input signals and acts as a 2-dimensional controller used with the mouse. In the module Properties you can set the display type for the audio signals to be visualized. The inputs X1 and Y1 control the position of the visual object (Pixel or Cross). When the object is set to Bar or Rectangle then X1 and Y1 control one corner and X2 and Y2 the opposite corner of the object. To display fast moving audio data select Scope mode. All other modes evaluate the input only at the lower graphics update rate. You can also set the size of the crosshair appearing at the mouse position. Everything drawn in the display fades out more or less slowly, depending on the value set for Fade Time in the Properties (maximum value is 99).  |
| 30 | Meter | 3 |  | Placed inside of a Stacked Macro this module is used to select one of the macros to be visible on the Panel. If the instrument Panel is unlocked, the indices of the available macros can be read from the panel context menu of the Stacked Macro. With a connection to an input of the Stacked Macro you can control from outside which macro is visible. |
| 31 | Level Meter | 1 |  |  |
| 32 | Picture | 2 |  | User definable decoration element. A bitmap file (.tga or .bmp) that contains the pictures can be loaded. TGA files allow for transparency (alpha channel). The picture manager allows to set the to be resizable. This means that the inner area of the picture is repeated to fill bigger area.Left, right, upper and lower borders can be defined which are repeated when the picture size is changed. |
| 35 | Text | 1 |  | <text> |
| 50 | Note Pitch | 7 |  | Source for MIDI Note On events. The output range is suitable to control logarithmic pitch inputs.  C3 = 60 |
| 51 | Pitchbend | 2 |  | Source for an event/audio signal, controlled by MIDI pitchbend events. The up and down range can be adjusted independently by (Min) and (Max).  (1 unit per semitone) |
| 52 | Gate | 2 |  | Source for MIDI Note On and Note Off events, switching the status of a gate output signal. The amplitude is controlled by the On velocity. |
| 55 | On Velocity | 1 |  | Source for event/audio signals, controlled by the velocity of the MIDI Note On events. The range is adjustable with (Min) and (Max). |
| 57 | Controller | 3 |  | Source for an event/audio signal, controlled by MIDI control change events. The range is adjustable with (Min) and (Max). |
| 100 | Constant | 98 |  | Source for a constant event/audio signal. |
| 110 | + | 88 |  | Adder for multiple signals. |
| 111 | X | 63 |  | Multiplier for multiple signals. |
| 112 | Invert, -x | 27 | mono::AudioInvert(TMem*) | Inverter for signals. |
| 113 | Reciprocal, 1/x | 6 | mono::AudioDiv1(TMem*) | Divider. The output value is 1 divided by the input value. |
| 114 | Divide, x/y | 27 | mono::AudioDivX(TMem*) | Divider. The output value is the X input value divided by the Y input value. |
| 117 | Expon. (F) | 19 | mono::AudioExponF(TMem*) | Exponentiator for converting logarithmic pitch values in semitones to linear frequency values in Hz |
| 118 | Expon. (A) | 7 | mono::AudioExponA(TMem*) | Exponentiator for converting logarithmic level values in dB to linear amplitude values. |
| 119 | Log | 4 | mono::AudioLogA(TMem*) | Logarithm for converting linear amplitude values to logarithmic level values in dB. Also for driving the logarithmic time inputs of envelopes etc. |
| 122 | \|x\| | 1 | mono::AudioRectify(TMem*) | The output is the absolute value of the input signal. It is the effect of a rectifier, i.e. negative values are inverted and become positive. |
| 127 | Square Root | 8 | mono::AudioSquareRoot(TMem*) | Computes the square root of the input values. |
| 155 | Distributor/Panner | 1 |  | Distributor/Panner. The Curve setting in the Properties decide between the Distributor and the Panner mode.    When Curve is set to None the module works as a switch between the input signal and one of the outputs. The index of the selected output is 0 for Pos < 1, 1 for Pos >= 1 and Pos < 2, 2 for Pos >= 2 and Pos < 3, and so on.    When Curve is set to Linear or Sine the input signals is panned between the two outputs whose indices are closest to the Pos value. The Sine mode delivers a higher level in the center position between two outputs.    When Wrap mode is selected in the Properties, Pos wraps around at its range limits which results in a repeating of the range. |
| 156 | Amp/Mixer | 5 |  | Mixer for audio input signals, controlled by logarithmic event inputs. |
| 158 | Crossfade | 3 |  | Crossfading module. The output signal is mixed from the both input signals 0 0 and 0 1. The mix ratio is controlled by the X input. |
| 162 | Relay 1, 2 | 3 |  | Relay. The upper input is connected to the output if Ctl > 0. Otherwise the lower input is connected to the output. If the lower input is not present a zero signal is produced at the output.  This module can be substituted by a Selector module with one or two inputs. It is available only for backward compatibility because the Ctl signal is interpreted differently. |
| 204 | Noise | 1 | mono::AudioOscNoise(TMem*) | Generator for white noise. The signal jumps randomly between two values (-0.5*A and +0.5*A). If you feed it to a lowpass filter it will be transformed to a continuously fluctuating random signal.  In the polyphonic mode only the amplitude modulation is polyphonic. |
| 260 |  LFO | 5 |  | Low frequency oscillator providing three different waveforms simultaneously. The symmetry of the waveforms is adjustable by (W). |
| 261 | Slow Random | 4 |  | Generator for a smoothed, slowly varying random signal. The rate of the steps of the output signal is set by the System Control Rate.   The speed of change of the values can be adjusted by (F) as the cutoff frequency of an internal (1-pole) lowpass filter. |
| 272 | ADSR | 1 | mono::AudioEnvADSR(TMem*) | Generator for Attack-Decay-Sustain-Release envelopes. The peak is controlled by the value of the Gate On event. |
| 300 | HP/LP 1-Pole | 5 | mono::AudioFilt1(TMem*) | First order (6 db/octave) highpass (HP) and lowpass (LP) filter, with logarithmic (P) control of the cutoff frequency. |
| 303 | Multi 2-Pole | 1 | mono::AudioFilt2(TMem*) | Second order (12 db/octave) resonant multimode filter with lowpass, bandpass and highpass outputs. With logarithmic (P) control input for cutoff frequency and a resonance (Res) control input. The gain in the passband is always unity, while boost is applied at the resonant frequency. |
| 316 | High Shelf EQ | 2 | mono::AudioEQHiShelf(TMem*) | Equalizer with high frequency shelving characteristic. With inputs for logarithmic (P) control of the corner frequency and for boost/cut in dB (B). |
| 318 | Low Shelf EQ | 2 | mono::AudioEQLoShelf(TMem*) | Equalizer with low frequency shelving characteristic. With inputs for logarithmic (P) control of the corner frequency and for boost/cut in dB (B). |
| 340 | Single Delay | 5 |  | Delay for event and audio signals. The delay time in milliseconds is controlled by the input (Dly).  For audio signals the interpolation for the delayed signal can be set in the Properties.If the interpolation is Off the delay time can be set only to an integer number of samples.With Linear or High Quality interpolation the delay time can be set precisely.If the time corresponds to a non-integer number of samples, interpolation occurs. |
| 342 | Diffuser Delay | 13 |  | Diffuser delay, containing a feedback delay. This is an allpass filter as used for constructing reverb effects. When Dly=0 this is a 1-pole allpass as used for building phaser effects. |
| 360 | Saturator | 4 | mono::AudioSaturator(TMem*) | Soft saturating overdrive modifier. Limits the output amplitude to +/- 2 units for input amplitudes greater than +/- 4 units. |
| 362 | Clipper | 2 | mono::AudioClipper(TMem*) | Signal modifier, clipping the input signal at two contollable levels. |
| 402 | Randomizer | 1 |  | The input events are modified by a random deviation. The maximum range of deviation is controlled by the signal at the input (Rng). |
| 412 | Order | 1 |  | An event arriving at the input is transmitted to the outputs in a defined order: first it is passed to (1), then to (2) and finally to (3). The event travels through ALL the modules in the chain connected to (1), before going to the first module connected to (2) etc. |
| 414 | Separator | 5 |  | Separates events by routing them to different outputs according to their value. (Thld) sets the separation threshold level. |
| 415 | Value | 6 |  | Events arriving at the Trig input are passed to the output with a new value that is taken from the lower input. It can also be used as an event-driven Sample&Hold: the value of an audio signal is sampled every time an event arrives. It can also be used with Table Reference wires as input and output. |
| 417 | M | 7 |  | Event-Merger. All events of all inputs are transmitted to the output. |
| 442 | Audio Voice Combiner | 2 | mono::AudioVC(TMem*) | Voice Combiner. Mixes a polyphonic audio signal to mono. |
| 446 | A/E | 1 |  | Audio to event converter. The audio signal at In is sampled with the Control Rate (adjustable in the Ensemble menu) and passed to the event output. |
| 452 | Audio Smoother | 8 | mono::AudioSmootherA(TMem*) | Smoother with audio output. Steps in the input signal are transformed to ramps. The transition time of the ramps is set in the Properties. |
| 455 | Tempo Info | 1 |  | Source for the current Tempo measured in Beats per Second. To get the BPM value, multiply by 60. |
| 456 | Voice Info | 1 |  | This module gives informations about the voice settings and assignments of the instrument. |
| 458 | System Info | 2 |  | Source for information about the system: Sampling rate, control rate (and control rate events), display update events, CPU load and audio buffer size |
| 524 | Core Cell | 4 |  | Core Cell |
| 536 | Out | 2 |  | Receive terminal for wireless connections with Send terminals.  The input signal of the selected Send terminal is passed to the output of this Receive terminal. The labels of all Send terminals which are inside of the same instrument are shown in the list in the Properties.  The Use column allows to set a selection that is displayed in the panel control. The entry in the State column shows if the signal type of the Send terminal is connectable with the signal type of this Receive terminal (Audio to Event is impossible). |

## Snapshots

- **Flute** — 59 parameters
- **Flute** — 62 parameters
- **Pan** — 62 parameters
- **The Pipe** — 62 parameters
- **SoloPhone** — 62 parameters
- **Harmonix** — 62 parameters
- **Guitar** — 62 parameters
- **StringThing** — 62 parameters
- **Bell** — 62 parameters
- **SteamPipe** — 62 parameters
- **Bowed Bell** — 62 parameters
- **SteamGhost** — 62 parameters
- **Bong** — 62 parameters
- **Pipe Organ 1** — 62 parameters
- **Pipe Organ 2** — 62 parameters
- **Flute 2** — 62 parameters
- **Acc Bass** — 62 parameters
- **Harp** — 62 parameters
- **Ethnic I** — 62 parameters
- **NoiseFlute** — 62 parameters
- **Tri** — 62 parameters
- **Ethnic II** — 62 parameters
- **SortofBass** — 62 parameters
- **Waterdrum** — 62 parameters
- **MuteGuitar** — 62 parameters
- **MuteBass** — 62 parameters
- **Glass?** — 62 parameters
- **Glockenspiel** — 62 parameters
- **GlassWoodTemplate** — 62 parameters
- **Far far away** — 62 parameters
- **Ethnic III** — 62 parameters
- **Banjo** — 62 parameters
- **ShangHai** — 62 parameters
- **Cello (use low registers)** — 62 parameters
- **Distant Brass** — 62 parameters
- **Fagot** — 62 parameters
- **Bariton Sax** — 62 parameters
- **Sax** — 62 parameters
- **Rough** — 62 parameters
- **MiddleAges** — 62 parameters
- **Rondo** — 62 parameters
- **SomeDrum** — 62 parameters
- **Tibet** — 62 parameters
- **Timpani** — 62 parameters
- **MW Cry** — 62 parameters
- **Vacquie** — 62 parameters
- **cloudy** — 62 parameters
- **starwars** — 62 parameters
- **spanish guitar** — 62 parameters
- **flamenco glass** — 62 parameters
- **blow** — 62 parameters
- **progressive** — 62 parameters
- **organic** — 62 parameters
- **pvc** — 62 parameters
- **coolharp** — 62 parameters
- **tarrega** — 62 parameters
- **nutcracker** — 59 parameters
- **war_of_worlds** — 62 parameters
- **war_of_worlds2** — 59 parameters
- **air** — 62 parameters
- **crystal_strings** — 62 parameters
- **carroussel** — 62 parameters
- **carroussel 1** — 62 parameters
- **carroussel 2** — 62 parameters
- **war_of_worlds3** — 59 parameters
- **war_of_worlds 1** — 59 parameters
- **Pizz** — 59 parameters

## Warnings

None.
