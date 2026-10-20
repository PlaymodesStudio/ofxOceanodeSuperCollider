//
//  ofxOceanodeSuperCollider.h
//
//
//  Created by Eduard Frigola Bagué on 14/01/2022.
//

#ifndef ofxOceanodeSuperCollider_h
#define ofxOceanodeSuperCollider_h

// Declarations only: the node headers and the bodies live in ofxOceanodeSuperCollider.cpp, so
// including this header does not compile every node of the addon again.

class ofxOceanode;

namespace ofxOceanodeSuperCollider{

void refreshSynthdefModels(ofxOceanode &o);
void registerModels(ofxOceanode &o);
void registerType(ofxOceanode &o);
void registerScope(ofxOceanode &o);
void registerCollection(ofxOceanode &o);
void setup(ofxOceanode &o);
void kill(ofxOceanode& o);

}

#endif /* ofxOceanodeSuperCollider_h */
