//
//  ofxOceanodeSuperColliderNodesUtils.cpp
//
//  Utils: buffers, I/O, mixing, routing, recording and the server. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "scOutput.h"
#include "nodes/scBuffer.h"
#include "nodes/scRecbufHold.h"
#include "nodes/scFreezer.h"
#include "nodes/scCustomBuffer.h"
#include "nodes/scBufferAllocator.h"
#include "nodes/scInfo.h"
#include "nodes/scA2K.h"
#include "nodes/scPolyMixer.h"
#include "nodes/scMixerPro.h"
#include "nodes/scChannelRouterMatrix.h"
#if OFXOCEANODESC_HAS_TIMELINE
#include "nodes/timelineWaveTrack.h"
#endif
#include "nodes/scBufferBrowser.h"
#include "nodes/scMultiTrackRecorder.h"
#include "nodes/scCodeNode.h"
#include "nodes/scNRTRecorder.h"

namespace ofxOceanodeSuperCollider{

void registerUtilNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scInfo>("SuperCollider", controller->getServers());
	o.registerModel<scFreezer>("SuperCollider");
	o.registerModel<scBuffer>("SuperCollider", controller->getServers());
	o.registerModel<scCustomBuffer>("SuperCollider", controller->getServers());
	o.registerModel<scBufferAllocator>("SuperCollider", controller->getServers());
	o.registerModel<scBufferBrowser>("SuperCollider", controller->getServers());
	o.registerModel<scRecbufHold>("SuperCollider", controller->getServers());
	o.registerModel<scA2k>("SuperCollider", controller->getServers());
	o.registerModel<scOutput>("SuperCollider", controller->getServers());
	o.registerModel<scPolyMixer>("SuperCollider");
	o.registerModel<scMixerPro>("SuperCollider", controller->getServers());
	o.registerModel<scChannelRouterMatrix>("SuperCollider");
#if OFXOCEANODESC_HAS_TIMELINE
	o.registerModel<timelineWaveTrack>("SuperCollider", controller->getServers());
#endif
	o.registerModel<scCodeNode>("SuperCollider", controller->getServers());
	o.registerModel<scMultiTrackRecorder>("SuperCollider", controller->getServers());
	o.registerModel<scNRTRecorder>("SuperCollider", controller.get());
}

}
