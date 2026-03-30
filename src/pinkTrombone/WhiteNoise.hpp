//
//  WhiteNoise.hpp
//  PinkTrombone - All
//
//  Created by Samuel Tarakajian on 8/30/19.
//  Vendored into ofxSantiNodes — no modifications.
//

#ifndef WhiteNoise_hpp
#define WhiteNoise_hpp

#include "config.h"

class WhiteNoise {
public:
    WhiteNoise(long sampleLength);
    ~WhiteNoise();
    sample_t runStep();
private:
    long      index;
    sample_t *buffer;
    long      size;
};

#endif /* WhiteNoise_hpp */
