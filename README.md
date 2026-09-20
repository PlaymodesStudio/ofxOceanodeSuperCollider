# ofxOceanodeSuperCollider

Addon for interfacing [ofxOceanode](https://github.com/PlaymodesStudio/ofxOceanode) and [Supercollider](https://github.com/supercollider/supercollider) via [ofxSupercollider](https://github.com/PlaymodesStudio/ofxSuperCollider)
## Optional timeline integration

Some features need ofxOceanode's timeline, its scheduling layer and its
frame-stepped transport, which currently live on an experimental ofxOceanode
branch. They are detected automatically (`__has_include` on
`ofxOceanodeScheduling.h` and `ofxOceanodeTimeline.h`) and compiled out when
they are missing, so this addon builds against a mainline ofxOceanode.

Compiled out without the timeline:

- the **Timeline Wave Track** node
- timestamped, sample-accurate parameter sends driven by timeline lanes
- non-realtime WAV rendering and the **SC NRT Recorder** node

Everything else, realtime SuperCollider playback included, is unaffected.

To override the detection, define `OFXOCEANODESC_HAS_TIMELINE` as `0` or `1`
in your project (see `src/ofxOceanodeSuperColliderConfig.h`).
