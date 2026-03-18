OceanodeParameter {
	*ar {arg name, default, size, min, max, units;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: "a"++units))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}

	*kr {arg name, default, size, min, max, units;
		^name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: units));
	}
}

OceanodeParameterLag {
	*ar {arg name, default, size, min, max, units, lagtime = 0, fixedLag = false;
		var raw, d, dNorm, enabled, sensitivity, minLag, maxLag,
		    enabledAr, sensitivityAr, minLagAr, maxLagAr, adaptiveLagTime;
		// No lag: on the kr — handled in AR domain so it can be adaptive at runtime
		raw         = K2A.ar(name.asSymbol.kr(default!size, fixedLag: fixedLag,
		                spec: ControlSpec(min, max, default: default, units: "a"++units)));
		enabled     = \lagEnabled.kr(0);        // 0 = fixed lag (default), 1 = adaptive
		sensitivity = \lagSensitivity.kr(20);
		minLag      = \lagMinTime.kr(0.001);
		maxLag      = \lagMaxTime.kr(lagtime);  // defaults to SCD lagtime if C++ hasn't set it
		// Upsample all KR controls to AR before entering the Select.ar graph
		enabledAr     = K2A.ar(enabled);
		sensitivityAr = K2A.ar(sensitivity);
		minLagAr      = K2A.ar(minLag);
		maxLagAr      = K2A.ar(maxLag);
		// HPZ1.ar = 0.5*(in - prevSample), so *2 gives |raw - prevRaw| per sample.
		// Explicit .ar avoids any ambiguity SC might have with Delay1 rate.
		d     = HPZ1.ar(raw).abs * 2;
		dNorm = d / max(0.0001, max - min);
		// Adaptive lag time — Lag.ar uses a simple one-pole IIR with no internal HPZ1,
		// so it safely accepts an AR UGen as its lagTime argument.
		// enabled=0: always maxLag (original fixed behaviour)
		// enabled=1: linlin map — large delta → minLag, no delta → maxLag
		adaptiveLagTime = Select.ar(enabledAr, [
			maxLagAr,
			linlin((dNorm * sensitivityAr).clip(0, 1), 0, 1, maxLagAr, minLagAr)
		]);
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				Lag.ar(raw, adaptiveLagTime),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}

	*kr {arg name, default, size, min, max, units, lagtime = 0, fixedLag = false;
		^name.asSymbol.kr(default!size, lag: lagtime, fixedLag: fixedLag, spec: ControlSpec(min, max, default: default, units: units));
	}
}

OceanodeParameterDropdown {
	*ar {arg name, default, size, options;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "ad:"++options))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
	*kr {arg name, default, size, options;
		^name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "d:"++options));
	}
}

OceanodeParameterFloatDropdown {
	*ar {arg name, default, size, options;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "adf:"++options))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
	*kr {arg name, default, size, options;
		^name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "df:"++options));
	}
}


OceanodeInput{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "input"));
	}
}

OceanodeOutput{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "output"));
	}
}

OceanodeBuffer{
  *kr {arg name, default = -1;
    ^name.asSymbol.kr(default, spec: ControlSpec(units: "buffer"));
  }
}

OceanodeInternalBuffer{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "internalbuffer"));
	}
}