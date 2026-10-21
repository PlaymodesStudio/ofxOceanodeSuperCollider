#!/usr/bin/env python3
"""Check scoped bank order against native UUID tables, not serialized offsets."""
import argparse
import collections
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('ensemble', type=pathlib.Path)
p.add_argument('--report', type=pathlib.Path)
a = p.parse_args()
extractor = ROOT / 'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'
r = json.loads(a.report.read_text() if a.report else subprocess.check_output([str(extractor), str(a.ensemble)], text=True))
raw = a.ensemble.read_bytes()
u32 = lambda o: struct.unpack_from('<I', raw, o)[0]
assert r['parser_revision'] == '2026-10-21-snapshot-bank-scope'
assert collections.Counter(s['snapshot_scope'] for s in r['snapshots']) == {'instrument': 65, 'ensemble': 107}
for s in r['snapshots']:
    offset = int(s['bank_name_offset'], 16)
    end = offset + 24 + u32(offset + 20) * 2
    count = u32(end + 8)
    slots = {raw[end+16+i*20:end+32+i*20].hex(): u32(end+12+i*20) for i in range(count)}
    assert slots[s['container_uuid']] == s['container_bank_slot']
    assert count == s['container_bank_size']

thriller = [s for s in r['snapshots'] if s.get('container_name') in ('THRILLER', 'THRILLER 1', 'THRILLER 2')]
assert [s['container_bank_slot'] for s in thriller] == [0, 1, 2]
assert all(s['snapshot_scope'] == 'ensemble' and s['name'] == 'Flute' and s['bank_slot'] == 0 for s in thriller)
assert all(s['ensemble_tempo_bpm'] == 120 for s in thriller)

folder = ROOT / 'synthdefs/Defaults/SteamPipeBox'
factory = json.loads((folder / 'factory.json').read_text())
banks = json.loads((folder / 'banks.json').read_text())
assert len(factory) == 172 and len(banks) == 5
assert [s['name'] for s in factory[:3]] == ['Flute', 'Pan', 'The Pipe']
assert factory[54]['name'] == 'tarrega'
assert all(s['scope'] == 'instrument' and s['bank_slot'] == i for i, s in enumerate(factory[:65]))
# The complete original instrument bank must keep exactly the same sound values.
values = json.dumps([s['values'] for s in factory[:65]], sort_keys=True, separators=(',', ':')).encode()
assert hashlib.sha256(values).hexdigest() == 'c0c01db57ad5a123a9d30f4d9a2aba1eace1f5fabf31355875382d076de1be90'
seen = []
for bank in banks:
    slots = json.loads((ROOT / 'presets/nodeSnapshots/SteamPipeBox/banks' / bank['file']).read_text())['slots']
    assert len(slots) == len(bank['indices']) == bank['slot_count']
    for slot, index in enumerate(bank['indices']):
        if index is None:
            assert slots[slot] is None
            continue
        preset = factory[index]
        assert preset['scope'] == bank['scope'] and preset['bank_slot'] == slot
        assert slots[slot]['name'] == preset['name']
        seen.append(index)
assert sorted(seen) == list(range(172))
assert banks[-1]['source_bank'] == 'AlgoMus' and banks[-1]['indices'][:6] == [None] * 6
print('PASS: 5 native banks, UUID slot order, Flute #1 / Tarrega #55, 65 unchanged instrument sounds, 107 separately scoped ensemble states and preserved unresolved slots')

# Exercise the actual migration block against real temporary files. A failed
# bank load must retain the old files; either path must preserve byte-exact edits.
node = (ROOT / 'src/nodes/scSteamPipeBox.cpp').read_text()
start = node.index('    const std::vector<std::string> legacyBanks=')
end = node.index('    if(snaps.active<0', start)
cpp = r'''
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
using J=nlohmann::json;
J ofLoadJson(std::string path){std::ifstream f(path);J j;f>>j;return j;}
std::string ofGetTimestampString(const char*){return "test";}
std::ostream& ofLogWarning(const char*){return std::cerr;}
struct Snap{bool ok,called=false;bool loadBank(std::string path){called=true;return ok && std::filesystem::exists(path);}};
int main(int argc,char**argv){
 const std::filesystem::path bankDir=argv[1];Snap snaps{std::string(argv[2])=="yes"};
 struct Host{std::string defaultBank="new.json";}sh;
 MIGRATION
 return snaps.called ? 0 : 1;
}
'''.replace('MIGRATION', node[start:end])
with tempfile.TemporaryDirectory() as folder:
    temp = pathlib.Path(folder)
    (temp / 'test.cpp').write_text(cpp)
    subprocess.run(['c++', '-std=c++17', '-I' + str(ROOT.parent.parent / 'libs/json/include'),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    for result in ('yes', 'no'):
        library = temp / result
        bank_dir = library / 'banks'
        bank_dir.mkdir(parents=True)
        files = {'library.json': '{"bank":"Reaktor SteamPipe 129-172.json","snapshots":1}',
                 'snapshot_0.json': '{"name":"User edited Tarrega","customCurve":42}',
                 'banks/Reaktor SteamPipe 001-128.json': '{"userEdited":123}',
                 'banks/Reaktor SteamPipe 129-172.json': '{"userEdited":456}',
                 'banks/User.json': '{"keep":789}'}
        for path, text in files.items():
            (library / path).write_text(text)
        (bank_dir / 'new.json').write_text('{"slots":[{"name":"Flute"}]}')
        subprocess.run([str(temp / 'test'), str(bank_dir), result], check=True)
        backup = library / 'Legacy bank layout - test'
        for path, text in files.items():
            if path != 'banks/User.json':
                assert (backup / path).read_text() == text
        assert (bank_dir / 'User.json').read_text() == files['banks/User.json']
        for name in ('Reaktor SteamPipe 001-128.json', 'Reaktor SteamPipe 129-172.json'):
            assert (bank_dir / name).exists() == (result == 'no')
print('PASS: migration preserves all working edits and old banks byte-for-byte; failed loads retain old files; custom banks stay untouched')
