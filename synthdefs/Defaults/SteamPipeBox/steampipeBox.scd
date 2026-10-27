// SteamPipeBox: sample-level pipe in OceanodeSteamPipe, shared box modulation.
// Evaluate this file; requires the OceanodeSteamPipe extension. No running server needed.
(
var here=thisProcess.nowExecutingPath.dirname;
var root=here +/+ "CompiledSynthdefs", dir;
dir=root +/+ "SteamPipeBox";File.mkdir(root);File.mkdir(dir);
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
        var trig=HPZ1.ar(gate)>0;
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
    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 15) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
    var gt = gg[0] + (gg[14] * 0.01 * Rand(-1, 1));   // + each voice's detune
    var gq = { |p| var r = p.round; Select.ar(gg[1] > 0.5, [p, r + Select.ar(r.mod(12), K2A.ar(gg[2..13]))]) };
    var pitch=gq.(Select.ar(\pitch_sel.kr(0),[SteamPipeStep.ar(\pitch.kr(60)),\pitch_ar.ar(60)])) + SteamPipeStep.ar(m.(\mpitch)+gt);
    var velocity=In.ar(\velabus.kr(0),1);
    var voice=\voice.kr(0), pairs, sig, amp;
    pairs=[
        \pitch, pitch,
        \gate, gate,
        \velocity, velocity,
        \mod_wheel, SteamPipeStep.ar((p.(\mod_wheel,0)+m.(\mmw)).clip(0,1)),
        \bend, SteamPipeStep.ar(p.(\bend,0).clip(-24,24)),
        \attack, SteamPipeStep.ar(p.(\attack,25.35433053970337).clip(-20,100)),
        \decay, SteamPipeStep.ar(p.(\decay,20.62991976737976).clip(-20,100)),
        \sustain, SteamPipeStep.ar(p.(\sustain,0.9055117964744568).clip(0,1)),
        \release, SteamPipeStep.ar(p.(\release,20.62991976737976).clip(-20,100)),
        \velocity_amount, SteamPipeStep.ar(p.(\velocity_amount,0.33070865273475647).clip(0,1)),
        \env_key_scaling, SteamPipeStep.ar(p.(\env_key_scaling,-0.5038866039338075).clip(-0.9998999834060669,0)),
        \dc_noise, SteamPipeStep.ar(p.(\dc_noise,0.6692913174629211).clip(0,1)),
        \noise_cut, SteamPipeStep.ar((p.(\noise_cut,80.31495809555054)+m.(\mnoise)).clip(0,120)),
        \noise_key_track, SteamPipeStep.ar(p.(\noise_key_track,0.33070865273475647).clip(0,2)),
        \noise_velocity_track, SteamPipeStep.ar(p.(\noise_velocity_track,0.30708661675453186).clip(0,1)),
        \noise_resonance, SteamPipeStep.ar((p.(\noise_resonance,0)+m.(\mnoiseres)).clip(0,1)),
        \noise_one_pole, SteamPipeStep.ar(p.(\noise_one_pole,1.0).clip(0,1)),
        \noise_env, SteamPipeStep.ar(p.(\noise_env,0).clip(0,120)),
        \legato, SteamPipeStep.ar(p.(\legato,0)),
        \steampipe2, SteamPipeStep.ar(p.(\steampipe2,0)),
        \tune, SteamPipeStep.ar((p.(\tune,1.4173221588134766)+m.(\mtune)).clip(-60,60)),
        \fine, SteamPipeStep.ar(p.(\fine,-0.10236221551895142).clip(-1,1)),
        \sample_correction, SteamPipeStep.ar(p.(\sample_correction,-1.5354330837726593).clip(-5,0)),
        \tune_mw, SteamPipeStep.ar(p.(\tune_mw,-1.2125983238220215).clip(-2,2)),
        \vibrato_amount, SteamPipeStep.ar(p.(\vibrato_amount,0.1599999964237213).clip(0,1)),
        \vibrato_frequency, SteamPipeStep.ar(p.(\vibrato_frequency,2.400000214576721).clip(0,12)),
        \polarity, SteamPipeStep.ar(p.(\polarity,1).clip(-1,1)),
        \allpass_tune, SteamPipeStep.ar((p.(\allpass_tune,0.4724407196044922)+m.(\maptune)).clip(-60,60)),
        \allpass_fine, SteamPipeStep.ar(p.(\allpass_fine,0.007874011993408203).clip(-1,1)),
        \allpass_sample_correction, SteamPipeStep.ar(p.(\allpass_sample_correction,0).clip(-5,0)),
        \allpass_mw, SteamPipeStep.ar(p.(\allpass_mw,-0.04724407196044922).clip(-2,2)),
        \allpass_on, SteamPipeStep.ar(p.(\allpass_on,0.0).clip(0,1).lag(0.03)),
        \diffusion, SteamPipeStep.ar((p.(\diffusion,0.007874011993408203)+m.(\mdiffusion)).clip(-1,1)),
        \offset, SteamPipeStep.ar((p.(\offset,0.41732287406921387)+m.(\moffset)).clip(-1,1)),
        \push, SteamPipeStep.ar((p.(\push,1.574803113937378)+m.(\mpush)).clip(0,2)),
        \feedback_rt, SteamPipeStep.ar((p.(\feedback_rt,-60.70866107940674)+m.(\mfeedback)).clip(-90,30)),
        \feedback_key_track, SteamPipeStep.ar(p.(\feedback_key_track,1.0078740119934082).clip(0,2)),
        \release_damp, SteamPipeStep.ar(p.(\release_damp,6.614173203706741).clip(0,120)),
        \soft_hard, SteamPipeStep.ar(p.(\soft_hard,0).clip(0,1)),
        \symmetry, SteamPipeStep.ar(p.(\symmetry,0.3385826647281647).clip(0,1)),
        \hp0, SteamPipeStep.ar((p.(\hp0,12.283464968204498)+m.(\mhp)).clip(0,120)),
        \hp1, SteamPipeStep.ar(p.(\hp1,66.1417293548584).clip(0,120)),
        \hp_key_track, SteamPipeStep.ar(p.(\hp_key_track,1.0078740119934082).clip(0,2)),
        \lp0, SteamPipeStep.ar((p.(\lp0,66.1417293548584)+m.(\mlp)).clip(0,120)),
        \lp1, SteamPipeStep.ar(p.(\lp1,96.37794971466064).clip(0,120)),
        \lp_key_track, SteamPipeStep.ar(p.(\lp_key_track,0.7716536521911621).clip(-2,2)),
        \gain, SteamPipeStep.ar(p.(\gain,-3.3070878982543945).clip(-60,12)),
        \amp_original, SteamPipeStep.ar(\amp_original.kr(1)),
        \amp_curve0, SteamPipeStep.ar(\amp_curve0.kr(-4)),
        \amp_curve1, SteamPipeStep.ar(\amp_curve1.kr(-4)),
        \amp_curve2, SteamPipeStep.ar(\amp_curve2.kr(-4))
    ];
    sig=SteamPipeVoice.ar(SteamPipeVoice.parameterNames.collect {|key| pairs[pairs.indexOf(key)+1]});
    // \vca fractional: a snapshot morph crossfades the two amp paths
    amp= { |vca| ((1+m.(\mamp)).clip(0,2)*(1-vca))+(m.(\mamp).clip(0,1)*vca) }.(\vca.kr(0,1/30,fixedLag:true).clip(0,1));
    // Original envelope excites the pipe: never multiply its ringing output by gate.
    Out.ar(\out.kr(0),Sanitize.ar(sig[0])*amp*\master.kr(1,0.02)*\normalization.kr(1));
    Out.kr(\vis.kr(0)+(voice*6),[A2K.kr(pitch).midicps,0,A2K.kr(sig[1]),Amplitude.kr(sig[0]),0,0]);
}).writeDefFile(dir);
"SteamPipeBox compiled (OceanodeSteamPipe)".postln;
)
