#!/usr/bin/env python3
"""Offline render of TwangerVoice alone for a few presets (no EQ / reverb).

  voice_render_test.py <TwangerVoiceTest.scsyndef> <out dir> [preset names...]
Notes 36, 48, 60 at gate 0.8, held 1 s, 2 s apart, unison 2. Prints peak / RMS
and the pitch measured by autocorrelation of each note's tail.
"""
import sys, struct, subprocess, pathlib, math
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import twanger_presets
VOICE = ['att','rel','cut','res','vel','spread','filter','r1damp','r1undamp','r1cut','r1res','r1amt','r1dec','r1rel',
         'r2hicut','r2tune','r2damp','r2on','timbre','scale','trans','dton']
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
PLUGINS = '/Applications/SuperCollider.app/Contents/Resources/plugins:' + str(pathlib.Path(__file__).resolve().parents[2] / 'ugens/OceanodeTwanger')

def string(s):
    b = s.encode() + b'\0'; return b + b'\0' * ((-len(b)) % 4)
def message(address, *args):
    tags = ','; data = b''
    for v in args:
        if isinstance(v, str): tags += 's'; data += string(v)
        elif isinstance(v, bytes): tags += 'b'; data += struct.pack('>I', len(v)) + v + b'\0' * ((-len(v)) % 4)
        elif isinstance(v, int): tags += 'i'; data += struct.pack('>i', v)
        else: tags += 'f'; data += struct.pack('>f', v)
    return string(address) + string(tags) + data
def bundle(t, messages):
    b = b'#bundle\0' + struct.pack('>Q', round(t * 2**32))
    for m in messages: b += struct.pack('>I', len(m)) + m
    return struct.pack('>I', len(b)) + b
def read_wav(path):
    b = path.read_bytes(); p = 12
    while p < len(b) - 8:
        tag, size = struct.unpack_from('<4sI', b, p); p += 8
        if tag == b'fmt ': fmt, ch, rate = struct.unpack_from('<HHI', b, p)
        if tag == b'data': return struct.unpack_from('<' + 'f' * (size // 4), b, p), ch, rate
        p += size + (size % 2)

def pitch_of(x, sr):
    n = len(x); best = None
    for lag in range(int(sr / 2000), int(sr / 25)):
        s = sum(x[i] * x[i + lag] for i in range(0, n - lag, 4))
        if best is None or s > best[0]: best = (s, lag)
    return 69 + 12 * math.log2(sr / best[1] / 440) if best and best[0] > 0 else float('nan')

def main():
    defpath, outdir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    names = sys.argv[3:] or ['acousticGitarroid']
    presets = {p['name']: p for p in twanger_presets.load()[0]}
    notes = [36, 48, 60]
    for name in names:
        v = presets[name]['values']
        args = []
        for k in VOICE: args += [k, float(v[k])]
        args += ['unison', 2.0]
        score = bundle(0, [message('/d_recv', defpath.read_bytes())])
        score += bundle(0.01, [message('/s_new', 'TwangerVoiceTest', 1000, 0, 0, *args)])
        for i, n in enumerate(notes):
            t = 0.1 + 2 * i
            score += bundle(t, [message('/n_set', 1000, 'pitch', float(n), 'gate', 0.8)])
            score += bundle(t + 1.0, [message('/n_set', 1000, 'gate', 0.0)])
        score += bundle(0.1 + 2 * len(notes), [message('/c_set', 0, 0.0)])
        osc = outdir / (name + '.osc'); wav = outdir / (''.join(c for c in name if c.isalnum()) + '.wav')
        osc.write_bytes(score)
        r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), '48000', 'WAV', 'float', '-o', '2', '-U', PLUGINS],
                           capture_output=True, text=True)
        data, ch, sr = read_wav(wav)
        mono = data[0::ch]
        print(name, 'peak %.3f' % max(abs(s) for s in mono))
        for i, n in enumerate(notes):
            a = int((0.1 + 2 * i + 0.3) * sr); seg = mono[a:a + 4096]
            rms = math.sqrt(sum(s * s for s in seg) / len(seg))
            print('   note %d  rms %.4f  pitch %.2f' % (n, rms, pitch_of(seg, sr)))
if __name__ == "__main__": main()
