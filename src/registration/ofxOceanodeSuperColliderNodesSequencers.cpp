//
//  ofxOceanodeSuperColliderNodesSequencers.cpp
//
//  Sequencers: transport-locked boxes and pitch helpers. Part of ofxOceanodeSuperCollider::registerModels, in its own file
//  so the node headers compile in parallel and an edited node header rebuilds
//  only its group. The registry keeps no order, so the grouping is free.
//

#include "../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanode.h"
#include "../ofxOceanodeSuperColliderController.h"
#include "../nodes/sequencing/scTonal.h"
#include "../nodes/boxSynths/metroBox/scMetroBox.h"
#include "../nodes/boxSynths/euclideanBox/scEuclideanBox.h"
#include "../nodes/boxSynths/arpeggiatorBox/scArpeggiatorBox.h"
#include "../nodes/boxSynths/chordBox/scChordBox.h"
#include "../nodes/boxSynths/rhythmBox/scRhythmBox.h"
#include "../nodes/boxSynths/dataBox/scDataBox.h"

namespace ofxOceanodeSuperCollider{

void registerSequencerNodes(ofxOceanode &o){
    auto controller = o.getController<ofxOceanodeSuperColliderController>();

	o.registerModel<scPitch>("SuperCollider/Sequencing");
	o.registerModel<scChord>("SuperCollider/Sequencing");
	o.registerModel<scMetroBox>("SuperCollider/boxSynths");
	o.registerModel<scEuclideanBox>("SuperCollider/boxSynths");
	o.registerModel<scArpeggiatorBox>("SuperCollider/boxSynths");
	o.registerModel<scChordBox>("SuperCollider/boxSynths");
	o.registerModel<scRhythmBox>("SuperCollider/boxSynths", controller->getServers());
	o.registerModel<scDataBox>("SuperCollider/boxSynths", controller->getServers());
}

}
