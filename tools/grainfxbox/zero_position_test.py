#!/usr/bin/env python3
"""Render the real Jump delay chain returning to zero at near-full input level."""
from pathlib import Path
import argparse, importlib.util, subprocess
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('grain_render', ROOT/'tools/grainfxbox/render_test.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
bundled = ROOT.parents[1]/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Scsynth/bin/scsynth'
ap = argparse.ArgumentParser()
ap.add_argument('--server', type=Path, default=bundled if bundled.exists() else Path('/Applications/SuperCollider.app/Contents/Resources/scsynth'))
ap.add_argument('--output', type=Path, default=Path('/tmp/grain-zero-position'))
args = ap.parse_args()
w = args.output
w.mkdir(parents=True, exist_ok=True)
defs = r.compile_defs(w)
(w/'modulation.scd').write_text((ROOT/'synthdefs/Defaults/Modulation/modulation.scd').read_text().replace('(1..32).do', '(1..1).do'))
extra = r'''
SynthDef(\ZeroInput, {
    Out.ar(16, [SinOsc.ar(\left.kr(440), 0, 0.8), SinOsc.ar(\right.kr(660), 0, 0.8)])
}).writeDefFile("WORK");
SynthDef(\ZeroPosition, { Out.kr(700, \value.kr(0.25)) }).writeDefFile("WORK");
'''.replace('WORK', str(w))
(w/'extra.scd').write_text('try {"'+str(w/'modulation.scd')+'".load;\n'+extra+'\n"COMPILED".postln;0.exit;} {|e|e.reportError;1.exit;}')
compiled = subprocess.run([str(r.SCLANG), '-l', str(w/'config.yaml'), str(w/'extra.scd')],
    capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
    env={'HOME': str(w/'home'), 'PATH': '/usr/bin:/bin'})
assert compiled.returncode == 0 and 'COMPILED' in compiled.stdout and 'ERROR' not in compiled.stdout, compiled.stdout+compiled.stderr
defs += list((w/'CompiledSynthdefs/Modulation').glob('*.scsyndef')) + list(w.glob('Zero*.scsyndef'))

def render(name, block=64, static=False, aligned=True, late=0):
    score = r.bundle(0, [r.message('/d_recv', p.read_bytes()) for p in defs] + [
        r.message('/g_new', 1, 0, 0),
        r.message('/b_alloc', 0, 16*r.SR, 1)] + ([] if late else [r.message('/b_alloc', 1, 16*r.SR, 1)]))
    score += r.bundle(.01, [
        r.message('/s_new', 'ZeroInput', 100, 1, 1, 'left', 440. if aligned else 437.3, 'right', 660. if aligned else 653.7),
        r.message('/s_new', 'ZeroPosition', 101, 1, 1, 'value', 0. if static else .25),
        r.message('/s_new', 'ModRoute1', 102, 1, 1, 'src', 700., 'out', 800., 'amount', 1., 'scale', 1.),
        r.message('/s_new', 'GrainBoxPolyRecC_2', 103, 1, 1, 'in', 16., 'phasebus', 60., 'vis', 0.),
        r.message('/n_setn', 103, 'bufs', 2, 0., 1.),
        r.message('/s_new', 'GrainBoxPolyCtl_8', 104, 1, 1, 'ctl', 100., 'clk', 50.,
            'bufnum0', 0., 'bufnum1', 1., 'live', 1., 'phasebus', 60., 'vis', 0.,
            'outpoint', .3, 'amp', 1., 'levels', 1., 'delayglide', .003, 'posmodscale', 1.),
        r.message('/n_setn', 104, 'bufs', 2, 0., 1.),
        r.message('/s_new', 'GrainBoxPolyDJC_2', 105, 1, 1, 'out', 32., 'ctl', 100., 'idx', 0., 'modbus', 800., 'modw', 1.),
        r.message('/s_new', 'GrainBoxPolyOut_2', 106, 1, 1, 'in', 32., 'out', 0., 'ctl', 100., 'srck', 2.)])
    if late:
        # buffer 1 valid `late` blocks after the recorder starts (async /b_alloc)
        score += r.bundle(.01+late*block/r.SR, [r.message('/b_alloc', 1, 16*r.SR, 1)])
    if not static:
        # 100 BPM, beat-quantized delay positions. Continue through the 16 s
        # buffer wrap, with repeated returns to the exact zero target.
        for k in range(28):
            score += r.bundle(3+k*.6, [r.message('/n_set', 101, 'value', 0. if k % 2 == 0 else ((k % 8)+1)/8)])
    duration = 3. if static else 20.
    score += r.bundle(duration, [r.message('/c_set', 0, 0.)])
    osc = w/(name+'.osc')
    wav = w/(name+'.wav')
    osc.write_bytes(score)
    rendered = subprocess.run([str(args.server), '-N', str(osc), '_', str(wav), str(r.SR),
        'WAV', 'float', '-o', '2', '-i', '0', '-c', '4096', '-a', '1024', '-z', str(block),
        '-D', '0', '-U', str(args.server.parent/'plugins')], capture_output=True, text=True, timeout=60)
    assert rendered.returncode == 0, rendered.stdout+rendered.stderr
    data, ch, sr = r.read_wav(wav)
    audio = np.array(data).reshape(-1, ch)
    assert np.all(np.isfinite(audio))
    segment = audio[int((1. if static else 3.)*sr):int((duration-.1)*sr)]
    peak = np.max(np.abs(segment))
    assert .7 < peak < .83, (name, '0.8 input gained an artificial boost', peak)
    if static:
        for c in range(2):
            v=segment[:,c]
            spectrum=np.abs(np.fft.rfft(v*np.hanning(len(v))))**2
            frequency=np.fft.rfftfreq(len(v),1/sr)
            ratio=spectrum[frequency>3000].sum()/max(spectrum.sum(),1e-24)
            assert ratio<1e-5,(name,'channel',c,'high-frequency artifact at static zero',ratio)
    if not static:
        for k in range(0,28,2):
            t=3+k*.6
            v=audio[int((t-.01)*sr):int((t+.04)*sr),0]
            spectrum=np.abs(np.fft.rfft(v*np.hanning(len(v))))**2
            frequency=np.fft.rfftfreq(len(v),1/sr)
            ratio=spectrum[frequency>3000].sum()/max(spectrum.sum(),1e-24)
            assert ratio<1e-5,(name,'high-frequency artifact at zero',t,ratio)
    return peak

print('PASS: static zero remains clean, peak', render('static-zero', static=True), flush=True)
for late in (1, 3):
    # each channel must write at the shared phase, or Position 0 reads unwritten samples
    print('PASS: static zero with buffer 1 valid', late, 'blocks late, peak', render('static-zero-late%d' % late, static=True, late=late), flush=True)
for block in (64, 512):
    for aligned in (True, False):
        name = 'jump-'+str(block)+('-correlated' if aligned else '-different-phases')
        print('PASS:', name, 'zero transitions and buffer wrap; no clipping boost; peak',
            render(name, block=block, aligned=aligned), flush=True)
