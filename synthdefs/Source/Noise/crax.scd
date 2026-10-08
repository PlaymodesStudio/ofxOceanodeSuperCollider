(
~synthCreator.value("crax", {|n|
	var sr, levels;
	var baseHz, pplusHz, rndDepth, clkHz, pw;
	var scannerRate, scannerW, scannerReset, scannerType;
	var oscType, filterPitch, res, filterType, drive;
	var randTrig, randHeld, scannerTrig, resetTrig;
	var scannerPhase, swUp, swDown, scannerPulse, scannerTri;
	var scannerSin, scannerRand, scanner;
	var freq, phase, saw, pulse, tri, sine, osc;
	var cutoff, rq, hp, bp, lp, filtered, sig;

	sr = SampleRate.ir;

	// Snapshot "crackle" defaults mapped from the updated CRAX 2 Reaktor JSON.
	baseHz = OceanodeParameterLag.ar(\hz, 5000, n, 1, 20000, "vf", 1/30, true);
	pplusHz = OceanodeParameterLag.ar(\pplus, 0, n, -5000, 5000, "vf", 1/30, true);
	rndDepth = OceanodeParameterLag.ar(\rnd, 0.39370077848434448, n, 0, 1, "vf", 1/30, true);
	clkHz = OceanodeParameterLag.ar(\clkhz, 10000, n, 0.001, 20000, "vf", 1/30, true);
	pw = OceanodeParameterLag.ar(\pw, 0.62204724550247192, n, 0.001, 0.999, "vf", 1/30, true);

	scannerRate = OceanodeParameterLag.ar(\scanrate, 7.5, n, 0.001, 20000, "vf", 1/30, true);
	scannerW = OceanodeParameterLag.ar(\scanw, 0, n, -1, 1, "vf", 1/30, true);
	scannerReset = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
	scannerType = OceanodeParameterDropdown.kr(\scantype, 3, n, "SawUp:SawDown:Pulse:Triangle:Sine:Random");

	oscType = OceanodeParameterDropdown.kr(\type, 2, n, "Saw:Pulse:Triangle:Sine");
	filterPitch = OceanodeParameterLag.ar(\filterpitch, 97.165353298187256, n, 20, 120, "vf", 1/30, true);
	res = OceanodeParameterLag.ar(\res, 0.62204724550247192, n, 0, 0.98, "vf", 1/30, true);
	filterType = OceanodeParameterDropdown.kr(\filtertype, 2, n, "Highpass:Bandpass:Lowpass");
	drive = OceanodeParameterLag.ar(\drive, 1.8, n, 0.1, 12, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 0.12, n, 0, 1, "vf", 1/30, true);

	// Decoded Core Cell #21119:
	// Rate/W/Rst -> SwU/SwD/Pls/Tri/Sin/Rnd
	resetTrig = HPZ1.ar(scannerReset).max(0);
	scannerTrig = Impulse.ar(scannerRate.clip(0.001, 20000));
	scannerPhase = Phasor.ar(
		resetTrig,
		scannerRate.clip(0.001, 20000) / sr,
		0,
		1,
		0
	);

	swUp = (scannerPhase * 2) - 1;
	swDown = 1 - (scannerPhase * 2);
	scannerPulse = (scannerPhase < pw) * 2 - 1;
	scannerTri = 1 - (abs((scannerPhase * 2) - 1) * 2);
	scannerSin = (scannerPhase * (2 * pi)).sin;
	scannerRand = Latch.ar(WhiteNoise.ar, scannerTrig);

	scanner = Select.ar(
		scannerType,
		[swUp, swDown, scannerPulse, scannerTri, scannerSin, scannerRand]
	);

	// Decoded Core Cell #21112:
	// rnd/clkhz/p+/P/PW/A -> Saw/Pls/Tri/Sin.
	// Top graph includes clocked random * rnd * 5000 added to oscillator Hz.
	randTrig = Impulse.ar(clkHz.clip(0.001, 20000));
	randHeld = Latch.ar(WhiteNoise.ar, randTrig);
	freq = (
		baseHz
		+ pplusHz
		+ (randHeld * rndDepth.clip(0, 1) * 5000)
		+ (scanner * scannerW.clip(-1, 1) * 5000)
	).abs.clip(1, 20000);

	phase = Phasor.ar(0, freq / sr, 0, 1, 0);
	saw = (phase * 2) - 1;
	pulse = (phase < pw) * 2 - 1;
	tri = 1 - (abs((phase * 2) - 1) * 2);
	sine = (phase * (2 * pi)).sin;

	osc = Select.ar(oscType, [saw, pulse, tri, sine]);
	osc = LeakDC.ar(osc);

	// Static filter path from Filter macro #21149. Delay and amp/filter
	// envelopes are intentionally omitted for this Oceanode source version.
	cutoff = filterPitch.clip(20, 120).midicps.clip(20, 18000);
	rq = res.clip(0, 0.98).linlin(0, 0.98, 1.0, 0.045);
	hp = RHPF.ar(osc, cutoff, rq);
	bp = BPF.ar(osc, cutoff, rq);
	lp = RLPF.ar(osc, cutoff, rq);
	filtered = Select.ar(filterType, [hp, bp, lp]);

	sig = HPF.ar(filtered, 9.midicps);
	sig = (sig * drive).tanh;
	sig = Limiter.ar(sig * levels, 0.95);

	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Crackle oscillator rebuilt from decoded Reaktor Core Cells, without delay or envelopes", category: "Source/Noise");
)
