//
//  ofxOceanodeSuperColliderNodesUtils.cpp
//
//  Utils: buffers, I/O, mixing, routing, recording and the server. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "../ofxOceanodeSuperColliderController.h"
#include "../nodes/routing/scOutput.h"
#include "../nodes/buffers/scBuffer.h"
#include "../nodes/recording/scRecbufHold.h"
#include "../nodes/system/scFreezer.h"
#include "../nodes/buffers/scCustomBuffer.h"
#include "../nodes/buffers/scBufferAllocator.h"
#include "../nodes/system/scInfo.h"
#include "../nodes/analysis/scA2K.h"
#include "../nodes/routing/scPolyMixer.h"
#include "../nodes/routing/scMixerPro.h"
#include "../nodes/routing/scChannelRouterMatrix.h"
#if OFXOCEANODESC_HAS_TIMELINE
#include "../nodes/timeline/timelineWaveTrack.h"
#endif
#include "../nodes/buffers/scBufferBrowser.h"
#include "../nodes/recording/scMultiTrackRecorder.h"
#include "../nodes/synthdefs/scCodeNode.h"
#include "../nodes/recording/scNRTRecorder.h"

namespace ofxOceanodeSuperCollider{

void registerUtilNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scInfo>("SuperCollider/System", ofxOceanodeNodeRegistrationFlags::Sink, controller->getServers());
	o.registerModel<scFreezer>("SuperCollider/System");
	o.registerModel<scBuffer>("SuperCollider/Buffers", controller->getServers());
	o.registerModel<scCustomBuffer>("SuperCollider/Buffers", controller->getServers());
	o.registerModel<scBufferAllocator>("SuperCollider/Buffers", ofxOceanodeNodeRegistrationFlags::Sink, controller->getServers());
	o.registerModel<scBufferBrowser>("SuperCollider/Buffers", ofxOceanodeNodeRegistrationFlags::Sink, controller->getServers());
	o.registerModel<scRecbufHold>("SuperCollider/Recording", ofxOceanodeNodeRegistrationFlags::Sink, controller->getServers());
	o.registerModel<scA2k>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scOutput>("SuperCollider/Routing", controller->getServers());
	o.registerModel<scPolyMixer>("SuperCollider/Routing");
	o.registerModel<scMixerPro>("SuperCollider/Routing", controller->getServers());
	o.registerModel<scChannelRouterMatrix>("SuperCollider/Routing");
#if OFXOCEANODESC_HAS_TIMELINE
	o.registerModel<timelineWaveTrack>("SuperCollider/Timeline", controller->getServers());
#endif
	o.registerModel<scCodeNode>("SuperCollider/SynthDefs", controller->getServers());
	o.registerModel<scMultiTrackRecorder>("SuperCollider/Recording", ofxOceanodeNodeRegistrationFlags::Sink, controller->getServers());
	o.registerModel<scNRTRecorder>("SuperCollider/Recording", ofxOceanodeNodeRegistrationFlags::Sink, controller.get());
}

}
