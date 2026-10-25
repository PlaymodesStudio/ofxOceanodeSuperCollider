#!/usr/bin/env python3
"""Check Box translation units against the neighboring native OF/addon headers.

Optional positional names select stems, e.g. scAnalogBox scAdditiveBox headers.
"""
from pathlib import Path
import argparse,concurrent.futures,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
of=root.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('names',nargs='*')
args=parser.parse_args()
include=[root/'src']
for base in [of/'libs/openFrameworks',*[of/'addons'/name for name in ('ofxOceanode','ofxImGui','ofxSuperCollider','ofxOsc','ofxOceanodeTimeline')]]:
    include+=sorted({p.parent for p in base.rglob('*.h') if '_to_delete' not in p.parts and 'tests' not in p.parts and 'example' not in str(p)})
include += [p for p in (of/'libs').glob('*/include') if p.is_dir()]
include += [of/'libs/freetype/include/freetype',of/'libs/cairo/include/cairo']
flags=['clang++','-std=c++20','-fsyntax-only','-w','-DOF_NO_FMOD','-DGL_SILENCE_DEPRECATION',*['-I'+str(p) for p in dict.fromkeys(include)]]
with tempfile.TemporaryDirectory(prefix='box-native-syntax-') as folder:
    header=Path(folder)/'headers.cpp'
    header.write_text('\n'.join('#include "'+str(p)+'"' for p in (root/'src/nodes/boxSynths').glob('*/*.h') if p.stem.startswith('sc')))
    paths=[*(root/'src/nodes/boxSynths').rglob('*.cpp'),header]
    if args.names:paths=[p for p in paths if p.stem in args.names]
    assert paths,'No matching translation units'
    def check(path):
        result=subprocess.run(flags+[str(path)],capture_output=True,text=True)
        print(path.name, 'PASS' if result.returncode==0 else 'FAIL\n'+result.stderr,flush=True)
        return result.returncode
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results=list(pool.map(check,paths))
    raise SystemExit(any(results))
