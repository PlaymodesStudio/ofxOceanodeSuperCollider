//
//  ofxOceanodeSuperColliderNodesEffects.cpp
//
//  Effects: audio processors. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "../ofxOceanodeSuperColliderController.h"
#include "../nodes/boxSynths/spectralFilterBox/scSpectralFilterBox.h"
#include "../nodes/effects/scConvolution.h"
#include "../nodes/effects/scRave.h"
#include "../nodes/effects/scVST.h"
#include "../nodes/effects/scGraphicEQ.h"
#include "../nodes/effects/scVelvetReverb.h"
#include "../nodes/effects/scBeatRepeatPro.h"
#include "../nodes/boxSynths/echoBox/scEchoBox.h"

namespace ofxOceanodeSuperCollider{

void registerEffectNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scSpectralFilterBox>("SuperCollider/boxSynths");
	o.registerModel<scConvolution>("SuperCollider/Effects");
	o.registerModel<scVST>("SuperCollider/Effects");
	o.registerModel<scGraphicEQ>("SuperCollider/Effects");
	o.registerModel<scVelvetReverb>("SuperCollider/Effects");
	o.registerModel<scBeatRepeatPro>("SuperCollider/Effect/Time/Looping", controller->getServers());
	o.registerModel<scEchoBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scRave>("SuperCollider/Effects", controller->getServers());
}

}
