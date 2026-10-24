# SynthDef library audit

Read-only inventory and curated consolidation review. This tooling does not move,
rename, compile, merge or remove production SynthDefs or migrate presets.

Run from the repository root with Python 3 and ReportLab:

```sh
python3 tools/synthdef_audit/scan.py
python3 tools/synthdef_audit/build_report.py
```

Outputs (under the ignored `output/pdf/` directory):

- `OceanAudio-SynthDef-Library-Review.pdf`: review and exhaustive catalog.
- `synthdef-inventory.json`: source hashes, source roles, creator declarations,
  raw declaration expressions, extracted controls, proposed categories and plans.
- `synthdef-inventory.csv`: one row per creator, otherwise per raw/support source.

`review.py` contains curated groups and category proposals. Counts are calculated
from disjoint groups and validated against the inventory. A structural family
with seven bank sizes is one maintained generator/browser family, but still
requires different compiled graphs for its sizes. Do not interpret entry
reductions as equivalent binary or CPU reductions.

The scanner strips nested block and line comments, extracts balanced creator
bodies, and detects both `SynthDef(...)` and `SynthDef.new(...)`. It is a lexical
inventory, not a SuperCollider interpreter. Dynamic name expressions remain
expressions/families; their generated binary count is not invented. Descriptions
in the main catalog come from metadata. Raw/support descriptions use metadata
matches and static inference from names/UGen calls; inspect source contracts
before changing them. Class calls without `.ar/.kr/.ir/.new` may not appear in
the extracted core signature. Compiler eligibility is modeled from
`src/synthdefs/scSynthdefSync.cpp`, not tested by compiling all sources.

The PDF embeds macOS Arial fonts from `/System/Library/Fonts/Supplemental`.
Adapt those paths if generating on another platform. Render the result with
Poppler and check page images after changing report layout. Keep report artifacts
and render images out of production SynthDef folders.
