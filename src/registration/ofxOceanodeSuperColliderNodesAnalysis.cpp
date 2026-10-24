//
//  ofxOceanodeSuperColliderNodesAnalysis.cpp
//
//  Analysis: trackers, meters and scopes. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "../ofxOceanodeSuperColliderController.h"
#include "../nodes/analysis/scBufferScope.h"
#include "../nodes/analysis/scPitchTracker.h"
#include "../nodes/analysis/scBeatTracker.h"
#include "../nodes/analysis/scWavescope.h"
#include "../nodes/analysis/scVUMeter.h"
#include "../nodes/analysis/scFastEnvelopeFollower.h"
#include "../nodes/analysis/scFFT.h"
#include "../nodes/analysis/scFFTHD.h"
#include "../nodes/analysis/scSpectrogram.h"
#include "../nodes/analysis/scLissajous.h"
#include "../nodes/analysis/scMorphOscScope.h"

namespace ofxOceanodeSuperCollider{

void registerAnalysisNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scPitchTracker>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scBeatTracker>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scFastEnvelopeFollower>("SuperCollider/Analysis");
	o.registerModel<scVUMeter>("SuperCollider/Analysis");
	o.registerModel<scBufferscope>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scWavescope>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scFFT>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scFFTHD>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scSpectrogram>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scLissajous>("SuperCollider/Analysis", controller->getServers());
	o.registerModel<scMorphOscScope>("SuperCollider/Analysis");
}

}
