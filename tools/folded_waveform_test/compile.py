#!/usr/bin/env python3
"""Compile via the real startup-sync runner and standalone load; verify harvest paths."""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'synthdefs/Defaults/Analysis/foldedWaveform.scd'
NAME = 'oceanodeFoldedWaveform.scsyndef'
with tempfile.TemporaryDirectory(prefix='folded-waveform-compile-') as folder:
    work = pathlib.Path(folder)
    source = work / 'Defaults/Analysis/foldedWaveform.scd'
    source.parent.mkdir(parents=True)
    shutil.copyfile(SOURCE, source)
    config = work / 'sclang.yaml'
    config.write_text('includePaths:\n    - "' + str(ROOT / 'sclang/SCClassLibrary') + '"\nexcludeDefaultPaths: true\n')
    command = [str(ROOT / 'sclang/osx/sclang'), '-l', str(config)]
    output = source.parent / 'CompiledSynthdefs/foldedWaveform' / NAME
    # Reproduce SyncRun::runSclang: argv begins with mode and unused outDir.
    result = subprocess.run(command + [str(ROOT / 'synthdefs/Oceanode/syncCompile.scd'),
                                      'script', str(work / 'unused'), str(source)],
                            cwd=work, capture_output=True, text=True, timeout=60)
    log = result.stdout + result.stderr
    assert result.returncode == 0 and 'OCEANODE_SYNC_OK ' + str(source) in log, log
    assert output.exists(), 'Startup sync found no SynthDef under CompiledSynthdefs\n' + log
    assert output.read_bytes()[:4] == b'SCgf'
    bundled = ROOT / 'data/Supercollider/Synthdefs/Defaults/Analysis/foldedWaveform' / NAME
    assert output.read_bytes() == bundled.read_bytes(), 'Bundled SynthDef differs from the synced build'
    assert not (work / 'script').exists(), 'Mode argument was mistaken for a directory'
    # A direct source load must create both missing directory levels itself.
    shutil.rmtree(source.parent / 'CompiledSynthdefs')
    wrapper = work / 'standalone.scd'
    wrapper.write_text('"' + str(source) + '".load;\n0.exit;\n')
    result = subprocess.run(command + [str(wrapper)], cwd=work,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and output.exists(), result.stdout + result.stderr
print('PASS: real startup-sync compilation, correct harvest location, bundled binary match and standalone compilation.')
