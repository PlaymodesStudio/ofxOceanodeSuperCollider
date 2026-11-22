#!/usr/bin/env python3
"""AnimaBox test helpers: compile SynthDef sources with the bundled sclang, NRT render with scsynth.

  compile_exact(work, source, n)       a ~synthCreator source (e.g. Effect/Time/Reverb/metaVerb.scd) for n channels
  compile_script(work, source, cut)    a Defaults script, run whole (cut: text replacements, e.g. shorter loops)
  render(score_bytes, wav, channels)   scsynth -N, returns (samples interleaved, channels, sr)
"""
import pathlib, struct, subprocess, shutil

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCLANG = ROOT / 'sclang/osx/sclang'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
PLUGINS = '/Applications/SuperCollider.app/Contents/Resources/plugins:' + str(ROOT / 'ugens/OceanodeAnalog')
SR = 48000
LAST = ''


def _run(work, body, timeout=1800):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'run.scd').write_text(body + '\n"COMPILED_OK".postln;\n0.exit;\n')
    (work / 'config.yaml').write_text(
        'includePaths:\n'
        f'    - {ROOT}/sclang/SCClassLibrary\n'
        f'    - {ROOT}/synthdefs/Oceanode\n'
        f'    - {ROOT}/ugens/OceanodeAnalog\n'
        'excludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    home = work / 'home'; home.mkdir(exist_ok=True)
    r = subprocess.run([str(SCLANG), '-l', str(work / 'config.yaml'), str(work / 'run.scd')], capture_output=True, text=True,
                       timeout=timeout, stdin=subprocess.DEVNULL, env={'HOME': str(home), 'PATH': '/usr/bin:/bin'})
    errors = [l for l in r.stdout.splitlines() if l.startswith('ERROR') and 'Class extension for nonexistent' not in l]
    if 'COMPILED_OK' not in r.stdout or errors:
        raise RuntimeError(r.stdout[-4000:])
    return r.stdout


def compile_exact(work, source, n):
    out = work / 'exact'
    _run(work, f'"{ROOT}/synthdefs/Oceanode/mainDefinitions.scd".load;\nd = "{out}";\nFile.mkdir(d);\n'
               f'~useExactSynthCreator.({n});\n"{ROOT / "synthdefs" / source}".load;')
    return out


def compile_script(work, source, cut=()):
    text = (ROOT / 'synthdefs' / source).read_text()
    for a, b in cut:
        assert a in text, a
        text = text.replace(a, b)
    copy = work / pathlib.Path(source).name
    work.mkdir(parents=True, exist_ok=True)
    copy.write_text(text)
    _run(work, f'"{copy}".load;')
    return work / 'CompiledSynthdefs'


def message(address, *args):
    def pad(b): return b + b'\0' * (4 - len(b) % 4)
    tags, data = ',', b''
    for a in args:
        if isinstance(a, bool): a = float(a)
        if isinstance(a, int): tags += 'i'; data += struct.pack('>i', a)
        elif isinstance(a, float): tags += 'f'; data += struct.pack('>f', a)
        elif isinstance(a, bytes): tags += 'b'; data += struct.pack('>i', len(a)) + pad(a)
        else: tags += 's'; data += pad(str(a).encode())
    return pad(address.encode()) + pad(tags.encode()) + data


def bundle(t, messages):
    body = b'#bundle\0' + struct.pack('>Q', int(t * 2 ** 32))
    for m in messages: body += struct.pack('>i', len(m)) + m
    return struct.pack('>i', len(body)) + body


def render(score, wav, channels, inputs=None):
    wav = pathlib.Path(wav)
    osc = wav.with_suffix('.osc'); osc.write_bytes(score)
    cmd = [SCSYNTH, '-N', str(osc), str(inputs) if inputs else '_', str(wav), str(SR), 'WAV', 'float',
           '-o', str(channels), '-m', '2097152', '-U', PLUGINS]   # OceanAudio's real-time memory
    if inputs: cmd[-4:-4] = []
    r = subprocess.run(cmd, capture_output=True, text=True)
    global LAST; LAST = r.stdout + r.stderr
    if not wav.exists(): raise RuntimeError(r.stdout[-3000:] + r.stderr[-2000:])
    return read_wav(wav)


def read_wav(path):
    b = pathlib.Path(path).read_bytes()
    ch = struct.unpack_from('<H', b, 22)[0]; sr = struct.unpack_from('<I', b, 24)[0]
    i = 12
    while i < len(b):
        cid, size = b[i:i + 4], struct.unpack_from('<I', b, i + 4)[0]
        if cid == b'data':
            n = size // 4
            return list(struct.unpack_from('<%df' % n, b, i + 8)), ch, sr
        i += 8 + size + (size & 1)
    raise RuntimeError('no data chunk')
