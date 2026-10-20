#!/usr/bin/env python3
"""Install translated PolyComb banks and optional DSP into an OceanAudio data folder.
Leaves current snapshot slots/library untouched. Saves a restore bank first and
backs up replaced assets. Use --dry-run to list the files without writing.
"""
import argparse, datetime, hashlib, json, pathlib, shutil
ROOT=pathlib.Path(__file__).resolve().parents[2]
SOURCE=ROOT/'synthdefs/Defaults/PolyComb/ReaktorSnapshots'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('data',type=pathlib.Path);ap.add_argument('--dry-run',action='store_true');a=ap.parse_args()
    data=a.data.resolve();assert (data/'Supercollider').is_dir(),data
    library=data/'nodeSnapshots/PolyComb';banks=library/'banks'
    synths=data/'Supercollider/Synthdefs/Defaults/polyComb'
    planned=[];count=0
    # PolyComb 1.7 (24 voices) and the 2018 PolyComb_9_32OSC (32 voices); the 2025 collection is retired
    for folder in (SOURCE,SOURCE/'2018'):
        manifest=json.loads((folder/'bank-manifest.json').read_text())
        for name in manifest['files']:
            src=folder/name;j=json.loads(src.read_text());assert 1<=len(j['slots'])<=128
            count+=len(j['slots'])
            planned.extend(((src,banks/name),(src,synths/'ReaktorSnapshots'/src.relative_to(SOURCE))))
    assert count==296
    # the retired 2025 banks, if an earlier installation left them: moved into the backup, not deleted
    retired=sorted(banks.glob('Reaktor PolyComb 2025 - *.json'))+sorted((synths/'ReaktorSnapshots'/'2025').glob('*.json'))
    # every voice count up to 32 (identical files are skipped), Delta and the
    # 32-lane spreader, and the shared modulators up to 32 voices
    defs=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
    for src in sorted(defs.glob('*.scsyndef')):
        if src.stem.startswith('PolyCombSpread'):continue      # the original spreaders: left as installed
        planned.append((src,synths/src.name))
    for family in ('PCSub','PCFilterSlopes','PCDelta','PCPitch','PCAmp'):
        for n in range(1,33):assert (defs/f'{family}{n}.scsyndef').is_file(),family+str(n)
    for n in range(1,17):assert (defs/f'PCSpreadWide{n}.scsyndef').is_file()
    mods=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
    for src in sorted(mods.glob('*.scsyndef')):planned.append((src,data/'Supercollider/Synthdefs/Defaults/Modulation'/src.name))
    for n in range(1,17):
        src=ROOT/f'synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro/mixerProComp{n}.scsyndef'
        assert src.is_file(),src
        planned.append((src,data/'Supercollider/Synthdefs/Defaults/mixerpro'/src.name))
    changed=[(s,d)for s,d in planned if not d.exists()or digest(s)!=digest(d)]
    protected={p:digest(p)for p in library.glob('snapshot_*.json')}
    if (library/'library.json').exists():protected[library/'library.json']=digest(library/'library.json')
    if a.dry_run:
        print(f'296 snapshots / 3 banks; {len(changed)} assets to install/update; {len(retired)} retired 2025 files to move into the backup. Current library: {len(protected)} protected files.')
        return
    stamp=datetime.datetime.now().astimezone().strftime('%Y-%m-%d %H-%M-%S-%f')
    banks.mkdir(parents=True,exist_ok=True)
    if changed:
        slots=[None]*128
        for p in protected:
            if p.name=='library.json':continue
            i=int(p.stem.split('_')[-1]);assert 0<=i<128
            slots[i]=json.loads(p.read_text())
        while slots and slots[-1]is None:slots.pop()
        restore=banks/f'Before Reaktor installation - {stamp}.json'
        with restore.open('x')as f:json.dump(dict(snapshotBank=1,active=-1,slots=slots),f,indent=2,ensure_ascii=False)
        for src,dst in changed:
            if dst.exists():
                backup=data/'polyCombImportBackups'/stamp/dst.relative_to(data)
                backup.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dst,backup)
            dst.parent.mkdir(parents=True,exist_ok=True)
            tmp=dst.with_name(dst.name+'.installing');shutil.copy2(src,tmp);tmp.replace(dst)
    for old in retired:
        backup=data/'polyCombImportBackups'/stamp/old.relative_to(data)
        backup.parent.mkdir(parents=True,exist_ok=True);shutil.move(str(old),str(backup))
    if (synths/'ReaktorSnapshots'/'2025').is_dir() and not any((synths/'ReaktorSnapshots'/'2025').iterdir()):(synths/'ReaktorSnapshots'/'2025').rmdir()
    for src,dst in planned:assert digest(src)==digest(dst),dst
    for p,sha in protected.items():assert digest(p)==sha,f'Current snapshot changed: {p}'
    print(f'Installed and verified 296 snapshots / 3 banks (1.7, 2018; {len(retired)} retired 2025 files moved into the backup) and the PolyComb / Modulation / MixerPro compressor definitions up to 32 voices; {len(changed)} assets updated. Current snapshot library unchanged.')
    print('In PolyComb: Banks > Load bank > Reaktor PolyComb ...')
if __name__=='__main__':main()
