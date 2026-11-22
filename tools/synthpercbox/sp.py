#!/usr/bin/env python3
"""SynthPercBox helpers: compile synthPercBox.scd with the bundled sclang, NRT render with scsynth."""
import pathlib, subprocess, struct, sys
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'animabox'))
import sc  # message, bundle
SCLANG = ROOT / 'sclang/osx/sclang'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
PLUGINS = ':'.join(['/Applications/SuperCollider.app/Contents/Resources/plugins',
                    str(ROOT / 'ugens/OceanodeAnalog'), str(ROOT / 'ugens/OceanodePrimitives')])
SOURCE = ROOT / 'synthdefs/Defaults/SynthPercBox/synthPercBox.scd'
COMPILED = SOURCE.parent / 'CompiledSynthdefs' / 'SynthPercBox'
SR = 48000

def compile_defs(work):
    work = pathlib.Path(work); work.mkdir(parents=True, exist_ok=True)
    (work / 'run.scd').write_text(f'"{SOURCE}".load;\n"COMPILED_OK".postln;\n0.exit;\n')
    (work / 'config.yaml').write_text(
        'includePaths:\n'
        f'    - {ROOT}/sclang/SCClassLibrary\n'
        f'    - {ROOT}/synthdefs/Oceanode\n'
        f'    - {ROOT}/ugens/OceanodeAnalog\n'
        f'    - {ROOT}/ugens/OceanodePrimitives\n'
        'excludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    home = work / 'home'; home.mkdir(exist_ok=True)
    r = subprocess.run([str(SCLANG), '-l', str(work / 'config.yaml'), str(work / 'run.scd')], capture_output=True,
                       text=True, timeout=1800, stdin=subprocess.DEVNULL, env={'HOME': str(home), 'PATH': '/usr/bin:/bin'})
    errors = [l for l in r.stdout.splitlines() if l.startswith('ERROR') and 'Class extension for nonexistent' not in l]
    if 'COMPILED_OK' not in r.stdout or errors:
        raise RuntimeError(r.stdout[-5000:])
    return COMPILED

def render(score, wav, channels=2):
    wav = pathlib.Path(wav); osc = wav.with_suffix('.osc'); osc.write_bytes(score)
    r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', str(channels),
                        '-m', '2097152', '-U', PLUGINS], capture_output=True, text=True)
    import wave as _w
    data = wav.read_bytes()
    # float WAV: find 'data' chunk
    i = data.find(b'data'); n = struct.unpack('<I', data[i+4:i+8])[0]
    samples = struct.unpack('<%df' % (n // 4), data[i+8:i+8+n])
    return samples, r.stdout + r.stderr

if __name__ == '__main__':
    import time; t = time.time()
    print(compile_defs(sys.argv[1] if len(sys.argv) > 1 else '/tmp/sp_compile'), round(time.time() - t, 1), 's')
