//
//  ofxOceanodeSuperColliderNodesSynths.cpp
//
//  Synths: the instruments and sound generators. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "ofxOceanodeSuperColliderController.h"
#include "nodes/scWavetable.h"
#include "nodes/scAdditiveBox.h"
#include "nodes/scPulsarBox.h"
#include "nodes/scFM7.h"
#include "nodes/scFMBox.h"
#include "nodes/scFM7Drone.h"
#include "nodes/scDynGenNode.h"
#include "nodes/scDynGenPolyNode.h"
#include "nodes/pinkTromboneNode.h"
#include "nodes/scGrainBoxPoly.h"
#include "nodes/scSampleBox.h"
#include "nodes/scPolyComb.h"
#include "nodes/scAnalogBox.h"
#include "nodes/scAliasingBox.h"
#include "nodes/scPhysicalBox.h"
#include "nodes/scSteamPipeBox.h"
#include "nodes/scSlicerBox.h"

namespace ofxOceanodeSuperCollider{

void registerSynthNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scWavetable>("SuperCollider", controller->getServers());
	o.registerModel<scAdditiveBox>("SuperCollider", controller->getServers());
	o.registerModel<scPulsarBox>("SuperCollider", controller->getServers());
	o.registerModel<scFMBox>("SuperCollider", controller->getServers());
	o.registerModel<scFM7>("SuperCollider");
	o.registerModel<scFM7Drone>("SuperCollider");
	o.registerModel<scDynGenNode>("SuperCollider");
	o.registerModel<scDynGenPolyNode>("SuperCollider");
	o.registerModel<pinkTromboneNode>("SuperCollider");
	o.registerModel<scGrainBoxPoly>("SuperCollider", controller->getServers());
	o.registerModel<scSampleBox>("SuperCollider", controller->getServers());
	o.registerModel<scSlicerBox>("SuperCollider", controller->getServers());
	o.registerModel<scAnalogBox>("SuperCollider", controller->getServers());
	o.registerModel<scAliasingBox>("SuperCollider", controller->getServers());
	o.registerModel<scPhysicalBox>("SuperCollider", controller->getServers());
	o.registerModel<scSteamPipeBox>("SuperCollider", controller->getServers());
	o.registerModel<scPolyComb>("SuperCollider", controller->getServers());
}

}
