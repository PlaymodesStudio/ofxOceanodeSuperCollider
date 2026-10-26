// pulsarbox.scd — SynthDefs for the SC PulsarBox node (src/nodes/boxSynths/pulsarBox/scPulsarBox.h).
//
// Pulsar synthesis (Roads): every voice emits pulses at its fundamental; each
// pulse is a pulsaret (a table read at the formant frequency) shaped by an
// envelope table lasting `cycles` periods of the formant. Up to four formant
// layers share a voice's pulses. Masking (probability, burst / rest, a sieve
// of up to 32 steps) removes pulses, and every pulse can land on another
// output. Inspired by nuPG (Marcin Pietruszewski); stock GrainBuf only.
//
//   PulsarBox_<v>_<o>_<l>   v = voices (1..8), o = outputs (1..16), l = formant
//                           layers (1..4): only the layers in use are built
//
// Controls (arrays of v per voice, of 4 per layer):
//   fund[v] glide lev[v] gate[v]     voice: fundamental (Hz), level, gate
//   atk dec sus rel ca cd cr gated   the Amp Envelope (ADSR, curves) on each
//                          voice's gate, x the gate's level; gated 0: drone
//   lmode[4] lhz[4] lrat[4]    formant: 0 = lhz Hz, 1 = lrat x the fundamental
//   lcyc[4]                cycles of the formant under the envelope
//   lamp[4] loff[4] lpan[4]    level, delay (share of a period), output offset
//   pbuf[4] ebuf[4]        pulsaret and envelope tables (one cycle each)
//   rjit fjit ajit pjit    per pulse: rate (octaves), formant (semitones),
//                          level (0..1), output (channels) jitter
//   prob burst rest sieve[32] slen      masking (pulses)
//   pmode rot              outputs: 0 voice, 1 cycle, 2 random, 3 layer;
//                          rotation (channels)
//   mod                    the shared modulation's target buses (16 channels
//                          each, one per voice; -1: none): 0 pitch (st),
//                          1 formant (st), 2 cycles (oct), 3 level, 4 probability,
//                          5 pan (channels)
//   out
//
// A pulse whose envelope outlasts the period overlaps the next ones: the level
// is divided by sqrt(overlap) so density does not blow up the output.
//
// Output: <this folder>/CompiledSynthdefs/PulsarBox/*.scsyndef, to be copied
// to the app's data/Supercollider/Synthdefs/Defaults/PulsarBox/.

(
var d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" +/+ "PulsarBox";
var maxVoices = 8, maxOutputs = 16, layers = 4, sieveSize = 32;
File.mkdir(d.dirname);
File.mkdir(d);

(1..maxVoices).do { |nv|
    (1..maxOutputs).do { |no|
    (1..layers).do { |nl|
        SynthDef(("PulsarBox_" ++ nv ++ "_" ++ no ++ "_" ++ nl).asSymbol, {
            var kv = { |name, def| NamedControl.kr(name,def!nv).asArray };
            var fund = \fund.kr(100 ! nv).clip(0.05, 4000).lag(kv.(\glide,0));
            var lev = \lev.kr(0.5 ! nv, 1/30, fixedLag: true).asArray;
            // the gates: their levels from ModGate (\gatebus, audio, nv), else the values
            var gbus = \gatebus.kr(-1);
            var gate = Select.kr(gbus >= 0, [\gate.kr(1 ! nv), A2K.kr(In.ar(gbus.max(0), nv)).asArray]);
            // the pitch from a cable (MIDI notes + \tr semitones), else \fund (Hz)
            var pcable = (A2K.kr(\pitch_ar.ar(57 ! nv)).asArray + \tr.kr(0)).midicps;
            var psel = \pitch_sel.kr(0) > 0.5;
            var atk = kv.(\atk,0.005), dec = kv.(\dec,0.3), sus = kv.(\sus,1), rel = kv.(\rel,0.3);
            var curves = [kv.(\ca,-4), kv.(\cd,-4), kv.(\cr,-4)].collect { |c| c.clip(-12, 12) };
            var gated = \gated.kr(1, 1/30, fixedLag: true).clip(0, 1);   // fractional: a snapshot morph to / from Drone
            var lmode = \lmode.kr(0 ! layers);
            var lhz = \lhz.kr(Array.fill(layers*nv,{ |i| [800,1600,3200,6400][i.div(nv)] }));
            var lrat = \lrat.kr(Array.fill(layers*nv,{ |i| [4,8,16,32][i.div(nv)] }));
            var lcyc = \lcyc.kr(1 ! (layers*nv));
            var lamp = \lamp.kr(1 ! (layers*nv), 1/30, fixedLag: true);
            var loff = \loff.kr(0 ! (layers*nv));
            var lpan = \lpan.kr(0 ! (layers*nv));
            var pbuf = \pbuf.kr(-1 ! layers);
            var ebuf = \ebuf.kr(-1 ! layers);
            var rjit = kv.(\rjit,0), fjit = kv.(\fjit,0), ajit = kv.(\ajit,0), pjit = kv.(\pjit,0);
            var prob = \prob.kr(1);
            var burst = \burst.kr(1).round.max(1), rest = \rest.kr(0).round.max(0);
            var sieve = \sieve.kr(1 ! sieveSize);
            var slen = \slen.kr(1).round.clip(1, sieveSize);
            var pmode = \pmode.kr(0), rot = \rot.kr(0);
            var pw = \panweights.kr(0!4);
            var psum = pw.sum;
            var panweights = (pw + Array.fill(4,{ |i| (pmode.floor.clip(0,3) |==| i) * (psum <= 0) })).lag(0.03);
            var modbus = \mod.kr(-1);
            var mod = { |t| (In.kr(modbus.max(0) + (t * 16), nv) * (modbus >= 0)).asArray };
            var mPitch = mod.(0), mForm = mod.(1), mCyc = mod.(2), mLev = mod.(3), mProb = mod.(4), mPan = mod.(5);
            var sig = 0;

            nv.do { |v|
                var rjit = kv.(\rjit,0)[v], fjit = kv.(\fjit,0)[v], ajit = kv.(\ajit,0)[v], pjit = kv.(\pjit,0)[v];
                var f0 = (Select.kr(psel, [fund[v], pcable[v]]) * (2 ** (mPitch[v] / 12))).clip(0.05, 4000);
                var f = f0 * (2 ** (LFNoise0.kr(f0.min(500)) * rjit));
                var pulse = Impulse.ar(f);
                var count = PulseCount.ar(pulse) - 1;
                var burstGate = (count % (burst + rest)) < burst;
                // the sieve's step for this pulse, read only when a pulse comes
                // (Dswitch1: no audio-rate copy of the 32 steps)
                var sieveGate = Demand.ar(pulse, 0, Dswitch1(sieve, count % slen)) > 0.5;
                var trig = CoinGate.ar((prob + mProb[v]).clip(0, 1), pulse) * burstGate * sieveGate;
                // the Amp Envelope on the gate (a gate rising to 0.2 plays at
                // 0.2: its level, held until the next); not gated: a drone
                var glev = Gate.kr(gate[v], gate[v] > 0);
                var env = (1 - gated) + (gated * EnvGen.kr(Env.adsr(atk[v].max(0.001), dec[v].max(0.001), sus[v].clip(0, 1),
                    rel[v].max(0.001), 1, curves.collect { |c| c[v] }), gate[v] > 0) * glev);
                var levMod = (1 + mLev[v]).max(0);   // routes to Level add
                var voice = nl.collect { |g|
                    // the layer's Delay (a share of the period): the trigger delayed
                    // (TDelay: no buffer; one pending pulse is enough below a period)
                    var t = Select.ar(K2A.ar(loff[g*nv+v] > 0), [trig, TDelay.ar(trig, (loff[g*nv+v].min(0.999) / f).clip(0, 20))]);
                    var hz = LinXFade2.kr(lhz[g*nv+v], f * lrat[g*nv+v], lmode[g].clip(0,1).lag(0.03)*2-1)
                        * (2 ** ((TRand.ar(-1, 1, t) * fjit + mForm[v]) / 12));
                    var dur, amp, base, pos, pan;
                    hz = hz.clip(1, SampleRate.ir * 0.45);
                    dur = (lcyc[g*nv+v] * (2 ** mCyc[v]) / hz).clip(2 * SampleDur.ir, 2).min(200 / f);   // GrainBuf holds 256 grains
                    amp = lamp[g*nv+v] * (dur * f).max(1).sqrt.reciprocal
                        * Latch.ar(1 - (TRand.ar(0, 1, t) * ajit), t);
                    // The output of this pulse, in channels (fractions crossfade)
                    base = ([DC.ar(v), count + v, TIRand.ar(0, no - 1, t), DC.ar(v + g)] * panweights).sum;
                    pos = base + lpan[g*nv+v] + rot + mPan[v] + (TRand.ar(-1, 1, t) * pjit);
                    pan = case
                        { no == 1 } { 0 }
                        { no == 2 } { pos.fold(0, 1) * 2 - 1 }
                        { ((2 * pos) - 1) / no };   // GrainBuf (PanAz-like): channel k at (2k - 1) / no
                    GrainBuf.ar(no, t, dur, pbuf[g], hz * BufFrames.kr(pbuf[g]) * SampleDur.ir,
                        0, 2, pan, ebuf[g], 256) * amp
                };
                sig = sig + (voice.sum * env * lev[v] * levMod);
            };
            Out.ar(\out.kr(0), Limiter.ar(LeakDC.ar(sig), 0.98) * \morphgain.kr(1,0.03));
        }).writeDefFile(d);
    };
    };
};
"PulsarBox: % defs in %".format(maxVoices * maxOutputs * layers, d).postln;
)
