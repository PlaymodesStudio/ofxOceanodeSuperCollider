// FlutePro test defs, mono. Written to d (set by the caller).
var orig = {|asum1, pch, ifeedbk1, ifeedbk2, lpf|
	// flute.scd's loop, verbatim
	var sr = SampleRate.ir, cr = ControlRate.ir, block = cr.reciprocal, ifqc = pch.midicps;
	var afqc, fdbckArray, aflute1, asum2, ax, apoly, asum3, avalue;
	afqc = ifqc.reciprocal - (asum1/20000) - (9/sr) + (ifqc/12000000) - block;
	fdbckArray = LocalIn.ar(1);
	aflute1 = fdbckArray;
	asum2 = asum1 + (aflute1 * ifeedbk1);
	ax = DelayC.ar(asum2, 0.07, afqc * 0.5 - (asum1/ifqc/cr) + 0.001);
	apoly = ax - (ax.cubed);
	asum3 = apoly + (aflute1 * ifeedbk2);
	avalue = Sanitize.ar(LPF.ar(asum3, lpf));
	aflute1 = DelayC.ar(avalue, 0.125, afqc);
	LocalOut.ar(Sanitize.ar(aflute1));
	avalue
};
var breath = {
	var aflow1 = LFClipNoise.ar(SampleRate.ir, \noise.kr(1));
	var kvibr = SinOsc.ar(\vibfreq.kr(5), 0, 0.1 * \vibamp.kr(0.2));
	(\ibreath.kr(0.09) * aflow1) + \noise.kr(1) + kvibr
};
SynthDef(\FluteAB, {
	var e = breath.value, p = \pitch.ar(32);
	var a = orig.(e, p, \fb1.kr(0.4), \fb2.kr(0.4), \lpf.kr(2000));
	var b = FluteVoice.ar(e, p, \fb1.kr(0.4), \fb2.kr(0.4), \lpf.kr(2000), mode: 1);
	Out.ar(0, [a, b]);
}).writeDefFile(d);
SynthDef(\FluteOrig, {
	var e = breath.value, p = \pitch.ar(32);
	Out.ar(0, orig.(e, p, \fb1.kr(0.4), \fb2.kr(0.4), \lpf.kr(2000)));
}).writeDefFile(d);
SynthDef(\FluteNew, {
	var e = breath.value, p = \pitch.ar(32);
	Out.ar(0, FluteVoice.ar(e, p, \fb1.kr(0.4), \fb2.kr(0.4), \lpf.kr(2000), \jet.kr(0.5), \jetoffs.kr(-1),
		\overblow.kr(0), \track.kr(0), \limit.kr(2), 0));
}).writeDefFile(d);
SynthDef(\FluteOrigAx, {
	// flute.scd's loop, the jet's swing into the cubic as output (for the bound)
	var asum1 = (\ibreath.kr(0.09) * LFClipNoise.ar(SampleRate.ir, \noise.kr(1))) + \noise.kr(1) + SinOsc.ar(\vibfreq.kr(5), 0, 0.1 * \vibamp.kr(0.2));
	var pch = \pitch.ar(32), ifeedbk1 = \fb1.kr(0.4), ifeedbk2 = \fb2.kr(0.4), lpf = \lpf.kr(2000);
	var sr = SampleRate.ir, cr = ControlRate.ir, block = cr.reciprocal, ifqc = pch.midicps;
	var afqc, aflute1, ax, avalue;
	afqc = ifqc.reciprocal - (asum1/20000) - (9/sr) + (ifqc/12000000) - block;
	aflute1 = LocalIn.ar(1);
	ax = DelayC.ar(asum1 + (aflute1 * ifeedbk1), 0.07, afqc * 0.5 - (asum1/ifqc/cr) + 0.001);
	avalue = Sanitize.ar(LPF.ar(ax - ax.cubed + (aflute1 * ifeedbk2), lpf));
	LocalOut.ar(Sanitize.ar(DelayC.ar(avalue, 0.125, afqc)));
	Out.ar(0, [ax, avalue]);
}).writeDefFile(d);
