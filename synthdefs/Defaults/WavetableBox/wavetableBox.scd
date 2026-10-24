// WavetableBox: the synth of the WavetableBox node (src/nodes/boxSynths/wavetableBox/scWavetableBox.*).
//
// The node decodes Serum / Vital / AKWF files (or a folder of single cycles)
// in C++ into one mono buffer of 2048-sample cycles, and plays it with
// GrainUtils' OscXPM (>= v1.3.21): band-limited by dynamic mipmapping and sinc
// interpolation, with a second oscillator B that phase-modulates A.
//
// OscXPM derives each mipmap level from the slope of the phase it is given,
// so the phase warps below (applied to A's phase before the lookup) stay
// band-limited too: a synced phase runs faster and reads a smoother level.
//
// WavetableOsc<n>    1D: pos scans the cycles (n = voices, 1..16)
// WavetableOsc2D<n>  2D: the cycles are a grid of `rows` rows; pos is X inside a
//                 row, posy is Y across rows. Two oscillators per voice: one
//                 reads the even row nearest Y and one the odd row, so a row
//                 change always happens on the oscillator that is silent.
// WavetableOscLive<n>, WavetableOscLive2D<n>: the same, reading an A/B pair of
//                 buffers (Source = Live): the node writes the new table into
//                 the buffer that is not heard and flips xf, which fades over
//                 xftime, so a table update is never a step in the wave.
//
// Controls (one value per voice unless noted):
//   out      output bus (scalar)
//   buf      table buffer, cycles = number of 2048-sample cycles in it (scalars)
//   bbuf     buffer of oscillator B (the table itself or a sine), bcycles (scalars)
//   os       oversampling 0..4 = 1x..16x, read when the synth starts (scalar)
//   rows     rows of the grid, 2D only (scalar)
//   pitch    MIDI note          levels   amplitude 0..1
//   pos      0..1 position      posy     0..1 row position (2D)
//   w1mode   warp 1: 0 Off 1 Sync 2 Bend+ 3 Bend- 4 PWM 5 Asym 6 Mirror 7 Quantize 8 PD Sine
//   w1amt    warp 1 amount 0..1, w2mode / w2amt: warp 2, applied after warp 1
//   fm       phase modulation of A by B, 0..10
//   ratio    frequency of B relative to A
//   bpos     0..1 position of B in its table
//   fb       phase modulation of B by A (cross feedback), 0..10
//   damp     damping of the B -> A modulation path, 0..1
//   phase    phase the oscillators restart from on trig
//   trig     restarts the phase of A and B (also on each note gate)
//   gateBus  audio note gates; velBus: latched note velocity (control bus)
//   drone    continuous playback; vca: Amp routes own the envelope release
//   mod0..10 control buses for Pitch, Position, Pos Y, Warp 1/2 Amt, FM,
//            FM Ratio, B Pos, B Feedback, FM Damp and Amp (16-wide strides)
//   buf2, bbuf2  second buffer of the pair (Live only, scalars)
//   xf       0: buf, 1: buf2; xftime: fade time in seconds (Live only, scalars)
//
// Output: <this folder>/CompiledSynthdefs/WavetableBox/WavetableOsc*<n>.scsyndef,
// to be copied to the app's
// data/Supercollider/Synthdefs/Defaults/WavetableBox/.

(
var d, warp, voices, read, maxVoices = 16;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "WavetableBox");

// Phase warp of one voice: p is a 0..1 audio-rate ramp, mode and amt control
// rate. Every warp keeps 0..1 and leaves the phase untouched at amt = 0.
// Mirrors warpPhase() in scWavetableBox.cpp, which draws the warped cycle.
//
// Sync: k = 1..16 cycles per period. A plain frac(p * k) ends mid-cycle and
// jumps back to 0 at every reset (a step in the wave: harsh, aliasing). Over
// the last 5% of the period the phase is steered (smoothstep) to the nearest
// whole number of cycles, so phase and slope both meet the restart.
warp = { |p, mode, amt|
    var x = p * 2 - 1;
    var bend = 1 + (amt * 4);
    var kink = 0.5 - (amt * 0.49);
    var steps = 2.pow(10 - (amt * 9)).round;
    var k = 1 + (amt * 15);
    var s = ((p - 0.95) / 0.05).clip(0, 1);
    var sync = ((p * k) + ((k.round - k) * s * s * (3 - (2 * s)))).frac;
    Select.ar(mode.clip(0, 8), [
        p,
        sync,
        (x.sign * x.abs.pow(bend)) + 1 * 0.5,
        (x.sign * x.abs.pow(bend.reciprocal)) + 1 * 0.5,
        (p / (1 - (amt * 0.98))).min(1),
        Select.ar(p < kink, [0.5 + ((p - kink) * 0.5 / (1 - kink)), p * 0.5 / kink]),
        p + ((1 - x.abs - p) * amt),
        (p * steps).floor / steps,
        (p + (amt * 0.25 * sin(2pi * p))).frac
    ])
};

// Everything the variants share: controls, phases, warps. Returns the
// controls and, per voice, A's warped phase and B's phase.
voices = { |n, twoD, live|
    var c = (
        buf: \buf.kr(-1), cycles: \cycles.kr(1),
        bbuf: \bbuf.kr(-1), bcycles: \bcycles.kr(1),
        os: \os.ir(1),
        pitch: \pitch.kr(60 ! n, 1/30, fixedLag: true),
        levels: \levels.kr(0 ! n, 1/30, fixedLag: true),
        pos: \pos.kr(0 ! n, 1/30, fixedLag: true),
        w1mode: \w1mode.kr(0 ! n), w1amt: \w1amt.kr(0 ! n, 1/30, fixedLag: true),
        w2mode: \w2mode.kr(0 ! n), w2amt: \w2amt.kr(0 ! n, 1/30, fixedLag: true),
        fm: \fm.kr(0 ! n, 1/30, fixedLag: true),
        ratio: \ratio.kr(1 ! n, 1/30, fixedLag: true),
        bpos: \bpos.kr(0 ! n, 1/30, fixedLag: true),
        fb: \fb.kr(0 ! n, 1/30, fixedLag: true),
        damp: \damp.kr(1 ! n, 1/30, fixedLag: true),
        phase: \phase.kr(0 ! n),
        trig: \trig.tr(0 ! n)
    );
    if(twoD) {
        c[\rows] = \rows.kr(1).max(1);
        c[\cols] = (c[\cycles] / c[\rows]).floor.max(1);
        c[\posy] = \posy.kr(0 ! n, 1/30, fixedLag: true);
    };
    if(live) {
        // Second table of the A/B pair, and the crossfade between them:
        // xf jumps to 0 or 1 on each upload, xftime makes it a linear fade
        c[\buf2] = \buf2.kr(-1);
        c[\bbuf2] = \bbuf2.kr(-1);
        c[\xf] = VarLag.kr(\xf.kr(0), \xftime.kr(0.015));
    };
    // Shared Box matrix targets. Source and route groups run before this voice.
    c[\gate] = In.ar(\gateBus.kr(0), n);
    c[\velocity] = In.kr(\velBus.kr(0), n);
    c[\drone] = \drone.kr(1);
    c[\ampMod] = In.kr(\mod10.kr(0), n);
    c[\vca] = \vca.kr(0);
    [\pitch, \pos, \posy, \w1amt, \w2amt, \fm, \ratio, \bpos, \fb, \damp].do { |key, t|
        if(c[key].notNil) {
            c[key] = c[key] + In.kr(NamedControl.kr(("mod" ++ t).asSymbol, 0), n);
            if(key != \pitch) { c[key] = c[key].clip(0, if([\fm, \fb].includes(key)) { 10 } { if(key == \ratio) { 16 } { 1 } }) };
        };
    };
    c[\trig] = c[\trig] + A2K.kr(HPZ1.ar(c[\gate]) > 0);
    c[\phA] = n.collect { |i|
        var freq = c[\pitch][i].midicps;
        var p = Phasor.ar(c[\trig][i], freq * SampleDur.ir, 0, 1, c[\phase][i]);
        p = warp.(p, c[\w1mode][i], c[\w1amt][i]);
        warp.(p, c[\w2mode][i], c[\w2amt][i])
    };
    c[\phB] = n.collect { |i|
        var freq = c[\pitch][i].midicps * c[\ratio][i];
        Phasor.ar(c[\trig][i], freq * SampleDur.ir, 0, 1, c[\phase][i])
    };
    c
};

// Voice i reading the table in `buf` (B reading `bbuf`): at Position (1D),
// or on the grid. 2D: one oscillator reads the even row nearest Pos Y and one
// the odd row, so a row change always happens on the silent oscillator.
read = { |c, i, buf, bbuf, twoD|
    var osc = { |pos|
        OscXPM.ar(
            c[\phA][i], c[\phB][i],
            c[\fm][i], c[\fb][i], c[\damp][i], 1,
            buf, c[\cycles], pos,
            bbuf, c[\bcycles], c[\bpos][i],
            c[\os]
        )
    };
    if(twoD.not) {
        osc.(c[\pos][i])
    } {
        var rows = c[\rows], cols = c[\cols];
        var y = c[\posy][i].clip(0, 1) * (rows - 1);
        var x = c[\pos][i].clip(0, 1) * (cols - 1);
        var evenRow = y.round(2).clip(0, rows - 1);
        var oddRow = ((y + 1).round(2) - 1).clip(0, rows - 1);
        var toPos = { |row| (row * cols + x) / (c[\cycles] - 1).max(1) };
        var even = osc.(toPos.(evenRow));
        // fold: 0 on even rows, 1 on odd rows, linear between
        even + ((osc.(toPos.(oddRow)) - even) * y.fold(0, 1))
    }
};

(1..maxVoices).do { |n|
    [
        ["WavetableOsc", false, false],
        ["WavetableOsc2D", true, false],
        ["WavetableOscLive", false, true],
        ["WavetableOscLive2D", true, true]
    ].do { |variant|
        var name, twoD, live;
        #name, twoD, live = variant;
        SynthDef((name ++ n).asSymbol, {
            var c = voices.(n, twoD, live);
            var sig = n.collect { |i|
                var a = read.(c, i, c[\buf], c[\bbuf], twoD);
                if(live) { a + ((read.(c, i, c[\buf2], c[\bbuf2], twoD) - a) * c[\xf]) } { a }
            };
            sig = Sanitize.ar(LeakDC.ar(sig)) * c[\levels];
            sig = sig * K2A.ar(Select.kr(c[\drone] > 0.5, [Gate.kr(c[\velocity], A2K.kr(c[\gate]) > 0).clip(0, 1), 1 ! n]));
            sig = sig * K2A.ar(Select.kr(c[\vca] > 0.5, [
                (1 + c[\ampMod]).clip(0, 2), c[\ampMod].clip(0, 1)
            ]));
            // An envelope routed to Amp owns its release; otherwise follow Gate.
            sig = sig * Select.ar(K2A.ar((c[\drone] > 0.5) + (c[\vca] > 0.5) > 0), [
                Lag.ar(c[\gate].clip(0, 1), 0.005), DC.ar(1) ! n
            ]);
            Out.ar(\out.kr(0), sig);
        }).writeDefFile(d +/+ "WavetableBox");
    };
};
)
