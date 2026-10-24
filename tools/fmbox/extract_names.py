#!/usr/bin/env python3
"""Extract upstream PDF catalogue text (pdftotext -layout) into bank-bound names.

Usage: extract_names.py /tmp/catalogue.txt
The PDF has zero-based indexes; fmBox presents one-based preset numbers.
Fingerprints bind each entry to the current bundled patch at that index, not
prove that the author's historical catalogue describes that patch correctly.
"""
import hashlib,pathlib,re,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
folder=ROOT/'data/Supercollider/Presets/FMBox'
rows={}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    m=re.fullmatch(r'\s*(\d+)\s+(.+?)\s+(\d{2})(?:\s+(.*?))?\s*',line)
    if m:
        index=int(m[1]);assert index not in rows,index
        rows[index]=(m[2].strip(),int(m[3]),(m[4]or'').strip())
bank=(folder/'DX7.afx').read_text().splitlines()
assert set(rows)==set(range(len(bank))) and len(bank)==16384
out=['# DX7-SC-Presets-v03 [BETA]; names unverified; zero-based index\tFNV1a64 of 145 field values\tbank\tslot\tname']
for i,line in enumerate(bank):
    assert len(line)==290
    h=14695981039346656037
    for k in range(0,290,2):h=((h^int(line[k:k+2]))*1099511628211)&((1<<64)-1)
    b,slot,name=rows[i];assert 1<=slot<=32
    out.append(f'{i}\t{h:016x}\t{b}\t{slot:02d}\t{name}')
(folder/'DX7.names.tsv').write_text('\n'.join(out)+'\n')
print(f'Extracted {len(rows)} entries; {sum(bool(r[2])for r in rows.values())} named; {sum(not r[2]for r in rows.values())} unnamed')
print('Catalogue text SHA-256:',hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
