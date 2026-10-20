#!/usr/bin/env python3
"""Migrate Oceanode presets to renamed ~synthCreator synths.

Renamed 2026-10-20, because their builds clashed with another synth's
(Pulsar's stereo build was Pulsar2.scsyndef):

  Pulsar2             -> Pulsar_B
  CircularBufferFFT2  -> CircularBufferFFT_B
  RLPF4               -> RLPF4Pole
  RHPF4               -> RHPF4Pole

Presets store a node by its name, with or without the trailing "*":
  modules.json          "SC Pulsar2*": {...}     (older: "SC Pulsar2")
  connections.json      "SC_Pulsar2*_<n>": ...
  SC_Pulsar2*_<n>.json  (the node's own values)
This script finds every preset folder under the data folders given (or every
apps/Playmodes/*/bin/data by default), copies each affected folder to
<data>/Presets_backup_SynthRenames/<its path> first, then rewrites the names
and renames the node files. Running it twice is harmless (nothing left to do).

  tools/migrate_synth_renames.py [data folder ...] [--dry-run]
"""
import os
import re
import shutil
import sys

RENAMES = {
    "Pulsar2": "Pulsar_B",
    "CircularBufferFFT2": "CircularBufferFFT_B",
    "RLPF4": "RLPF4Pole",
    "RHPF4": "RHPF4Pole",
}
BACKUP = "Presets_backup_SynthRenames"

# "SC Pulsar2" or "SC Pulsar2*", as a whole JSON string
NAMES = [(re.compile(r'"SC ' + re.escape(old) + r'(\*?)"'), r'"SC ' + new + r'\1"') for old, new in RENAMES.items()]
# SC_Pulsar2_3 or SC_Pulsar2*_3, in node ids and file names
IDS = [(re.compile(r'(?<![A-Za-z0-9])SC_' + re.escape(old) + r'(\*?)_(\d+)'), r'SC_' + new + r'\1_\2') for old, new in RENAMES.items()]


def rewrite(text):
    for pattern, replacement in NAMES + IDS:
        text = pattern.sub(replacement, text)
    return text


def affected(folder):
    for f in os.listdir(folder):
        if not f.endswith(".json"):
            continue
        if rewrite(f) != f:
            return True
        try:
            with open(os.path.join(folder, f), encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        if rewrite(text) != text:
            return True
    return False


def migrate(data, dry):
    presets = os.path.join(data, "Presets")
    if not os.path.isdir(presets):
        return 0
    count = 0
    for root, dirs, files in os.walk(presets):
        if BACKUP in root or not affected(root):
            continue
        rel = os.path.relpath(root, data)
        backup = os.path.join(data, BACKUP, rel)
        print(("would migrate " if dry else "migrating ") + root)
        count += 1
        if dry:
            continue
        if not os.path.exists(backup):
            os.makedirs(os.path.dirname(backup), exist_ok=True)
            shutil.copytree(root, backup, dirs_exist_ok=True)
        for f in os.listdir(root):
            path = os.path.join(root, f)
            if not f.endswith(".json") or not os.path.isfile(path):
                continue
            try:
                with open(path, encoding="utf-8") as fh:
                    text = fh.read()
            except (OSError, UnicodeDecodeError):
                continue
            new = rewrite(text)
            if new != text:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new)
        for f in os.listdir(root):
            renamed = rewrite(f)
            if f.endswith(".json") and renamed != f:
                target = os.path.join(root, renamed)
                if os.path.exists(target):
                    print("  skipped (exists): " + target)
                    continue
                os.rename(os.path.join(root, f), target)
    return count


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv
    if not args:
        here = os.path.dirname(os.path.abspath(__file__))
        apps = os.path.normpath(os.path.join(here, "..", "..", "..", "apps", "Playmodes"))
        args = [os.path.join(apps, a, "bin", "data") for a in sorted(os.listdir(apps))
                if os.path.isdir(os.path.join(apps, a, "bin", "data"))]
    total = sum(migrate(d, dry) for d in args)
    print(f"{total} preset folder(s) {'to migrate' if dry else 'migrated'}")


if __name__ == "__main__":
    main()
