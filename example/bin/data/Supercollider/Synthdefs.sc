s.options.numWireBufs = 8192
s.options.memSize = 2097152

~maxValue = 2147483647;
d = thisProcess.nowExecutingPath.dirname ++ "/Synthdefs";

//EXEMPLE 6 juny
(
SynthDef.new(\filter, {
	arg source,out=0;
	var in, input,filters,filtered,freq,res,type;
	input = \source.ar(0, spec: ControlSpec(units: "input")); //posar en funcio que lectura sigui mes facil tipu: "createInput("source");
	buf = \buffer.kr(0, spec: ControlSpec(units: "buffer")); //CreateBuffer("buffer");
	// input = In.ar(source, 1);
	freq=\pitch.ar(127, 0.05, fixedLag:true, spec: ControlSpec(0, 127, default: 127, units: "vf")).midicps;
	res=\q.kr(1,  spec: ControlSpec(0, 1, default: 1));

	Out.ar(\out.kr(0, spec: ControlSpec(units: "output")), RLPF.ar(input,freq,res,1,0),);
}, metadata: (name: "Filter", type: "effect",  numBuffers: 0)).writeDefFile(d);
)
/////

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


//Helper funtion to create synths
(
~synthCreator = {|name, func, description = "", category = ""|
	File.mkdir(d ++ "/" ++ name);
	description = description.replace(" ", "_");
	//Create first synth for metadata.
	SynthDef.new(name, {
			var sig = SynthDef.wrap(func, prependArgs: [1]);
}, metadata: (name: name, type: "source", description: description, category: category)).writeDefFile(d ++ "/" ++ name);
	//Create array of synths without metadata
	(1..100).do({arg n;
		SynthDef.new(name ++ (n).asSymbol, {
			var sig = SynthDef.wrap(func, prependArgs: [n]);
		}).writeDefFile(d ++ "/" ++ name, mdPlugin: AbstractMDPlugin); //AbstractMDPlugin to disable metadata
	});
};
)

//Make shure you have OceanodeParameter pseudo-Ugen in user extensions folder
Platform.userExtensionDir;
//You should have a class containing the file oceanodeParameter.sc with the following contents:

/*
OceanodeParameter {
	*ar {arg name, default, size, min, max, units;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: units))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
}

OceanodeParameterLag {
	*ar {arg name, default, size, min, max, units, lagtime = 0, fixedLag = false;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, lag: lagtime, fixedLag: fixedLag, spec: ControlSpec(min, max, default: default, units: units))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
}
*/

//Simple synth
(
~synthCreator.value("Simple", {|n|
	var pitch,level, sig;
	pitch = OceanodeParameter.ar(\pitch, 40, n, 0, 127, "vf");
	level = OceanodeParameterLag.ar(\level, default: 0, size: n, min: 0, max: 1, units: "vf", lagtime: 1/30, fixedLag: true);
	sig = Saw.ar(pitch.midicps, mul: level);
	Out.ar(\out.kr(0, spec: ControlSpec(units: "output")), sig);
}, description: "This is a simple synth with a simple saw wave", category: "Source/Oscillator"
);
)

//Simple filter
(
~synthCreator.value("Filter", {|n|
	var input, freq, res, filtered, filters;
	input = In.ar(\in.kr(0, spec: ControlSpec(units: "input")), n);
	freq = OceanodeParameterLag.ar(\pitch, 127, n, 0, 127, "vf", 0.05, true).midicps;
	res = OceanodeParameter.ar(\q, 1, n, 0, 1, "vf");

	filters=[
		RLPF.ar(input,freq,res,1,0),
		RHPF.ar(input,freq,res,1,0),
		BPF.ar(input,freq,res,1,0),
		BRF.ar(input,freq,res,1,0),
		BPeakEQ.ar(input,freq,1,res*12,1,0)
	];

	filtered=Select.ar(\type.kr(0, spec: ControlSpec(default: 0, units: "d:Low Pass:High Pass:Band Pass:Band Reject:Parametric")),filters);

	Out.ar(\dry.kr(0, spec: ControlSpec(units: "output")), input);
	Out.ar(\wet.kr(0, spec: ControlSpec(units: "output")), filtered);
}, category: "Effect/Filter");
)

//stereo downmixer
(
~sourceCreator.value("StereoMix", {|n|
	var sig;
	sig = In.ar(\in.kr(0, spec: ControlSpec(units: "input")), n);
	sig=Splay.ar(sig,1,1,0);
	Out.ar(\out.kr(0, spec: ControlSpec(units: "output")), sig);
}, category: "Effect/Downmixer");
)

//Sampler
(
~synthCreator.value("Sampler", {|n|
	var tr, signal, spd, bucle, start, gain, buf;
	tr=\trigger.kr(0!n, spec: ControlSpec(0, 1, step: 1.0, default: 0, units: "vi"));
	gain=\levels.kr(0!n, 1/30, fixedLag:true, spec: ControlSpec(0, 1, default: 0, units: "vf"));
	buf=\bufnum.kr(0!n, spec: ControlSpec(units: "buffer"));
	spd=\speed.kr(1!n, spec: ControlSpec(-32, 32, default: 1, units: "vf"));
	bucle=\loop.kr(0!n, spec: ControlSpec(0, 1, step: 1.0, default: 0, units: "vi"));
	start=\startpos.kr(0!n, spec: ControlSpec(0, 1, default: 0, units: "vf"))*BufFrames.kr(buf);
	signal=PlayBuf.ar(1, buf, spd, tr, start,bucle)*gain;
	Out.ar(\out.kr(0, spec: ControlSpec(units: "output")), signal);
}, category: "Source/Sampling");
)

//Mapper for audio signals
(
~synthCreator.value("Mapper", {|n|
	var input, sig, min, max;
	input = In.ar(\in.kr(0, spec: ControlSpec(units: "input")), n);
	min = OceanodeParameter.ar(\min, 0, n, -1000, 1000, "vf");
	max = OceanodeParameter.ar(\max, 1, n, -1000, 1000, "vf");
	sig = LinLin.ar(input, -1, 1, min, max);
	Out.ar(\out.kr(0, spec: ControlSpec(units: "output")), sig);
});
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
