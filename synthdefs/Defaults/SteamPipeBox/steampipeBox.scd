// SteamPipeBox: sample-level pipe in OceanodeSteamPipe, shared box modulation.
// Evaluate this file; requires the OceanodeSteamPipe extension. No running server needed.
(
var here=thisProcess.nowExecutingPath.dirname;
var root=here +/+ "CompiledSynthdefs", dir, pause;
dir=root +/+ "SteamPipeBox";File.mkdir(root);File.mkdir(dir);
// A voice silent (before its VCA) with its gate down for 0.1 s pauses itself;
// re-armed 4 x / s, so a resume while still silent pauses it again. The node
// resumes it with the gate of its next note and wakes it (\wake) when its
// controls change, so their lags settle before it pauses. \pauser 0 (the
// default, and wherever the node cannot see a gate open): never.
pause = { |sig|
    var closed = A2K.kr(In.ar(\gatebus.kr(0), 1)) <= 0;
    var awake = Sweep.kr(\wake.tr(0), 1) < 0.2;   // each \wake: 0.2 s more
    PauseSelf.kr(closed * A2K.kr(DetectSilence.ar(sig, 1e-5, 0.1)) * (1 - awake) * LFPulse.kr(4) * \pauser.kr(0));
};
// SteamPipeStep holds control values without K2A interpolation. A note edge
// must latch the requested gate level/velocity, not the first sample of a ramp.
// Keep velocity available in the gate's first sample: short Steam ADSRs can
// finish before the shared ModGate's delayed velocity latch fires.
(1..16).do { |n|
    SynthDef(("SteamPipeBoxGate"++n).asSymbol, {
        var values=NamedControl.kr(\gate,0!n);
        var cable=NamedControl.ar(\gate_ar,0!n)*\gate_sel.kr(0);
        var drone=\drone.kr(0)>0.5;
        var gate=(((SteamPipeStep.ar(values)+cable)>0)+SteamPipeStep.ar(drone))>0;
        var trig=((HPZ1.ar(gate)>0)+(Impulse.ar(0)*gate))>0;   // a note: an edge, or a gate open at start (a cable already high)
        var level=Select.ar(drone,[Select.ar(\gate_sel.kr(0)>0,[SteamPipeStep.ar(values),Latch.ar(cable,trig)]),DC.ar(1)!n]);
        var vel=Select.ar(\vel_sel.kr(0)>0,[SteamPipeStep.ar(NamedControl.kr(\velocity,1!n)),NamedControl.ar(\vel_ar,1!n)]);
        Out.ar(\out.kr(0),gate*level.max(0));
        Out.ar(\velout.kr(0),vel*level);
        Out.kr(\vel.kr(0),A2K.kr(vel*level));
        Out.kr(\rnd.kr(0),A2K.kr(TRand.ar(-1,1,trig)));
    }).writeDefFile(dir);
};
(1..16).do { |n|
    SynthDef(("SteamPipeBoxSpread"++n).asSymbol, {
        var input=In.ar(\in.kr(0),16), voices=\voices.kr(6).clip(1,16);
        // ring weight: 1 Ring, 0 Line; fractional while a snapshot morph
        // crossfades the two layouts' gains (no jump at the midpoint)
        var ring=if(n>2,{1-\line.kr(1,1/30,fixedLag:true).clip(0,1)},{0});
        var pan=NamedControl.kr(\pan,0.5!16)+In.kr(\mpan.kr(0),16);
        var pans=(pan.clip(0,1)*(1-ring))+(pan.wrap(0,1)*ring);   // shown in the window
        var output=0!n;
        16.do { |v|
            var sig=input[v]*(v<voices);
            var posLine=pan[v].clip(0,1)*(n-1), posRing=pan[v].wrap(0,1)*n;
            n.do { |ch|
                var lineGain=(1-(posLine-ch).abs).max(0);
                var d=(posRing-ch).abs, ringGain=(1-d.min(n-d)).max(0);
                output[ch]=output[ch]+(sig*((lineGain*(1-ring))+(ringGain*ring)));
            };
        };
        Out.ar(\out.kr(0),output);
        Out.kr(\vis.kr(0),Array.fill(n,{|i| Amplitude.kr(output[i])}));
        Out.kr(\vispan.kr(0),pans);
    }).writeDefFile(dir);
};
SynthDef(\SteamPipeBoxVoice, {
    var m={|name| In.kr(NamedControl.kr(name,0),1)};
    var p={|name,def| NamedControl.kr(name,def)};
    var gate=In.ar(\gatebus.kr(0),1);
    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
    var gt = gg[14] * 0.01 * Rand(-1, 1);   // each voice's detune (the transpose is in gq)
    var gq = { |p| var r = p.round, q = p + (((((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1])) > 0.5) * Select.ar(r.mod(12) + ((r < gg[17]) * 12), K2A.ar(gg[2..13] ++ gg[18..29]))), w = gg[16] + (((q - gg[16]) / 12).floor * 12); q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };
    var pitch=gq.(Select.ar(\pitch_sel.kr(0),[SteamPipeStep.ar(\pitch.kr(60)),\pitch_ar.ar(60)])) + SteamPipeStep.ar(m.(\mpitch)+gt);
    var velocity=In.ar(\velabus.kr(0),1);
    var voice=\voice.kr(0), pairs, sig, dry, amp;
    // SteamPipeVoice reads a control-rate input once per block, as
    // SteamPipeStep would copy it: no audio-rate wrappers here
    pairs=[
        \pitch, pitch,
        \gate, gate,
        \velocity, velocity,
        \mod_wheel, (p.(\mod_wheel,0)+m.(\mmw)).clip(0,1),
        \bend, p.(\bend,0).clip(-24,24),
        \attack, p.(\attack,25.35433053970337).clip(-20,100),
        \decay, p.(\decay,20.62991976737976).clip(-20,100),
        \sustain, p.(\sustain,0.9055117964744568).clip(0,1),
        \release, p.(\release,20.62991976737976).clip(-20,100),
        \velocity_amount, p.(\velocity_amount,0.33070865273475647).clip(0,1),
        \env_key_scaling, p.(\env_key_scaling,-0.5038866039338075).clip(-0.9998999834060669,0),
        \dc_noise, p.(\dc_noise,0.6692913174629211).clip(0,1),
        \noise_cut, (p.(\noise_cut,80.31495809555054)+m.(\mnoise)).clip(0,120),
        \noise_key_track, p.(\noise_key_track,0.33070865273475647).clip(0,2),
        \noise_velocity_track, p.(\noise_velocity_track,0.30708661675453186).clip(0,1),
        \noise_resonance, (p.(\noise_resonance,0)+m.(\mnoiseres)).clip(0,1),
        \noise_one_pole, p.(\noise_one_pole,1.0).clip(0,1),
        \noise_env, p.(\noise_env,0).clip(0,120),
        \legato, p.(\legato,0),
        \steampipe2, p.(\steampipe2,0),
        \tune, (p.(\tune,1.4173221588134766)+m.(\mtune)).clip(-60,60),
        \fine, p.(\fine,-0.10236221551895142).clip(-1,1),
        \sample_correction, p.(\sample_correction,-1.5354330837726593).clip(-5,0),
        \tune_mw, p.(\tune_mw,-1.2125983238220215).clip(-2,2),
        \vibrato_amount, p.(\vibrato_amount,0.1599999964237213).clip(0,1),
        \vibrato_frequency, p.(\vibrato_frequency,2.400000214576721).clip(0,12),
        \polarity, p.(\polarity,1).clip(-1,1),
        \allpass_tune, (p.(\allpass_tune,0.4724407196044922)+m.(\maptune)).clip(-60,60),
        \allpass_fine, p.(\allpass_fine,0.007874011993408203).clip(-1,1),
        \allpass_sample_correction, p.(\allpass_sample_correction,0).clip(-5,0),
        \allpass_mw, p.(\allpass_mw,-0.04724407196044922).clip(-2,2),
        \allpass_on, p.(\allpass_on,0.0).clip(0,1).lag(0.03),
        \diffusion, (p.(\diffusion,0.007874011993408203)+m.(\mdiffusion)).clip(-1,1),
        \offset, (p.(\offset,0.41732287406921387)+m.(\moffset)).clip(-1,1),
        \push, (p.(\push,1.574803113937378)+m.(\mpush)).clip(0,2),
        \feedback_rt, (p.(\feedback_rt,-60.70866107940674)+m.(\mfeedback)).clip(-90,30),
        \feedback_key_track, p.(\feedback_key_track,1.0078740119934082).clip(0,2),
        \release_damp, p.(\release_damp,6.614173203706741).clip(0,120),
        \soft_hard, p.(\soft_hard,0).clip(0,1),
        \symmetry, p.(\symmetry,0.3385826647281647).clip(0,1),
        \hp0, (p.(\hp0,12.283464968204498)+m.(\mhp)).clip(0,120),
        \hp1, p.(\hp1,66.1417293548584).clip(0,120),
        \hp_key_track, p.(\hp_key_track,1.0078740119934082).clip(0,2),
        \lp0, (p.(\lp0,66.1417293548584)+m.(\mlp)).clip(0,120),
        \lp1, p.(\lp1,96.37794971466064).clip(0,120),
        \lp_key_track, p.(\lp_key_track,0.7716536521911621).clip(-2,2),
        \gain, p.(\gain,-3.3070878982543945).clip(-60,12),
        \amp_original, \amp_original.kr(1),
        \amp_curve0, \amp_curve0.kr(-4),
        \amp_curve1, \amp_curve1.kr(-4),
        \amp_curve2, \amp_curve2.kr(-4)
    ];
    sig=SteamPipeVoice.ar(SteamPipeVoice.parameterNames.collect {|key| pairs[pairs.indexOf(key)+1]});
    // \vca fractional: a snapshot morph crossfades the two amp paths
    amp= { |vca| ((1+m.(\mamp)).clip(0,2)*(1-vca))+(m.(\mamp).clip(0,1)*vca) }.(\vca.kr(0,1/30,fixedLag:true).clip(0,1));
    // Original envelope excites the pipe: never multiply its ringing output by gate.
    dry=Sanitize.ar(sig[0]);
    pause.(dry);
    Out.ar(\out.kr(0),dry*amp*\master.kr(1,0.02)*\normalization.kr(1));
    Out.kr(\vis.kr(0)+(voice*6),[A2K.kr(pitch).midicps,0,A2K.kr(sig[1]),Amplitude.kr(sig[0]),0,0]);
}).writeDefFile(dir);
"SteamPipeBox compiled (OceanodeSteamPipe)".postln;
)
