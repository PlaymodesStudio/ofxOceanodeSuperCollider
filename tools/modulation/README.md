# Shared LFO outputs

Right-click the bars at the top of an LFO card and choose **Publish LFO output
to Node GUI**. The `LFO 1 Out` (etc.) output carries a float vector in 0..1,
one value per voice. AdditiveBox exports the displayed partial vector when
that LFO is running across partials. SampleBox pads and GrainBox engines
have separately named outputs.

The outputs keep updating when the editor is closed. Server values are polled
at approximately 30 Hz and forwarded on node updates; these are Oceanode
control values, rather than an audio-rate cable. A removed/inactive LFO emits
zeros. Disconnect an output before unpublishing it.

Publication is saved with the patch and restored before connections, separately
from sound snapshots. Removing a pad or engine removes its output pins too.
No output parameters or extra server polling are created until publication.

Run `python3 tools/modulation/lfo_outputs_test.py` for lifecycle, persistence,
hidden-window polling, vector sizing and connection protection checks.
