"""YIN pitch + level of each note in a voice_render_test WAV (needs numpy)."""
import sys, struct, numpy as np
def read_wav(path):
    b = open(path, 'rb').read(); p = 12
    while p < len(b) - 8:
        tag, size = struct.unpack_from('<4sI', b, p); p += 8
        if tag == b'fmt ': fmt, ch, rate = struct.unpack_from('<HHI', b, p)
        if tag == b'data': return np.frombuffer(b[p:p+size], dtype='<f4').reshape(-1, ch)[:, 0], rate
        p += size + (size % 2)
def yin(x, sr, fmin=25, fmax=2000, th=0.15):
    n = len(x); W = n // 2
    d = np.array([np.sum((x[:W] - x[t:t + W]) ** 2) for t in range(int(sr / fmin))])
    cmnd = d.copy(); cmnd[0] = 1
    cs = np.cumsum(d[1:]); cmnd[1:] = d[1:] * np.arange(1, len(d)) / np.maximum(cs, 1e-20)
    lo = int(sr / fmax)
    for t in range(lo, len(cmnd) - 1):
        if cmnd[t] < th and cmnd[t] < cmnd[t + 1]:
            a, b_, c = cmnd[t - 1], cmnd[t], cmnd[t + 1]
            off = 0.5 * (a - c) / (a - 2 * b_ + c) if (a - 2 * b_ + c) != 0 else 0
            return 69 + 12 * np.log2(sr / (t + off) / 440), b_
    t = lo + int(np.argmin(cmnd[lo:])); return 69 + 12 * np.log2(sr / t / 440), cmnd[t]
x, sr = read_wav(sys.argv[1]); notes = [int(a) for a in sys.argv[2:]] or [36, 48, 60]
for i, n in enumerate(notes):
    t0 = 0.1 + 2 * i
    seg = x[int((t0 + 0.25) * sr):int((t0 + 0.25) * sr) + 8192]
    tail = x[int((t0 + 1.2) * sr):int((t0 + 1.9) * sr)]
    p, q = yin(seg.astype(np.float64), sr)
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))); f = np.fft.rfftfreq(len(seg), 1 / sr)
    pk = f[np.argmax(spec[5:]) + 5]
    print('  note %d: rms %.4f  yin %.2f (q %.2f)  fft-peak %.1f Hz (midi %.1f)  tail rms %.5f' % (
        n, np.sqrt(np.mean(seg ** 2)), p, q, pk, 69 + 12 * np.log2(pk / 440), np.sqrt(np.mean(tail ** 2))))
