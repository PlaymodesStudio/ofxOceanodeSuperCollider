#!/usr/bin/env python3
"""Replace the Chord Sequence node (ofxSantiNodes) with SC ChordBox in Oceanode presets.

ChordBox (2026-10-18) keeps the old node's parameters and outputs under the same
names (Index, NumChords, Transpose, Pitchbend, Inversion, Scale, Reset, Root,
ChPh, ProgPh, OutputN, outNsize) and reads its "state" JSON as it is, so a
preset is converted by renaming the node:
  modules.json        "Chord Sequence" -> "SC ChordBox"
  connections.json    "Chord_Sequence_<n>" -> "SC_ChordBox_<n>"
  Chord_Sequence_<n>.json -> SC_ChordBox_<n>.json (unchanged)
Each affected folder is copied first to <data>/Presets_backup_ChordBox/<its path>.
Running it twice is harmless.

  tools/migrate_chordsequence.py [data folder ...] [--dry-run]
"""
import json
import os
import re
import shutil
import sys

OLD_KEY, NEW_KEY = "Chord Sequence", "SC ChordBox"
OLD_FILE, NEW_FILE = "Chord_Sequence_", "SC_ChordBox_"
BACKUP = "Presets_backup_ChordBox"
HERE = os.path.dirname(os.path.abspath(__file__))
# GUI parameter names -> ArpeggiatorBox's (connections)
PARAM_RENAMES = {}


def control_keys():
    return set()


def num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def convert_node(old, keys):
    return old


def rename_conn_node(name):
    return NEW_FILE + name[len(OLD_FILE):] if name.startswith(OLD_FILE) else name


def migrate_folder(folder, data, keys, dry):
    mpath = os.path.join(folder, "modules.json")
    modules = json.load(open(mpath, encoding="utf-8"))
    if OLD_KEY not in modules:
        return False
    print(("would migrate " if dry else "migrating ") + folder)
    if dry:
        return True
    backup = os.path.join(data, BACKUP, os.path.relpath(folder, data))
    if not os.path.exists(backup):
        shutil.copytree(folder, backup)
    ids = modules.pop(OLD_KEY)
    target = modules.setdefault(NEW_KEY, {})
    renumber = {}
    for i, pos in ids.items():
        new_id = i
        while new_id in target:
            new_id = "%02d" % (int(new_id) + 50)
        renumber[str(int(i))] = str(int(new_id))
        target[new_id] = pos
    json.dump(modules, open(mpath, "w", encoding="utf-8"), indent=4)

    def node_name(n):
        if not n.startswith(OLD_FILE):
            return n
        return NEW_FILE + renumber.get(n[len(OLD_FILE):], n[len(OLD_FILE):])

    cpath = os.path.join(folder, "connections.json")
    if os.path.exists(cpath):
        conns = json.load(open(cpath, encoding="utf-8"))
        out = {}
        for src, params in conns.items():
            nsrc = node_name(src)
            for sp, dsts in params.items():
                nsp = PARAM_RENAMES.get(sp, sp) if src.startswith(OLD_FILE) else sp
                for dst, dps in dsts.items():
                    ndst = node_name(dst)
                    for dp, v in dps.items():
                        ndp = PARAM_RENAMES.get(dp, dp) if dst.startswith(OLD_FILE) else dp
                        out.setdefault(nsrc, {}).setdefault(nsp, {}).setdefault(ndst, {})[ndp] = v
        json.dump(out, open(cpath, "w", encoding="utf-8"), indent=4)
    for f in os.listdir(folder):
        if f.startswith(OLD_FILE) and f.endswith(".json"):
            old = json.load(open(os.path.join(folder, f), encoding="utf-8"))
            new_name = node_name(f[:-5]) + ".json"
            json.dump(convert_node(old, keys), open(os.path.join(folder, new_name), "w", encoding="utf-8"), indent=4)
            os.remove(os.path.join(folder, f))
    return True


def migrate(data, keys, dry):
    presets = os.path.join(data, "Presets")
    count = 0
    for root, dirs, files in os.walk(presets):
        if BACKUP in root or "modules.json" not in files:
            continue
        if OLD_KEY not in open(os.path.join(root, "modules.json"), encoding="utf-8").read():
            continue
        if migrate_folder(root, data, keys, dry):
            count += 1
    return count


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv
    keys = control_keys()
    if not args:
        apps = os.path.normpath(os.path.join(HERE, "..", "..", "..", "apps", "Playmodes"))
        args = [os.path.join(apps, a, "bin", "data") for a in ("OceanAudio", "OceanodeScroller")]
    total = sum(migrate(d, keys, dry) for d in args)
    print(f"{total} preset folder(s) {'to migrate' if dry else 'migrated'}")


if __name__ == "__main__":
    main()
