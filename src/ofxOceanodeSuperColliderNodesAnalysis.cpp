//
//  ofxOceanodeSuperColliderNodesAnalysis.cpp
//
//  Analysis: trackers, meters and scopes. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "nodes/scBufferScope.h"
#include "nodes/scPitchTracker.h"
#include "nodes/scBeatTracker.h"
#include "nodes/scWavescope.h"
#include "nodes/scVUMeter.h"
#include "nodes/scFastEnvelopeFollower.h"
#include "nodes/scFFT.h"
#include "nodes/scFFTHD.h"
#include "nodes/scSpectrogram.h"
#include "nodes/scLissajous.h"
#include "nodes/scMorphOscScope.h"

namespace ofxOceanodeSuperCollider{

void registerAnalysisNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scPitchTracker>("SuperCollider", controller->getServers());
	o.registerModel<scBeatTracker>("SuperCollider", controller->getServers());
	o.registerModel<scFastEnvelopeFollower>("SuperCollider");
	o.registerModel<scVUMeter>("SuperCollider");
	o.registerModel<scBufferscope>("SuperCollider", controller->getServers());
	o.registerModel<scWavescope>("SuperCollider", controller->getServers());
	o.registerModel<scFFT>("SuperCollider", controller->getServers());
	o.registerModel<scFFTHD>("SuperCollider", controller->getServers());
	o.registerModel<scSpectrogram>("SuperCollider", controller->getServers());
	o.registerModel<scLissajous>("SuperCollider", controller->getServers());
	o.registerModel<scMorphOscScope>("SuperCollider");
}

}
