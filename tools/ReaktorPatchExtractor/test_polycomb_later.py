#!/usr/bin/env python3
"""Regression for named Unicode containers around older inner state names."""
import collections,json,pathlib,subprocess,sys
HERE=pathlib.Path(__file__).resolve().parent
exe=HERE/'dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'
assert len(sys.argv)==3,'usage: test_polycomb_later.py /path/to/2018.ens /path/to/2025.ens'
for version,path in zip(('2018','2025'),sys.argv[1:]):
    j=json.loads(subprocess.check_output([str(exe),path],text=True))
    states=j['snapshots'];n=472 if version=='2018' else 373
    assert len(states)==(358 if version=='2018' else 403)
    assert all(s['parameter_count']==n and s['object_id_mapping_method']=='stable_panel_reference'for s in states)
    bank=[s for s in states if s.get('bank_name')==('2018' if version=='2018' else 'OCTOBER 2020')]
    assert len(bank)==(86 if version=='2018' else 35)
    names=[s.get('container_name',s['name'])for s in bank]
    assert names[0]==('Science Fiction' if version=='2018' else 'DESFASES-NOTCH')
    assert names[-1]==('xespills' if version=='2018' else 'SAPIENS_MELODRAMA')
    if version=='2025':
        assert len(set(names))==35 and all(s['name']=='TEST-CHANS'for s in bank)
        assert collections.Counter(s['bank_name']for s in states)=={'OCTOBER 2020':35,'SELECTED':128,'Bank 2':128,'Bank 3':6,'Bank 4':8,'2018':96,'BEAMS':2}
    print(f'PASS: {version}: complete {n}-record states; bank names and outer preset names retained separately from inner state names.')
