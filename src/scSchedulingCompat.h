//
//  scSchedulingCompat.h
//  ofxOceanodeSuperCollider
//

#ifndef scSchedulingCompat_h
#define scSchedulingCompat_h

#include "ofxOceanodeSuperColliderConfig.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

class ofxOceanodeAbstractParameter;

#if OFXOCEANODESC_HAS_TIMELINE
	#include "ofxOceanodeScheduling.h"
#endif

// Thin shim over ofxOceanodeScheduling, which only exists on ofxOceanode's
// timeline branch. Without it every call here is a no-op and the backend keeps
// sending on the frame clock, exactly as it did before scheduling existed.
//
// The handler takes plain arguments rather than ofxOceanodeScheduledParameterEvent
// so call sites need no conditional compilation of their own.
namespace scScheduling {

// (dueSteadyTimeUs, text value) -> true when the backend scheduled it.
using Handler = std::function<bool(uint64_t, const std::string&)>;

inline bool isBackendSendSuppressed(){
#if OFXOCEANODESC_HAS_TIMELINE
	return ofxOceanodeScheduling::isBackendSendSuppressed();
#else
	return false;
#endif
}

inline void registerParameterTarget(const ofxOceanodeAbstractParameter* parameter,
									const void* owner, Handler handler){
#if OFXOCEANODESC_HAS_TIMELINE
	ofxOceanodeScheduling::registerParameterTarget(parameter, owner,
		[handler = std::move(handler)](const ofxOceanodeScheduledParameterEvent& event){
			return handler(event.dueSteadyTimeUs, event.value);
		});
#else
	(void)parameter;
	(void)owner;
	(void)handler;
#endif
}

inline void unregisterOwner(const void* owner){
#if OFXOCEANODESC_HAS_TIMELINE
	ofxOceanodeScheduling::unregisterOwner(owner);
#else
	(void)owner;
#endif
}

}

#endif /* scSchedulingCompat_h */
