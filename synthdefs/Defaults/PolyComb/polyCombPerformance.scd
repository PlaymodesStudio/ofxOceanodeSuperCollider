// PolyComb pitch maps, finite-window Delta and 32-wide spatialization.
(
var dir=thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs/PolyComb";
(1..32).do { |n|
    var kv={|name,def|NamedControl.kr(name,def!n)};
    SynthDef(("PCPitchMap"++n).asSymbol,{
        var cable=A2K.kr(\pitch_ar.ar(60!n));
        var count=\pitch_channels.kr(n).clip(1,n);
        var repeated=n.collect{|i|Select.kr(i.min(count-1),cable)};
        var p=Select.kr(\pitch_sel.kr(0)>0.5,[kv.(\pitch,60),repeated]);
        var mapped=p+(kv.(\map,60)-60)*\internal.kr(0);
        var b=Lag.kr(mapped,\glide.kr(0))+kv.(\transpose,0)+(({Rand(-1,1)}!n)*kv.(\detune,0));
        var rate=\vibrate.kr(5)*(1+(({Rand(-1,1)}!n)*\vibrand.kr(0).clip(0,1)));
        var v=SinOsc.kr(rate,{Rand(0,2pi)}!n)*\vibamt.kr(0);
        Out.kr(\out.kr(0),(b+v+In.kr(\mpitch.kr(0),n))++b);
    }).writeDefFile(dir);
    SynthDef(("PCDelta"++n).asSymbol,{
        var x=In.kr(\a.kr(0),n)*\mix1.kr(1)+In.kr(\b.kr(0),n)*\mix2.kr(0);
        var initial=Latch.kr(x,Impulse.kr(0));
        var window=\window.kr(1).clip(ControlDur.ir,1);
        var change=(x-(DelayN.kr(x-initial,1,window)+initial)).abs;
        var pole=(-2pi*ControlDur.ir/window).exp;
        Out.kr(\out.kr(0),OnePole.kr(change,pole)*\gain.kr(1)/4);
    }).writeDefFile(dir);
    SynthDef(("PCVoiceFade"++n).asSymbol,{
        // Every rebuild starts behind silence, then ramps into the completed graph.
        ReplaceOut.ar(\bus.kr(0),In.ar(\bus.kr(0),n)*EnvGen.kr(Env([0,0,1],[0.03,0.04])));
    }).writeDefFile(dir);
};
(1..16).do { |c|
    SynthDef(("PCSpreadWide" ++ c).asSymbol, {
        var v = \voices.kr(1).clip(1, 32);
        var sig = In.ar(\in.kr(0), 32).asArray;
        var pos = (NamedControl.kr(\pan, 0.5 ! 32) + Sanitize.kr(In.kr(\mpan.kr(0), 32))).asArray.collect { |p| p.lag(0.02) };
        var line = \line.kr(0) > 0.5;
        var comp = (v / c).max(1).sqrt.reciprocal;
        var out = 32.collect { |i|
            var x = sig[i] * (i < v);
            var p = pos[i];
            if(c == 1) { [x] } {
                if(c == 2) { Pan2.ar(x, (p.clip(0, 1) * 2) - 1) } {
                    PanAz.ar(c, x, Select.kr(line, [p.wrap(0, 1) * 2, p.clip(0, 1) * 2 * (c - 1) / c]), 1, 2, 0)
                }
            }
        }.sum;
        Out.kr(\vis.kr(0), pos);
        Out.ar(\out.kr(0), out * comp);
    }).writeDefFile(dir);
};
"PolyComb performance definitions written".postln;
)
