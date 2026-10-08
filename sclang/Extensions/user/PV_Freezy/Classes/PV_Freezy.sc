PV_Freezish : PV_ChainUGen {
	*new { |buffer, atkCoeff = 0, dcyCoeff = -1|
		if(dcyCoeff.rate > \control) {
			if(dcyCoeff < 0.0) { dcyCoeff = atkCoeff };
		};
		^this.multiNew('control', buffer, atkCoeff, dcyCoeff)
	}

	// exp(log001 / numDecay)
	// numDecay = time * (sr / framesize / hop)
	// exp(log001 / (time * sr / framesize / hop)))
	// exp(log001 / time * sd * framesize * hop))
	*lag { |buffer, atkTime = 0.01, dcyTime = 0.01, hop = 0.5|
		var sdur = SampleDur.ir;
		var frames = if(buffer.rate == \control) {
			Latch.kr(BufFrames.kr(buffer), buffer >= 0)
		} {
			buffer.numFrames
		};
		var factor = log(0.001) * hop * frames * sdur;
		var coeff = { |time|
			var return;
			if(time.rate > \control) {
				if(time == 0) {
					return = 0
				} {
					return = exp(factor / time)
				}
			} {
				return = Select.perform(UGen.methodSelectorForRate(time.rate),
					time <= 0, [
						exp(factor / time),  // time > 0
						if(time.rate == \control) { 0 } { DC.ar(0) }
					]
				)
			};
			return
		};
		^this.multiNew('control', buffer, coeff.(atkTime), coeff.(dcyTime))
	}
}
