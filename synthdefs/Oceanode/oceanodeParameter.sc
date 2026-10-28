// Global Pitch for a note control's audio cable (Oceanode values get it in
// C++, scGlobalPitch::applyInRange). The node sets \gtrbus to the block of
// buses it follows (scGlobalPitch.h: transpose, scale on, 12 pitch-class
// offsets, detune cents, wrap on, wrap base, split note, 12 low-scale
// offsets, low scale on); without one the cable passes untouched. The notes
// are worked out at control rate: a note changes per block, and a band bank's
// \pitcharray would cost too much at audio rate.
OceanodeGlobalPitch {
	*isNote {arg name; ^[\pitch, \pitcharray, \carp].includes(name.asSymbol) }

	*ar {arg cable, size, min, max;
		var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);
		var p = A2K.kr(cable).asArray;
		var r = p.round, low = r < gg[17];
		var on = (low * gg[30]) + ((1 - low) * gg[1]);
		// the scale moves a note by its nearest semitone's offset; the note keeps its fraction
		var q = p + (on * Select.kr(r.mod(12) + (low * 12), gg[2..13] ++ gg[18..29]));
		var w = gg[16] + (((q - gg[16]) / 12).floor * 12);
		var t = q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor);
		var note = (t + (gg[14] * 0.01 * ({ Rand(-1, 1) } ! p.size))).clip(min, max);
		^Select.ar(gb >= 0, [cable, K2A.ar(note)])
	}

	*cable {arg name, cable, size, min, max;
		^if(this.isNote(name)) { this.ar(cable, size, min, max) } { cable }
	}
}

OceanodeParameter {
	*ar {arg name, default, size, min, max, units;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: "a"++units))),
				OceanodeGlobalPitch.cable(name, (name ++ "_ar").asSymbol.ar(default!size), size, min, max);
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
				OceanodeGlobalPitch.cable(name, (name ++ "_ar").asSymbol.ar(default!size), size, min, max);
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

// Void parameter: a bang button in Oceanode (connects from void outputs).
// Each bang sets the trigger control to 1 for one control block.
//     reset = OceanodeParameterVoid.kr(\reset);
// .ar gives the same pulse at the first sample of that block.
OceanodeParameterVoid {
	*kr {arg name;
		^name.asSymbol.tr(0, spec: ControlSpec(0, 1, default: 0, units: "v"));
	}
	*ar {arg name;
		^T2A.ar(this.kr(name));
	}
}
