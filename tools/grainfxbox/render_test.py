#!/usr/bin/env python3
"""GrainFxBox offline checks (scsynth NRT, no app): one engine's real chain
(recorder -> control -> voice -> output -> dry) on a stereo test input,
L = 440 Hz, R = 660 Hz.

  1. Source placement (1:1), grains, wet only: L stays 440, R stays 660.
  2. Wet Mode Gated + Gate Motion tape stop (1 -> 0 over 1 s, Grainless):
     before the gate the dry passes; halfway through the stop the tape plays
     at half speed (220 / 330 Hz) with the dry muted; then silence.
  3. Stutter (Freeze + Repeat, grains) on a rising sweep: while the gate is
     up the output keeps repeating the quarter second before it.

Compiles scGrainBoxPoly.scd (reduced: Ctl_8, 2 channels) with the bundled
sclang into the output folder.   tools/grainfxbox/render_test.py [--output DIR]
"""
import argparse, json, pathlib, shutil, subprocess, sys
import numpy as np
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

SCLANG = ROOT / 'sclang/osx/sclang'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
SR = 48000
IN, GATE, MIX, CLK, PHASE, CTL = 16, 20, 32, 50, 60, 100   # buses (audio / control)
AMP = 1000   # outside Ctl_8's 104 + 15*8 control channels


def compile_defs(work):
    s = (ROOT / 'synthdefs/Defaults/GrainBox/scGrainBoxPoly.scd').read_text()
    s = s.replace('caps = [8, 32, 128, 500]', 'caps = [8]').replace('(2..16).do', '(2..2).do').replace('(1..16).do', '(2..2).do')
    (work / 'gb.scd').write_text(s)
    probes = ('SynthDef(\\GBTestSrc, { Out.ar(%d, [SinOsc.ar(440, 0, 0.25), SinOsc.ar(660, 0, 0.25)]) }).writeDefFile("%s");\n'
              'SynthDef(\\GBTestSweep, { Out.ar(%d, SinOsc.ar(Line.ar(200, 1000, 4)) * 0.25 ! 2) }).writeDefFile("%s");\n'
              'SynthDef(\\GBTestGate, { Out.ar(%d, K2A.ar(\\gate.kr(0))) }).writeDefFile("%s");\n' % (IN, work, IN, work, GATE, work))
    probes += ('SynthDef(\\GBTestMotion, { |ctl=100| Out.ar(0, K2A.ar(In.kr(ctl+98, 2))) }).writeDefFile("%s");\n' % work)
    (work / 'run.scd').write_text('"%s".load;\n%s"COMPILED".postln;\n0.exit;\n' % (work / 'gb.scd', probes))
    (work / 'config.yaml').write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    home = work / 'home'; home.mkdir(exist_ok=True)
    r = subprocess.run([str(SCLANG), '-l', str(work / 'config.yaml'), str(work / 'run.scd')], capture_output=True, text=True,
                       timeout=900, stdin=subprocess.DEVNULL, env={'HOME': str(home), 'PATH': '/usr/bin:/bin'})
    assert 'COMPILED' in r.stdout and 'ERROR' not in r.stdout, r.stdout[-3000:]
    return sorted((work / 'CompiledSynthdefs/GrainBoxPoly').glob('*.scsyndef')) + sorted(work.glob('GBTest*.scsyndef'))


def render(work, defs, name, voice, ctl_args, dry_args, gate_events, dur, src='GBTestSrc', buffer_seconds=4, preset=None, lfo=None):
    frames = buffer_seconds * SR
    t0 = [message('/d_recv', p.read_bytes()) for p in defs]
    t0 += [message('/b_alloc', 0, frames, 1), message('/b_alloc', 1, frames, 1), message('/g_new', 1, 0, 0)]
    if preset:
        # Match the production custom envelope (linear, attack/release normalized).
        atk, rel = preset['EnvAttack'], preset['EnvRelease']
        total = max(1.0,atk+rel); atk /= total; rel /= total
        env = [min(1.0, t/max(atk,1e-9), (1-t)/max(rel,1e-9)) for t in np.linspace(0,1,512)]
        t0 += [message('/b_alloc',2,512,1),message('/c_setn',AMP,8,0.005,0.01,1.0,0.05,0.0,0.0,0.0,1.0)]
    score = bundle(0, t0)
    ctl = ['ctl', CTL, 'clk', CLK, 'bufnum0', 0, 'bufnum1', 1, 'bufs', '$bufs', 'live', 1.0, 'phasebus', PHASE,
           'envbuf', -1.0, 'bpm', 120.0, 'inpoint', 0.0, 'outpoint', 1.0, 'vis', 0.0] + ctl_args
    ctl_msg = message('/s_new', 'GrainBoxPolyCtl_8', 1003, 1, 1, *[a for a in ctl if a != '$bufs' and a != 'bufs'])
    if preset:
        score += bundle(0.005,[message('/b_setn',2,0,512,*env)])
    mod_messages=[]
    if lfo:
        hz=2.0/lfo['LFO1_Beats'] if lfo['LFO1_Sync'] else lfo['LFO1_Hz']
        span=lfo['Position_Mod_Span']*(0.5 if lfo['Position_Mod_Units']==2 else 0.001)
        mod_messages=[message('/s_new','ModLfoX1',1010,1,1,'out',700.0,'gate',float(GATE),'freq',hz,
                              'round',0.0,'skew',lfo['LFO1_Skew'],'trigmode',float(lfo['LFO1_Trigger_Mode']),
                              'phasepower',lfo['LFO1_Ramp_Power'],'restartsec',lfo['LFO1_Restart_Beats']*0.5),
                      message('/s_new','ModRoute1',1011,1,1,'src',700.0,'out',800.0,'amount',lfo['gbpModMatrix'][0],'scale',1.0)]
        ctl_msg=message('/s_new','GrainBoxPolyCtl_8',1003,1,1,*[a for a in ctl if a!='$bufs' and a!='bufs'],
                        'posmodscale',span/(buffer_seconds-0.01))
    voice_args = ['ampbus' ,float(AMP),'modw',1.0,'gatebus',GATE] if preset else []
    if lfo: voice_args+=['modbus',800.0,'modw',1.0,'spansec',span]
    score += bundle(0.01, [
        message('/s_new', src, 1000, 1, 1),
        message('/s_new', 'GBTestGate', 1001, 1, 1),
        *mod_messages,
        message('/s_new', 'GrainBoxPolyRecC_2', 1002, 1, 1, 'in', IN, 'phasebus', PHASE, 'vis', 0.0, 'tfrz', CTL + 100),
        message('/n_setn', 1002, 'bufs', 2, 0.0, 1.0),
        ctl_msg,
        message('/n_setn', 1003, 'bufs', 2, 0.0, 1.0),
        message('/s_new', voice, 1004, 1, 1, 'out', MIX, 'ctl', CTL, 'clk', CLK, 'idx', 0, *voice_args),
        message('/s_new', 'GrainBoxPolyOut_2', 1005, 1, 1, 'in', MIX, 'out', 0, 'ctl', CTL, 'srck', 2.0),
        message('/s_new', 'GrainBoxPolyDry_2', 1006, 1, 1, 'in', IN, 'out', 0, *dry_args)])
    for t, g in gate_events:
        score += bundle(t, [message('/n_set', 1001, 'gate', float(g))])
    score += bundle(dur, [message('/c_set', 0, 0.0)])
    osc = work / (name + '.osc'); osc.write_bytes(score)
    wav = work / (name + '.wav')
    r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', '2', '-i', '0',
                        '-c', '4096', '-a', '1024'], capture_output=True, text=True, timeout=300)
    assert wav.exists(), r.stdout[-2000:] + r.stderr[-2000:]
    data, ch, rate = read_wav(wav)
    return np.array(data, dtype=np.float64).reshape(-1, ch)


def level(x, a, b):
    seg = x[int(a * SR):int(b * SR)]
    return float(np.sqrt(np.mean(seg ** 2)) + 1e-12)


def tone(x, a, b, f):
    seg = x[int(a * SR):int(b * SR)]
    n = np.arange(len(seg))
    return float(abs(np.sum(seg * np.exp(-2j * np.pi * f * n / SR))) * 2 / len(seg))


def peak(x, a, b):
    seg = x[int(a * SR):int(b * SR)] * np.hanning(int(b * SR) - int(a * SR))
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    return float(np.argmax(spec) * SR / (1 << 17))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=pathlib.Path, default=pathlib.Path('/tmp/grainfxbox-render'))
    a = ap.parse_args()
    shutil.rmtree(a.output, ignore_errors=True); a.output.mkdir(parents=True)
    defs = compile_defs(a.output)
    ok = True

    def check(cond, text):
        nonlocal ok
        ok &= bool(cond)
        print(('PASS ' if cond else 'FAIL ') + text)

    # 1. Source placement, grains (40 ms interval, overlap 2, 200 ms behind), wet only
    grains = ['autotrig', 1.0, 'trigdur', 1.0, 'overlap', 2.0, 'autotrigbeatdiv', '$div',
              'amp', 1.0, 'pitch', 0.0, 'duration', 0.1, 'position', 0.05, 'chance', 1.0, 'levels', 1.0]
    args = []
    for i, v in enumerate(grains):
        args.append(12.5 if v == '$div' else v)   # 25 grains/s at 120 bpm = 12.5 per beat
    x = render(a.output, defs, 'source', 'GrainBoxPolyVC_2', args, ['dry', 0.0], [], 3.0)
    L, R = x[:, 0], x[:, 1]
    l440, l660, r440, r660 = tone(L, 1, 2.5, 440), tone(L, 1, 2.5, 660), tone(R, 1, 2.5, 440), tone(R, 1, 2.5, 660)
    print('  L: 440 %.3f 660 %.3f   R: 440 %.3f 660 %.3f' % (l440, l660, r440, r660))
    check(l440 > 0.05 and r660 > 0.05, 'Source: both channels sound')
    check(l440 > 20 * l660 and r660 > 20 * r440, 'Source: L keeps 440 Hz, R keeps 660 Hz (1:1, > 26 dB apart)')

    # 2. Wet Mode Gated + tape stop (Grainless): gate 1.5 .. 3.5 s
    tape = ['amp', 1.0, 'position', 0.0, 'levels', 1.0, 'tmotion', 1.0, 'tstart', 1.0, 'tend', 0.0,
            'ttime', 1.0, 'tcurve', 0.0, 'livelen', 3.98, 'gatebus', GATE, 'tenvbus', -1.0]
    x = render(a.output, defs, 'tapestop', 'GrainBoxPolyDC_2', tape, ['dry', 1.0, 'gated', 1.0, 'gatebus', GATE],
               [(1.5, 1), (3.5, 0)], 4.0)
    L, R = x[:, 0], x[:, 1]
    # before the gate: dry (0.25) + the tape at 1 sample's delay (0.25), in phase
    print('  before: L %.3f   during 1.85-2.15: L peak %.0f Hz R peak %.0f Hz   after stop: %.4f' %
          (level(L, 0.5, 1.4), peak(L, 1.85, 2.15), peak(R, 1.85, 2.15), level(L, 2.7, 3.4)))
    check(abs(level(L, 0.5, 1.4) - 0.354) < 0.03, 'Gated: before the gate the dry and the in-step tape (one sample apart) pass')
    check(abs(peak(L, 1.85, 2.15) - 220) < 25 and abs(peak(R, 1.85, 2.15) - 330) < 35,
          'Tape stop: half way the tape is at half speed (220 / 330 Hz), each channel its own')
    check(tone(L, 1.85, 2.15, 440) < 0.02, 'Gated: the dry (440 Hz) is muted while the gate is up')
    check(level(L, 2.7, 3.4) < 0.003 and level(R, 2.7, 3.4) < 0.003, 'Tape stop: silent once stopped')
    check(level(L, 3.7, 3.95) > 0.1, 'Gate down: the dry is back')
    # 3. Stutter: Freeze + Repeat every 0.25 s at speed 1, grains; gate 1.5 .. 3.5 s
    stut = args + ['tmotion', 1.0, 'tstart', 1.0, 'tend', 1.0, 'ttime', 0.25, 'trepeat', 1.0, 'tfreeze', 1.0,
                   'livelen', 3.98, 'gatebus', GATE, 'tenvbus', -1.0]
    stut[stut.index('position') + 1] = 0.0
    x = render(a.output, defs, 'stutter', 'GrainBoxPolyVC_2', stut, ['dry', 1.0, 'gated', 1.0, 'gatebus', GATE],
               [(1.5, 1), (3.5, 0)], 4.0, src='GBTestSweep')
    L = x[:, 0]
    early, late = peak(L, 1.6, 1.85), peak(L, 3.1, 3.35)
    print('  sweep at the gate ~500 Hz; looped 1.6-1.85 s: %.0f Hz, 3.1-3.35 s: %.0f Hz (live would be ~830)' % (early, late))
    check(440 < early < 510 and 440 < late < 510, 'Stutter: the frozen quarter second before the gate repeats')
    check(level(L, 1.6, 3.4) > 0.1, 'Stutter: it sounds')

    # Stretch scans the recording independently of each grain's pitch. A pure
    # tone must retain its pitch even while scan speed falls all the way to 0.
    stretch = args + ['position', 0.0, 'tmotion', 1.0, 'tmode', 1.0,
                     'tstart', 1.0, 'tend', 0.0, 'ttime', 1.0,
                     'livelen', 3.98, 'gatebus', GATE, 'tenvbus', -1.0]
    x = render(a.output, defs, 'stretch-freeze', 'GrainBoxPolyVC_2', stretch, ['dry', 0.0],
               [(1.5, 1), (3.5, 0)], 4.0)
    check(abs(peak(x[:, 0], 2.7, 3.3)-440) < 12 and abs(peak(x[:, 1], 2.7, 3.3)-660) < 12,
          'Stretch: zero scan speed sustains original 440 / 660 Hz pitches')
    check(level(x[:, 0], 2.7, 3.3) > 0.05, 'Stretch: stopped scan keeps sounding')
    half = stretch + ['tstart', 0.5, 'tend', 0.5]
    x = render(a.output, defs, 'stretch-sweep', 'GrainBoxPolyVC_2', half, ['dry', 0.0],
               [(1.5, 1), (3.5, 0)], 4.0, src='GBTestSweep')
    f1, f2 = peak(x[:, 0], 1.9, 2.1), peak(x[:, 0], 2.9, 3.1)
    print('  half-speed scan: sweep %.0f -> %.0f Hz (expected ~550 -> ~650)' % (f1, f2))
    check(abs(f1-550) < 35 and abs(f2-650) < 35,
          'Stretch: recording advances at half speed without halving grain pitch')

    # The reference graph is a saw delay (SpeedA is delay slope), hard-reset by
    # Go and by its BeatsBack clock. Compare actual control-synth output with
    # that independently recovered equation for all eight factory adaptations.
    source = (ROOT/'src/nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp').read_text()
    factory = source[source.index('std::vector<ofJson> scGrainBoxPoly::factorySnapshots'):]
    banks = [json.loads(part.split(')JSON"',1)[0]) for part in factory.split('R"JSON(')[1:3]]
    bank = {p['name']:p.get('set',{}) for p in banks[1]}
    reference = json.loads((ROOT/'tools/grainfxbox/grain_stretch_reference.json').read_text())
    for ref in reference['snapshots']:
        name = 'GrainStretch %02d - %s' % (ref['slot'],ref['name'])
        p = bank[name]
        assert p['Motion Start'] == p['Motion End'] == 1-ref['speed_a']
        assert p['Motion Time'] == ref['beats_back'] and p['Motion Beats'] and p['Motion Repeat']
        assert p['Interval'] == ref['grain_beats'] and abs(p['Grain Tail']-ref['decay_ms']) < 1e-5
        assert p['Pitch'] == ref['pitch'] and p['TrigDur'] and p['Overlap'] == 1
        window = ref['beats_back']/2
        ctl = ['tmotion',1.0,'tmode',2.0,'tstart',p['Motion Start'],'tend',p['Motion End'],
               'ttime',window,'trepeat',1.0,'gatebus',GATE,'livelen',11.98]
        x = render(a.output,defs,'reference-%02d'%ref['slot'],'GBTestMotion',ctl,['dry',0.0],
                   [(1.5,1),(10.0,0),(10.2,1)],11.0,buffer_seconds=12)
        errors = []
        for t in np.arange(1.6,9.9,0.071):
            elapsed = (t-1.5) % window
            expected = (elapsed*ref['speed_a']) % window
            # Discontinuities can straddle an audio/control block.
            if min(elapsed,window-elapsed,expected,window-expected) < 0.03 and ref['speed_a']:
                continue
            errors.append(abs(x[int(t*SR),0]-expected))
        check(max(errors) < 0.025 and np.max(np.abs(x[SR:,1]-1)) < 0.001,
              '%s: reference delay sweep, wrap/reset and independent grain rate' % name)
        check(abs(x[int(10.3*SR),0] - ((0.1*ref['speed_a']) % window)) < 0.025,
              '%s: Go retriggers from a fresh ramp' % name)
        audio_ctl = ctl + ['autotrig',1.0,'autotrigbeatdiv',1/p['Interval'],'trigdur',1.0,
                          'overlap',p['Overlap'],'graintail',p['Grain Tail']/1000,
                          'envbuf',2.0,'pitch',p['Pitch'],'position',0.0,'amp',1.0,'levels',1.0]
        audio = render(a.output,defs,'preset-%02d'%ref['slot'],'GrainBoxPolyVC_2',audio_ctl,
                       ['dry',1.0,'gated',1.0,'gatebus',GATE],[(9.0,1),(11.0,0)],11.5,
                       buffer_seconds=12,preset=p)
        check(np.all(np.isfinite(audio)) and level(audio[:,0],9.8,10.8)>0.01,
              '%s: actual grains, tail and gated envelope produce audio' % name)
        check(abs(level(audio[:,0],8.0,8.8)-0.25/np.sqrt(2)) < 0.005
              and abs(level(audio[:,0],11.2,11.4)-0.25/np.sqrt(2)) < 0.005,
              '%s: clean input before Go and after Stop' % name)
    print('ALL PASS' if ok else 'SOME CHECKS FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
