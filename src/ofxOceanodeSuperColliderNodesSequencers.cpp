//
//  ofxOceanodeSuperColliderNodesSequencers.cpp
//
//  Sequencers: transport-locked boxes and pitch helpers. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "nodes/scTonal.h"
#include "nodes/scMetroBox.h"
#include "nodes/scEuclideanBox.h"
#include "nodes/scArpeggiatorBox.h"
#include "nodes/scChordBox.h"
#include "nodes/scRhythmBox.h"
#include "nodes/scDataBox.h"

namespace ofxOceanodeSuperCollider{

void registerSequencerNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scPitch>("SuperCollider");
	o.registerModel<scChord>("SuperCollider");
#if OFXOCEANODESC_HAS_TRANSPORT
	o.registerModel<scMetroBox>("SuperCollider");
	o.registerModel<scEuclideanBox>("SuperCollider");
	o.registerModel<scArpeggiatorBox>("SuperCollider");
	o.registerModel<scChordBox>("SuperCollider");
#endif
	o.registerModel<scRhythmBox>("SuperCollider", controller->getServers());
	o.registerModel<scDataBox>("SuperCollider", controller->getServers());
}

}
