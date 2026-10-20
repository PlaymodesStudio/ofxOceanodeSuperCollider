# Oscillators Pitch Map reference

Read-only extraction of the user-supplied Reaktor macro with the repository's
ReaktorPatchExtractor. Source SHA-256: `2e4d4b0f53dfe4377614b7e43d8df3c6e107de123d2927b4089f64f1a4f85b8f`.
The extractor resolved 255/255 primary connections, three Core cells and
91 Core connections, without warnings.

`sort V#3`: the index enters Modulo #9; its quotient goes through multiply #10
and add #12, while its remainder selects an input chord note through the
From Voice / To V chain. This is octave repetition of a finite pitch set:
`scale[floor(degree) mod N] + 12 * floor(degree / N)`. C++ handles negative
indices using floor division, so descending scales work across octave zero.

`Pitch Index#39` contains Comb, invert, symm, rnd dev, Q, pow and deterministic
branches. This implementation shares one index pipeline across User and
Harmonic series: existing linear/circular layout and bend, inversion,
periodic symmetry, comb folding, quantization and stable seeded scatter.
This is an adaptation, not a bit-identical port of the deterministic Core
cells or the macro's stateful randomizer. Scatter is stable per voice and
seed, so adding voices does not redraw existing random offsets.

The original macro remains untouched. No Reaktor runtime is required.
