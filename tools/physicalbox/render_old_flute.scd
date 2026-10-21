// The old Source/PhysicalModelling/flute.scd voice (one channel, its defaults),
// rendered offline as a reference for PhysicalBox's flute:
//   sclang render_old_flute.scd [outDir]
(
var outDir = (thisProcess.argv[0] ? "/tmp/physicalbox-oldflute").standardizePath;
var notes = [36, 48, 60, 72], run;
File.mkdir(outDir);
SynthDef(\oldFlute, {
	var pch = \pitch.kr(48), ibreath = \ibreath.kr(0.09), ifeedbk1 = \ifeedbk1.kr(0.4), ifeedbk2 = \ifeedbk2.kr(0.4);
	var vibamp = \vibamp.kr(0.2), vibfreq = \vibfreq.kr(5), lpf = \lpf.kr(2000), noise = \noise.kr(1);
	var sr = SampleRate.ir, cr = ControlRate.ir, block = cr.reciprocal, ifqc = pch.midicps;
	var aflow1 = LFClipNoise.ar(sr, noise);
	var kvibr = SinOsc.ar(vibfreq, 0, 0.1 * vibamp);
	var asum1 = (ibreath * aflow1) + noise + kvibr;
	var afqc = ifqc.reciprocal - (asum1 / 20000) - (9 / sr) + (ifqc / 12000000) - block;
	var aflute1 = LocalIn.ar(1);
	var asum2 = asum1 + (aflute1 * ifeedbk1);
	var ax = DelayC.ar(asum2, 0.07, afqc * 0.5 - (asum1 / ifqc / cr) + 0.001);
	var apoly = ax - ax.cubed;
	var asum3 = apoly + (aflute1 * ifeedbk2);
	var avalue = Sanitize.ar(LPF.ar(asum3, lpf));
	aflute1 = DelayC.ar(avalue, 0.125, afqc);
	LocalOut.ar(Sanitize.ar(aflute1));
	Out.ar(0, Limiter.ar(LeakDC.ar(avalue)) ! 2);
}).store;
run = { |i|
	if(i < notes.size) {
		var path = outDir +/+ "old_" ++ notes[i] ++ ".wav";
		Score([[0.0, [\s_new, \oldFlute, 1000, 0, 0, \pitch, notes[i]]], [3, [\c_set, 0, 0]]]).recordNRT(nil, path,
			sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",
			options: ServerOptions.new.numOutputBusChannels_(2), duration: 3, action: { run.(i + 1) });
	} { "done".postln; 0.exit };
};
run.(0);
)
