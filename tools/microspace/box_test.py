#!/usr/bin/env python3
"""Compile production Box adapters and render mono/stereo/odd/16-channel cases."""
import argparse
import pathlib
import subprocess
import sys
import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import model_test as ms

ROOT = ms.ROOT


def compile_defs(work):
    conf = work / 'sclang_conf.yaml'
    paths = [ROOT / p for p in ('sclang/SCClassLibrary', 'sclang/Extensions',
                               'synthdefs/Oceanode')]
    conf.write_text('includePaths:\n' + ''.join(f'    - "{p}"\n' for p in paths)
                    + 'excludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    # Copies redirect output to this test's directory; the production bodies
    # and all of their reverb channel counts are compiled unchanged.
    for name, rel in [('mixer', 'MixerPro/mixerpro.scd'), ('sample', 'SampleBox/samplebox.scd'),
                      ('physical', 'PhysicalBox/physicalBox.scd')]:
        source = work / f'{name}.scd'
        text = (ROOT / 'synthdefs/Defaults' / rel).read_text()
        markers = {'mixer': '    SynthDef(("boxMicroSpace"',
                   'sample': '    SynthDef(("SampleBoxReverb_"',
                   'physical': '    SynthDef(("PhysicalBoxSpace"'}
        start = text.index(markers[name])
        end = text.index('}).writeDefFile(dir);', start) + len('}).writeDefFile(dir);')
        body = text[start:end]
        prefix = 'var dir = "' + str(work / 'CompiledSynthdefs') + '"; var modOf;\n'
        prefix += 'File.mkdir(dir); dir = dir +/+ "' + {'mixer': 'mixerpro', 'sample': 'SampleBox', 'physical': 'PhysicalBox'}[name] + '"; File.mkdir(dir);\n'
        if name == 'sample':
            a = text.index('modOf = {')
            b = text.index('\n};', a) + len('\n};')
            prefix += text[a:b] + '\n'
        variable = {'mixer': 'numChannels', 'sample': 'n', 'physical': 'c'}[name]
        text = '(\n' + prefix + '(1..16).do { |' + variable + '|\n' + body + '\n};\n)\n'
        source.write_text(text)
        runner = work / 'compile.scd'
        runner.write_text(f'try {{ "{source}".load; 0.exit }} {{ |e| e.reportError; 1.exit }};\n')
        result = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf), str(runner)],
                                capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stdout + result.stderr
    for n in range(1, 17):
        for folder, name in [('mixerpro', f'boxMicroSpace{n}'),
                             ('SampleBox', f'SampleBoxReverb_{n}'),
                             ('PhysicalBox', f'PhysicalBoxSpace{n}')]:
            assert (work / 'CompiledSynthdefs' / folder / f'{name}.scsyndef').exists(), name


def render(work, server, name, n, bypass=False):
    frames = ms.SR * 2
    signal = np.zeros((frames, n))
    t = np.arange(ms.SR // 10) / ms.SR
    signal[:len(t)] = (0.1 * np.sin(2 * np.pi * 440 * t))[:, None]
    raw = work / 'input.wav'
    ms.write_wav(raw, signal)
    folder = 'SampleBox' if name.startswith('SampleBox') else ('PhysicalBox' if name.startswith('Physical') else 'mixerpro')
    definition = work / 'CompiledSynthdefs' / folder / f'{name}.scsyndef'
    m, b = ms.message, ms.bundle
    params = ['bus', 16] if name.startswith('SampleBox') else ['in', 16, 'out', 0]
    score = b(0, [m('/d_recv', definition.read_bytes()),
                  m('/d_recv', ms.player_def(work, n).read_bytes()),
                  m('/b_allocRead', 0, str(raw)), m('/g_new', 1, 0, 0)])
    # SampleBox replaces its input bus; a final reader routes it to output.
    if name.startswith('SampleBox'):
        src = work / 'reader.scd'
        src.write_text(f'SynthDef(\\Reader, {{ Out.ar(0, In.ar(16, {n})) }}).writeDefFile("{work}"); 0.exit;')
        result = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(work / 'sclang_conf.yaml'), str(src)], capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        score += b(0.005, [m('/d_recv', (work / 'Reader.scsyndef').read_bytes())])
    score += b(0.01, [m('/s_new', f'MSPlayer{n}', 1000, 0, 1),
                      m('/s_new', name, 1001, 1, 1, *params)])
    if name.startswith('SampleBox'):
        score += b(0.01, [m('/s_new', 'Reader', 1002, 1, 1)])
    if bypass:
        score += b(0.2, [m('/n_set', 1001, 'on', 0.)])
    score += b(2.01, [m('/g_freeAll', 1)])
    score += b(2.02, [m('/c_set', 0, 0.)])
    osc, wav = work / 'score.osc', work / 'output.wav'
    osc.write_bytes(score)
    result = subprocess.run([server, '-N', str(osc), '_', str(wav), str(ms.SR), 'WAV', 'float',
                             '-o', str(n), '-i', '0', '-z', '64', '-m', '65536', '-w', '8192', '-D', '0'],
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0 and 'FAILURE' not in result.stdout + result.stderr, result.stdout + result.stderr
    samples, channels, _ = ms.read_wav(wav)
    audio = np.asarray(samples).reshape(-1, channels)
    assert channels == n and np.isfinite(audio).all()
    assert 0.001 < np.max(np.abs(audio)) < 2, (name, np.max(np.abs(audio)))
    assert np.max(np.abs(audio[ms.SR // 2:])) > 1e-7, f'{name}: missing tail'
    print(f'{name}, bypass={bypass}: finite output, audible tail, peak={np.max(np.abs(audio)):.4f}')
    return audio


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=pathlib.Path, default=pathlib.Path('/tmp/box-microspace-test'))
    ap.add_argument('--server', default='/Applications/SuperCollider.app/Contents/Resources/scsynth')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    compile_defs(args.output)
    for n in (1, 2, 3, 16):
        strip = render(args.output, args.server, f'boxMicroSpace{n}', n)
        sample = render(args.output, args.server, f'SampleBoxReverb_{n}', n)
        assert np.max(np.abs(strip - sample)) < 1e-6, f'{n}: sampler/strip reverb mismatch'
        render(args.output, args.server, f'PhysicalBoxSpace{n}', n)
    render(args.output, args.server, 'boxMicroSpace2', 2, bypass=True)
    print('PASS: all 48 production Box reverb variants compile; adapter renders agree; bypass retains tails.')


if __name__ == '__main__':
    main()
