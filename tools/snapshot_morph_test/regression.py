#!/usr/bin/env python3
"""Common Box morph checks. Coverage/remaining engine work: src/docs/snapshotMorphing.md."""
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
CHECKS=(
    ('policies','quick','tools/snapshot_morph_test/run.py',()),
    ('modulation','quick','tools/snapshot_morph_test/modulation_test.py',()),
    ('sound-controls','quick','tools/snapshot_morph_test/sound_controls_test.py',()),
    ('fx-lifecycle','quick','tools/snapshot_morph_test/fx_lifecycle_test.py',()),
    ('additive-count','quick','tools/additivebox/morph_test.py',()),
    ('pulsar-banks','quick','tools/pulsarbox/morph_test.py',()),
    ('steam-feedback','quick','tools/steampipebox/morph_test.py',()),
    ('steam-legacy','quick','tools/steampipebox/v2_test.py',()),
    ('polycomb','quick','tools/polycomb/morph_regression.py',('--group','quick')),
    ('engine-contract','native','tools/snapshot_morph_test/engine_contract_test.py',()),
    ('node-lifecycle','native','tools/snapshot_morph_test/node_lifecycle_test.py',()),
    ('shared-effects','audio','tools/snapshot_morph_test/audio_test.py',()),
    ('bank-presence','audio','tools/snapshot_morph_test/bank_audio_test.py',()),
    ('physical-modes','audio','tools/physicalbox/morph_test.py',()),
    ('additive-partials','audio','tools/additivebox/partial_morph_render_test.py',()),
    ('additive-presence','audio','tools/additivebox/morph_render_test.py',()),
    ('steam-factory','audio','tools/steampipebox/render_test.py',()),
    ('aliasing-factory','audio','tools/aliasingbox/render_test.py',()),
    ('fm-tuning','audio','tools/fmbox/tuning_morph_test.py',()),
    ('fm-factory','audio','tools/fmbox/render_test.py',()),
)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group',choices=('quick','native','audio','all'),default='quick')
    parser.add_argument('--check',choices=[c[0] for c in CHECKS],help='Run one named check')
    parser.add_argument('--list',action='store_true')
    parser.add_argument('--report-dir',type=Path,default=Path('/tmp/box-morph-regression'))
    args=parser.parse_args()
    checks=[c for c in CHECKS if (c[0]==args.check if args.check else args.group=='all' or c[1]==args.group)]
    if args.list:
        for name,group,script,extra in checks:print(group,name,script,*extra)
        return 0
    args.report_dir.mkdir(parents=True,exist_ok=True)
    report={'root':str(ROOT),'checks':[]}
    for name,group,script,extra in checks:
        command=[sys.executable,str(ROOT/script),*extra]
        if name=='polycomb':command+=['--report-dir',str(args.report_dir/name)]
        elif name in ('fm-tuning','physical-modes','shared-effects','additive-partials','bank-presence','steam-factory','aliasing-factory','fm-factory'):
            command+=['--output',str(args.report_dir/name)]
        entry={'name':name,'group':group,'command':command,'log':str(args.report_dir/(name+'.log'))}
        started=time.monotonic()
        print('RUN',name,flush=True)
        with Path(entry['log']).open('w') as log:
            process=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                entry['returncode']=process.wait(timeout=1200)
                entry['passed']=entry['returncode']==0
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL)
                process.wait()
                entry['passed']=False;entry['error']='timeout'
        entry['seconds']=round(time.monotonic()-started,2)
        report['checks'].append(entry)
        (args.report_dir/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS' if entry['passed'] else 'FAIL',name,'('+entry['log']+')',flush=True)
    return int(any(not c['passed'] for c in report['checks']))


if __name__=='__main__':raise SystemExit(main())
