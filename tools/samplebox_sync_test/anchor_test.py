#!/usr/bin/env python3
"""NRT render of a synced, looping SampleBox pad reading a sine buffer:
measures the read position's noise and its step at each transport anchor."""
import math, pathlib, struct, subprocess, sys
OUT = pathlib.Path(sys.argv[1]); DEF = pathlib.Path(sys.argv[2])
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
SR = 48000; FRAMES = 605112; PERIOD = 200.0; FIT = 32.0; BPM = 140.0; DUR = 9.0
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
        if tag == b'data': return struct.unpack_from('<' + 'f' * (size // 4), b, p)
        p += size + (size % 2)
src = OUT / 'sine.wav'
if not src.exists():
    data = struct.pack('<%df' % FRAMES, *[0.5 * math.sin(2 * math.pi * n / PERIOD) for n in range(FRAMES)])
    src.write_bytes(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, 1, 44100, 44100 * 4, 4, 32) + b'data' + struct.pack('<I', len(data)) + data)
def render(name, a0, wrap, seek=0.0, grid=False):
    beat = 60.0 / BPM
    t0 = 0.1
    score = bundle(0, [message('/d_recv', DEF.read_bytes()), message('/b_allocRead', 0, str(src)), message('/g_new', 1, 0, 0)])
    anchors = []
    q = (lambda t: round(t * SR / 64) * 64 / SR) if grid else (lambda t: t)
    t0 = q(t0)
    def pos(k):
        p = a0 + (q(t0 + k * 4 * beat) - t0) / beat + (seek if k >= 3 else 0.0)
        return p % wrap if wrap else p
    score += bundle(t0, [message('/s_new', DEF.stem, 1001, 0, 1, 'buf0', 0, 'loaded', 1.0, 'loop', 1.0, 'sync', 1.0,
        'fitbeats', FIT, 'fitrate', 1.0, 'tbpm', BPM, 'run', 1.0, 'anchorPos', float(pos(0)), 'anchorId', 1.0,
        'modbus', 1000, 'vis', 2000, 'velbus', 2100, 'rndbus', 2200, 'gatebus', 100, 'chokeblock', 2300, 'latestbus', 2400,
        'out', 0, 'gain', 1.0, 'velocity', 1.0, 'pitch', 60.0, 'poly', 1.0, 'voices', 8.0)])
    score += bundle(t0 + 0.02, [message('/n_set', 1001, 'fire', 1.0)])
    k = 1
    while t0 + k * 4 * beat < DUR - 0.2:
        t = q(t0 + k * 4 * beat)
        anchors.append(t)
        score += bundle(t, [message('/n_set', 1001, 'tbpm', BPM, 'run', 1.0, 'anchorPos', float(pos(k)), 'anchorId', float(k + 1))])
        k += 1
    score += bundle(DUR, [message('/c_set', 0, 0.0)])
    osc = OUT / (name + '.osc'); wav = OUT / (name + '.wav'); osc.write_bytes(score)
    if wav.exists(): wav.unlink()
    subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', '1', '-c', '4096'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    y = read_wav(wav)
    # ideal phase of the buffer's sine, up to a constant
    rate = FRAMES / (FIT * beat) / SR          # frames per output sample
    w = 2 * math.pi * rate / PERIOD
    edges = [t0 + 0.3] + anchors + [DUR - 0.05]
    print(f'{name}:')
    base = None
    for i in range(len(edges) - 1):
        n0 = int((edges[i] + 0.01) * SR); n1 = int((edges[i + 1] - 0.01) * SR)
        ss = cc = sc = ys = yc = yy = 0.0
        for n in range(n0, n1):
            s = math.sin(w * n); c = math.cos(w * n); v = y[n]
            ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c; yy += v * v
        det = ss * cc - sc * sc
        a = (ys * cc - yc * sc) / det; b = (yc * ss - ys * sc) / det
        res = yy - a * ys - b * yc
        ph = math.atan2(b, a) * PERIOD / (2 * math.pi)      # frames
        if base is None: base = ph
        d = (ph - base + PERIOD / 2) % PERIOD - PERIOD / 2
        print(f'  segment {i}: position offset {d / rate:+8.3f} output samples, level {math.hypot(a, b):.3f}, SNR {10 * math.log10(yy / max(res, 1e-30)):6.1f} dB')
    def phase(n0, n1):
        ss = cc = sc = ys = yc = 0.0
        for n in range(n0, n1):
            s = math.sin(w * n); c = math.cos(w * n); v = y[n]
            ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c
        det = ss * cc - sc * sc
        return math.atan2((yc * ss - ys * sc) / det, (ys * cc - yc * sc) / det) * PERIOD / (2 * math.pi)
    for i, t in enumerate(anchors):
        n = int(t * SR) // 64 * 64
        before = phase(n - 1100, n - 100); after = phase(n + 100, n + 1100); late = phase(n + int(1.4 * SR), n + int(1.4 * SR) + 1000) if n + int(1.4 * SR) + 1000 < len(y) else float('nan')
        st = lambda a, b: ((b - a + PERIOD / 2) % PERIOD - PERIOD / 2) / rate
        print(f'  anchor {i + 1}: step within 20 ms {st(before, after):+8.3f} samples, after 1.4 s {st(before, late):+8.3f} samples')
    return y
if __name__ == '__main__':
    tag = sys.argv[3]
    render(tag + '_high', 1504.0, 0)
    render(tag + '_wrapped', 1504.0, FIT)
    render(tag + '_seek', 1504.0, FIT, seek=0.02)
    render(tag + '_grid_high', 1504.0, 0, grid=True)
    render(tag + '_grid_wrapped', 1504.0, FIT, grid=True)
