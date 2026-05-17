//
//  Tract.hpp
//  PinkTrombone - VST3
//
//  Created by Samuel Tarakajian on 9/5/19.
//  Vendored into ofxSantiNodes.
//  Changes vs. original:
//    • Removed JUCE dependency (JuceHeader.h not needed for DSP code)
//    • runStep 4th arg is sample_t glottalNoiseModulator (matches Tract.cpp)
//    • addTurbulenceNoise/addTurbulenceNoiseAtIndex signatures updated to match .cpp
//

#ifndef Tract_hpp
#define Tract_hpp

#include "Glottis.hpp"
#include "config.h"

struct t_transient;

typedef struct t_tractProps {
    int      n;
    int      lipStart;
    int      bladeStart;
    int      tipStart;
    int      noseStart;
    int      noseLength;
    sample_t noseOffset;
    sample_t tongueIndex;
    sample_t tongueDiameter;
    sample_t *noseDiameter;
    sample_t *tractDiameter;
} t_tractProps;

void initializeTractProps(t_tractProps *props, int n);

class Tract {
public:
    Tract(sample_t sampleRate, sample_t blockTime, t_tractProps *p);
    ~Tract();

    // blockTime = blockSize / sampleRate (used by reshapeTract each block)
    // 4th arg: glottalNoiseModulator (NOT Glottis*) — this is what .cpp expects
    void runStep(sample_t glottalOutput, sample_t turbulenceNoise,
                 sample_t lambda, sample_t glottalNoiseModulator);
    void finishBlock();

    void setRestDiameter(sample_t tongueIndex, sample_t tongueDiameter);
    void setConstriction(sample_t cindex, sample_t cdiam, sample_t fricativeIntensity);

    sample_t lipOutput;
    sample_t noseOutput;

    long getTractIndexCount();
    long tongueIndexLowerBound();
    long tongueIndexUpperBound();

private:
    void init();
    void addTransient(int position);
    void addTurbulenceNoise(sample_t turbulenceNoise, sample_t glottalNoiseModulator);
    void addTurbulenceNoiseAtIndex(sample_t turbulenceNoise, sample_t index,
                                   sample_t diameter, sample_t glottalNoiseModulator);
    void calculateReflections();
    void calculateNoseReflections();
    void processTransients();
    void reshapeTract(sample_t deltaTime);

    sample_t  sampleRate, blockTime;
    t_tractProps *tractProps;

    sample_t  glottalReflection;
    sample_t  lipReflection;
    int       lastObstruction;
    sample_t  fade;
    sample_t  movementSpeed;
    sample_t  velumTarget;
    t_transient *transients;
    int          transientCount;

    sample_t *diameter, *restDiameter, *targetDiameter, *newDiameter;
    sample_t *R, *L, *reflection, *newReflection;
    sample_t *junctionOutputR, *junctionOutputL;
    sample_t *A, *maxAmplitude;

    sample_t *noseR, *noseL;
    sample_t *noseJunctionOutputR, *noseJunctionOutputL;
    sample_t *noseReflection;
    sample_t *noseDiameter, *noseA, *noseMaxAmplitude;

    sample_t reflectionLeft,    reflectionRight,    reflectionNose;
    sample_t newReflectionLeft, newReflectionRight, newReflectionNose;

    sample_t constrictionIndex;
    sample_t constrictionDiameter;
    sample_t fricativeIntensity = 0.0f;
};

#endif /* Tract_hpp */
