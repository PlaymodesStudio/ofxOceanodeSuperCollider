#!/usr/bin/env python3
"""Install a prepared SpaceSystem-Geiger build into OceanAudio, backing up overwritten files.

    python3 install.py <build directory> <OceanAudio bin/data>
"""
import datetime
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
work, data = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:3])
backup = data / 'Supercollider/_backups' / ('spacesystem-geiger-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
sources = {}
for p in (ROOT / 'synthdefs').rglob('*.scd'):
    if 'Compatibility' in p.parts or 'WIP' in p.parts: continue
    match = re.search(r'~synthCreator.value\("([^"]+)"', p.read_text(errors='replace'))
    if match: sources[match[1]] = p

def copy(src, dst):
    if dst.exists() and src.read_bytes() == dst.read_bytes(): return
    if dst.exists():
        saved = backup / dst.relative_to(data)
        saved.parent.mkdir(parents=True, exist_ok=True)
        if not saved.exists(): shutil.copy2(dst, saved)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)

# Metadata files make the newly introduced nodes appear in the node browser.
for base in (work / 'metadata', work / 'defs'):
    for p in base.rglob('*'):
        if not p.is_file() or p.suffix not in ('.scsyndef', '.txarcmeta'): continue
        name = p.parent.name
        source = sources[name]
        category = source.relative_to(ROOT / 'synthdefs').parent
        copy(p, data / 'Supercollider/Synthdefs' / category / name / p.name)
        copy(source, data / 'Supercollider/SynthdefSources' / source.relative_to(ROOT / 'synthdefs'))
# Replace complete owned folders: otherwise removed nodes survive a rebuild.
folders = [pathlib.Path('Presets/GENERATIVE_MUSIC/196--SPACESYSTEM_GEIGER')]
folders += [pathlib.Path('Macros/AUDIO/REAKTOR') / name for name in
            ('GEIGER_CLICK', 'GEIGER_NOISE', 'GEIGER_SAW', 'NI_RESONATOR_QUAD')]
for relative in folders:
    src, dst = work / 'data' / relative, data / relative
    if dst.exists():
        saved = backup / relative
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(dst, saved)
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
for p in (ROOT / 'data/Supercollider/Synthdefs/Mixing/AudioMerge').iterdir():
 copy(p, data / 'Supercollider/Synthdefs/Mixing/AudioMerge' / p.name)
copy(ROOT / 'synthdefs/Mixing/audioMerge.scd', data / 'Supercollider/SynthdefSources/Mixing/audioMerge.scd')
for filename in ('OceanodeAnalog.sc', 'OceanodeAnalog.scx'):
    copy(ROOT / 'ugens/OceanodeAnalog' / filename, data / 'Supercollider/Extensions/OceanodeAnalog' / filename)
print('Installed:', data / 'Presets/GENERATIVE_MUSIC/196--SPACESYSTEM_GEIGER')
print('Backup:', backup)
