Run `python3 tools/rhythmbox_test/run.py` for sanitizer-backed checks of the production sample embedding, graph lifecycle, transport anchor sender and legacy preset publication methods. IO doubles replace SuperCollider connections; the tested method bodies are extracted from the current source.

Run `python3 tools/rhythmbox_test/render.py` for offline audio renders of the real RhythmBox Click SynthDef. It checks realtime and frame-stepped transport anchors through loops, tempo changes and stops, and verifies that node-local Play/Reset does not disturb transport sync. It requires the bundled sclang and a SuperCollider installation at `/Applications/SuperCollider.app`. Compilation and renders use a temporary directory and do not update the repository's compiled SynthDefs.

The shared follower has additional coverage in `python3 tools/transport_sync_test/run.py`.
