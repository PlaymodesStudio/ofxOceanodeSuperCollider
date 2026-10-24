#!/usr/bin/env python3
"""Migrate Oceanode presets to renamed ~synthCreator synths.

Renamed 2026-10-24: Wavetable -> WavetableBox.

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
Also renamed 2026-10-22: ReaktorFilter4 -> LadderMultimode and
ReaktorSaturator -> ParabolicSaturator (DSP unchanged).
Retired 2026-10-23: MonoBandPassBank32 -> MonoBandPassBank64 (both built 64 bands).

This script finds every preset and reusable macro folder under the data folders given (or every
apps/Playmodes/*/bin/data by default), copies each affected folder to
<data>/Presets_backup_SynthRenames/<its path> first, then rewrites the names
and renames the node files. Running it twice is harmless (nothing left to do).

  tools/migrate_synth_renames.py [data folder ...] [--dry-run]
"""
import os
import json
import re
import shutil
import sys

RENAMES = {
    "Wavetable": "WavetableBox",
    "Pulsar2": "Pulsar_B",
    "CircularBufferFFT2": "CircularBufferFFT_B",
    "RLPF4": "RLPF4Pole",
    "RHPF4": "RHPF4Pole",
    "ReaktorFilter4": "LadderMultimode",
    "ReaktorSaturator": "ParabolicSaturator",
    # The old "32" entry actually built the identical 64-band graph.
    "MonoBandPassBank32": "MonoBandPassBank64",
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


def planned_changes(folder):
    """Preflight files and merge disjoint old/new model groups without data loss."""
    contents, renames = {}, {}
    merged = False

    def unique_pairs(pairs):
        nonlocal merged
        result = {}
        for key, value in pairs:
            if key in result:
                previous = result[key]
                if (isinstance(previous, dict) and isinstance(value, dict)
                        and not previous.keys() & value.keys()):
                    previous.update(value)
                    merged = True
                else:
                    raise ValueError("overlapping JSON key after rename: " + key)
            else:
                result[key] = value
        return result

    for filename in sorted(os.listdir(folder)):
        path = os.path.join(folder, filename)
        if not filename.endswith(".json") or not os.path.isfile(path):
            continue
        target = rewrite(filename)
        if target != filename:
            if os.path.exists(os.path.join(folder, target)):
                raise ValueError("node filename collision: " + target)
            renames[filename] = target
        with open(path, encoding="utf-8") as fh:
            original = fh.read()
        updated = rewrite(original)
        if updated != original:
            merged = False
            parsed = json.loads(updated, object_pairs_hook=unique_pairs)
            contents[filename] = json.dumps(parsed, indent=4) + "\n" if merged else updated
    return contents, renames


def migrate(data, dry):
    count = 0
    roots = [os.path.join(data, name) for name in ("Presets", "Macros")]
    for root, dirs, files in (entry for tree in roots if os.path.isdir(tree) for entry in os.walk(tree)):
        if BACKUP in root or not affected(root):
            continue
        # Validate even in dry-run mode, before backing up or writing anything.
        contents, renames = planned_changes(root)
        rel = os.path.relpath(root, data)
        backup = os.path.join(data, BACKUP, rel)
        print(("would migrate " if dry else "migrating ") + root)
        count += 1
        if dry:
            continue
        if not os.path.exists(backup):
            os.makedirs(os.path.dirname(backup), exist_ok=True)
            shutil.copytree(root, backup, dirs_exist_ok=True)
        for filename, text in contents.items():
            with open(os.path.join(root, filename), "w", encoding="utf-8") as fh:
                fh.write(text)
        for filename, target in renames.items():
            os.rename(os.path.join(root, filename), os.path.join(root, target))
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
