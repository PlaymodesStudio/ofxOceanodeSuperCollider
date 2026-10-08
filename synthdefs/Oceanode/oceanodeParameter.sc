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
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, lag: lagtime, fixedLag: fixedLag, spec: ControlSpec(min, max, default: default, units: "a"++units))),
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