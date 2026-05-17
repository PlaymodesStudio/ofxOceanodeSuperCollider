//
//  noise.hpp
//  PinkTrombone - VST3
//
//  Created by Samuel Tarakajian on 8/29/19.
//  Vendored into ofxSantiNodes.
//

#ifndef noise_hpp
#define noise_hpp

#include "config.h"

// Seed the permutation table (call once at startup; optional)
void pt_noise_seed(unsigned int s);

// 2-D and 1-D simplex noise in [-1, 1]
sample_t simplex2(sample_t xin, sample_t yin);
sample_t simplex1(sample_t xin);

#endif /* noise_hpp */
