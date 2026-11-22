// Test-only defs for tools/synthpercbox/render_test.py: a gate + pitch on audio buses, opening at
// an exact sample (\at samples after its start), like a cable from a sequencer.
(
var dir = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" +/+ "SynthPercBoxTest";
File.mkdir(thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs"); File.mkdir(dir);
SynthDef(\SPTestCable, {
	var n = Sweep.ar(0, SampleRate.ir);                       // samples since start
	var open = (n >= \at.ir(0)) * (n < (\at.ir(0) + \len.ir(14400)));
	Out.ar(\gbus.ir(0), open * \level.ir(1));
	Out.ar(\pbus.ir(0), DC.ar(\pitch.ir(60)));
}).writeDefFile(dir);
)
