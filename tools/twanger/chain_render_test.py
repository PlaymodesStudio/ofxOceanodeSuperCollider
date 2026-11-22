#!/usr/bin/env python3
"""Offline render of the whole t'wanGer chain (Twanger8 -> TwangerFX1) per preset.

  chain_render_test.py <compiled dir with Twanger/ and TwangerFX/> <out.wav> [preset names... | all]
Each preset plays C3 G3 C4 E4 (staggered 0.25 s, gate 0.8, 1.2 s) and rings out; 4 s per preset.
Prints peak, RMS and NaN checks per preset.
"""
import sys, struct, subprocess, pathlib, math
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import twanger_presets
from voice_render_test import message, bundle, read_wav, SCSYNTH, PLUGINS
VOICE = ['att','rel','cut','res','vel','spread','filter','r1damp','r1undamp','r1cut','r1res','r1amt','r1dec','r1rel',
         'r2hicut','r2tune','r2damp','r2on','timbre','scale','trans','dton','sub','bass','mid','freq','treb','sup']
FX = ['delay','time','rev','lpf','drywet','rvon','level']
def diffusers(v):
    return [float(v.get('ap%ddly' % i, 0)) for i in range(1, 6)] + [float(v.get('ap%dfine' % i, 0)) for i in range(1, 6)] + \
           [float(v.get('ap%dpol' % i, 1)) for i in range(1, 6)] + [float(v.get('rndamp', 0)), float(v.get('rndrate', 1))]
def main():
    comp, wav = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    presets = twanger_presets.load()[0]
    names = sys.argv[3:]
    if names and names != ['all']: presets = [p for p in presets if p['name'] in names]
    score = bundle(0, [message('/d_recv', (comp / 'Twanger/Twanger8.scsyndef').read_bytes()),
                       message('/d_recv', (comp / 'TwangerFX/TwangerFX1.scsyndef').read_bytes())])
    dur = 4.0; notes = [48, 55, 60, 64]
    for k, p in enumerate(presets):
        v = p['values']; t = k * dur + 0.05
        va = []; fa = []
        for n in VOICE: va += [n, float(v[n])]
        for n in FX: fa += [n, float(v[n])]
        score += bundle(t, [message('/s_new', 'Twanger8', 2000 + k, 0, 0, 'out', 16, 'unison', 2.0, *va),
                            message('/s_new', 'TwangerFX1', 3000 + k, 1, 0, 'in', 16, 'out', 0, *fa),
                            message('/n_setn', 3000 + k, 'diffusers', 17, *diffusers(v))])
        gates = [0.0] * 8; pitches = [60.0] * 8
        for i, n in enumerate(notes):
            pitches[i] = float(n)
            score += bundle(t + 0.05 + 0.25 * i, [message('/n_setn', 2000 + k, 'pitch', 8, *pitches),
                                                 message('/n_setn', 2000 + k, 'gate', 8, *[0.8 if j <= i else 0.0 for j in range(8)])])
        score += bundle(t + 1.6, [message('/n_setn', 2000 + k, 'gate', 8, *[0.0] * 8)])
        score += bundle(t + dur - 0.02, [message('/n_free', 2000 + k, 3000 + k)])
    score += bundle(len(presets) * dur + 0.05, [message('/c_set', 0, 0.0)])
    osc = wav.with_suffix('.osc'); osc.write_bytes(score)
    r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), '48000', 'WAV', 'float', '-o', '2', '-U', PLUGINS],
                       capture_output=True, text=True)
    if r.returncode not in (0,) and not wav.exists(): print(r.stdout[-2000:], r.stderr[-2000:]); return
    data, ch, sr = read_wav(wav)
    for k, p in enumerate(presets):
        seg = data[int(k * dur * sr) * ch:int((k + 1) * dur * sr) * ch]
        bad = sum(1 for s in seg if s != s)
        peak = max(abs(s) for s in seg); rms = math.sqrt(sum(s * s for s in seg) / len(seg))
        print('%-36s peak %6.3f  rms %.4f %s' % (p['name'], peak, rms, ' NaN %d' % bad if bad else ''))
if __name__ == "__main__": main()
