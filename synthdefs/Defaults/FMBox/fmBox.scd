// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Aziz Ege Gonul's DX7-Supercollider, with conversion tables
// derived there from Hexter/TX7 measurements. See upstream/ for source/license.
// Oceanode adaptation: persistent voices, scheduled/audio gates, shared native
// LFO, live controls, editable algorithms, modulation buses and Box effects.
(
var root, dir, data;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "FMBox";
data = (thisProcess.nowExecutingPath.dirname +/+ "dx7Data.inc").load;
File.mkdir(root); File.mkdir(dir);
SynthDef(\FMBoxDX7LFO, {
    var table = { |key, index| Select.kr(index.round.clip(0,data[key].size-1),data[key]) };
    var gates = In.ar(\gatebus.kr(0),16);
    var note = HPZ1.ar(gates > 0).max(0).sum;
    var reset = (note * \lfo_sync.kr(1)) + Impulse.ar(0);
    var rate = table.(\dx7_voice_lfo_frequency,\lfo_speed.kr(35));
    var wave = \lfo_wave.kr(0).round.clip(0,5);
    var phases = [0.5pi,0.98pi,0.98,0.98pi,0,0];
    var coefficients = [
        Array.series(50,1,2).collect { |i,k| (1/i.squared) * [1,-1].wrapAt(k) },
        Array.series(100,1,1).collect { |i,k| (1/i) * [-1,1].wrapAt(k) },
        Array.series(100,1,1).collect { |i,k| (1/i) * [-1,1].wrapAt(k) },
        Array.series(50,1,2).reciprocal, [1]
    ];
    var harmonics = [Array.series(50,1,2),(1..100),(1..100),Array.series(50,1,2),[1]];
    var tables = coefficients.collect { |amps,i|
        var sineAmps = Array.fill(100,0);
        harmonics[i].do { |harmonic,k| sineAmps[harmonic-1]=amps[k] };
        LocalBuf.newFrom(Signal.sineFill(256,sineAmps,if(i==2,{pi!100},{0!100})).normalize.asWavetable)
    };
    var sweep = Phasor.ar(reset,rate*SampleDur.ir,0,1);
    var buffer = Select.kr(wave.clip(0,4),tables);
    var signal = Osc.ar(buffer,0,(sweep*2pi)+Select.kr(wave,phases))*0.5+0.5;
    var random = TRand.ar(0,1,HPZ1.ar(sweep)<0);
    var delay = \lfo_delay.kr(0).clip(0,99).linlin(0,99,0.01,3.4);
    var ramp = EnvGen.ar(Env([0,1],[delay],-5),note+Impulse.ar(0));
    var depth = table.(\dx7_voice_pms_to_semitones,\lfo_pms.kr(0)) * (\lfo_pmd.kr(0)/99);
    var tameM = Select.kr(wave,[1,0,0,1,1,1]), tameC = Select.kr(wave,[0,1,1,0,0,0]);
    var lfo = Select.ar(wave>4.5,[signal,random])*ramp;
    var pitch = LinLin.ar(lfo,0,1,(depth.neg.midiratio*tameM)+tameC,depth.midiratio);
    var amp = LinLin.ar(lfo,0,1,table.(\dx7_voice_amd_to_ol_adjustment,\lfo_amd.kr(0)) * (-3/4),0);
    Out.ar(\out.kr(0),[pitch,amp]);
}).writeDefFile(dir);
SynthDef(\FMBoxVoice, {
    var p = { |name, def| NamedControl.kr(name, def) };
    var m = { |name| In.kr(NamedControl.kr(name,0),1) };
    var table = { |key, index| Select.kr(index.round.clip(0,data[key].size-1),data[key]) };
    var integer = { |value| value.abs.floor * value.sign };
    // ModGate latches Oceanode velocity after its audio gate rises. Align the
    // native envelopes with that latch, including sample-accurate audio gates.
    var gate = DelayN.ar(In.ar(\gatebus.kr(0),1),0.1,ControlDur.ir*2);
    var vel = In.kr(\velbus.kr(0),1).clip(0,1)*127;
    var native = In.ar(\nativebus.kr(0),2);
    var pitch = Select.kr(\pitch_sel.kr(0),[\pitch.kr(60),A2K.kr(\pitch_ar.ar(60))]);
    var note = pitch + p.(\transpose,24)-24 + m.(\mpitch);
    var timeScale = p.(\env_time,1).clip(0.01,20);
    var feedback = (p.(\feedback,0)+m.(\mfeedback)).clip(0,7);
    var fb = table.(\feedbackIndex,feedback)*feedback;
    var fbCell = table.(\feedbackSel,p.(\algorithm,0));
    var pitchLevels = Array.fill(4,{ |k| table.(\dx7_voice_pitch_level_to_shift,p.(("pitch_l"++k).asSymbol,50)) });
    var pitchTimes = Array.fill(4,{ |k|
        var prev = pitchLevels.wrapAt(k-1);
        var speed = 99-p.(("pitch_r"++k).asSymbol,0);
        ((speed-70.337897)/ -25.580953).exp * ((pitchLevels[k]-prev)/96).abs * 2.45 * timeScale
    });
    var pitchEnv = EnvGen.kr(Env([pitchLevels[3]]++pitchLevels,pitchTimes.max(0.00001),0,3),A2K.kr(gate>0));
    var freq = note.clip(-100,160).midicps * pitchEnv.midiratio;
    var envs = Array.newClear(6);
    var ctls = Array.fill(6,{ |op|
        var pre = "op"++op++"_";
        var q = { |key,def| p.((pre++key).asSymbol,def) };
        var transposed = note+3;
        var breakpt = q.("breakpoint",39);
        var scaling = { |dummy,curve,depth|
            var lin = integer.(dummy/45*depth);
            var exp = integer.(((dummy-72)/13.5).exp*depth);
            Select.kr(curve.round.clip(0,3),[lin.neg,exp.neg,exp,lin])
        };
        var output = (q.("output",99)+m.(("mlevel"++op).asSymbol)).clip(0,99);
        var left = (transposed< (breakpt+21)) * scaling.(breakpt-(integer.((transposed+2)/3)*3)+21,q.("left_curve",0),q.("left_depth",0));
        var right = (transposed> (breakpt+21)) * scaling.((integer.((transposed+2)/3)*3)-breakpt-21,q.("right_curve",0),q.("right_depth",0));
        var volume = (output+left+right).clip(0,99);
        var velFin = table.(\velTable,vel) * q.("velocity",0);
        var rateScale = integer.((q.("rate_scale",0)*(transposed-21)/105*127/128*6.5)-0.5);
        var levels = Array.fill(4,{ |k| q.("l"++k,if(k==3,0,99)).clip(0,99) });
        var db = levels.collect { |level| (99-(level+volume-99+velFin))/1.33333333 };
        var envMap = { |level,next=false|
            Select.kr((level>5)+(level>16)+(level>20),[
                64*(2*level+if(next,5,0)),64*(5+level),64*(4+level),64*(14+(level/2).floor)])
        };
        var volumeDec = (volume+velFin).clip(0,127);
        var vrDec = Select.kr((volumeDec>5)+(volumeDec>16)+(volumeDec>20),[
            32*volumeDec,32*(volumeDec+11),32*(volumeDec+17),32*(volumeDec+28)]);
        var times = Array.fill(4,{ |k|
            var from = levels.wrapAt(k-1),to=levels[k];
            var a=envMap.(from),b=envMap.(to,true);
            var speed=(rateScale+99-q.("r"++k,if((k==0)or:(k==3),0,60))).clip(0,99);
            var decay=(((a+vrDec-4064).clip(1,9000)-(b+vrDec-4064).clip(1,9000))*0.0235)/(0.2819*(2**(speed*0.16)));
            var rise=table.(\voice_eg_rate_rise_duration,speed)*(table.(\dx7_voice_eg_rate_rise_percent,integer.((to*volume/99+velFin).clip(0,99)))-table.(\dx7_voice_eg_rate_rise_percent,integer.((from*volume/99+velFin).clip(0,99))));
            Select.kr(a>b,[rise,decay]).max(0.00001)*timeScale
        });
        var curves=Array.fill(4,{ |k| Select.kr(db.wrapAt(k-1)>db[k],[-3,3]) });
        var env=EnvGen.kr(Env(([db[3]]++db).neg.dbamp,times,curves,3),A2K.kr(gate>0));
        var fixed=q.("mode",0)>0.5;
        var coarse=(q.("coarse",1)+m.(("mratio"++op).asSymbol)).clip(0,31);
        var fine=q.("fine",0).clip(0,99);
        var ratio=Select.kr(coarse<0.5,[coarse,0.5])*(1+(fine/100));
        // Preserve the source's frequency equations; remove its later duplicate
        // assignments which inadvertently replaced fixed coarse/fine by ratios.
        var fixedCoarse=Select.kr(coarse.round%4,[1,10,100,1000]);
        var fixedFine=10**(fine/100);
        var detune=(q.("detune",7)-7+m.(("mdetune"++op).asSymbol))/32;
        var hz=Select.kr(fixed,[ratio*(freq+detune),fixedCoarse*fixedFine*pitchEnv.midiratio]);
        var phase=TRand.kr(0,2pi,(HPZ1.kr(A2K.kr(gate>0))>0)+Impulse.kr(0))*(1-p.(\osc_sync,1));
        envs[op]=env;
        [hz*Lag2.ar(native[0],0.01),phase,env*(Lag2.ar(native[1],0.01)*(q.("amp_sens",0)/3)).dbamp]
    });
    var matrix = Array.fill(6,{ |dst| Array.fill(6,{ |src|
        var amount=p.(("fm"++dst++"_"++src).asSymbol,if((dst==0)and:(src==1),1,0)).clip(0,4);
        amount*4pi*(p.(\mod_scale,1)+m.(\mscale)).clip(0,10)*Select.kr(((dst*6+src)-fbCell).abs<0.5,[1,fb])
    }) });
    var carriers=Array.fill(6,{ |op| p.(("mix"++op).asSymbol,if((op==0)or:(op==2),1,0)).clip(0,1) });
    var amp=Select.kr(\vca.kr(0)>0.5,[(1+m.(\mamp)).clip(0,2),m.(\mamp).clip(0,1)]);
    var sig=Sanitize.ar((FM7.ar(ctls,matrix)*carriers).sum)*(-12).dbamp/carriers.sum.max(1);
    sig=LeakDC.ar(sig)*amp*\master.kr(0.6,0.02)*\normalization.kr(1);
    Out.ar(\out.kr(0),sig);
    Out.kr(\vis.kr(0)+(\voice.kr(0)*6),envs);
}).writeDefFile(dir);
"FMBox SynthDefs written to ".post;dir.postln;
)
