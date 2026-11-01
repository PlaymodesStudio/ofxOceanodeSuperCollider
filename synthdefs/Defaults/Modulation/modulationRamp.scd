// A shared Ramp source, routed by the same ModRoute matrix as LFOs/envelopes.
// Value: From -> To. Rate: integral of that shaped rate, in spans per cycle.
(
var root,dir;
root=thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
dir=root +/+ "Modulation";File.mkdir(root);File.mkdir(dir);
(1..32).do { |n|
    SynthDef(("ModRamp"++n).asSymbol, {
        var gate=A2K.kr(InFeedback.ar(\gate.kr(0),n))>0;
        var mode=\trigmode.kr(2), reset=Changed.kr(\reset.kr(0))+((HPZ1.kr(gate)>0)*(mode>0));
        var elapsed=Sweep.kr(reset,Select.kr(\transport.kr(0)>0,[1,In.kr(\transportbus.kr(0)+1)])),restart=\restartsec.kr(0);
        var periodic=(HPZ1.kr((elapsed/restart.max(0.001)).floor)>0)*(restart>0)*(mode>0)*gate;
        var freePhase=Sweep.kr(reset+periodic,NamedControl.kr(\freq,1 ! n));
        var clock=In.kr(\transportbus.kr(0),3),linked=\transport.kr(0);
        var absolute=clock[0]*NamedControl.kr(\cpb,1 ! n);
        var origin=Latch.kr(absolute.ceil,reset+periodic+(Changed.kr(clock[2])*clock[1])+Impulse.kr(0));
        var locked=Select.kr(((mode>0)+(\ending.kr(0) |!=| 1))>0,[absolute,(absolute-origin).max(0)]);
        var phase=Select.kr(linked>0,[freePhase,locked])+NamedControl.kr(\phoff,0 ! n);
        var ending=\ending.kr(0),bounds=\bounds.kr(0),rate=\ratemode.kr(0);
        var u=Select.kr(ending |==| 1,[phase.max(0),phase.frac]),t=u.clip(0,1);
        var tension={|p| p=p.clip(-0.9999,0.9999); (2*p*0.99999)/(1-(p*0.999999))};
        var pk=tension.value(NamedControl.kr(\pow,0 ! n)),bk=tension.value(NamedControl.kr(\bipow,0 ! n));
        var from=\from.kr(0),to=\to.kr(1),curve=\curve.kr(0),offset=\offset.kr(0);
        var c=Select.kr(curve.abs<0.01,[curve,1]);
        var shaped,area,terminal,value;
        t=(t*(pk+1))/((pk*t.abs)+1);
        t=(2*t)-1;
        t=(((t*(bk+1))/((bk*t.abs)+1))+1)*0.5;
        shaped=Select.kr(curve.abs<0.01,[((c*t).exp-1)/(c.exp-1),t]);
        area=Select.kr(curve.abs<0.01,[(((c*t).exp-1)/c-t)/(c.exp-1),t*t*0.5]);
        terminal=Select.kr(rate>0,[ (to-from)*Select.kr(curve.abs<0.01,[c*c.exp/(c.exp-1),1]),to]);
        value=offset+Select.kr(rate>0,[from+((to-from)*shaped),(from*t)+((to-from)*area)]);
        terminal=terminal/((pk+1)*(bk+1));
        value=value+((u-1).max(0)*terminal*(ending |==| 2));
        value=Select.kr(bounds.clip(0,2),[value,value.clip(0,1),value.frac]);
        value=value*Select.kr(mode>0,[1,gate])*\enabled.kr(1);
        // Only the first chunk reports completion; the host owns the Go latch.
        // SendReply is independent of editor visibility and uses the audio clock.
        SendReply.kr((HPZ1.kr(phase.asArray[0]>=1)>0)*gate.asArray[0]*(mode>0)*(ending |==| 3)*\enabled.kr(1)*\reportdone.kr(0),'/modRampDone',[phase.asArray[0]]);
        Out.kr(\out.kr(0),Sanitize.kr(value));
    }).writeDefFile(dir);
};
"Modulation Ramps written to ".post;dir.postln;
)
