#!/usr/bin/env python3
"""Migrate Oceanode presets to the filters' Cutp input (renamed from Pitch).

Renamed 2026-10-27: in these generic filters the "pitch" control is a cutoff
(or centre) frequency, not a note, so Global Pitch must not reach it. Its
SynthDef control is now \\cutp, shown as "Cutp":

  BandpassFilter CompensatedSVF Filter4Pole LadderFilter LadderMultimode
  MoogFilter MoogVCF MultimodeFilter Notch OnePole PeakFilter RLPF RHPF RBPF
  RLPF4Pole RHPF4Pole VadimFilter BloobsBandpass FilteredFeedbackDelay

Presets refer to the input of a node SC_<Name>*_<n> as:
  SC_<Name>*_<n>.json      {"Pitch": ...}                      (its value)
  connections.json         {... "SC_<Name>*_<n>": {"Pitch": null}}
  globalSnapshots.json     "SC_<Name>*_<n>/Pitch": {...}
  custom_guis.json         "parameterPath": "SC_<Name>*_<n>/Pitch",
                           "parameterDisplayName": "Pitch"
Every one of them becomes Cutp. Old-format nodes (SC_<Name>_<n>, no "*")
are other SynthDefs and are left alone.

A data folder is only migrated when its installed SynthDefs already have the
renamed control (<data>/Supercollider/Synthdefs/.../<Name>.txarcmeta holds
'cutp'): migrated presets on an app still running the old builds would lose
their cutoffs. Sync the app first, or pass --force (for a folder of shared
macros that has no SynthDefs of its own, nothing is checked).

Each affected folder is copied to <data>/Presets_backup_Cutp/<its path> first.
Running it twice is harmless (nothing left to do).

  tools/migrate_filter_cutp.py [data folder ...] [--dry-run] [--force]
"""
import json
import os
import re
import shutil
import sys

NAMES = ["BandpassFilter", "CompensatedSVF", "Filter4Pole", "LadderFilter", "LadderMultimode",
         "MoogFilter", "MoogVCF", "MultimodeFilter", "Notch", "OnePole", "PeakFilter",
         "RLPF", "RHPF", "RBPF", "RLPF4Pole", "RHPF4Pole", "VadimFilter", "BloobsBandpass",
         "FilteredFeedbackDelay"]
OLD, NEW = "Pitch", "Cutp"
BACKUP = "Presets_backup_Cutp"
FOLDERS = ("Presets", "Macros", "Snapshots", "nodeSnapshots", "clipboardPreset")

_names = "|".join(map(re.escape, NAMES))
NODE_ID = re.compile(r"^SC_(?:" + _names + r")\*_\d+$")
NODE_FILE = re.compile(r"^SC_(?:" + _names + r")\*_\d+\.json$")
PATH = re.compile(r"(SC_(?:" + _names + r")\*_\d+)/" + OLD + r"\b")


def rename_key(d, old, new):
    """d with key old renamed to new, in place of it (no-op if new is there already)."""
    if old not in d or new in d:
        return d, False
    return {(new if k == old else k): v for k, v in d.items()}, True


def convert(value, own=False):
    """(converted value, changed). own: the node's own value file."""
    changed = False
    if isinstance(value, dict):
        if own:
            value, c = rename_key(value, OLD, NEW)
            changed |= c
        out = {}
        for k, v in value.items():
            nk = PATH.sub(r"\1/" + NEW, k) if isinstance(k, str) else k
            changed |= nk != k
            if isinstance(k, str) and NODE_ID.match(k) and isinstance(v, dict):
                v, c = rename_key(v, OLD, NEW)
                changed |= c
            v, c = convert(v)
            changed |= c
            out[nk] = v
        # a GUI widget bound to the renamed input also shows its name
        path = out.get("parameterPath")
        if isinstance(path, str) and path.endswith("/" + NEW) and NODE_ID.match(path.rsplit("/", 1)[0]) \
                and out.get("parameterDisplayName") == OLD:
            out["parameterDisplayName"] = NEW
            changed = True
        return out, changed
    if isinstance(value, list):
        items = [convert(v) for v in value]
        return [v for v, _ in items], any(c for _, c in items)
    if isinstance(value, str):
        nv = PATH.sub(r"\1/" + NEW, value)
        return nv, nv != value
    return value, False


def planned_changes(folder):
    contents = {}
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if not name.endswith(".json") or not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
        except (OSError, UnicodeDecodeError):
            continue
        own = bool(NODE_FILE.match(name))
        # cheap filter: a filter's own file, or one that mentions a filter
        if not own and not any(n in text for n in NAMES):
            continue
        try:
            data = json.loads(text)
        except ValueError:
            print("  skipping unreadable " + path)
            continue
        data, changed = convert(data, own=own)
        if changed:
            contents[name] = json.dumps(data, indent=4, ensure_ascii=False) + "\n"
    return contents


def installed_ok(data):
    """True when the data folder's installed filters have the renamed control (or it has none)."""
    synthdefs = os.path.join(data, "Supercollider", "Synthdefs")
    if not os.path.isdir(synthdefs):
        return True
    stale = []
    for root, _, files in os.walk(synthdefs):
        for name in NAMES:
            meta = name + ".txarcmeta"
            if os.path.basename(root) == name and meta in files:
                with open(os.path.join(root, meta), errors="replace") as fh:
                    if "'cutp'" not in fh.read():
                        stale.append(name)
    if stale:
        print(data + ": its installed " + ", ".join(sorted(set(stale))) +
              " still use pitch; sync its SynthDefs first (or --force)")
    return not stale


def migrate(data, dry, force):
    if not force and not installed_ok(data):
        return 0
    roots = [os.path.join(data, f) for f in FOLDERS if os.path.isdir(os.path.join(data, f))]
    if not roots:
        roots = [data]   # a folder of macros / presets on its own
    count = 0
    for tree in roots:
        for root, dirs, _ in os.walk(tree):
            if BACKUP in root:
                dirs[:] = []
                continue
            contents = planned_changes(root)
            if not contents:
                continue
            rel = os.path.relpath(root, data)
            print(("would migrate " if dry else "migrating ") + root + " (" + ", ".join(sorted(contents)) + ")")
            count += 1
            if dry:
                continue
            backup = os.path.join(data, BACKUP, rel)
            if not os.path.exists(backup):
                os.makedirs(os.path.dirname(backup), exist_ok=True)
                shutil.copytree(root, backup, dirs_exist_ok=True)
            for name, text in contents.items():
                with open(os.path.join(root, name), "w", encoding="utf-8") as fh:
                    fh.write(text)
    return count


def main():
    flags = {"--dry-run", "--force"}
    args = [a for a in sys.argv[1:] if a not in flags]
    dry, force = "--dry-run" in sys.argv, "--force" in sys.argv
    if not args:
        here = os.path.dirname(os.path.abspath(__file__))
        apps = os.path.normpath(os.path.join(here, "..", "..", "..", "apps", "Playmodes"))
        args = [os.path.join(apps, a, "bin", "data") for a in sorted(os.listdir(apps))
                if os.path.isdir(os.path.join(apps, a, "bin", "data"))]
    total = sum(migrate(d, dry, force) for d in args)
    print(f"{total} folder(s) {'to migrate' if dry else 'migrated'}")


if __name__ == "__main__":
    main()
