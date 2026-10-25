#!/usr/bin/env python3
"""Rebuild the catalogue from parameter-matched original SysEx banks.

Download the archive recorded in catalogue_sources.json, then run:
    python3 tools/fmbox/extract_names.py ARCHIVE.zip [--check]

The historical PDF is deliberately not used: its names describe other patches.
The manifest preserves AFX indexes and records sources, reordered slots,
exact-match overrides and explicitly unverified voices.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
FOLDER = ROOT / 'data/Supercollider/Presets/FMBox'
MANIFEST = Path(__file__).with_name('catalogue_sources.json')
GLOBAL_LIMITS = [31, 7, 48, 5, 99, 7, 99, 99, 1, 99, 1] + [99] * 8
OP_LIMITS = [1, 31, 99, 14, 7] + [99] * 9 + [99, 99, 3, 3, 99, 7, 3]


def convert_voice(v):
    """Packed OP6..OP1 VMEM -> AFX OP1..OP6, including inverted rates.

    Historical banks contain invalid high bits and values above Yamaha's
    ranges. Mask packed fields and clamp scalar values to the AFX limits;
    this reproduces the normalization in the existing sound bank.
    """
    if len(v) != 128:
        raise ValueError('Expected a 128-byte packed DX7 voice')
    p = [v[110] & 31, v[111] & 7, v[117], (v[116] >> 1) & 7,
         v[112], (v[116] >> 4) & 7, v[114], v[115], v[116] & 1,
         v[113], (v[111] >> 3) & 1]
    p += [99 - r for r in v[102:106]] + list(v[106:110])
    for op in range(6):
        o = v[(5 - op) * 17:(6 - op) * 17]
        p += [o[15] & 1, (o[15] >> 1) & 31, o[16], (o[12] >> 3) & 15,
              o[12] & 7]
        p += [99 - r for r in o[:4]] + list(o[4:8])
        p += [o[14], o[9], o[10], o[11] & 3, (o[11] >> 2) & 3,
              o[8], (o[13] >> 2) & 7, o[13] & 3]
    limits = GLOBAL_LIMITS + OP_LIMITS * 6
    assert len(p) == len(limits) == 145
    return tuple(max(0, min(value, limit)) for value, limit in zip(p, limits))


def read_bank(archive, path):
    data = archive.read(path)
    if (len(data) == 4104 and data[:2] == bytes([0xf0, 0x43])
            and data[3:6] == bytes([9, 32, 0]) and data[-1] == 0xf7):
        data = data[6:-2]
    elif len(data) != 4096:
        raise ValueError(f'Unsupported packed bank: {path}')
    # Some source banks have bad checksums or reserved high bits. The pinned
    # archive hash plus all-field comparisons below establish identity.
    return [data[i:i + 128] for i in range(0, 4096, 128)]


def fingerprint(patch):
    value = 14695981039346656037
    for field in patch:
        value = ((value ^ field) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def voice_name(voice):
    # Preserve printable DX7 ASCII; keep controls out of the TSV.
    return ' '.join(''.join(chr(c) if 32 <= c <= 126 else ' '
                            for c in voice[118:128]).split())


def generate(archive_path):
    manifest = json.loads(MANIFEST.read_text())
    raw = (FOLDER / 'DX7.afx').read_bytes()
    if hashlib.sha256(raw).hexdigest() != manifest['afx_sha256']:
        raise ValueError('AFX bank differs from the audited source')
    if hashlib.sha256(archive_path.read_bytes()).hexdigest() != manifest['archive_sha256']:
        raise ValueError('Archive differs from the pinned reference')
    patches = [tuple(int(row[i:i + 2]) for i in range(0, 290, 2))
               for row in raw.decode().splitlines()]
    assert len(patches) == 16384
    rows = ['# SysEx parameter-matched catalogue; 3 voices explicitly unverified; '
            'zero-based index\tFNV1a64 of 145 field values\tbank\tslot\tname']
    seen_banks = set()
    verified = 0
    with zipfile.ZipFile(archive_path) as archive:
        cache = {}

        def source(path):
            if path not in cache:
                cache[path] = read_bank(archive, path)
            return cache[path]

        for number, bank in enumerate(manifest['banks']):
            start, label = bank['start'], bank['bank']
            assert start == number * 32 and label not in seen_banks
            seen_banks.add(label)
            digest = hashlib.sha256(bytes(x for p in patches[start:start + 32] for x in p))
            assert digest.hexdigest() == bank['afx_sha256']
            slots = bank.get('slots', list(range(32)))
            assert sorted(slots) == list(range(32))
            overrides = {entry['slot']: entry for entry in bank.get('overrides', [])}
            unknown = bank.get('unverified_slots', [])
            for slot in range(32):
                index = start + slot
                if slot in unknown:
                    name = f'Unverified {slot + 1:02d}'
                else:
                    entry = overrides.get(slot, {})
                    voice = source(entry.get('source', bank['source']))[
                        entry.get('source_slot', slots[slot])]
                    if convert_voice(voice) != patches[index]:
                        raise ValueError(f'Parameter mismatch at AFX index {index}')
                    name = voice_name(voice)
                    verified += 1
                rows.append(f'{index}\t{fingerprint(patches[index])}\t{label}\t{slot + 1:02d}\t{name}')
    assert len(seen_banks) == 512 and verified == 16381
    return '\n'.join(rows) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--check', action='store_true', help='Verify without rewriting the catalogue')
    args = parser.parse_args()
    catalogue = generate(args.archive)
    target = FOLDER / 'DX7.names.tsv'
    if args.check:
        if target.read_text() != catalogue:
            raise ValueError('Catalogue differs from the verified reconstruction')
    else:
        target.write_text(catalogue)
    print('PASS: 512 banks, 16381 parameter-matched voices; 3 explicitly unverified; AFX unchanged')


if __name__ == '__main__':
    main()
