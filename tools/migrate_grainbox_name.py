#!/usr/bin/env python3
"""Migrate Oceanode presets from the node name "SC GrainBoxPoly" to "SC GrainBox".

The node was renamed (2026-10-17). Presets store a node by its name:
  modules.json        "SC GrainBoxPoly": {...}
  connections.json    "SC_GrainBoxPoly_<n>": ...
  SC_GrainBoxPoly_<n>.json   (the node's own values)
This script finds every preset folder under the data folders given (or every
apps/Playmodes/*/bin/data by default), copies each affected folder to
<data>/Presets_backup_GrainBoxRename/<its path> first, then rewrites the names
and renames the node files. Running it twice is harmless (nothing left to do).

  tools/migrate_grainbox_name.py [data folder ...] [--dry-run]
"""
import os
import shutil
import sys

OLD_KEY, NEW_KEY = "SC GrainBoxPoly", "SC GrainBox"
OLD_FILE, NEW_FILE = "SC_GrainBoxPoly_", "SC_GrainBox_"
BACKUP = "Presets_backup_GrainBoxRename"


def affected(folder):
    files = os.listdir(folder)
    if any(f.startswith(OLD_FILE) and f.endswith(".json") for f in files):
        return True
    for f in files:
        if f.endswith(".json"):
            try:
                with open(os.path.join(folder, f), encoding="utf-8") as fh:
                    text = fh.read()
            except (OSError, UnicodeDecodeError):
                continue
            if OLD_KEY in text or OLD_FILE in text:
                return True
    return False


def migrate(data, dry):
    presets = os.path.join(data, "Presets")
    if not os.path.isdir(presets):
        return 0
    count = 0
    for root, dirs, files in os.walk(presets):
        if BACKUP in root:
            continue
        if not affected(root):
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
            new = text.replace(OLD_KEY + '"', NEW_KEY + '"').replace(OLD_FILE, NEW_FILE)
            if new != text:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new)
        for f in os.listdir(root):
            if f.startswith(OLD_FILE) and f.endswith(".json"):
                target = os.path.join(root, NEW_FILE + f[len(OLD_FILE):])
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
