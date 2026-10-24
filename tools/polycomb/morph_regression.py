#!/usr/bin/env python3
"""Run the consolidated PolyComb morph regression suite without an audio device."""
from dataclasses import dataclass
from pathlib import Path
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]

@dataclass(frozen=True)
class Check:
    name: str
    group: str
    script: str
    args: tuple=()
    numpy: bool=False

CHECKS=(
    Check('pitch-map-reference','quick','tools/polycomb/pitchmap_test.py'),
    Check('pitch-map-geometry','quick','tools/polycomb/morph_index_test.py'),
    Check('pitch-map-morph','quick','tools/polycomb/pitch_map_morph_test.py'),
    Check('voice-bank-lifecycle','quick','tools/polycomb/voice_morph_test.py'),
    Check('modulation-adoption','quick','tools/polycomb/voice_morph_modulation_test.py'),
    Check('gate-seeds','quick','tools/polycomb/gate_seed_test.py'),
    Check('resonator-follow','quick','tools/polycomb/resonator_follow_test.py'),
    Check('note-traffic','quick','tools/polycomb/note_traffic_test.py'),
    # The separate historical binary-hash audit is not a source regression.
    Check('legacy-state','quick','tools/polycomb/moving_saws_test.py',('--skip-dsp-hashes',)),
    Check('snapshot-policies','quick','tools/snapshot_morph_test/run.py'),
    Check('compile-synthdefs','audio','tools/polycomb/compile_morph_synthdefs.py'),
    Check('voice-bank-audio','audio','tools/polycomb/voice_morph_render_test.py'),
    Check('pitch-map-audio','audio','tools/polycomb/pitch_map_morph_render_test.py'),
    Check('pitch-smoothing','audio','tools/polycomb/pitch_smoothing_render_test.py',numpy=True),
    Check('resonator-vibrato','audio','tools/polycomb/resonator_vibrato_render_test.py',numpy=True),
    Check('voice-rebuild','audio','tools/polycomb/voice_rebuild_test.py'),
    Check('macromixer-integration','native','tools/polycomb/pitch_map_morph_integration_test.py'),
)

def prerequisites(check):
    missing=[]
    if not shutil.which('clang++'): missing.append('clang++')
    if not (ROOT.parents[1]/'libs/json/include/nlohmann/json.hpp').is_file():
        missing.append('openFrameworks JSON headers (libs/json/include)')
    if check.numpy and importlib.util.find_spec('numpy') is None: missing.append('numpy in this Python interpreter')
    if check.group=='audio':
        for path in (ROOT/'sclang/osx/sclang',Path('/Applications/SuperCollider.app/Contents/Resources/scsynth')):
            if not path.is_file() or not os.access(path,os.X_OK): missing.append(str(path))
    if check.group=='native':
        for path in (ROOT.parent/'ofxOceanode/tests/macro_mixer_polycomb_regression.py',
                     ROOT.parents[1]/'libs/openFrameworksCompiled/lib/macos/openFrameworks.a'):
            if not path.is_file(): missing.append(str(path))
        for addon in ('ofxSuperCollider','ofxOsc','ofxImGui'):
            if not (ROOT.parent/addon).is_dir(): missing.append('neighboring '+addon+' addon')
    return missing

def run(check,directory):
    command=[sys.executable,str(ROOT/check.script),*check.args]
    result={'name':check.name,'group':check.group,'command':command,'log':str(directory/(check.name+'.log'))}
    missing=prerequisites(check)
    if missing:
        result.update(status='blocked',missing=missing,seconds=0)
        Path(result['log']).write_text('Missing prerequisites:\n'+'\n'.join(missing)+'\n')
        return result
    start=time.monotonic()
    with open(result['log'],'w') as log:
        # A timeout must also stop nested compilers/renderers, not leave them
        # writing caches after the suite moves on to another check.
        process=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            code=process.wait(timeout=900 if check.group=='native' else 180)
            result.update(status='passed' if code==0 else 'failed',exit_code=code)
        except (subprocess.TimeoutExpired,KeyboardInterrupt) as error:
            os.killpg(process.pid,signal.SIGTERM)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL);process.wait()
            if isinstance(error,KeyboardInterrupt): raise
            result.update(status='failed',error='timeout',exit_code=process.returncode)
    result['seconds']=round(time.monotonic()-start,3)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group',choices=('quick','audio','native','all'),default='quick')
    parser.add_argument('--list',action='store_true',help='List selected checks without running them')
    parser.add_argument('--report-dir',type=Path,help='Keep per-check logs and report.json in this directory')
    args=parser.parse_args()
    checks=[c for c in CHECKS if args.group=='all' or c.group==args.group]
    if args.list:
        for c in checks: print(f'{c.group:6} {c.name:24} {c.script} {" ".join(c.args)}')
        return 0
    directory=args.report_dir.resolve() if args.report_dir else Path(tempfile.mkdtemp(prefix='polycomb-morph-regression-'))
    directory.mkdir(parents=True,exist_ok=True)
    fixture=ROOT/'tools/polycomb/fixtures/morph_reference.json'
    report={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'group':args.group,'python':sys.version,'root':str(ROOT),
            'reference_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),'checks':[]}
    print(f'Report: {directory}/report.json',flush=True)
    for check in checks:
        print(f'RUN   {check.name}',flush=True)
        result=run(check,directory);report['checks'].append(result)
        (directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f'{result["status"].upper():7} {check.name} ({result["seconds"]:.1f}s)',flush=True)
        if result['status']!='passed': print('        '+result['log'],flush=True)
    passed=sum(r['status']=='passed' for r in report['checks'])
    print(f'{passed}/{len(checks)} passed. Logs: {directory}',flush=True)
    return 0 if passed==len(checks) else 1

if __name__=='__main__':sys.exit(main())
