s.options.numWireBufs = 8192
s.options.memSize = 2097152

~maxValue = 2147483647;
d = thisProcess.nowExecutingPath.dirname ++ "/Synthdefs";

(
SynthDef.new("simple", {
	arg out;
	var p,level, sig;
	p=\pitch.kr(40,  spec: ControlSpec(0, 127, default: 36));
	level = \level.kr(0, 1/30, fixedLag:true,  spec: ControlSpec(0, 1, default: 0));
	sig = Saw.ar(p.midicps, mul: level);
	Out.ar(out, sig);
}, metadata: (name: "Simple", type: "source", numInputs: 0, numOutputs: 1,  numBuffers: 0)).writeDefFile(d);
)

(
SynthDef.new(\filter, {
	arg in,out=0;
	var input,filters,filtered,freq,res,type;
	input = In.ar(in, 1);
	freq=\pitch.kr(127, 0.05, fixedLag:true, spec: ControlSpec(0, 127, default: 127)).midicps;
	res=\q.kr(1,  spec: ControlSpec(0, 1, default: 1));

	Out.ar(out, RLPF.ar(input,freq,res,1,0),);
}, metadata: (name: "Filter", type: "effect", numInputs: 1, numOutputs: 1,  numBuffers: 0)).writeDefFile(d);
)

(// samples
SynthDef(\sampler,
	{
		arg out=0, buf;
		var t, signal, spd,bucle, start, gain;
		t=\trigger.kr(0, spec: ControlSpec(0, 1, step: 1.0, default: 0));
		gain=\levels.kr(0, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0));
		//buf=\bufnum.kr(0!78);
		spd=\speed.kr(1, spec: ControlSpec(-32, 32, default: 1));
		bucle=\loop.kr(0, spec: ControlSpec(0, 1, step: 1.0, default: 0));
		start=\startpos.kr(0, spec: ControlSpec(0, 1, default: 0))*BufFrames.kr(buf);
		signal=PlayBuf.ar(1, buf, spd, t, start,bucle)*gain;
		Out.ar(out, signal);
}, metadata: (name: "Sampler", type: "source", numInputs: 0, numBuffers: 1)).writeDefFile(d);
)

(///// GranularSampler
SynthDef(\grainsampler,
	{
		arg out=0, buf=0;
		var t, signal, spd,bucle, start, gain,dur, envbuf;
		t=\trigger.kr(0, spec: ControlSpec(0, 1, step: 1.0, default: 0));
		gain=\levels.kr(0, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0));
		spd=\speed.kr(1, spec: ControlSpec(-32, 32, default: 1));
		start=\startpos.kr(0, spec: ControlSpec(0, 1, default: 0));
		dur=\grainsize.kr(0.1, spec: ControlSpec(0.00001, 2, default: 0.1));
		envbuf=\envbuf.kr(-1, spec: ControlSpec(-1, ~maxValue, step: 1.0, default: -1));
		signal=GrainBuf.ar(1, t, dur, buf, spd, start,2,0, envbuf,1024)*gain;
		Out.ar(out, signal);
}, metadata: (name: "GrainSampler", type: "source", numInputs: 0, numBuffers: 1)).writeDefFile(d);
)

(//stereo downmixer
SynthDef.new(\stereomix, {
	arg in, out, inChannels, outChannels, index=0, spread = 1, center = 0;
	var sig, size, position;
	sig = In.ar(in, 1);
	size = max(2, inChannels) - 1;
	position = (index * (2 / size) - 1) * spread + center;
	sig=Pan2.ar(sig, position);
	Out.ar(out, sig);
}, metadata: (name: "Stereomix", type: "mixdown", numInputs: 1, numOutputs: 1,  numBuffers: 0)).writeDefFile(d);
)

//Helper funtion to create panners
(
~variChannelEffect = {|name, func, minChannels = 1, maxChannels = 100|
	File.mkdir(d ++ "/" ++ name);
	(minChannels..maxChannels).do({arg n;
		SynthDef.new(name ++ (n).asSymbol, {
			arg in, out, inChannels, index;
			var sig;
			sig = SynthDef.wrap(func, prependArgs: [in, out, inChannels, index, n]);
			Out.ar(out, sig);
		}, metadata: (name: name[0].toUpper ++ name[1..], type: "multi", numInputs: 1, numOutputs: 1, numBuffers: 0)).writeDefFile(d ++ "/" ++ name);
	});
};
)

//Creation of Panners, from Pan2 to Pan4 in this example
(
~variChannelEffect.value("pan", {|in, out, inChannels, index, n|
	var sig, size, position, w;
	w = \width.kr(2, spec: ControlSpec(0, n, default: 2));
	sig = In.ar(in, 1);
	size = max(n, inChannels) - 1;
	position = (index * (2 / size) - 1);
	sig = PanX.ar(n, sig, position, 1.0, w);
}, minChannels: 2, maxChannels: 4);
)


//Infos
(
File.mkdir(d ++ "/" ++ "info");
(1..100).do({arg n;
	var numChan = n;
	SynthDef.new(\info ++ (numChan).asSymbol, {
		arg in, amp, peak, lagTime = 0.2, decay = 0.99;
		var sig;
		sig = In.ar(in, numChan);
		Out.kr(amp, Lag.kr(Amplitude.kr(sig), lagTime));
		Out.kr(peak, PeakFollower.kr(sig, decay));
	}).writeDefFile(d ++ "/info");
})
)


(/// output generator
SynthDef.new(\output, {
	arg in = 0, out=0;
	var sig;
	sig = In.ar(in, 100);
	sig=LeakDC.ar(sig);
	sig=Sanitize.ar(sig);
	sig = DelayN.ar(sig, 5, \delay.kr(0));
	ReplaceOut.ar(out, sig.tanh * \levels.kr(0));
}).add;
)

(
SynthDef.new(\mixer, {
	arg in=0, in2=0, in3=0, in4=0, in5=0, in6=0, in7=0, in8=0, out=0;
	var sig1, sig2, sig3,sig4,sig5,sig6,sig7,sig8, finalsig;
	sig1 = In.ar(in)*\levels1.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig2 = In.ar(in)*\levels2.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig3 = In.ar(in)*\levels3.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig4 = In.ar(in)*\levels4.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig5 = In.ar(in)*\levels5.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig6 = In.ar(in)*\levels6.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig7 = In.ar(in)*\levels7.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	sig8 = In.ar(in)*\levels8.kr(0.5, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0.5));
	finalsig=sig1+sig2+sig3+sig4+sig5+sig6+sig7+sig8;
	finalsig=finalsig*\masterlevel.kr(1, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 1));
	Out.ar(out, finalsig);
}, metadata: (name: "Mixer", type: "mixdown", numInputs: 8, numOutputs: 1,  numBuffers: 0)).writeDefFile(d);
)

