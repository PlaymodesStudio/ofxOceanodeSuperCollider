# Reaktor Patch Extractor 0.5.1

Aplicació macOS universal (`arm64` i `x86_64`) i executable de terminal per
inspeccionar qualsevol fitxer binari `NRKT` de Reaktor (`.ism` i `.ens`).
SteamPipe és el corpus de regressió, no un cas especial del parser. És només de
lectura.

## Aplicació

Obre `dist/Reaktor Patch Extractor.app`, selecciona un fitxer i exporta:

- JSON: jerarquia, mòduls, macros, ports, connexions, controls, constants,
  snapshots i Core Cells descodificades en estructures, mòduls, propietats i
  connexions;
- DOT: graf complet compatible amb Graphviz.

L'aplicació inclou `module_catalog.json`: els 552 IDs possibles de Reaktor
6.5.0, incloent slots reservats. Conté 289 tipus amb nom, 284 tipus documentats,
1.451 ports enumerats (1.421 amb tooltip exacte) i 130 funcions DSP
`mono::Audio…`. També conserva algunes classes d'implementació.

També inclou `core_module_catalog.json`, derivat de les factories C++ de la
mateixa build: 90 tipus Core amb ID, nom, classe d'implementació, flavor i ID
de recurs. Els documents Core `TaggedFile` es recomponen a partir dels seus
segments `none`/`zlib`; el JSON conserva el graf recursiu, els ports de cel·la,
els valors ràpids, les propietats i tots els cables resolts per camí local.

## Terminal

El mateix binari funciona sense interfície gràfica:

```bash
"dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor" patch.ism > patch.json
"dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor" --summary patch.ism
"dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor" --dot patch.ism > patch.dot
```

## Compilació

Requereix les Command Line Tools de Xcode:

```bash
chmod +x build.sh
./build.sh
```

El parser Swift és autònom i no necessita Python. La implementació Python de
referència es manté a `docs/steampipe_extraction/parse_nrkt.py`.

## Regenerar el diccionari per a una altra versió de Reaktor

```bash
python3 extract_reaktor_catalog.py \
  --runtime-catalog runtime_tmoddata_reaktor-6.5.0.json \
  --output module_catalog.json
python3 extract_core_catalog.py \
  --output core_module_catalog.json
./build.sh
```

El generador llegeix l'executable instal·lat sense modificar-lo. Cada catàleg
inclou el SHA-256 de l'executable: els IDs no es tracten com constants eternes
entre versions de Reaktor.

`lldb_dump_catalog.py` genera el JSON runtime aturant temporalment Reaktor a
`main`, després dels inicialitzadors estàtics. Això permet llegir exactament
les descripcions i els arrays de ports, incloent rangs, escales, pendents de
filtres i altres textos contextuals. El procés de depuració es tanca
immediatament després de l'exportació.

El catàleg enumera tots els IDs acceptats per `TSModul::GetModData`, però
Reaktor 6 només inicialitza documentació per als tipus disponibles en aquesta
build. Els altres queden marcats
`reserved_or_unavailable`; no s'inventa informació.

## Compatibilitat de serialització

El parser reconeix les dues revisions trobades fins ara:

- `0x22f300`: Reaktor 6.5 i SteamPipe;
- `0x228d00`: Ensembles antics, verificat amb CRAX 2 i comets.

La revisió antiga mou la metainformació de macros i afegeix dos words als
snapshots. Si un snapshot antic conserva una numeració d'objectes que no es pot
relacionar de manera segura amb els IDs actuals, els valors s'exporten amb l'ID
local i `object_id_mapping_status: unresolved`.

L'esquema `reaktor-patch-report-v4` també extreu l'estat global desat dels
controls de panell (`saved_control_normalized` i `saved_control_value`). Per a
cada snapshot resolt, `effective_controls` combina aquest estat amb els
`snapshot_override` corresponents. Això és necessari per reconstruir presets
complets quan Reaktor exclou controls del snapshot mitjançant Snap Isolate.

Els mòduls `Note Pitch` exporten també
`module_properties.pitch_offset_semitones`. Aquesta propietat no és un valor
de preset: modifica el senyal del mòdul i és necessària per reconstruir
correctament macros de key tracking.

## Verificació creuada

Després de compilar, compara la sortida Swift amb la implementació Python:

```bash
python3 verify_parsers.py patch.ism patch.ens
```

La gramàtica binària coneguda està documentada a
`docs/steampipe_extraction/FORMAT.md`.

## Aliasing Synth regression / report v5

The native parser now recognizes legacy Stacked Macro (kind 7) scopes. In
`aliasing_synth_midi.ens` this keeps all instrument modules in their proper
scope and resolves 533/533 Primary wires (previously 521/533). Legacy constants
export their stored float at +0x44 as `constant_numeric_value` and the
backwards-compatible numeric `constant_value`; incidental printable strings
are not a reliable source for these values.

Legacy panel records can carry extra link words. Their stable panel reference
is exported as `snapshot_control_id` (stored as 10000 + ID). Snapshot IDs
precede the state record; they are not traversal indices. V5 resolves unique
stable references before attempting a validated traversal offset. Ambiguous
references are left without a module assignment, and scalar fallback matches
must land on panel controls. Switch selections are exported as integer
`selection_index`, not clamped to a fake normalized 0/1.

Run the regression against an external copy of the ensemble:

```sh
python3 test_aliasing.py /path/to/aliasing_synth_midi.ens
```

It checks all 38 x 61 state identities, numeric constants, linked panels,
stacked scopes and a deliberately duplicated panel ID. The original ensemble
and the user reference recordings are not included in the repository. Other
ensembles should be rechecked before treating v5 output as a conversion
reference; the old Python comparison parser has not been updated in this
change.


### PolyComb 1.7: complete compound states (0.5.1)

Snapshot records are not restricted to lengths 32/40/60: PolyComb contains
548-byte scale-table states and 76-byte UI states. They are now retained as
bounded, aligned compound payloads instead of silently ending the snapshot.
This recovers 210 x 237 states, including 193 uniquely mapped panel references
per preset. MIDI assignments and other non-panel state remain unassigned;
they do not veto the panel identities. Aliasing Synth exports also become
complete: 61 states per snapshot, including its compound UI state and isolated
ADSR records, rather than the previous truncated 47-state prefix.

```sh
python3 test_polycomb.py /path/to/PolyComb_1.7.ens
python3 test_aliasing.py /path/to/aliasing_synth_midi.ens
```

The separate `../polycomb/import_ensemble.py` creates candidate preset files
and an audit without changing the Oceanode synth or installed libraries.

### PolyComb preset containers (0.5.2)

Newer ensembles can wrap an instrument state in a named UTF-16 preset container.
The JSON retains `name` for the inner state and adds `container_name`,
`bank_name`, their byte offsets, and validated `bank_index` / `bank_slot` fields.
For example, the 2025 file has 35 distinct outer names around inner states all
named `TEST-CHANS`. Bank names follow the serialized DSIN metadata boundaries;
snapshot IDs remain local to their instrument and can be ambiguous across
instruments. Do not treat the flattened panel lookup as a universal ID scope.

Regression: `python3 test_polycomb_later.py /path/to/2018.ens /path/to/2025.ens`.
