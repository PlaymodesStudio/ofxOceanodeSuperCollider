# Shared presets

Presets and snapshot banks shared by everyone who uses these nodes. The
SuperCollider menu's **Sync with SynthDefs Git...** installs them into the
app's data folder, at the same path as here:

| Here | What |
|---|---|
| `nodeSnapshots/<Node>/banks/<bank>.json` | Snapshot banks (Snapshots ▸ Load bank) |
| `nodeSnapshots/<Node>/Presets/<preset>.json` | Named presets of the shared snapshot bar |

Nothing else is synced: the working slots (`nodeSnapshots/<Node>/snapshot_*.json`,
`library.json`) stay each user's own.

## Adding or updating one

SuperCollider menu ▸ **Share Presets to Git...** lists the banks and presets
that are new or changed in your app since the last sync. Tick the ones to share
and press Share: they are copied here, committed on their own (nothing else in
the checkout is touched) and pushed to the checkout's branch with your git
account. Everyone gets them on their next sync. It needs the addon's git
checkout with push access; one marked "replaces a newer shared version" was
also changed by someone else since your last sync, so it starts unticked.

By hand: copy the file from the app's data folder to the same path here,
commit and push.

A preset someone changed on their machine is never overwritten: the sync
keeps theirs and writes the shared version next to it as `<name> (repo).json`.
A file removed from here is removed from apps where it was not changed.

## PolyComb

- `SANTI_1` is the main bank: a PolyComb with no snapshots yet loads it.
- `Reaktor PolyComb 1.7 - 001-128`, `- 129-210` and `Reaktor PolyComb 2018 - 2018`:
  the original Reaktor ensembles' snapshots translated to PolyComb
  (approximations; a bank holds at most 128 snapshots, hence two files for 1.7).
  The translation tools are in `tools/polycomb/`; the raw Reaktor data they
  read was removed after the translation.

AdditiveBox and PulsarBox named presets (they lived in `Supercollider/<Node>/Presets`
until 2026-10-21; the nodes move a user's old ones over on load) are written by `tools/additivebox_demo_presets.py`
and `tools/pulsarbox_presets.py`.

## SteamPipeBox

The default **Reaktor SteamPipe 01 - Instrument - Bank 1** preserves the 65
instrument snapshots and their native numbering, starting with **1 Flute**;
**tarrega** is **55**. Four separately labelled Ensemble banks hold the other
107 extracted instrument states. They preserve original bank boundaries and
UUID slot order, including six unresolved/empty AlgoMus slots. THRILLER is an
ensemble snapshot name; its inner instrument state is Flute. See
`synthdefs/Defaults/SteamPipeBox/README.md` for scope and migration details.
