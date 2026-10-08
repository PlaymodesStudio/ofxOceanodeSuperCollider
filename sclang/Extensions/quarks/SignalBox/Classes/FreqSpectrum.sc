// may want to change the superclass
FreqSpectrum : Number {
	var <>magnitude, <>phase;

	*new { arg magnitude, phase;
		var mag, pha;

		mag = (magnitude == nil).if({
			Array.fill(phase.size, { 1.0 })
		}, {
			magnitude
		});

		pha = (phase == nil).if({
			Array.zeroFill(magnitude.size)
		}, {
			phase
		});

		^super.newCopyArgs(mag, pha)
	}

	*newComplex { arg complex;
		var polar = complex.asPolar;
		^FreqSpectrum.new(polar.magnitude, polar.phase)
	}

	*logShelf { arg size, freq0, freq1, gainDC, gainNy, sampleRate;
		var f0, f1;
		var delta, sigma;
		var kdc, kny;
		var freqs;
		var mag;

		(freq0.abs < freq1.abs).if({
			f0 = freq0.abs;
			f1 = freq1.abs;
		}, {
			f0 = freq1.abs;
			f1 = freq0.abs;
		});

		delta = (f1 / f0).log2.reciprocal;
		sigma = (f0 * f1).log2;
		kdc = gainDC.dbamp;
		kny = gainNy.dbamp;

		freqs = size.isPowerOfTwo.if({
			size.fftFreqs(sampleRate)
		}, {
			size.dftFreqs(sampleRate)
		});

		mag = freqs.collect({ arg freq;
			var beta, sinBeta2, cosBeta2;
			var freqAbs = freq.abs;

			case
			{ freqAbs <= f0 } {
				kdc
			}
			{ (freqAbs > f0) && (freqAbs < f1) } {
				beta = (pi/4) * (1 + (delta * (sigma - (2 * freqAbs.log2))));  // direct beta
				sinBeta2 = beta.sin.squared;
				cosBeta2 = 1 - sinBeta2;

				kdc.pow(sinBeta2) * kny.pow(cosBeta2)  // return as scale
			}
			{ freqAbs >= f1 } {
				kny
			}
		});

		^FreqSpectrum.new(mag)
	}

	*powerLaw { arg size, beta = 0;
		var magnitude;

		// magnitude
		magnitude = size.dftFreqs(2).abs.pow(beta.neg/2);  // power law
		magnitude.put(0, magnitude.at(1));  // reset DC

		^FreqSpectrum.new(magnitude)
	}

	rho { ^magnitude }

	angle { ^phase }
	theta { ^phase }

	// Signal classs / fft related
	real { ^(magnitude * cos(phase)).as(Signal) }
	imag { ^(magnitude * sin(phase)).as(Signal) }

	asFreqSpectrum { ^this }
	asPolar { ^Polar.new(magnitude, phase) }
	asComplex { ^Complex.new(this.real, this.imag) }

	size { ^magnitude.size }

	ks { ^Array.series(magnitude.size) }
	freqs { arg sampleRate;
		^this.ks.kFreqs(this.size, sampleRate)
	}

	peakMagnitude { ^magnitude.maxItem }

	// Pries energy measures
	energyDensity { ^magnitude.squared }
	totalEnergy { ^this.energyDensity.sum }
	rms { ^(this.totalEnergy.sqrt / this.size) }

	// phase - in place
	linearPhase { arg sym = false;
		var start, step;

		sym.if({
			step = pi.neg * (this.size-1) / this.size;
		}, {
			step = pi.neg;
		});

		phase = this.size.even.if({
			start = step.neg * this.size / 2;  // start with negative freqs
			Array.series(this.size, start, step).rotate((this.size / 2).asInteger)  // then rotate
		}, {
			start = step.neg * (this.size-1) / 2;  // start with negative freqs
			Array.series(this.size, start, step).rotate(((this.size+1) / 2).asInteger)  // then rotate
		})
	}

	// Hilbert minimum phase
	minimumPhase { arg mindb = -120.0;
		var logMag = magnitude.max(magnitude.maxItem * mindb.dbamp).log;
		phase = logMag.as(Signal).analytic.imag.neg.as(Array);  // -1 * Hilbert
	}

	// principal phase - in place
	principalPhase {
		phase = this.phase.sin.atan2(this.phase.cos)
	}

	// unwrap phase - via group delay
	/*
	assume signal is real, i.e., mirrored spectrum
	*/
	continuousPhase { arg groupDelay;
		var size, halfsize, rfftsize;
		var principalPhase, principalPhaseRe, groupDelayRe;
		var c, continuousPhase;
		var conform;

		// function to conform angle to reference
		conform = { |angle, angleRef|
			var angleDiff = angle - angleRef;
			var angleExc = angleDiff.sin.atan2(angleDiff.cos);

			angle - angleExc
		};

		// set up
		size = this.size;
		c = (phase.size / 2 / 2pi).reciprocal;  // group delay coeff


		// wrapped phase (principal phase)
		principalPhase = this.phase.collect({ |phi|
			phi.sin.atan2(phi.cos)
		});

		size.even.if({
			halfsize = (size / 2).asInteger;
			rfftsize = halfsize + 1;

			// reshape principal phase (+/- average): DC, +, +Ny
			principalPhaseRe = Array.with(
				principalPhase.at(0)  // DC
			) ++ (
				0.5 * (
					principalPhase.keep(halfsize).drop(1) +  // +
					principalPhase.reverse.keep(halfsize - 1).neg  // -
				) ++ Array.with(
					principalPhase.at(halfsize).neg  // +Nyquist
				)
			);

			// reshape group delay (+/- average): DC, +, +Ny
			groupDelayRe = Array.with(
				groupDelay.at(0)  // DC
			) ++ (
				0.5 * (
					groupDelay.keep(halfsize).drop(1) +  // +
					groupDelay.reverse.keep(halfsize - 1)  // -
				) ++ Array.with(
					groupDelay.at(halfsize)  // +Nyquist
				)
			);

			// find continuous phase
			continuousPhase = Array.newClear(rfftsize);

			// DC
			continuousPhase.put(0, principalPhaseRe.at(0));

			// freq(1), symmetric w/ freq(-1)
			continuousPhase.put(
				1,
				conform.value(
					groupDelay.at(0).neg / 2 * c,
					principalPhaseRe.at(1)
				)
			);

			// freq(2..)
			(rfftsize - 2).do({ |i|
				var n = i + 2;
				continuousPhase.put(
					n,
					conform.value(
						(groupDelay.at(n - 1).neg * c) + continuousPhase.at(n - 2),
						principalPhaseRe.at(n)
					)
				)
			});

			// return mirrored phase -  in place
			phase = continuousPhase.keep(halfsize) ++ continuousPhase.reverse.keep(halfsize).neg
		}, {
			halfsize = ((size - 1) / 2).asInteger;

			// reshape principal phase (+/- average): DC, +
			principalPhaseRe = Array.with(
				principalPhase.at(0)  // DC
			) ++ (
				0.5 * (
					principalPhase.keep(halfsize + 1).drop(1) +  // +
					principalPhase.reverse.keep(halfsize).neg  // -
				)
			);

			// reshape group delay (+/- average): DC, +
			groupDelayRe = Array.with(
				groupDelay.at(0)  // DC
			) ++ (
				0.5 * (
					groupDelay.keep(halfsize + 1).drop(1) +  // +
					groupDelay.reverse.keep(halfsize)  // -
				)
			);

			// find continuous phase
			continuousPhase = Array.newClear(halfsize + 1);

			// DC
			continuousPhase.put(0, principalPhaseRe.at(0));

			// freq(1), symmetric w/ freq(-1)
			continuousPhase.put(
				1,
				conform.value(
					groupDelay.at(0).neg / 2 * c,
					principalPhaseRe.at(1)
				)
			);

			// freq(2..)
			(halfsize + 1 - 2).do({ |i|
				var n = i + 2;
				continuousPhase.put(
					n,
					conform.value(
						(groupDelay.at(n - 1).neg * c) + continuousPhase.at(n - 2),
						principalPhaseRe.at(n)
					)
				)
			});

			// return mirrored phase -  in place
			phase = continuousPhase ++ continuousPhase.reverse.keep(halfsize).neg
		})
	}

	// Gaussian noise phase
	gaussianPhase {
		var gaussianNoise = Signal.gaussianNoise(this.size);
		// rfft: avoid inline warning
		var rfftSize;
		var rcosTable;
		var halfPhase;

		(this.size.isPowerOfTwo).if({  // fft
			rfftSize = (this.size / 2 + 1).asInteger;
			rcosTable = Signal.rfftCosTable(rfftSize);
			halfPhase = gaussianNoise.rfft(rcosTable).phase;

			// reset phase at DC & Nyquist - to 0 or pi
			halfPhase.put(0, pi * 2.rand).put(this.size / 2, pi * 2.rand);

			// mirror phase, FreqSpectrum expects full spectrum
			phase = halfPhase ++ halfPhase[(rfftSize-2)..1].neg;
		}, {  // dft
			phase = gaussianNoise.dft(Signal.zeroFill(this.size)).phase;

			// reset phase at DC - to 0 or pi
			phase.put(0, pi * 2.rand);
			// reset phase at Nyquist - to 0 or pi
			this.size.even.if({
				phase.put(this.size / 2, pi * 2.rand);
			})
		})
	}

	// NOTE: match Hilbert phase rotation
	rotateWave { arg phase;
		var start, step, phaseOffset;

		step = phase;

		this.size.even.if({
			start = step.neg * this.size / 2;  // start with negative freqs
			phaseOffset = Array.series(this.size, start, step);
			phaseOffset = phaseOffset.rotate((this.size / 2).asInteger)  // rotate
		}, {
			start = step.neg * (this.size-1) / 2;  // start with negative freqs
			phaseOffset = Array.series(this.size, start, step);
			phaseOffset = phaseOffset.rotate(((this.size+1) / 2).asInteger)  // rotate
		});

		this.phase = this.phase + phaseOffset
	}

	// NOTE: match Hilbert phase rotation
	rotatePhase { arg phase;
		var phaseOffset = this.freqs.collect({ arg freq;
			freq.isPositive.if({ phase }, { phase.neg })
		});
		this.phase = this.phase + phaseOffset
	}

	/* phase and group delay */

	// set DC to average of bin 1 and N-1
	phaseDelay {
		var num = phase.neg;
		var dem = this.size.isPowerOfTwo.if({  // ks
			this.size.fftFreqs(this.size)
		}, {
			this.size.dftFreqs(this.size)
		});

		^(
			[[num.at(1) / dem.at(1), num.at(this.size - 1) / dem.at(this.size - 1)].mean] ++
			((num.drop(1) / dem.drop(1)))
		) * this.size / 2pi // samples
	}

	groupDelay { arg mindb = -90.0;
		// avoid inline warning
		// continuous phase method
		var halfsize;
		var c;
		var minu, subt, diff;

		// complex method
		var complex;
		var ramped, complexr;
		var num, den;
		var minMag, singlBins;
		// rfft
		var rfftSize;
		var cosTable;
		var rcomplex, rcomplexr;

		(mindb == -inf).if({
			// continuous phase method
			// phase must be continuous!

			c = (this.size / 2 / 2pi);

			this.size.even.if({  // even
				halfsize = (this.size / 2).asInteger;

				minu = phase.keep(halfsize).drop(1) ++ (
					(
						phase.drop(halfsize) ++ Array.with(phase.first)
					) - (2 * Array.with(phase.at(halfsize)))
				);
				subt = (
					Array.with(phase.last) ++ phase.keep(halfsize)
				) ++ (
					(
						phase.drop(halfsize).drop(-1)
					) - (2 * Array.with(phase.at(halfsize)))
				)
			}, {  // odd
				halfsize = ((this.size - 1) / 2).asInteger + 1;

				minu = phase.keep(halfsize).drop(1) ++ (
					(
						phase.drop(halfsize) ++ Array.with(phase.first)
					) - (2 * Array.with(phase.at(halfsize)))
				);
				subt = (
					Array.with(phase.last) ++ phase.keep(halfsize)
				) ++ (
					(
						phase.drop(halfsize).drop(-1)
					) - (2 * Array.with(phase.at(halfsize)))
				)
			});

			diff = minu - subt;

			^(c * diff.neg)  // samples
		}, {
			// complex method

			complex = this.asComplex;

			this.size.isPowerOfTwo.if({
				rfftSize = this.size / 2 + 1;
				cosTable = Signal.rfftCosTable(rfftSize);
				rcomplex = complex.real.fftToRfft(complex.imag);

				ramped = Array.series(this.size).as(Signal) * rcomplex.real.irfft(rcomplex.imag, cosTable);
				rcomplexr = ramped.rfft(cosTable);
				complexr = rcomplexr.real.rfftToFft(rcomplexr.imag);
			}, {
				ramped = Array.series(this.size).as(Signal) * complex.real.idft(complex.imag).real;
				complexr = ramped.dft(Signal.zeroFill(this.size));
			});

			num = complexr.real.collect({ arg real, i;
				Complex.new(real, complexr.imag.at(i))
			});  // deinterleave
			den = complex.real.collect({ arg real, i;
				Complex.new(real, complex.imag.at(i))
			});  // deinterleave

			// find & replace singularities
			minMag = mindb.dbamp;
			singlBins = magnitude.selectIndices({arg item; item < minMag});
			num.put(singlBins, 0.0);
			den.put(singlBins, 1.0);

			^(num / den).real  // samples
		})
	}

	// Pries intercept phase
	phaseOffset { arg mindb = -90.0, sampleRate;
		var deltaDelay = this.groupDelay(mindb) - this.phaseDelay;
		^(deltaDelay / sampleRate * this.freqs(sampleRate) * 2pi)
	}

	// Pries averages
	averageFreq { arg sampleRate;
		^(this.freqs(sampleRate).abs * this.energyDensity / this.totalEnergy).sum
	}
	averageGroupDelay { arg mindb = -90.0;
		^(this.groupDelay(mindb) * this.energyDensity / this.totalEnergy).sum
	}
	averageMagnitude { arg mindb = -120.0;
		^(this.magnitude.max(mindb.dbamp).ampdb * this.energyDensity / this.totalEnergy).sum.dbamp
	}
	averagePhaseDelay { ^(this.phaseDelay * this.energyDensity / this.totalEnergy).sum }
	averagePhaseOffset { arg mindb = -90.0, sampleRate;
		var halfsize = (this.size / 2).asInteger;
		^(
			(
				this.phaseOffset(mindb, sampleRate) * ( Array.fill(halfsize, { 1.0 }) ++ Array.fill(halfsize, { 1.0.neg }) )
			) * this.energyDensity / this.totalEnergy
		).sum
	}

	// bandwidth
	bandwidth { arg fraction, sampleRate;
		var size = this.size;
		var energyDensity = this.energyDensity;
		var totalEnergy = this.totalEnergy;
		var averageFreq = (this.freqs(sampleRate).abs * energyDensity / totalEnergy).sum;
		var averageK = ((averageFreq / sampleRate) * size).round.asInteger;

		// fractional bandwidth: avoid inline warning
		var halfsize, rfftsize;
		var positiveEnergyDensity, mirroredEnergyDensity;
		var k;

		^(fraction == nil).if({  // equivalent bandwidth
			// +freq (assume real signal spectrum)
			(totalEnergy / (2 * energyDensity.at(averageK)) * sampleRate / size)

		}, {  // fractional bandwidth
			halfsize = (size / 2).asInteger;
			rfftsize = halfsize + 1;

			positiveEnergyDensity = size.isPowerOfTwo.if({
				energyDensity.keep(rfftsize) * Array.fill(rfftsize, { |i|
					((i == 0) || (i == (rfftsize - 1))).if({ 1.0 }, { 2.0 })
				})
			}, {
				energyDensity.keep(rfftsize) * Array.fill(rfftsize, { |i|
					(i == 0).if({ 1.0 }, { 2.0 })
				})
			});
			mirroredEnergyDensity = positiveEnergyDensity.shift(averageK.neg) +
			positiveEnergyDensity.shift(halfsize - averageK).put(halfsize, 0.0).reverse;

			(this.bandwidth(sampleRate: 2) > 0.5).if({  // broad band
				k = halfsize;
				({
					(mirroredEnergyDensity.keep(k).sum / totalEnergy > fraction) && (k > -1)
				}).while({ k = k - 1 })
			}, {  // narrow band
				k = 0;
				({
					(mirroredEnergyDensity.keep(k + 1).sum / totalEnergy < fraction) && (k < rfftsize)
				}).while({ k = k + 1 })
			});

			2 * (k * (sampleRate / size))
		})
	}

	// allpass - reset magnitude, in place
	allpass {
		magnitude = Array.fill(this.size, { 1.0 })
	}

	// math
	neg { ^FreqSpectrum.new(magnitude, phase + pi) }

	// math - in place
	invert { phase = phase + pi }
	scale { arg scale;
		magnitude = magnitude * scale
	}
	// Normalize the Signal in place such that the maximum absolute peak value is 1.
	normalize { this.scale(this.peakMagnitude.reciprocal) }

	/*
	Consider migrating or otherwise implementing extSequencableCollection:

	maxdb { arg aNumber, adverb; ^this.performBinaryOp('maxdb', aNumber, adverb) }
	mindb { arg aNumber, adverb; ^this.performBinaryOp('mindb', aNumber, adverb) }
	clipdb2 { arg aNumber, adverb; ^this.performBinaryOp('clipdb2', aNumber, adverb) }
	threshdb { arg aNumber, adverb; ^this.performBinaryOp('threshdb', aNumber, adverb) }

	clipdb { arg ... args; ^this.multiChannelPerform('clipdb', *args) }

	*/

	/*
	Implement ...?
	*/

	// // do math as Complex
	// + { arg aNumber;  ^this.asComplex + aNumber  }
	// - { arg aNumber;  ^this.asComplex - aNumber  }
	// * { arg aNumber;  ^this.asComplex * aNumber  }
	// / { arg aNumber;  ^this.asComplex / aNumber  }
	//
	// == { arg aPolar;
	// 	^aPolar respondsTo: #[\rho, \theta] and: {
	// 		rho == aPolar.rho and: { theta == aPolar.theta }
	// 	}
	// }

	// performBinaryOpOnSomething { |aSelector, thing, adverb|
	// 	^thing.asComplex.perform(aSelector, this, adverb)
	// }
	//
	// performBinaryOpOnUGen { arg aSelector, aUGen;
	// 	^Complex.new(
	// 		BinaryOpUGen.new(aSelector, aUGen, this.real),
	// 		BinaryOpUGen.new(aSelector, aUGen, this.imag)
	// 	);
	// }


	hash {
		^magnitude.hash bitXor: phase.hash
	}

	printOn { arg stream;
		stream << "FreqSpectrum( " << magnitude << ", " << phase << " )";
	}

	storeArgs { ^[magnitude, phase] }
}
