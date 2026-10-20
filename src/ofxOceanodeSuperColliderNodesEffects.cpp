//
//  ofxOceanodeSuperColliderNodesEffects.cpp
//
//  Effects: audio processors. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "nodes/scSpectralFilterBox.h"
#include "nodes/scConvolution.h"
#include "nodes/scRave.h"
#include "nodes/scVST.h"
#include "nodes/scGraphicEQ.h"
#include "nodes/scVelvetReverb.h"
#include "nodes/scBeatRepeatPro.h"
#include "nodes/scEchoBox.h"

namespace ofxOceanodeSuperCollider{

void registerEffectNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scSpectralFilterBox>("SuperCollider");
	o.registerModel<scConvolution>("SuperCollider");
	o.registerModel<scVST>("SuperCollider");
	o.registerModel<scGraphicEQ>("SuperCollider");
	o.registerModel<scVelvetReverb>("SuperCollider");
	o.registerModel<scBeatRepeatPro>("SuperCollider/Effect/Time/Looping", controller->getServers());
	o.registerModel<scEchoBox>("SuperCollider", controller->getServers());
	o.registerModel<scRave>("SuperCollider", controller->getServers());
}

}
