// ChordBox's players (scChordBox.h): the progression's chord changes, sample
// accurately.
//
// ChordBoxClock: which chord plays now -> \rowbus (audio: the row of the
// chord tables), and the Trig / Root / ChPh / ProgPh outputs.
//   \mode 1: the transport (MetroBox's clock: scTransportSync anchors,
//   \base, 48 ticks per beat). \marks: 4 channels per tick, 4 chunks of 8
//   beats (1536 frames): the step's row + 1 (0: none), 1 on the tick a step
//   starts, the ticks into the step, the ticks into the lap.
//   \mode 0: Index. The node's chord for the Index value (row \liveRow, a new
//   one each \liveCount), or an Index cable (\index_sel 1): the row 256 +
//   the index (wrapped to \steps), sample accurately.
//   \info: per row [root pitch class, step ticks, lap ticks, entry].
// ChordBoxVoices: one per output: its notes for the row (\rows: 16 channels
// per row), gliding over \glide seconds (linear), 16 channels out.

(
var root, dir, clocked = 4 * 384;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "ChordBox";
File.mkdir(root); File.mkdir(dir);

SynthDef(\ChordBoxClock, {
    var mode = \mode.kr(1);
    var marks = \marks.kr(-1), info = \info.kr(-1);
    // transport
    var anchorId = \anchorId.kr(0), anchorHard = \anchorHard.kr(0);
    var anchorPos = \anchorPos.ar(0), anchorFire = \anchorFire.ar(0), run = \run.ar(0), bpm = \bpm.kr(120), base = \base.ar(0);
    var tps = bpm / 60 * 48;
    var anchorTrig = T2A.ar(Changed.kr(anchorId)), hardTrig = T2A.ar(Changed.kr(anchorHard));
    var pos = anchorPos + Sweep.ar(anchorTrig, tps * run);
    var idx = pos.floor;
    var reset = (hardTrig + (anchorTrig * (idx < (Delay1.ar(idx) - 2)))) > 0;
    var top = RunningMax.ar(idx, reset);
    // any new tick (a jump too: the chord there is read at once)
    var tick = ((idx > Delay1.ar(top)) * (1 - reset)) + reset;
    var fireNew = ((idx > Delay1.ar(top)) * (1 - reset)) + (hardTrig * anchorFire);
    var frame = (base + idx - anchorPos.floor) % clocked;
    var m = BufRd.ar(4, marks, frame, 0, 1);
    var rowT = Latch.ar(m[0] - 1, tick * (m[0] > 0));
    var trigT = fireNew * (m[1] > 0) * (run > 0);
    var intoStep = Latch.ar(m[2], tick), intoLap = Latch.ar(m[3], tick);
    var frac = (pos - idx).clip(0, 1);
    // index
    var sel = \index_sel.kr(0), steps = \steps.kr(4).max(1);
    var cableRow = 256 + (\index_ar.ar(0).round % steps);
    var liveRow = K2A.ar(\liveRow.kr(288));
    var rowI = Select.ar(sel > 0.5, [liveRow, cableRow]);
    var trigI = Select.ar(sel > 0.5, [T2A.ar(Changed.kr(\liveCount.kr(0))), (HPZ1.ar(cableRow).abs > 0)]);
    var row = Select.ar(mode > 0.5, [rowI, rowT]);
    var trig = Select.ar(mode > 0.5, [trigI, trigT]);
    var r = BufRd.ar(4, info, row.max(0), 0, 1);
    var chph = Select.ar(mode > 0.5, [DC.ar(0), ((intoStep + frac) / r[1].max(1)).clip(0, 1)]);
    var progph = Select.ar((mode > 0.5) * (r[2] > 0), [DC.ar(0), ((intoLap + frac) / r[2].max(1)).clip(0, 1)]);
    Out.ar(\rowbus.kr(0), row);
    Out.ar(OceanodeOutput.kr(\trigout), trig > 0);
    Out.ar(OceanodeOutput.kr(\rootout), r[0]);
    Out.ar(OceanodeOutput.kr(\chphout), chph);
    Out.ar(OceanodeOutput.kr(\progphout), progph);
}).writeDefFile(dir);

SynthDef(\ChordBoxVoices, {
    var row = In.ar(\rowbus.kr(0), 1).max(0);
    var notes = BufRd.ar(16, \rows.kr(-1), row, 0, 1);
    var glide = \glide.kr(0).max(0);
    Out.ar(OceanodeOutput.kr(\pitchout), notes.collect { |n| VarLag.ar(n, glide, 0, \lin) });
}).writeDefFile(dir);

"ChordBox SynthDefs written to ".post; dir.postln;
)
