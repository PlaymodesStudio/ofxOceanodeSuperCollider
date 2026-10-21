#ifndef ofxOceanodeSuperColliderConfig_h
#define ofxOceanodeSuperColliderConfig_h

#define MAX_NODE_CHANNELS 128
#define SYNTHDEF_DIRECTORY "Supercollider/Synthdefs"

// Transport, frame stepping, scheduling and instrument metadata are core APIs.
// Only clip-specific integrations require the optional ofxOceanodeTimeline
// addon. Its addon_config.mk exports this capability for the entire build;
// header discovery alone does not mean the addon is linked.
#ifndef OFXOCEANODESC_HAS_TIMELINE
    #if defined(OFX_OCEANODE_HAS_TIMELINE) && OFX_OCEANODE_HAS_TIMELINE
        #define OFXOCEANODESC_HAS_TIMELINE 1
    #else
        #define OFXOCEANODESC_HAS_TIMELINE 0
    #endif
#endif

#endif /* ofxOceanodeSuperColliderConfig_h */
