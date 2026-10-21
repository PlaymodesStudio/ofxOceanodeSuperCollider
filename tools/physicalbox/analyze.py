#!/usr/bin/env python3
"""Measures the WAVs written by render_cases.scd.

For each case: peak, RMS over time, the strongest periodicity (f0) of the
sustained or ringing part, spectral centroid, and how long it stays audible.
"""
import pathlib, struct, sys
import numpy as np


def read_wav(path):
    b = path.read_bytes()
    pos = 12
    while pos < len(b) - 8:
        tag, size = struct.unpack_from('<4sI', b, pos)
        pos += 8
        if tag == b'fmt ':
            fmt, channels, rate = struct.unpack_from('<HHI', b, pos)
        if tag == b'data':
            x = np.frombuffer(b[pos:pos + size], dtype=np.float32 if fmt == 3 else np.int16)
            return x.reshape(-1, channels).astype(np.float64), rate
        pos += size + (size % 2)
    raise ValueError(path)


def f0(x, rate, lo=30, hi=4000):
    x = x - x.mean()
    if np.sqrt((x * x).mean()) < 1e-5:
        return 0.0, 0.0
    n = len(x)
    spec = np.fft.rfft(x * np.hanning(n), 2 * n)
    ac = np.fft.irfft(np.abs(spec) ** 2)[:n]
    ac /= ac[0]
    a, b = int(rate / hi), int(rate / lo)
    seg = ac[a:b]
    i = int(np.argmax(seg)) + a
    if 1 <= i < n - 1:
        y0, y1, y2 = ac[i - 1], ac[i], ac[i + 1]
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) != 0 else 0
        d = max(-1, min(1, d))
    else:
        d = 0
    return rate / (i + d), ac[i]


def lowest_partial(x, rate):
    """Lowest spectral peak within 20 dB of the strongest one."""
    n = 1 << 16
    s = np.abs(np.fft.rfft(x[:n] * np.hanning(min(len(x), n)), n))
    fr = np.fft.rfftfreq(n, 1 / rate)
    s[fr < 25] = 0
    top = s.max()
    if top <= 0:
        return 0.0
    pk = np.nonzero((s[1:-1] > s[:-2]) & (s[1:-1] >= s[2:]) & (s[1:-1] > top * 0.1))[0] + 1
    if not len(pk):
        return 0.0
    i = pk[0]
    y0, y1, y2 = np.log(s[i - 1] + 1e-12), np.log(s[i] + 1e-12), np.log(s[i + 1] + 1e-12)
    d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) != 0 else 0
    return float(fr[i] + d * (fr[1] - fr[0]))


def centroid(x, rate):
    s = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    fr = np.fft.rfftfreq(len(x), 1 / rate)
    return float((s * fr).sum() / max(s.sum(), 1e-12))


def main():
    folder = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '/tmp/physicalbox-render')
    cases = [l.split(' ') for l in (folder / 'cases.txt').read_text().splitlines() if l]
    print(f"{'case':26} {'peak':>6} {'rms.2-1':>8} {'rmsEnd':>8} {'f0':>8} {'clar':>5} {'cent':>6} {'audible':>7} {'L/R':>5} {'low':>7}")
    for name, dur, *_ in cases:
        x, rate = read_wav(folder / f'{name}.wav')
        bad = not np.isfinite(x).all()
        m = x.mean(axis=1)
        peak = np.abs(x).max()
        seg = m[int(0.4 * rate):int(1.4 * rate)]
        rms = np.sqrt((seg ** 2).mean())
        end = m[-int(0.3 * rate):]
        rmse = np.sqrt((end ** 2).mean())
        fr, cl = f0(seg[:int(0.25 * rate)], rate)
        env = np.sqrt(np.convolve(m ** 2, np.ones(480) / 480, 'same'))
        aud = np.nonzero(env > peak * 0.001)[0]
        audible = (aud[-1] / rate) if len(aud) else 0
        lr = np.corrcoef(x[:, 0], x[:, 1])[0, 1] if x[:, 0].std() > 0 and x[:, 1].std() > 0 else 1
        print(f"{name:26} {peak:6.3f} {rms:8.4f} {rmse:8.4f} {fr:8.1f} {cl:5.2f} {centroid(seg, rate):6.0f} {audible:7.2f} {lr:5.2f} {lowest_partial(seg, rate):7.1f}{'  NaN!' if bad else ''}")


if __name__ == '__main__':
    main()
