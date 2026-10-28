// AliasingBox: Dietrich Pank's Aliasing Synth, reconstructed from the Primary
// graph in aliasing_synth_midi.ens. Each voice owns a normalized 1024-cell table:
// read oscillator -> table VALUE, write oscillator -> table WRITE ADDRESS,
// read oscillator -> table READ ADDRESS. Do not substitute buffer increments
// for oscillator pitch: Primary Expon.(F) converts semitones to Hz.
//
// The two original LFOs use amount^5 in semitones and rate*(rate+voice offset).
// Notch -> HP4 -> drive/saturate -> LP4 -> optional DC removal -> drive/saturate.
// BHiPass4 / BLowPass4 and tanh follow SatFMFilter; they approximate NI's
// proprietary Primary filters/saturators, rather than claiming bit equivalence.
//
// One synth per voice. The node around it is modular (shared ModGate /
// ModLfo / ModEnv / ModRoute, PolyCombSpread, MixerPro effects that run only
// while on), but the voice stays whole: its filters start from the table's
// first value and keep running while switched off, so a snapshot that turns
// the Notch or HP / LP on finds them warm, as in the original. Split into
// synths, each part would start from a bus value instead (a different
// start-up transient) and still have to run while off; no CPU to gain.
(
var root, dir;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "AliasingBox";
File.mkdir(root); File.mkdir(dir);
SynthDef(\AliasingBoxVoice, {
    var m = { |name| In.kr(NamedControl.kr(name, 0), 1) };
    var p = { |name, def| NamedControl.kr(name, def) };
    // continuous controls: a short lag, so per-frame changes (snapshot morphs,
    // modulated publishing) do not step the read/write pitches or levels
    var pl = { |name, def| NamedControl.kr(name, def, 1/30, fixedLag: true) };
    // Reaktor ADSR times: 0=1 ms, 20=10 ms, 40=100 ms, 60=1 s.
    var time = { |name, def| (10 ** (p.(name,def) / 20)) * 0.001 };
    var voice = \voice.kr(0), voices = \voices.kr(4).max(1);
    var side = ((voice + 0.5) / voices) - 0.5;
    var index = Select.kr(voices > 1, [0.5, voice / (voices - 1).max(1)]);
    var gate = In.ar(\gatebus.kr(0), 1);
    var trig = HPZ1.ar(gate > 0).max(0);
    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
    var gt = gg[14] * 0.01 * Rand(-1, 1);   // each voice's detune (the transpose is in gq)
    var gq = { |p| var r = p.round, q = p + (((((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1])) > 0.5) * Select.kr(r.mod(12) + ((r < gg[17]) * 12), gg[2..13] ++ gg[18..29])), w = gg[16] + (((q - gg[16]) / 12).floor * 12); q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };
    var note = gq.(Select.kr(\pitch_sel.kr(0), [\pitch.kr(60), A2K.kr(\pitch_ar.ar(60))])) + gt
        + pl.(\transpose,0) + (pl.(\fine_tune,0)) + m.(\mpitch);
    var tracking = 2 ** (((60 - note) * p.(\envelope_keytrack,0)) / 12);
    var ampEnv = EnvGen.ar(Env.adsr(time.(\amp_attack,-20)*tracking, time.(\amp_decay,40)*tracking,
        p.(\amp_sustain,1), time.(\amp_release,40)*tracking, curve: [\amp_curve0.kr(-4), \amp_curve1.kr(-4), \amp_curve2.kr(-4)]), gate > 0);
    var filterEnv = EnvGen.kr(Env.adsr(time.(\filter_attack,0), time.(\filter_decay,40),
        p.(\filter_sustain,0), time.(\filter_release,40), curve: [\filter_curve0.kr(-4), \filter_curve1.kr(-4), \filter_curve2.kr(-4)]), A2K.kr(gate > 0));
    var wrate = p.(\write_lfo_rate,0), rrate = p.(\read_lfo_rate,0);
    var wlfo = SinOsc.kr(wrate * (wrate + index)) * pl.(\write_lfo_amount,0).pow(5);
    var rlfo = SinOsc.kr(rrate * (rrate + index)) * pl.(\read_lfo_amount,0).pow(5);
    var wnote = note + pl.(\write_pitch,-32) + pl.(\write_fine,0) + (((index*2)-1) * pl.(\write_spread,0)) + wlfo + m.(\mwrite);
    var rnote = note + pl.(\read_pitch,-51) + pl.(\read_fine,0) + (side * pl.(\read_spread,0)) + rlfo + m.(\mread);
    var wf = wnote.clip(-100,160).midicps, rf = rnote.clip(-100,160).midicps;
    // Raw phase ramps deliberately preserve aliasing; a band-limited oscillator
    // here would erase the defining read/write collisions.
    var wphase = Phasor.ar(trig, wf * SampleDur.ir, 0, 1);
    var rphase = Phasor.ar(trig, rf * SampleDur.ir, 0, 1);
    var symmetry = (((pl.(\write_symmetry,0) + m.(\msym)) + 1) * 0.5).clip(0.00001,0.99999);
    // Primary Tri/Par starts at its rising zero crossing, whereas Ramp
    // starts at zero in 0..1. Preserve that relative phase in the table.
    var wtphase = (wphase+(symmetry*0.5)).wrap(0,1);
    var wtri = Select.ar(wtphase < symmetry, [(1-wtphase)/(1-symmetry), wtphase/symmetry]) * 2 - 1;
    var rtri = 1 - ((((rphase+0.25).wrap(0,1) * 2) - 1).abs);
    // Endpoint weights blend only the selected alternatives, including stored
    // intermediate sounds. Legacy callers keep their ordinary selector values.
    var blendMode = { |key, index, signals|
        var weights=signals.collect { |sig,k| p.((key++"_w"++k).asSymbol,0) };
        var total=weights.sum;
        var fallback=signals.collect { |sig,k| (index.round.clip(0,signals.size-1) |==| k) };
        weights=weights.collect { |weight,k| Select.kr(total>0,[fallback[k],weight]).lag(0.03) };
        (signals*weights).sum;
    };
    var read = blendMode.("read_quality",p.(\read_quality,0),[
        LinXFade2.ar((rphase+0.5).wrap(0,1),rtri,p.(\read_bidirectional,0).clip(0,1).lag(0.03)*2-1),rphase]);
    var write = blendMode.("write_quality",p.(\write_quality,0),[wphase,wtri,read]);
    var tableRange = pl.(\table_range,1).clip(0,1);
    var table = LocalBuf(1024,1).clear;
    var raw, notch, hp, lp, sig, flfo, frate, cut, vel, amp, cell;
    var phase, saw, tri, sine, pulse, random, smooth, cubic, shape, x, exponent, reset;
    BufWr.ar(read,table,write.clip(0,1) * tableRange * 1023,0);
    raw = (BufRd.ar(1,table,read * tableRange * 1023,0,1) * 2) - 1;
    notch = BPeakEQ.ar(raw,(pl.(\notch_pitch,100)+((note-60)*pl.(\notch_keytrack,0))).midicps.clip(10,SampleRate.ir*0.45),
        (1-pl.(\notch_resonance,0.3)).clip(0.02,1),pl.(\notch_gain,-30));
    raw = LinXFade2.ar(raw, notch, p.(\notch_on,0).lag(0.03)*2-1);
    // Core MultiWave: phase is -0.5..0.5; the screenshot and decoded
    // arithmetic agree on triangle 1-4|phase| and this sine polynomial.
    // The main rate is always tempo * numerator / denominator. The old
    // "rate" guess was actually the random output's smoothing control.
    frate = (\bpm.kr(120)/60) * p.(\filter_lfo_numerator,1) / p.(\filter_lfo_denominator,1).max(1);
    shape = p.(\filter_lfo_shape,0).clip(-1,1);
    reset = (A2K.kr(trig)*p.(\filter_lfo_gate_sync,0)) + Impulse.kr(0);
    phase = Phasor.kr(reset,frate*ControlDur.ir,-0.5,0.5,p.(\filter_lfo_reset_phase,0).wrap(-0.5,0.5));
    saw = phase*2;
    tri = 1-(phase.abs*4);
    x = 0.25-phase.abs;
    sine = (x*6.254883)+(x.pow(3)* -41.15548)+(x.pow(5)*81.23766);
    pulse = (phase > (shape*0.5))*2-1;
    random = Latch.kr(WhiteNoise.kr,HPZ1.kr(phase)< -0.5);
    smooth = Lag.kr(random,p.(\filter_lfo_smoothing,0).max(0)*0.001);
    cubic = LFDNoise3.kr(frate);
    flfo = blendMode.("filter_lfo_wave",p.(\filter_lfo_wave,5),[saw,saw.neg,pulse,tri,sine,random,smooth,cubic]);
    exponent = Select.kr(shape<0,[1+(shape*7),1+shape]).max(0.001);
    flfo = ((flfo+1)*0.5).clip(0,1).pow(exponent);
    cut = pl.(\lp_pitch,90) + (filterEnv*pl.(\filter_env_amount,0)) + (flfo*pl.(\filter_lfo_amount,0)) + m.(\mlp);
    // Both FM signals come from the unfiltered input, as wired in the ensemble.
    hp = BHiPass4.ar(raw,((pl.(\hp_pitch,20)+m.(\mhp)).midicps +
        (raw*(pl.(\hp_fm,0)+m.(\mhpfm)))).clip(10,SampleRate.ir*0.45),
        (1-pl.(\hp_resonance,0)-m.(\mhpres)).clip(0.02,1));
    hp = (hp*(pl.(\drive,0)+m.(\mdrive)).clip(0,30).dbamp).tanh;
    lp = BLowPass4.ar(hp,(cut.midicps+(raw*(pl.(\lp_fm,0)+m.(\mlpfm)))).clip(10,SampleRate.ir*0.45),
        (1-pl.(\lp_resonance,0)-m.(\mlpres)).clip(0.02,1));
    lp = LinXFade2.ar(lp, LeakDC.ar(lp), p.(\dc_block,1).lag(0.03)*2-1);
    sig = (lp*(pl.(\drive,0)+m.(\mdrive)).clip(0,30).dbamp).tanh;
    sig = LinXFade2.ar(raw, sig, p.(\filter_on,1).lag(0.03)*2-1);
    vel = In.kr(\velbus.kr(0),1);
    // \vca fractional: a snapshot morph crossfades the two amp paths
    amp = { |vca| ((1+m.(\mamp)).clip(0,2)*(1-vca))+(m.(\mamp).clip(0,1)*vca) }.(\vca.kr(0,1/30,fixedLag:true).clip(0,1));
    sig = Sanitize.ar(sig) * ampEnv * amp *
        ((1-pl.(\velocity_sensitivity,1)) + (vel*pl.(\velocity_sensitivity,1))) *
        pl.(\level,-6).dbamp * \master.kr(0.6,0.02) * \normalization.kr(0.25);
    Out.ar(\out.kr(0),sig);
    // Layout (scAliasingTableView.h): 16 x 6 meters, 16 pan positions from
    // the spreader, then 256 normalized table cells per voice, then each
    // voice's filter envelope. Every row comes from that voice's real table,
    // not a GUI simulation: 16 of its 256 cells per control block, the whole
    // table every 16 blocks (~21 ms at 48 kHz), 1/16 of the cost of all 256.
    Out.kr(\vis.kr(0)+(voice*6),[wf,rf,A2K.kr(ampEnv),Amplitude.kr(sig),A2K.kr(read),A2K.kr(write.clip(0,1))]);
    cell = Phasor.kr(0, 1, 0, 16).floor * 16;
    Out.kr(\vis.kr(0)+176+(voice*256)+cell,BufRd.kr(1,table,(cell+(0..15))*(1023/255)*tableRange,0,1));
    Out.kr(\vis.kr(0)+176+(voices*256)+voice,filterEnv);
}).writeDefFile(dir);
"AliasingBox SynthDef written to ".post; dir.postln;
)
