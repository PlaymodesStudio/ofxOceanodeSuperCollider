# Notes sobre el format binari NRKT

Aquest document descriu només els camps comprovats empíricament amb
`SteamPipe.ism` i `SteamPipe_santi_2.ens`. Els enters són `uint32` little-endian,
excepte quan s'indica el contrari.

## Registres de classe

Els objectes serialitzats comencen així:

```text
0x5b                     '['
uint32                   longitud del nom de classe
bytes[length]            nom ASCII de classe
...                      payload de la classe
0x5d                     ']'
```

Classes verificades: `KSModul`, `KInPort` i `KOutPort`.

## KSModul

Capçalera comuna observada:

```text
+0x00  '[' + uint32(7) + "KSModul"
+0x0c  uint32 version        (1)
+0x10  uint32 signature      (0x0022f300 actual; 0x00228d00 antiga)
+0x14  uint32 kind           tipus numèric de mòdul
+0x18  uint32 object_id      ID global dins del fitxer
```

L'arrel especial té `object_id = 0x75646f4d` (`Modu` en bytes) i segueix un
esquema diferent; no s'ha de confondre amb un mòdul ordinari.

### Macro (`kind = 4`)

Els camps següents són estables en les macros examinades:

```text
+0x20  uint32 input_count
+0x24  uint32 output_count
+0x30  int32  x
+0x34  int32  y
+0x38  uint32 width
+0x3c  uint32 height
+0x104 uint32 name_length + UTF-8 name
       uint32 description_length + UTF-8 description
       ']'
       uint32 4
       uint32 immediate_child_count
```

En la revisió `0x00228d00`, el nom comença a `+0xf0` i el word que segueix
el `']'` és `2` en comptes de `4`. La resta de la reconstrucció en preordre
és equivalent.

Els mòduls fills apareixen immediatament després en preordre. Per tant, el
`immediate_child_count` permet reconstruir tot l'arbre sense delimitadors
addicionals.

Un instrument incrustat dins un Ensemble (`kind = 9`) també acaba la seva
capçalera variable amb `']' + uint32(revision) + uint32(child_count)`, on
`revision` és `2` o `4`.

## KOutPort i connexions

Capçalera:

```text
uint32 version                 (1)
uint32 signal_code
int32  declared_port_index     (-1 en molts ports externs de macro)
uint32 encoding
```

Metadades segons `encoding`:

- `0`: nom i descripció, tots dos `uint32 length + UTF-8`;
- `2`: descripció;
- `4`: nom;
- `6` o `22`: cap text.

El final del registre és sempre:

```text
uint32 connection_count
repeat connection_count times:
    uint32 target_local_module_index
    uint32 target_input_index
0x5d
```

`target_local_module_index` indexa la llista de fills del mateix contenidor que
el mòdul origen. Resoldre aquesta referència contra l'arbre converteix totes les
connexions locals en `object_id` globals.

## KInPort

Capçalera verificada:

```text
uint32 version
uint32 signal_code
int32  declared_port_index
uint32 source_module_sentinel
uint32 source_port_sentinel
uint32 encoding
```

Els tres bits baixos d'`encoding` descriuen el text amb el mateix esquema
`0/2/4/6` de `KOutPort`. Això permet recuperar noms d'entrada com `L` i `R` i
assignar-los a les connexions resoltes.

## Controls de panell

Per als tipus de control observats (`20`, `22`, `24` i `25`), la
instància conté una parella consecutiva de strings UTF-8 amb l'etiqueta i el
text d'ajuda. Els camps comuns verificats són:

```text
+0x48  float64  step_or_resolution
+0x5c  float32  range_endpoint_1
+0x60  float32  range_endpoint_2
```

Els endpoints es conserven en ordre perquè alguns controls, com la polaritat,
tenen un rang invertit. Alguns subtipus visuals no fan servir aquesta capçalera;
el parser només publica els valors quan passen controls de plausibilitat.

## Snapshots

Un candidat de snapshot té un nom `uint32 length + UTF-8` seguit pels words
`9, 2, 9`. Després de tres registres de metadades de 36 bytes, els paràmetres
tenen aquesta forma:

```text
uint32 payload_size            (32, 40 o 60)
bytes[payload_size]            comença per uint32(9), uint32(2), value_type
uint32 local_module_object_id
```

Un payload de 40 bytes guarda el valor principal al word 8: `value_type=1`
és enter i `value_type=2` és `float32` normalitzat. 32 representa un estat
buit/nul i 60 un estat compost, que es conserva com words crus.

Quan un instrument està incrustat en un Ensemble, els snapshots mantenen IDs
locals. L'extractor infereix l'offset de l'instrument i, quan la correspondència
és fiable, publica tant l'ID local com el global.

La revisió `0x00228d00` afegeix dos words (8 bytes) després de les metadades
comunes. En alguns Ensembles antics la correspondència entre IDs de snapshot i
IDs actuals no és una translació lineal. En aquest cas el snapshot no es perd:
es conserva l'ID local i queda marcat `object_id_mapping_status: unresolved`.

## Catàleg global de tipus

`extract_reaktor_catalog.py` deriva el diccionari de la build instal·lada:

- `TSModul::GetModData(eModType)` defineix el rang complet 4…555;
- Quick Search associa els noms públics amb `eModType`;
- `__GLOBAL__sub_I_moddat.cpp` conté descripcions i ajuda de ports;
- `mono::GetAudioFktTypeDependent` associa tipus amb funcions DSP.

`lldb_dump_catalog.py` atura una instància temporal a `main` i llegeix els
`std::string` i arrays de ports ja inicialitzats. Per a Reaktor 6.5.0 això
dóna 287 TModData, 284 tipus documentats, 1.451 ports i 1.421 tooltips exactes.
La lectura runtime preval sobre l'associació estàtica.

El JSON resultant inclou versió i SHA-256 de l'executable. Els slots sense nom,
documentació ni DSP es conserven explícitament com
`reserved_or_unavailable`.

## Core Cell / R5X

Els mòduls `kind=524` i els Legacy Event Core Cells `kind=525` corresponen, en
ordre, a documents `#NI#CS#Document##NI#Reaktor#Core#TaggedFile#` del
contenidor `RktX`. Cada document està fragmentat en fibres `data`:

```text
"atad" + uint32(2)
"crngbilz" | "crngenon"       compressió zlib o cap
uint32 stored_size
uint32 decoded_size
bytes[stored_size]
```

Les fibres descomprimides s'han de concatenar abans de llegir-les. El resultat
és un arbre de registres `uint32 tag, uint32 size, bytes[size]`. Els tags s'han
verificat contra `NI::SDIF::RXTaggedFileRead/Write` de Reaktor. El parser
descodifica recursivament:

- tres arrays de mòduls per estructura (`input`, `output`, `normal`);
- ID de tipus Core, posició, propietats, nom, descripció i estructura interna;
- connexions de mòdul/port, constants ràpides i referències de bus;
- metadades, bounds, IDs i checksums de cada estructura.

Cada mòdul rep un camí estable com `root/m9/m3`. Els índexs dels cables es
resolen contra aquests camins. El registre Primary original continua
preservant-se en base64, i cada fibra i document descodificat inclou SHA-256.

## Resultats de regressió SteamPipe

| Fitxer | Mòduls | Macros | Entrades | Sortides | Connexions | Snapshots |
|---|---:|---:|---:|---:|---:|---:|
| `SteamPipe.ism` | 947 | 90 | 1.011 | 896 | 989/989 resoltes | 65 |
| `SteamPipe_santi_2.ens` | 950 | 90 | 1.013 | 896 | 991/991 resoltes | 67 |

Les dues connexions addicionals de l'`.ens` pertanyen a l'embolcall de
l'Ensemble. Els IDs globals canvien, però la jerarquia de l'instrument i el seu
cablejat són equivalents.
