#ifndef ofxOceanodeSuperColliderConfig_h
#define ofxOceanodeSuperColliderConfig_h

#define MAX_NODE_CHANNELS 128
#define SYNTHDEF_DIRECTORY "Supercollider/Synthdefs"

// ---------------------------------------------------------------------------
// Optional ofxOceanode timeline integration
//
// The timeline, its scheduling layer and the frame-stepped transport only
// exist on ofxOceanode's experimental timeline branch. Everything in this
// addon that needs them is compiled out otherwise, so the addon still builds
// against a mainline ofxOceanode.
//
// Detection is automatic: both headers are on the include path exactly when
// the branch provides them. Define OFXOCEANODESC_HAS_TIMELINE to 0 or 1 in
// your project to override.
//
// What is lost when the timeline is absent:
//   - the timelineWaveTrack node
//   - timestamped (sample-accurate) parameter sends from timeline lanes
//   - non-realtime WAV rendering and the SC NRT Recorder node
// Everything else, including realtime SuperCollider playback, is unaffected.
// ---------------------------------------------------------------------------
#ifndef OFXOCEANODESC_HAS_TIMELINE
	#if defined(__has_include)
		#if __has_include("ofxOceanodeScheduling.h") && __has_include("ofxOceanodeTimeline.h")
			#define OFXOCEANODESC_HAS_TIMELINE 1
		#else
			#define OFXOCEANODESC_HAS_TIMELINE 0
		#endif
	#else
		// No __has_include (pre-C++17 MSVC): assume the mainline branch.
		#define OFXOCEANODESC_HAS_TIMELINE 0
	#endif
#endif

// ---------------------------------------------------------------------------
// Optional ofxOceanode global transport
//
// Nodes with a step/phase clock (RhythmBox, Polyphonic Arpeggiator, GrainBox)
// can follow ofxOceanode's global transport ("Sync To Transport"). The
// transport only exists on branches that define OFX_OCEANODE_HAS_GLOBAL_TRANSPORT
// in ofxOceanodeTransport.h; without it those options are compiled out and
// the nodes run free, as before. Define OFXOCEANODESC_HAS_TRANSPORT to 0 or 1
// in your project to override.
// ---------------------------------------------------------------------------
#ifndef OFXOCEANODESC_HAS_TRANSPORT
	#if defined(__has_include)
		#if __has_include("ofxOceanodeTransport.h")
			#include "ofxOceanodeTransport.h"
		#endif
	#endif
	#if defined(OFX_OCEANODE_HAS_GLOBAL_TRANSPORT)
		#define OFXOCEANODESC_HAS_TRANSPORT 1
	#else
		#define OFXOCEANODESC_HAS_TRANSPORT 0
	#endif
#endif

#endif /* ofxOceanodeSuperCollider_h */
