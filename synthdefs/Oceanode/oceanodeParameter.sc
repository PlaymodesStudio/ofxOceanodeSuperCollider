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

// Sync To Transport for ~synthCreator synths. A synth that calls any of these
// gets a "Sync To Transport" switch in its node's inspector (scSynthdef finds
// the \tsync control by its "transport" units); with it on, the node keeps the
// synth on the transport's beat with timetagged anchors (scTransportSync.h):
//   \tsync    1 = follow the transport, 0 = the synth's own free-running clock
//   \tanchor  counter: every change starts a new anchor
//   \thard    counter: changes on jumps (seek, stop, start, loop restart)
//   \tposhi   the anchor's beat, whole multiples of 1024 (exact in float32)
//   \tposlo   the rest of it, 0..1024
//   \trate    beats per second from the anchor on (0 while stopped)
//   \tbpm     tempo, also while stopped
// Off (the default) nothing changes: old patches sound the same.
//
//     var sync = OceanodeTransport.sync;                 // 0 / 1, control rate
//     var phase = OceanodeTransport.phase(beats);        // 0..1 every `beats`
//     sig = Select.ar(sync, [freeRunning, phase]);       // not K2A.ar(sync): it glides through the block of the switch
OceanodeTransport {
	*sync {
		^\tsync.kr(0, spec: ControlSpec(0, 1, default: 0, units: "transport"));
	}

	*bpm { ^\tbpm.kr(120).max(1) }

	// 1 for one sample when the transport jumped
	*jump { ^T2A.ar(Changed.kr(\thard.kr(0))) }

	// The whole 1024-beat blocks, and the beat inside the block (audio rate,
	// moving sample by sample between anchors)
	// (audio-rate controls: a control-rate one would glide across the block
	// of a jump when combined with an audio-rate signal)
	*hi { ^\tposhi.ar(0) }
	// Never backwards between jumps: an anchor lands on a block boundary, up
	// to a block early, so a drift correction can step back a millisecond.
	// Holding still instead keeps a step or a cycle from being crossed twice.
	*lo {
		^RunningMax.ar(
			\tposlo.ar(0) + Sweep.ar(T2A.ar(Changed.kr(\tanchor.kr(0))), \trate.ar(0)),
			this.jump);
	}

	// Phase 0..1 of a cycle of `beats` beats (a number, or one signal per channel)
	*phase {arg beats;
		var b = beats.max(0.0001);
		var phase = (((this.hi % b) + this.lo) / b).frac;
		^phase * (phase < 1);   // float rounding can give exactly 1 at a cycle start
	}

	// A cycle length worked out from two tempos (the transport's, and a Hz or
	// ms value the patch derived from the tempo, which arrives a little later
	// and rounded differently) is never exactly the musical length, and the
	// phase multiplies the difference by the whole beat count. Snapped to
	// 1/480 beat (halves, triplets and quintuplets stay exact) it is.
	*snapBeats {arg beats;
		^((beats * 480).round.max(1) / 480);   // k/480 exactly: 480/480 is 1
	}

	// The same for a clock set in Hz: at the transport's tempo that many
	// cycles per second, on the beat grid (a beat-hz conversion of 1 is one
	// cycle per beat).
	*beatsOfHz {arg hz;
		^this.snapBeats(this.bpm / 60 / hz.max(0.0001));
	}
	*phaseHz {arg hz;
		^this.phase(this.beatsOfHz(hz));
	}
	*trigHz {arg hz;
		var beats = this.beatsOfHz(hz);
		^this.cycleTrig(this.phase(beats), beats);
	}

	// The synth's own clock, or the transport's when Sync To Transport is on
	*select {arg free, synced;
		^Select.ar(this.sync, [free, synced]);
	}

	// One-sample trigger at every cycle start of `phase`. A jump onto a cycle
	// start (or a hair before it: rounding) fires too; a jump elsewhere does
	// not; a drift correction that steps back over a start it just crossed
	// does not fire it twice.
	*cycleTrig {arg phase, beats;
		var jump = this.jump;
		var held = Trig1.ar(jump, ControlDur.ir);              // the block of the jump
		var wrap = ((Delay1.ar(phase) - phase) > 0.5) * (1 - held);
		var cycleSec = (beats.max(0.0001) * 60 / this.bpm);
		var onStart = jump * (\trate.ar(0) > 0) * ((phase.min(1 - phase) * cycleSec) < (ControlDur.ir * 2));
		var gate = Trig1.ar(wrap + onStart, (cycleSec * 0.25).max(SampleDur.ir * 2));
		^(gate - Delay1.ar(gate)) > 0;
	}
}
