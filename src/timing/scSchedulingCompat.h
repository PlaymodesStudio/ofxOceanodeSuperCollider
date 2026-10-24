//
//  scSchedulingCompat.h
//  ofxOceanodeSuperCollider
//

#ifndef scSchedulingCompat_h
#define scSchedulingCompat_h

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

class ofxOceanodeAbstractParameter;

#include "ofxOceanodeScheduling.h"

// Adapter for the core scheduling interface. Backends register regardless of
// whether the optional timeline addon is present. Core scheduling producers
// can dispatch timestamped parameter events through this same interface.
namespace scScheduling {

// (dueSteadyTimeUs, text value) -> true when the backend scheduled it.
using Handler = std::function<bool(uint64_t, const std::string&)>;

inline bool isBackendSendSuppressed(){
	return ofxOceanodeScheduling::isBackendSendSuppressed();
}

inline void registerParameterTarget(const ofxOceanodeAbstractParameter* parameter,
									const void* owner, Handler handler){
	ofxOceanodeScheduling::registerParameterTarget(parameter, owner,
		[handler = std::move(handler)](const ofxOceanodeScheduledParameterEvent& event){
			return handler(event.dueSteadyTimeUs, event.value);
		});
}

inline void unregisterParameterTarget(const ofxOceanodeAbstractParameter* parameter){
	ofxOceanodeScheduling::unregisterParameterTarget(parameter);
}

inline void unregisterOwner(const void* owner){
	ofxOceanodeScheduling::unregisterOwner(owner);
}

}

#endif /* scSchedulingCompat_h */
