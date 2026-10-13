// pulsarbox.scd — SynthDefs for the SC PulsarBox node (src/nodes/scPulsarBox.h).
//
// Pulsar synthesis (Roads): every voice emits pulses at its fundamental; each
// pulse is a pulsaret (a table read at the formant frequency) shaped by an
// envelope table lasting `cycles` periods of the formant. Up to four formant
// layers share a voice's pulses. Masking (probability, burst / rest, a sieve
// of up to 32 steps) removes pulses, and every pulse can land on another
// output. Inspired by nuPG (Marcin Pietruszewski); stock GrainBuf only.
//
//   PulsarBox_<v>_<o>   v = voices (1..8), o = outputs (1..16)
//
// Controls (arrays of v per voice, of 4 per layer):
//   fund[v] glide lev[v] gate[v] atk rel     voice: fundamental (Hz), level, ASR
//   lon[4]                 layer on (0 / 1)
//   lmode[4] lhz[4] lrat[4]    formant: 0 = lhz Hz, 1 = lrat x the fundamental
//   lcyc[4]                cycles of the formant under the envelope
//   lamp[4] loff[4] lpan[4]    level, delay (share of a period), output offset
//   pbuf[4] ebuf[4]        pulsaret and envelope tables (one cycle each)
//   rjit fjit ajit pjit    per pulse: rate (octaves), formant (semitones),
//                          level (0..1), output (channels) jitter
//   prob burst rest sieve[32] slen      masking (pulses)
//   pmode rot              outputs: 0 voice, 1 cycle, 2 random, 3 layer;
//                          rotation (channels)
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
        SynthDef(("PulsarBox_" ++ nv ++ "_" ++ no).asSymbol, {
            var fund = \fund.kr(100 ! nv).clip(0.05, 4000).lag(\glide.kr(0));
            var lev = \lev.kr(0.5 ! nv, 1/30, fixedLag: true);
            var gate = \gate.kr(1 ! nv);
            var atk = \atk.kr(0.005), rel = \rel.kr(0.3);
            var lon = \lon.kr([1, 0, 0, 0]);
            var lmode = \lmode.kr(0 ! layers);
            var lhz = \lhz.kr([800, 1600, 3200, 6400]);
            var lrat = \lrat.kr([4, 8, 16, 32]);
            var lcyc = \lcyc.kr(1 ! layers);
            var lamp = \lamp.kr(1 ! layers, 1/30, fixedLag: true);
            var loff = \loff.kr(0 ! layers);
            var lpan = \lpan.kr(0 ! layers);
            var pbuf = \pbuf.kr(-1 ! layers);
            var ebuf = \ebuf.kr(-1 ! layers);
            var rjit = \rjit.kr(0), fjit = \fjit.kr(0), ajit = \ajit.kr(0), pjit = \pjit.kr(0);
            var prob = \prob.kr(1);
            var burst = \burst.kr(1).round.max(1), rest = \rest.kr(0).round.max(0);
            var sieve = \sieve.kr(1 ! sieveSize);
            var slen = \slen.kr(1).round.clip(1, sieveSize);
            var pmode = \pmode.kr(0), rot = \rot.kr(0);
            var sig = 0;

            nv.do { |v|
                var f = fund[v] * (2 ** (LFNoise0.kr(fund[v].min(500)) * rjit));
                var pulse = Impulse.ar(f);
                var count = PulseCount.ar(pulse) - 1;
                var burstGate = (count % (burst + rest)) < burst;
                var sieveGate = Select.ar(count % slen, K2A.ar(sieve)) > 0.5;
                var trig = CoinGate.ar(prob, pulse) * burstGate * sieveGate;
                var env = EnvGen.kr(Env.asr(atk, 1, rel), gate[v]);
                var voice = layers.collect { |g|
                    var t = DelayN.ar(trig * lon[g], 2, (loff[g] / f).clip(0, 2));
                    var hz = Select.kr(lmode[g], [lhz[g], f * lrat[g]])
                        * (2 ** (TRand.ar(-1, 1, t) * fjit / 12));
                    var dur, amp, base, pos, pan;
                    hz = hz.clip(1, SampleRate.ir * 0.45);
                    dur = (lcyc[g] / hz).clip(2 * SampleDur.ir, 2).min(200 / f);   // GrainBuf holds 256 grains
                    amp = lamp[g] * (dur * f).max(1).sqrt.reciprocal
                        * Latch.ar(1 - (TRand.ar(0, 1, t) * ajit), t);
                    // The output of this pulse, in channels (fractions crossfade)
                    base = Select.ar(pmode, [
                        DC.ar(v), count + v, TIRand.ar(0, no - 1, t), DC.ar(v + g)
                    ]);
                    pos = base + lpan[g] + rot + (TRand.ar(-1, 1, t) * pjit);
                    pan = case
                        { no == 1 } { 0 }
                        { no == 2 } { pos.fold(0, 1) * 2 - 1 }
                        { ((2 * pos) - 1) / no };   // GrainBuf (PanAz-like): channel k at (2k - 1) / no
                    GrainBuf.ar(no, t, dur, pbuf[g], hz * BufFrames.kr(pbuf[g]) * SampleDur.ir,
                        0, 2, pan, ebuf[g], 256) * amp
                };
                sig = sig + (voice.sum * env * lev[v]);
            };
            Out.ar(\out.kr(0), Limiter.ar(LeakDC.ar(sig), 0.98));
        }).writeDefFile(d);
    };
};
"PulsarBox: % defs in %".format(maxVoices * maxOutputs, d).postln;
)
