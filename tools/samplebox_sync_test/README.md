# SampleBox Sync To Transport render test

Offline (scsynth -N) renders of a synced, looping pad reading a sine buffer.
The output's phase is the read position.

    python3 anchor_test.py <work folder> <SampleBoxPad_1_1_R.scsyndef> <tag>
    python3 start_test.py  <work folder> <SampleBoxPad_1_1_R.scsyndef>

- `anchor_test.py`: noise of the position (SNR per segment) and its step at
  each 4-beat anchor, with the anchor's beat high (1504) or wrapped at Fit, a
  small seek, and anchors on the block grid. The SNR of a segment that holds a
  glide is low by construction: read the `grid` cases for the noise.
- `start_test.py`: transport start, stop, seek, start: the position right
  after each start, against the transport's (within a block).
