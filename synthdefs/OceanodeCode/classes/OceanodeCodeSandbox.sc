// Compiled only into the SC Code node's capture runs (OceanodeCode/captureCode.scd),
// never into the channel-count builds or the user's own SuperCollider.
//
// Code pasted from the community expects a running server. Here there is
// none, and Oceanode's own scsynth must never be booted, quit or killed from
// it: those calls do nothing, every server reports itself running (its
// address is a discard port, see captureCode.scd), waiting for a boot runs
// the action at once and s.sync returns at once. {}.play builds a SynthDef
// with an `out` control instead of playing.

OceanodeCodeCapture {
	classvar <defs, playCount = 0, watcher;

	// Every SynthDef built from now on, in order (rebuilt names included).
	*start {
		defs = List.new;
		watcher = { |who, what, def| if(what == \synthDefReady) { defs.add(def) } };
		([SynthDef] ++ SynthDef.allSubclasses).do(_.addDependant(watcher));
	}

	*mergeSpec { |name, spec|
		var specs = UGen.buildSynthDef.specs;
		var existing = specs[name];
		if(existing.notNil and: { existing.respondsTo(\setFrom).not }) {
			existing = existing.asSpec;
			specs[name] = existing;
		};
		existing !? { existing.setFrom(spec) } ?? { specs[name] = spec };
	}

	*nextPlayName {
		playCount = playCount + 1;
		^("play" ++ playCount).asSymbol
	}
}

+ Server {
	boot { ^this }
	reboot { |func| func.value(this); ^this }
	quit { |onComplete| onComplete.value(this); ^this }
	*killAll {}
	*quitAll {}
	waitForBoot { |onComplete| ^this.doWhenBooted(onComplete) }
	doWhenBooted { |onComplete|
		if(onComplete.notNil) { Routine({ onComplete.value(this) }).play(SystemClock) };
		^this
	}
	bootSync {}
	sync {}
	serverRunning { ^true }
	hasBooted { ^true }
}

+ Function {
	play { arg target, outbus = 0, fadeTime = 0.02, addAction, args;
		var func = this;
		var def = SynthDef(OceanodeCodeCapture.nextPlayName, { arg out = 0;
			var result = SynthDef.wrap(func).asUGenInput;
			var rate = result.rate;
			if(rate.isNil or: { rate === \scalar }) {
				result
			} {
				Out.replaceZeroesWithSilence(result.asArray);
				Out.multiNewList([rate, out] ++ result)
			}
		});
		def.metadata[\oceanodeFunctionPlay] = true;
		^nil
	}
}

// SC 3.14 merges a control's spec: into SynthDef metadata specs with
// setFrom, which only a ControlSpec has. Code that gives the same range both
// ways, with the metadata entry as an array (specs: (freq: [20, 20000, \exp])),
// fails with "Message 'setFrom' not understood". Make the entry a spec first.
+ ControlName {
	spec_ { arg spec;
		if (spec.notNil) {
			spec = spec.asSpec;
			defaultValue ?? { defaultValue = spec.default };
			OceanodeCodeCapture.mergeSpec(name, spec);
		}
	}
}

+ NamedControl {
	spec_ { |spec|
		OceanodeCodeCapture.mergeSpec(name, spec.asSpec);
	}
}
