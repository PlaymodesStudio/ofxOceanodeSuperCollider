# PhysicalBox render tests

Offline (NRT) checks of the Phase 0 voice in
`synthdefs/WIP/PhysicalBox/physicalbox.eel2`. They need no sound device and
no running server, but do need DynGen in the user extensions.

```bash
/Applications/SuperCollider.app/Contents/MacOS/sclang render_cases.scd /tmp/physicalbox-render
python3 analyze.py /tmp/physicalbox-render
```

`render_cases.scd [outDir] [filter|all] [script.eel2]` renders one WAV per
case: a filter keeps the cases whose name contains it, and a script path
renders a variant of the voice. `PB_CASES=<file>` replaces the built-in cases
with the array written in that file, which is useful for parameter scans.
`PB_ENGINE=ugen` renders the same cases through the Phase 1 C++ `PhysicalVoice`
instead of the DynGen script, to compare the two.

`analyze.py` prints, for each case:
- peak and RMS (0.4–1.4 s and the last 0.3 s);
- the autocorrelation pitch and its clarity;
- the spectral centroid;
- how long it stays within 60 dB of the peak;
- the L/R correlation;
- the lowest partial within 20 dB of the strongest one.

The measured results are listed in `synthdefs/WIP/PhysicalBox/README.md`.
