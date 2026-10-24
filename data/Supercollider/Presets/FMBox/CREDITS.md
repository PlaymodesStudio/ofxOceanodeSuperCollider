The DX7.afx preset bank and the DX7-based FMBox SynthDefs are distributed with
an adaptation of DX7-Supercollider by Aziz Ege Gonul, under GNU GPL version 3.
See LICENSE.md in this directory.

Upstream: https://github.com/everythingwillbetakenaway/DX7-Supercollider
Pinned source commit: 1128e24c5f68a3bc2586927d2b8bbdeb311852ca

The modified source, original source, generation tools and attribution are in
ofxOceanodeSuperCollider/synthdefs/Defaults/FMBox/ and tools/fmbox/ in the addon
repository. FMBox retains the upstream conversion formulas and tables while
adding the Oceanode editor, editable routing, modulation, snapshots and effects.

AFX presets have no embedded names. Their original order is preserved.

DX7.names.tsv is extracted from the same upstream repository's
DX7-SC-Presets-v03 [BETA].pdf at the pinned commit above (GPL v3).
Catalogue PDF SHA-256: d8769d89718943cab1cbd57b0e315f745b221aafc7456451cf8d48ee458f57db
Extraction: pdftotext -layout, then tools/fmbox/extract_names.py.
It has 16,384 entries: 16,251 named and 133 blank. PDF indexes are zero-based;
fmBox displays one-based preset numbers. Names and banks are unverified hints:
the author reported uncertainty about the original bank's ordering:
https://sccode.org/1-57R (comment dated 14 November 2017).
Per-entry fingerprints bind metadata to this bundled bank's current patches;
they do not verify the PDF's historical correspondence to those sounds.
