#!/usr/bin/env python3
"""Offline GrainFxBox recorder -> shared Ramp/route -> voice -> wet/dry checks.
Run render_test.py [--output DIR] to run the full routing/preset suite.
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
        if lfo.get('LFO1_Ramp',False):
            hz=(2.0 if lfo['LFO1_Sync'] else 1.0)/lfo['LFO1_Ramp_Duration']
            generator=message('/s_new','ModRamp1',1010,1,1,'out',700.0,'gate',float(GATE),'freq',hz,
                              'from',lfo['LFO1_Ramp_From'],'to',lfo['LFO1_Ramp_To'],'curve',lfo['LFO1_Ramp_Curve'],
                              'offset',lfo['LFO1_Ramp_Offset'],'ratemode',float(lfo['LFO1_Ramp_Integrate']),
                              'ending',float(lfo['LFO1_Ramp_End']),'bounds',float(lfo['LFO1_Ramp_Bounds']),
                              'pow',float(np.asarray(lfo.get('LFO1_Pow',0)).flat[0]),'bipow',float(np.asarray(lfo.get('LFO1_BiPow',0)).flat[0]))
        else:
            hz=2.0/lfo['LFO1_Beats'] if lfo['LFO1_Sync'] else lfo['LFO1_Hz']
            generator=message('/s_new','ModLfoX1',1010,1,1,'out',700.0,'gate',float(GATE),'freq',hz,
                              'round',0.0,'skew',lfo['LFO1_Skew'],'trigmode',float(lfo['LFO1_Trigger_Mode']),
                              'phasepower',lfo['LFO1_Ramp_Power'],'restartsec',lfo['LFO1_Restart_Beats']*0.5)
        span=lfo['Position_Mod_Span']*(0.5 if lfo['Position_Mod_Units']==2 else 0.001)
        mod_messages=[generator,message('/s_new','ModRoute1',1011,1,1,'src',700.0,'out',800.0,'amount',lfo['gbpModMatrix'][0],'scale',1.0)]
        ctl_msg=message('/s_new','GrainBoxPolyCtl_8',1003,1,1,*[a for a in ctl if a!='$bufs' and a!='bufs'],
                        'posmodscale',span/(buffer_seconds-0.01),'freezegate',float(lfo.get('Freeze_On_Gate',False)),
                        'scanpitch',float(lfo.get('Follow_Scan_Pitch',False)),'gatebus',float(GATE))
    voice_args = ['ampbus' ,float(AMP),'modw',1.0,'gatebus',GATE] if preset else []
    if lfo: voice_args+=['modbus',800.0,'modw',1.0,'spansec',span]
    score += bundle(0.01, [
        message('/s_new', src, 1000, 1, 1),
        message('/s_new', 'GBTestGate', 1001, 1, 1),
        *mod_messages,
        message('/s_new', 'GrainBoxPolyRecC_2', 1002, 1, 1, 'in', IN, 'phasebus', PHASE, 'vis', 0.0, 'gatefreeze', CTL + 97),
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
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/grainfxbox-ramp-render'))
    args=ap.parse_args()
    subprocess.run([sys.executable,str(ROOT/'tools/grainfxbox/routing_test.py'),'--output',str(args.output)],check=True)

if __name__=='__main__': main()
