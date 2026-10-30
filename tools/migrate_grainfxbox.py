#!/usr/bin/env python3
"""Move GrainBox nodes that process an input to the new GrainFxBox node.

GrainBox was split (2026-10-30): GrainBox plays samples (no In port), and
GrainFxBox records its In port. A saved "SC GrainBox" node is converted when
an engine records the input ("sampleLive", "sampleLive 2"... true) or
something is connected to its In port. Other GrainBox nodes stay as they are.
Presets store a node by its name:
  modules.json        "SC GrainBox": {"<id>": [x, y]}
  connections.json    "SC_GrainBox_<n>": ...   (and as a target)
  SC_GrainBox_<n>.json   (the node's own values)
Every preset folder under the data folders given (or every
apps/Playmodes/*/bin/data by default) is checked, macros' folders included.
Each affected folder is first copied to <data>/Presets_backup_GrainFxBox/<its
path>. Running it twice is harmless (nothing left to do).

  tools/migrate_grainfxbox.py [data folder ...] [--dry-run]
"""
import json
import os
import re
import shutil
import sys

OLD_KEY, NEW_KEY = "SC GrainBox", "SC GrainFxBox"
OLD_FILE, NEW_FILE = "SC_GrainBox_", "SC_GrainFxBox_"
BACKUP = "Presets_backup_GrainFxBox"


def load(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def records_input(folder, n, connections):
    node = load(os.path.join(folder, "%s%d.json" % (OLD_FILE, n))) or {}
    if any(k.startswith("sampleLive") and v is True for k, v in node.items()):
        return True
    target = "%s%d" % (OLD_FILE, n)
    for outs in (connections or {}).values():
        for targets in (outs or {}).values():
            if isinstance(targets, dict) and isinstance(targets.get(target), dict) and "In" in targets[target]:
                return True
    return False


def to_convert(folder):
    modules = load(os.path.join(folder, "modules.json"))
    if not isinstance(modules, dict) or not isinstance(modules.get(OLD_KEY), dict):
        return []
    connections = load(os.path.join(folder, "connections.json"))
    ids = []
    for key in modules[OLD_KEY]:
        try:
            n = int(key)
        except ValueError:
            continue
        if records_input(folder, n, connections):
            ids.append((key, n))
    return ids


def convert(folder, ids):
    path = os.path.join(folder, "modules.json")
    modules = load(path)
    new = modules.setdefault(NEW_KEY, {})
    for key, n in ids:
        new[key] = modules[OLD_KEY].pop(key)
    if not modules[OLD_KEY]:
        del modules[OLD_KEY]
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(modules, fh, indent=4)
    names = [n for _, n in ids]
    pattern = re.compile(r"%s(%s)(?!\d)" % (re.escape(OLD_FILE), "|".join(str(n) for n in names)))
    for f in os.listdir(folder):
        p = os.path.join(folder, f)
        if not os.path.isfile(p) or not (f.endswith(".json") or f.endswith(".ini")):
            continue
        try:
            with open(p, encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        out = pattern.sub(lambda m: NEW_FILE + m.group(1), text)
        if out != text:
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(out)
    for n in names:
        src = os.path.join(folder, "%s%d.json" % (OLD_FILE, n))
        if os.path.exists(src):
            os.replace(src, os.path.join(folder, "%s%d.json" % (NEW_FILE, n)))


def migrate(data, dry):
    presets = os.path.join(data, "Presets")
    if not os.path.isdir(presets):
        return 0
    count = 0
    for root, dirs, files in os.walk(presets):
        if BACKUP in root:
            continue
        ids = to_convert(root)
        if not ids:
            continue
        print(("would convert " if dry else "converting ") + root + "  (GrainBox " +
              ", ".join(str(n) for _, n in ids) + ")")
        count += 1
        if dry:
            continue
        backup = os.path.join(data, BACKUP, os.path.relpath(root, data))
        if not os.path.exists(backup):
            os.makedirs(os.path.dirname(backup), exist_ok=True)
            shutil.copytree(root, backup, dirs_exist_ok=True)
        convert(root, ids)
    return count


def main():
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv
    if not args:
        apps = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../apps/Playmodes")
        args = [os.path.join(apps, a, "bin/data") for a in sorted(os.listdir(apps))
                if os.path.isdir(os.path.join(apps, a, "bin/data"))]
    total = sum(migrate(d, dry) for d in args)
    print("%d preset folder(s) %s" % (total, "to convert" if dry else "converted"))


if __name__ == "__main__":
    main()
