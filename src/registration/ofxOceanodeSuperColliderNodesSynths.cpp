//
//  ofxOceanodeSuperColliderNodesSynths.cpp
//
//  Synths: the instruments and sound generators. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "../ofxOceanodeSuperColliderController.h"
#include "../nodes/boxSynths/wavetableBox/scWavetableBox.h"
#include "../nodes/boxSynths/additiveBox/scAdditiveBox.h"
#include "../nodes/boxSynths/pulsarBox/scPulsarBox.h"
#include "../nodes/boxSynths/fmBox/scFMBox.h"
#include "../nodes/generators/scFM7.h"
#include "../nodes/generators/scFM7Drone.h"
#include "../nodes/generators/scDynGenNode.h"
#include "../nodes/generators/pinkTromboneNode.h"
#include "../nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.h"
#include "../nodes/boxSynths/sampleBox/scSampleBox.h"
#include "../nodes/boxSynths/polyComb/scPolyComb.h"
#include "../nodes/boxSynths/analogBox/scAnalogBox.h"
#include "../nodes/boxSynths/aliasingBox/scAliasingBox.h"
#include "../nodes/boxSynths/physicalBox/scPhysicalBox.h"
#include "../nodes/boxSynths/steamPipeBox/scSteamPipeBox.h"
#include "../nodes/boxSynths/slicerBox/scSlicerBox.h"

namespace ofxOceanodeSuperCollider{

void registerSynthNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scWavetableBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scAdditiveBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scPulsarBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scFM7>("SuperCollider/Generators");
	o.registerModel<scFM7Drone>("SuperCollider/Generators");
	o.registerModel<scFMBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scDynGenNode>("SuperCollider/Generators");
	o.registerModel<pinkTromboneNode>("SuperCollider/Generators");
	o.registerModel<scGrainBoxPoly>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scSampleBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scSlicerBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scAnalogBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scAliasingBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scPhysicalBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scSteamPipeBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scPolyComb>("SuperCollider/boxSynths", controller->getServers());
}

}
