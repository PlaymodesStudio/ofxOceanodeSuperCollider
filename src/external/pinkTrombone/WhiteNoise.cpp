//
//  WhiteNoise.cpp
//  PinkTrombone - All
//
//  Created by Samuel Tarakajian on 8/30/19.
//  Vendored into ofxSantiNodes.
//

#include "WhiteNoise.hpp"
#include <stdlib.h>

WhiteNoise::WhiteNoise(long sampleLength) : index(0), size(sampleLength) {
    buffer = new sample_t[size];
    for (long i = 0; i < size; i++) {
        buffer[i] = (sample_t)rand() / (sample_t)RAND_MAX * 2.0f - 1.0f;
    }
}

WhiteNoise::~WhiteNoise() {
    delete[] buffer;
}

sample_t WhiteNoise::runStep() {
    sample_t s = buffer[index];
    index = (index + 1) % size;
    return s;
}
