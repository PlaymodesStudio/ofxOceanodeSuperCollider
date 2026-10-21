#!/usr/bin/env python3
"""Turn a folder of named presets (<name>.json) into a snapshot bank (scSnapshots).

Each preset becomes a slot, in name order (as the old Preset combo listed them),
named after its file. The preset files are left as they are.

    python3 tools/presets_to_bank.py presets/Supercollider/AdditiveBox/Presets \\
        "presets/nodeSnapshots/AdditiveBox/banks/Demo and Astres.json"
"""
import argparse, json, pathlib, sys

SLOTS = 128   # scSnapshots::SLOTS


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('presets', type=pathlib.Path, help='folder of <name>.json presets')
    parser.add_argument('bank', type=pathlib.Path, help='bank file to write')
    args = parser.parse_args()
    files = sorted(args.presets.glob('*.json'), key=lambda p: p.stem)
    if not files: sys.exit('no presets in %s' % args.presets)
    if len(files) > SLOTS: sys.exit('%d presets: a bank holds %d' % (len(files), SLOTS))
    slots = []
    for f in files:
        sound = json.loads(f.read_text())
        if not isinstance(sound, dict): sys.exit('%s is not a preset' % f)
        sound['name'] = f.stem   # as scSnapshots::readPreset
        slots.append(sound)
    args.bank.parent.mkdir(parents=True, exist_ok=True)
    args.bank.write_text(json.dumps({'snapshotBank': 1, 'active': 0, 'slots': slots}, indent=2) + '\n')
    print('%s: %d snapshots' % (args.bank, len(slots)))


if __name__ == '__main__':
    main()
