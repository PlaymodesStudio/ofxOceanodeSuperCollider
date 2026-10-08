// scGrainBoxPoly.scd — SynthDefs for the scGrainBoxPoly C++ node
//
// One ENGINE = a chain of synths on the server, in this order:
//   [GrainBoxPolyRec]       (live input only) records the input into a rolling buffer
//   GrainBoxPolyCtl_<cap>   one per engine, cap = 8 / 32 / 128 / 500 playheads
//                           (the C++ picks the smallest that fits, so a small
//                           engine costs little): takes every control (same
//                           names as GrainBox_K_N), runs the engine's mono LFOs
//                           (single-value lfo* controls) and the clock, writes
//                           the shared values and each playhead's values to a
//                           block of control buses (\ctl), the clock to a
//                           7-channel audio bus (\clk)
//   GrainBoxPolyV_K_N       one per PLAYHEAD (\idx), free clock
//   GrainBoxPolyVS_K_N      the same, on the transport grid (sync on)
//   GrainBoxPolyVM_K_N / GrainBoxPolyVSM_K_N   Mono trigger variants (no overlap)
//                           Only the trigger is audio rate; every value is
//                           control rate (GrainBuf reads its arguments when a
//                           grain starts); random draws at control rate on the
//                           block of the trigger. 512 overlapping grains.
//   GrainBoxPolyD_N         one per PLAYHEAD instead, Live input "Grainless":
//                           a delay line (Position = delay, 0 = now)
//   GrainBoxPolyDry_N       one per node (after engine 1): Dry/Wet's dry input
//   GrainBoxPolyOut_N       one per engine: private bus -> levels -> output
//                           filter -> gain (active * volume * mute/solo) -> \out
//
// Control bus block (80 + 15 * cap channels):
//   +0 inpoint +1 outpoint +2 bufnum0 +3 bufnum1 +4 envbuf +5 beats/s
//   +6 dynamicdur +7 trigdur*autotrig +8 autotrig +9 uniquetrig +10 uniquejit
//   +11 latchamp +12 scaleon +13 scaleroot +14 live +15 phasebus +16 vis
//   +17 sync +18..41 scaletable (24) +42..47 "Link" draws of playhead 1
//   (chance, pitchjit, posjit, durjit, panjit, ampjit) +48..53 scratch (the
//   other playheads write their draws there) +54 filteron +55 filtertype
//   +56 cutoff (with the Cut LFO) +57 filterq +58 gain +59..74 levels (16)
//   +75 monotrig (1: a playhead starts a grain only once its previous one ended)
//   +76 grains sounding (every playhead adds its estimate, Out.kr)
//   +77 grain thinning factor (Max Grains / estimate, <= 1; written by the ctl)
//   +78 Grainless glide / crossfade time (s)
//   +79 Overlap (TrigDur: grain length = Overlap x the trigger period)
//   +80 + i * 15: playhead i: pos dur pitch amp pan div chance posjit pitchjit
//                 durjit ampjit panjit reverse manual-trigger-counter trigphase
// Clock bus (audio, 7): syncTrig, freeBeat, beatW, beatAbs, hardImp, run, anchorFire

(
var d, subDir, nslot = 15, g0 = 0, vbase = 80, caps = [8, 32, 128, 500], makeLFOk;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
subDir = d +/+ "/GrainBoxPoly";
File.mkdir(d);
File.mkdir(subDir);

// Mono control-rate LFO (same shapes / sync semantics as GrainBox's LFOs).
// Random shapes come from a hash of the cycle count (salted per LFO).
makeLFOk = {|shape, speed, phaseOff, quant, str, pow, anchor, salt, barHz, gateTrigK, anchorTrigK, runK, sync|
    var freq, phaseFree, phaseSync, phase, cycleTrig, cnt, r0, r1, raw, powed, q, quantized;
    freq      = (barHz * speed).max(0.0001);
    phaseFree = Phasor.kr(gateTrigK, freq / ControlRate.ir, 0, 1, 0);
    phaseSync = Phasor.kr(anchorTrigK, freq * runK / ControlRate.ir, 0, 1, anchor);
    phase     = (Select.kr(sync, [phaseFree, phaseSync]) + phaseOff).mod(1);
    cycleTrig = Select.kr(sync, [HPZ1.kr(phaseFree) < 0, HPZ1.kr(phaseSync) < -0.25]);
    cnt       = PulseCount.kr(cycleTrig, gateTrigK) + (salt * 100003);
    r0        = Hasher.kr(cnt) * 0.5 + 0.5;
    r1        = Hasher.kr(cnt + 1) * 0.5 + 0.5;
    raw       = Select.kr(shape.clip(0, 5), [
        sin(phase * 2pi) * 0.5 + 0.5,
        1.0 - (phase * 2.0 - 1.0).abs,
        phase,
        1.0 - phase,
        r0,
        r0 + ((r1 - r0) * phase)
    ]);
    powed     = raw.clip(0, 1).pow(pow.clip(0.1, 10));
    q         = quant.clip(1, 32);
    quantized = Select.kr((quant > 0), [powed, (powed * q).floor.clip(0, q - 1) / q]);
    // [output, phase (0..1, offset included), shape value 0..1] (the last two
    // for the window's LFO display)
    [quantized * str, phase, quantized]
};

// ── Control synth (one per engine) ─────────────────────────────────────────
caps.do({|pmax|
SynthDef(("GrainBoxPolyCtl_" ++ pmax).asSymbol, {
    |ctl = 0, clk = 0, envbuf = -1,
     dynamicdur = 0, autotrig = 0, trigdur = 0, uniquetrig = 0, uniquejit = 0,
     bpm = 120.0, syncgate = 0, inpoint = 0.0, outpoint = 1.0, active = 1,
     sync = 0, anchorId = 0, anchorHard = 0,
     transpose = 0, mastervol = 1, phgain = 1, speed = 1, latchamp = 0,
     bufnum0 = 0, bufnum1 = 0,
     filteron = 0, filtertype = 0, cutoff = 1000, filterq = 0.707,
     scaleon = 0, scaleroot = 0, live = 0, phasebus = 0, vis = 1, maxoverlap = 0, monotrig = 0, delayglide = 0.05, overlap = 1|

    var total, thin, phaseP;
    var vec, lfoArgs, gateTrigK, anchorTrigK, runK, barHz, beatHz;
    var anchorPos, anchorFire, run, syncTrig, anchorTrig, hardImp, beatSweep, beatX, beatW, beatAbs, freeBeat;
    var ampP, pitchP, durP, posP, panP, divP, chanceP, posJ, pitchJ, durJ, ampJ, panJ, revP, trigP, levelsP, scaleTable;
    var lfo, lfoAll, targets, salts, slots, globals, outVals, fc;

    vec = {|name, def| NamedControl.kr(name, def ! pmax) };

    anchorPos  = \anchorPos.ar(0);
    anchorFire = \anchorFire.ar(0);
    run        = \run.ar(0);

    beatHz = bpm / 60.0;
    barHz  = beatHz / 4.0;

    // ── Clock ──
    syncTrig   = T2A.ar(HPZ1.kr(syncgate) > 0);
    freeBeat   = Sweep.ar(syncTrig, beatHz);
    anchorTrig = T2A.ar(Changed.kr(anchorId));
    hardImp    = T2A.ar(Changed.kr(anchorHard));
    beatSweep  = Sweep.ar(anchorTrig, beatHz * run);
    beatX      = anchorPos.mod(32) + beatSweep;
    beatW      = beatX.mod(32);
    beatAbs    = (anchorPos - anchorPos.mod(32)) + beatX.floor;
    Out.ar(clk, [syncTrig, freeBeat, beatW, beatAbs, hardImp, run, anchorFire]);

    // ── Mono LFOs (element 0 of the lfo vectors) ──
    gateTrigK   = HPZ1.kr(syncgate) > 0;
    anchorTrigK = Changed.kr(anchorId);
    runK        = A2K.kr(run);
    targets = [\Pos, \Dur, \Pitch, \Amp, \Pan, \TrRt, \Cut];
    lfoAll = targets.collect({|t, ti|
        // one LFO per engine: single-value controls (the C++ sends 1 value)
        var sh = NamedControl.kr(("lfoShape" ++ t).asSymbol, 0);
        var sp = NamedControl.kr(("lfoSpeed" ++ t).asSymbol, 1);
        var ph = NamedControl.kr(("lfoPhase" ++ t).asSymbol, 0);
        var qu = NamedControl.kr(("lfoQuant" ++ t).asSymbol, 0);
        var st = NamedControl.kr(("lfoStr"   ++ t).asSymbol, 0);
        var pw = NamedControl.kr(("lfoPow"   ++ t).asSymbol, 1);
        var an = NamedControl.kr(("lfoAnchor" ++ t).asSymbol, 0);
        makeLFOk.(sh, sp, ph, qu, st, pw, an, ti + 1, barHz, gateTrigK, anchorTrigK, runK, sync)
    });
    lfo = lfoAll.collect({|x| x[0] });
    // the window's LFO displays: every LFO's phase and 0..1 value, 30 x / s
    SendReply.kr(Impulse.kr(30) * vis, '/lfoState', lfoAll.collect({|x| x[1] }) ++ lfoAll.collect({|x| x[2] }));
    // the Link flags of the old per-voice LFOs are accepted and ignored (mono)
    targets.do({|t| NamedControl.kr(("uniquelfo" ++ t).asSymbol, 0) });

    // ── Per-playhead values ──
    ampP    = vec.(\amp, 1);
    pitchP  = vec.(\pitch, 0);
    durP    = vec.(\duration, 0.1);
    posP    = vec.(\position, 0);
    panP    = vec.(\panaz, 0);
    divP    = vec.(\autotrigbeatdiv, 1);
    chanceP = vec.(\chance, 1);
    posJ    = vec.(\posjit, 0);
    pitchJ  = vec.(\pitchjit, 0);
    durJ    = vec.(\durjit, 0);
    ampJ    = vec.(\ampjit, 0);
    panJ    = vec.(\panazjit, 0);
    revP    = vec.(\reverse, 0);
    trigP   = vec.(\trigger, 0);
    phaseP  = vec.(\trigphase, 0);
    levelsP = NamedControl.kr(\levels, 1 ! 16);
    scaleTable = NamedControl.kr(\scaletable, (0..23).collect({|j| (j / 2).round(1) }));

    slots = [
        posP + lfo[0],
        durP + lfo[1],
        pitchP + lfo[2] + transpose,
        ampP + lfo[3],
        panP + lfo[4],
        // grains per beat (the C++ converts the Interval control to it),
        // + TrRt LFO, * Speed: 1/64 .. 4096 (audio-rate triggers: Pitch unit)
        ((divP + lfo[5]).max(1/64) * speed).clip(1/64, 4096),
        chanceP, posJ, pitchJ, durJ, ampJ, panJ, revP, trigP,
        phaseP.clip(0, 1)
    ];
    ReplaceOut.kr(ctl + vbase, slots.flop.flat);   // playhead i: 15 values at 80 + i * 15

    // ── Max Grains: every playhead adds its estimate of the grains it keeps
    //    sounding (rate * chance * duration) to +76 during the previous block;
    //    above maxoverlap the playheads skip grains evenly (factor at +77) ──
    total = In.kr(ctl + 76);
    thin  = Select.kr(maxoverlap > 0, [1, (maxoverlap / total.max(0.0001)).min(1)]);
    ReplaceOut.kr(ctl + 77, thin);
    SendReply.kr(Impulse.kr(5) * vis, '/grainLoad', [total, thin]);

    globals = [
        inpoint, outpoint, bufnum0, bufnum1, envbuf, beatHz,
        dynamicdur, trigdur * autotrig, autotrig, uniquetrig, uniquejit,
        latchamp, scaleon, scaleroot, live, phasebus, vis, sync
    ] ++ scaleTable;
    ReplaceOut.kr(ctl + g0, globals);
    ReplaceOut.kr(ctl + 75, monotrig);
    ReplaceOut.kr(ctl + 78, delayglide);   // Grainless: glide / crossfade time (s)
    ReplaceOut.kr(ctl + 79, overlap.max(0.01));   // TrigDur: grain length = Overlap x the trigger period

    fc = (Lag.kr(cutoff, 0.015) * (lfo[6] * 12).midiratio).clip(20, min(20000, SampleRate.ir * 0.45));
    outVals = [filteron, filtertype.round(1).clip(0, 4), fc, filterq.clip(0.5, 20),
               active * Lag.kr(mastervol * phgain, 0.02)] ++ levelsP;
    ReplaceOut.kr(ctl + g0 + 54, outVals);
}).writeDefFile(subDir);
});

// ── LEAN voice synth (one per playhead) ────────────────────────────────────
// Only the trigger is audio rate; every value is control rate (GrainBuf polls
// its arguments when a grain starts). Random draws happen at control rate on
// the block of the trigger. syncMode 0: free clock only; 1: transport grid.
[0, 1].do({|syncMode|
[0, 1].do({|mono|
(1..2).do({|k|
    (1..16).do({|n|
        SynthDef(("GrainBoxPolyV" ++ (if(syncMode == 1) { "S" } { "" }) ++ (if(mono == 1) { "M" } { "" })
                  ++ "_" ++ k ++ "_" ++ n).asSymbol, {
            |out = 0, ctl = 0, clk = 0, idx = 0|
            var v, g, bufnums, buf0, envbuf, beatHz, inP, outP;
            var posV, durV, pitchV, ampV, panV, divV, chanceV, posJ, pitchJ, durJ, ampJ, panJ, revV, trigC;
            var syncTrigA, offs, freePhase, autoImp, manImp, rawTrig, rawK, chanceRand, usedRand, okK, trig, trigK;
            var pjR, poR, duR, paR, amR, link, pjO, poO, duO, paO, ampMul;
            var sampleRateCorr, pitchSum, x, rate, regionDur, trigPeriod, durSec, actualDur;
            var mappedPos, ampWithJit, panK, ampLatched, ampUsed, revFlag, headK, frL, gapL, availL, durF, readLen, dLin, dMin, dMax, livePos, basePos, grainPos, grainRate;
            var trigPh, thinK, autoRaw, estK, monoTrig, grainTrig, visK, resetA, forced, monoSlot;
            var c, beatW, beatAbs, hardImp, run, anchorFire, beatInt, beatFrac, beatEdge, syncLatchTrig, syncDiv, m, sub, cell, cond, allowed, top, stepT;

            v = In.kr(ctl + vbase + (idx * nslot), nslot);
            g = In.kr(ctl + g0, 80);
            posV = v[0]; durV = v[1]; pitchV = v[2]; ampV = v[3]; panV = v[4]; divV = v[5];
            chanceV = v[6]; posJ = v[7]; pitchJ = v[8]; durJ = v[9]; ampJ = v[10]; panJ = v[11];
            revV = v[12]; trigC = v[13]; trigPh = v[14];
            thinK = g[77];
            inP = g[0]; outP = g[1];
            bufnums = [g[2], g[3]].keep(k);
            buf0 = bufnums[0];
            envbuf = g[4];
            beatHz = g[5];

            // ── Trigger (the only audio-rate part) ──
            if(syncMode == 0) {
                syncTrigA = In.ar(clk, 2);                 // syncTrig, freeBeat
                resetA    = syncTrigA[0];
                offs      = Latch.kr(A2K.kr((syncTrigA[1] * divV).frac), Impulse.kr(0) + A2K.kr(syncTrigA[0]));
                // + Trigger phase: this playhead's place within the step (0..1)
                freePhase = (Phasor.ar(syncTrigA[0], (beatHz * divV).max(0.0001) / SampleRate.ir, 0, 1, 0) + offs + trigPh).frac;
                autoImp   = HPZ1.ar(freePhase) < 0;
                trigPeriod = 1.0 / (beatHz * divV).max(0.0001);
            } {
                c = In.ar(clk, 7);
                resetA = c[0];                             // Reset (the grid itself stays)
                beatW = c[2]; beatAbs = c[3]; hardImp = c[4]; run = c[5]; anchorFire = c[6];
                beatInt  = beatW.floor;
                beatFrac = beatW - beatInt;
                beatEdge = HPZ1.ar(beatAbs).abs > 0;
                syncLatchTrig = (beatEdge + hardImp + Impulse.ar(0)).min(1);
                // the grid cells hold up to 127 steps per beat (beatAbs * 128)
                syncDiv = Latch.ar(K2A.ar(divV), syncLatchTrig).clip(0.125, 127);
                m      = syncDiv;
                sub    = (beatFrac * m).floor * (m >= 1);
                cell   = (beatAbs * 128) + sub;
                cond   = ((m >= 1) + (beatInt < 0.5)
                          + (((beatInt * m) + 0.0001).floor > (((beatInt - 1) * m) + 0.0001).floor)).min(1);
                allowed = ((sub > 0) + cond).min(1);
                top    = RunningMax.ar(cell, hardImp);
                stepT  = ((cell > Delay1.ar(top)) * (1 - hardImp) * allowed)
                         + (hardImp * anchorFire * allowed)
                         + (hardImp * (m >= 1) * (sub > 0) * ((beatFrac * m).frac < 0.001));
                autoRaw = (stepT > 0) * run;
                trigPeriod = 1.0 / (beatHz * A2K.kr(syncDiv)).max(0.0001);
                // Trigger phase: each grid trigger delayed by phase * one step
                // (TDelay: one pending trigger at a time, enough for phase < 1)
                autoImp = Select.ar(K2A.ar(trigPh > 0.0001), [
                    autoRaw,
                    TDelay.ar(autoRaw, (trigPh * trigPeriod).min(60))
                ]);
            };
            manImp  = T2A.ar(HPZ1.kr(trigC) > 0);
            // Reset: a grain at once (auto trigger on; playheads with a
            // Trigger phase keep their place in the interval)
            forced  = resetA * (trigPh < 0.0001);
            rawTrig = (((autoImp + forced) * g[8]) + manImp).min(1);
            // the control block that holds a trigger (one draw per block)
            rawK = Changed.kr(A2K.kr(PulseCount.ar(rawTrig)));

            // ── Chance + jitter at control rate (Link: playhead 1's draws) ──
            link       = g[42..47];
            chanceRand = TRand.kr(0, 1, rawK);
            usedRand   = Select.kr(g[9], [chanceRand, link[0]]);
            // chance, thinned evenly when the engine is above Max Grains
            okK        = usedRand < (chanceV * thinK);
            trig       = rawTrig * okK;
            trigK      = rawK * okK;
            pjR = TRand.kr(0, pitchJ + 0.00001, trigK);
            poR = TRand.kr(0, posJ   + 0.00001, trigK);
            duR = TRand.kr(0, durJ   + 0.00001, trigK);
            paR = TRand.kr(0, panJ   + 0.00001, trigK);
            amR = TRand.kr(0, ampJ,             trigK);
            ReplaceOut.kr(ctl + g0 + Select.kr(idx < 0.5, [48, 42]), [chanceRand, pjR, poR, duR, paR, amR]);
            pjO = Select.kr(g[10], [pjR, link[1]]);
            poO = Select.kr(g[10], [poR, link[2]]);
            duO = Select.kr(g[10], [duR, link[3]]);
            paO = Select.kr(g[10], [paR, link[4]]);
            ampMul = 1.0 - Select.kr(g[10], [amR, link[5]]);

            // ── Pitch -> rate (scale: one Select.kr) ──
            sampleRateCorr = BufSampleRate.kr(buf0) / SampleRate.ir;
            pitchSum = pitchV + pjO;
            x = (pitchSum - g[13]).wrap(0, 12);
            rate = Select.kr(g[12], [
                pitchSum,
                ((pitchSum - x) + Select.kr((x * 2).floor.clip(0, 23), g[18..41])).round(1)
            ]).midiratio * sampleRateCorr;

            // ── Duration / position / amp / pan ──
            regionDur = (outP - inP) * BufDur.kr(buf0);
            // TrigDur: Overlap x the trigger period (x2: every grain overlaps the next by half)
            durSec    = Select.kr(g[7], [(durV + duO).clip(0, 1) * regionDur.max(0.001), trigPeriod * g[79]]);
            actualDur = Select.kr(g[6], [durSec, durSec / rate.abs.max(0.001)]);
            // grains this playhead keeps sounding (auto triggers): rate * chance * duration
            estK = (g[8] * beatHz * divV * chanceV.clip(0, 1) * actualDur).min(512);
            if(mono == 1) { estK = estK.min(1) };   // mono: one grain at a time
            Out.kr(ctl + 76, estK);
            mappedPos  = inP + ((posV + poO).clip(0, 1) * (outP - inP));
            ampWithJit = ampV.clip(0, 1) * ampMul;
            panK       = (panV + paO).wrap(0, 2);
            ampLatched = Latch.kr(ampWithJit, trigK);
            ampUsed    = Select.kr(g[11], [ampWithJit, ampLatched]);

            // ── Mono trigger: no overlap. Trig1 ignores triggers while it is
            //    high (one grain length, minus 2 samples so a trigger exactly
            //    at the end gets through); its rising edges are the grains ──
            //    Reset moves on to a fresh Trig1 (4 in rotation), so the
            //    reset grain starts at once even inside a long grain ──
            //    (only in the M variants: it costs CPU on every playhead) ──
            if(mono == 1) {
                monoSlot  = PulseCount.ar(resetA) % 4;
                monoTrig  = Mix.new(4.collect({|j|
                    var h = Trig1.ar(trig * (monoSlot |==| j), (actualDur - (2 * SampleDur.ir)).max(SampleDur.ir));
                    (h - Delay1.ar(h)) > 0
                }));
                grainTrig = monoTrig;
                // the block of a grain that really starts (display)
                visK = Changed.kr(A2K.kr(PulseCount.ar(monoTrig)));
            } {
                grainTrig = trig;
                visK = trigK;
            };

            SendReply.kr(visK * g[16], '/grainTrig', [mappedPos, actualDur, ampWithJit], idx);

            // ── Reverse (re-uses the chance draw, which is held between
            //    triggers: no latch, so the first grain is decided too) ──
            revFlag = usedRand < (revV * chanceV * thinK);

            // ── Live input (head read once per block) ──
            headK  = A2K.kr(In.ar(g[15], 1));
            frL    = BufFrames.kr(buf0).max(1);
            gapL   = 0.005 * SampleRate.ir;   // safety gap to the write head (= the C++ kLiveGap)
            availL = frL - (2 * gapL);
            durF    = actualDur * SampleRate.ir;
            readLen = durF * rate.abs;
            dLin    = gapL + (mappedPos * availL);
            dMin    = gapL + Select.kr(revFlag, [(readLen - durF).max(0), readLen]);
            dMax    = frL - gapL - Select.kr(revFlag, [(durF - readLen).max(0), durF]);
            livePos = ((headK - dLin.clip(dMin, dMax)) / frL).wrap(0, 1);
            basePos = Select.kr(g[14], [mappedPos, livePos]);
            grainPos  = Select.kr(revFlag, [basePos, (basePos + (readLen / frL)).wrap(0, 1)]);
            grainRate = Select.kr(revFlag, [rate, rate.neg]);

            Out.ar(out, (1 / k) * Mix.new(k.collect({|ki|
                GrainBuf.ar(
                    numChannels: n,
                    trigger:     grainTrig,
                    dur:         actualDur,
                    sndbuf:      bufnums[ki],
                    rate:        grainRate,
                    pos:         grainPos,
                    interp:      2,
                    pan:         if(n == 2) { (panK + 1).wrap(0, 2) - 1 } { panK },
                    envbufnum:   envbuf,
                    maxGrains:   512,
                    mul:         ampUsed
                )
            })));
        }).writeDefFile(subDir);
    });
});
});
});

// ── Grainless voices (Live input): a plain delay line per playhead ────────
// Read the live recorder's rolling buffer behind its write head. Position
// (+ Pos LFO), within In / Out, sets the delay: 0 = now (one sample),
// 1 = the whole buffer (Live Length). Amp (+ LFO) and Pan (+ LFO) per
// playhead; no pitch, no grains. A new delay is reached by
//   GrainBoxPolyD_N   Glide: a tape-like slide (Lag, time = ctl +78), which
//                     bends the pitch while it moves
//   GrainBoxPolyDJ_N  Jump: a second read head starts at the new delay and
//                     the two crossfade (equal power, time = ctl +78, min
//                     5 ms): no pitch bend, no click. During a fade, new
//                     changes wait for it to end (a continuous LFO steps).
// Both report the tap's place 20 x / s (/grainTrig) for the waveform.
[0, 1].do({|jump|
(1..16).do({|n|
    SynthDef(("GrainBoxPolyD" ++ (if(jump == 1) { "J" } { "" }) ++ "_" ++ n).asSymbol, {
        |out = 0, ctl = 0, clk = 0, idx = 0|
        var v, g, buf, frames, head, r, dK, dA, sig, amp, pan, glide, shown;
        var fb, dX, dY, sel, t, curD, fade, sw, newSel, newX, newY, newT, p;
        v      = In.kr(ctl + vbase + (idx * nslot), nslot);
        g      = In.kr(ctl + g0, 79);
        buf    = g[2];
        frames = BufFrames.kr(buf).max(512);
        head   = In.ar(g[15], 1);
        glide  = g[78];
        r      = g[0] + (v[0].clip(0, 1) * (g[1] - g[0]));
        dK     = (r * frames).clip(1, frames - 256);
        if(jump == 0) {
            dA    = Lag.ar(K2A.ar(dK), glide.max(0));
            sig   = BufRd.ar(1, buf, (head - dA).wrap(0, frames), 1, 4);
            shown = A2K.kr(dA);
        } {
            // two heads X / Y (delays held at control rate), sel = the one
            // fading in, t = time since the last switch (control-rate feedback)
            fade   = glide.max(0.005);
            fb     = LocalIn.kr(4, [0, 0, 0, 100]);
            dX = fb[0]; dY = fb[1]; sel = fb[2]; t = fb[3];
            curD   = Select.kr(sel, [dX, dY]);
            sw     = ((dK - curD).abs > 0.5) * (t >= fade);
            newSel = (sel + sw) % 2;
            newX   = Select.kr(sw * (newSel < 0.5), [dX, dK]);
            newY   = Select.kr(sw * (newSel > 0.5), [dY, dK]);
            newT   = Select.kr(sw, [t + ControlDur.ir, 0]);
            LocalOut.kr([newX, newY, newSel, newT]);
            // p: 0 = all X .. 1 = all Y (ramps over the fade, per block)
            p      = Select.kr(newSel, [1 - (newT / fade).clip(0, 1), (newT / fade).clip(0, 1)]);
            sig    = (BufRd.ar(1, buf, (head - newX).wrap(0, frames), 1, 4) * K2A.ar((p * 0.5pi).cos))
                   + (BufRd.ar(1, buf, (head - newY).wrap(0, frames), 1, 4) * K2A.ar((p * 0.5pi).sin));
            shown  = Select.kr(newSel, [newX, newY]);
        };
        amp    = Lag.kr(v[3].clip(0, 1), 0.02);
        // display: the tap's current place (Pos LFO and glide included), 20 times
        // a second, as a short "grain" (same message as the grain voices)
        SendReply.kr(Impulse.kr(20) * g[16], '/grainTrig', [(shown / frames).clip(0, 1), 0.06, amp], idx);
        pan    = Lag.kr(v[4], 0.02);
        Out.ar(out, case
            { n == 1 } { sig * amp }
            { n == 2 } { Pan2.ar(sig, (pan + 1).wrap(0, 2) - 1, amp) }
            { PanAz.ar(n, sig, pan.wrap(0, 2), amp, 2, 0.5) });   // as GrainBuf pans
    }).writeDefFile(subDir);
});
});

// ── Dry (one per node, after engine 1): the node's input, unprocessed, ────
//    straight to the node's output (Dry/Wet; the engines get the wet gain)
(1..16).do({|n|
    SynthDef(("GrainBoxPolyDry_" ++ n).asSymbol, {
        |in = 0, out = 0, dry = 0|
        Out.ar(out, In.ar(in, n) * Lag.kr(dry, 0.02));
    }).writeDefFile(subDir);
});

// ── Output synth (one per engine): levels -> filter -> gain ────────────────
// Filter types: 0 LPF2, 1 BPF, 2 Notch, 3 HPF2 (one SOS), 4 LP-HP ("DJ": a
// lowpass then a highpass; the cutoff's place on its log 20 Hz .. 20 kHz
// slider: left of the middle = lowpass closing, a small open zone around
// the middle, right of it = highpass opening). The cutoff goes through a
// 50 Hz lowpass (in log, audio rate) before every coefficient, so jumps
// (random-step Cut LFO) glide instead of clicking or zippering.
(1..16).do({|n|
    SynthDef(("GrainBoxPolyOut_" ++ n).asSymbol, {
        |in = 0, out = 0, ctl = 0, fbbus = -1|
        var v, filterOn, fType, fc, qP, gain, levels, dry, filtG, wet;
        var isDJ, xPos, lpCut, hpCut, nyq, coefs, stage1, stage2, fcS;
        v = In.kr(ctl + g0 + 54, 5 + 16);
        filterOn = v[0]; fType = v[1]; fc = v[2]; qP = v[3]; gain = v[4];
        levels = v[5..(5 + n - 1)];
        dry = In.ar(in, n) * levels;
        filtG = Lag.kr(filterOn, 0.02);
        nyq   = SampleRate.ir * 0.45;
        // the cutoff (with the Cut LFO) through a one-pole 50 Hz lowpass, in log,
        // at audio rate: jumps (random steps) become smooth glides, no zipper
        // (relative to the first value, so it starts there instead of rising from 0)
        fcS   = { var l = fc.max(1).log, l0 = Latch.kr(l, Impulse.kr(0)); (OnePole.ar(K2A.ar(l - l0), exp(-2pi * 50 / SampleRate.ir)) + l0).exp }.value;
        isDJ  = fType > 3.5;
        xPos  = (fcS / 20).log / 1000.log;                   // 0..1: place on the log slider
        lpCut = Select.ar(xPos < 0.45, [DC.ar(1) * nyq, xPos.linexp(0, 0.45, 40, 20000)]).min(nyq);
        hpCut = Select.ar(xPos > 0.55, [DC.ar(10), xPos.linexp(0.55, 1, 20, 12000)]).min(nyq);
        // RBJ biquad coefficients [b0, b1, b2, a1, a2] (normalised), per type;
        // computed at audio rate from the (control-rate) cutoff ramped per
        // sample, so a moving cutoff never steps once per block
        coefs = {|f, type, q|
            var w0, cw, alpha, a0r, ff;
            f     = if(f.rate == \audio) { f } { K2A.ar(f) };
            w0    = 2pi * f / SampleRate.ir;
            cw    = cos(w0);
            alpha = sin(w0) / (2 * q);
            a0r   = (1 + alpha).reciprocal;
            ff    = [
                [(1 - cw) * 0.5, 1 - cw, (1 - cw) * 0.5],
                [alpha, 0 * cw, alpha.neg],
                [1 + (0 * cw), -2 * cw, 1 + (0 * cw)],
                [(1 + cw) * 0.5, (1 + cw).neg, (1 + cw) * 0.5]
            ];
            3.collect({|c| Lag.ar(Select.ar(K2A.ar(type), ff.collect({|t|
                var e = t[c]; if(e.isNumber) { DC.ar(e) } { e }   // (0 * cw folds to a number)
            })), 0.02) * a0r })
                ++ [2 * cw * a0r, (alpha - 1) * a0r]
        };
        // stage 1: the chosen type (DJ: its lowpass); stage 2: DJ's highpass, else a wire
        stage1 = coefs.(Select.ar(K2A.ar(isDJ), [fcS, lpCut]).clip(10, nyq), Select.kr(isDJ, [fType.clip(0, 3), 0]), qP);
        stage2 = coefs.(hpCut, 3, qP);
        stage2 = [Select.ar(K2A.ar(isDJ), [DC.ar(1), stage2[0]])] ++ stage2[1..4].collect({|c| c * isDJ });
        wet = dry.asArray.collect({|sig|
            var x = SOS.ar(sig, stage1[0], stage1[1], stage1[2], stage1[3], stage1[4]);
            SOS.ar(x, stage2[0], stage2[1], stage2[2], stage2[3], stage2[4])
        });
        wet = (dry + ((wet - dry) * filtG)) * K2A.ar(gain);
        Out.ar(out, wet);
        // Live feedback: this engine's sound, mono (equal-power sum), before the node FX
        Out.ar(fbbus.max(0), Mix.new(wet.asArray) * (if(n > 1) { 0.7071 } { 1 }) * (fbbus >= 0));
    }).writeDefFile(subDir);
});

// ── Live-input recorder (as GrainBoxRec) ───────────────────────────────────
// Feedback: the engine's own sound (its output synth, before the node FX,
// mono, on \fbbus; read one block late with InFeedback) is mixed back in:
// DC-blocked, through Tone (0..1: left = lowpass closing, middle = open,
// right = highpass opening), times Feedback, soft-saturated (tanh), so even
// Feedback > 1 thickens instead of exploding.
SynthDef(\GrainBoxPolyRec, {
    |in = 0, bufnum = 0, phasebus = 0, freeze = 0, vis = 1, fbbus = -1, fbamt = 0, fbtone = 0.5|
    var sig, run, phase, tick, hi, lo, fb, fbG, lpF, hpF;
    fb    = InFeedback.ar(fbbus.max(0), 1) * (fbbus >= 0);
    fbG   = Lag.kr(fbamt.clip(0, 2), 0.05);
    lpF   = Lag.kr(Select.kr(fbtone < 0.45, [20000, fbtone.linexp(0, 0.45, 200, 20000)]), 0.05);
    hpF   = Lag.kr(Select.kr(fbtone > 0.55, [10, fbtone.linexp(0.55, 1, 20, 4000)]), 0.05);
    fb    = HPF.ar(LPF.ar(LeakDC.ar(fb), lpF.min(SampleRate.ir * 0.45)), hpF);
    fb    = (fb * fbG).tanh;
    sig   = In.ar(in, 1) + fb;
    run   = 1 - freeze;
    RecordBuf.ar(sig, bufnum, 0, 1, 0, run, 1);
    phase = Phasor.ar(0, run, 0, BufFrames.kr(bufnum));
    Out.ar(phasebus, phase);
    tick  = Impulse.ar(125);
    hi    = Delay1.ar(RunningMax.ar(sig, tick));
    lo    = Delay1.ar(RunningMin.ar(sig, tick));
    SendReply.ar(tick * vis, '/grainRec', [hi, lo, phase / BufFrames.kr(bufnum).max(1)]);
}).writeDefFile(subDir);

"GrainBoxPoly SynthDefs written to: ".post; subDir.postln;
)
