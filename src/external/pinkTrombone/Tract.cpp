//
//  Tract.cpp
//  PinkTrombone - VST3
//
//  Created by Samuel Tarakajian on 9/5/19.
//  Vendored into ofxSantiNodes — no modifications beyond Tract.hpp fixes.
//

#include "Tract.hpp"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "util.h"

typedef struct t_transient {
    int      position;
    sample_t timeAlive;
    sample_t lifeTime;
    sample_t strength;
    sample_t exponent;
    bool     living;
} t_transient;

// ── initializeTractProps ──────────────────────────────────────────────────────
void initializeTractProps(t_tractProps *props, int n) {
    props->n          = n;
    props->bladeStart = BLADE_START;
    props->tipStart   = TIP_START;
    props->lipStart   = LIP_START;
    props->noseLength = NOSE_LENGTH;

    props->bladeStart = (int)floor(props->bladeStart * (sample_t)n / NUM_CONSTRICTIONS);
    props->tipStart   = (int)floor(props->tipStart   * (sample_t)n / NUM_CONSTRICTIONS);
    props->lipStart   = (int)floor(props->lipStart   * (sample_t)n / NUM_CONSTRICTIONS);

    props->tongueIndex    = props->bladeStart;
    props->tongueDiameter = TONGUE_DIAMETER;
    props->tractDiameter  = (sample_t *)calloc(n, sizeof(sample_t));

    props->noseLength  = (int)floor(props->noseLength * (sample_t)props->n / NUM_CONSTRICTIONS);
    props->noseDiameter = (sample_t *)calloc(props->noseLength, sizeof(sample_t));
    props->noseStart   = props->n - props->noseLength + 1;
    props->noseOffset  = NOSE_OFFSET;
}

// ── Constructor ───────────────────────────────────────────────────────────────
Tract::Tract(sample_t sampleRate, sample_t blockTime, t_tractProps *props) :
    lipOutput(0), noseOutput(0),
    glottalReflection(GLOTTAL_REFLECTION),
    lipReflection(LIP_REFLECTION),
    lastObstruction(-1),
    fade(TRACT_FADE),
    movementSpeed(MOVEMENT_SPEED),
    velumTarget(0.01f),
    constrictionIndex(3.0f),
    constrictionDiameter(1.0f)
{
    this->sampleRate    = sampleRate;
    this->blockTime     = blockTime;
    this->transients    = (t_transient *)calloc(MAX_TRANSIENTS, sizeof(t_transient));
    this->transientCount = 0;
    this->tractProps    = props;
    this->init();
}

Tract::~Tract() {
    free(diameter);      free(restDiameter);
    free(targetDiameter); free(newDiameter);
    free(R);  free(L);
    free(reflection);    free(newReflection);
    free(junctionOutputR); free(junctionOutputL);
    free(A);  free(maxAmplitude);
    free(noseR); free(noseL);
    free(noseJunctionOutputR); free(noseJunctionOutputL);
    free(noseReflection);
    free(noseDiameter);  free(noseA);  free(noseMaxAmplitude);
    free(transients);
}

void Tract::init() {
    int n = this->tractProps->n;

    diameter      = (sample_t *)calloc(n, sizeof(sample_t));
    restDiameter  = (sample_t *)calloc(n, sizeof(sample_t));
    targetDiameter = (sample_t *)calloc(n, sizeof(sample_t));
    newDiameter   = (sample_t *)calloc(n, sizeof(sample_t));

    for (int i = 0; i < n; i++) {
        sample_t d = 0;
        if      (i < TRACT_BOUND_A * (sample_t)n - 0.5f) d = TRACT_DIAMETER_A;
        else if (i < TRACT_BOUND_B * (sample_t)n)         d = TRACT_DIAMETER_B;
        else                                               d = TRACT_DIAMETER_C;
        diameter[i] = restDiameter[i] = targetDiameter[i] = newDiameter[i] = d;
    }

    R              = (sample_t *)calloc(n,     sizeof(sample_t));
    L              = (sample_t *)calloc(n,     sizeof(sample_t));
    reflection     = (sample_t *)calloc(n + 1, sizeof(sample_t));
    newReflection  = (sample_t *)calloc(n + 1, sizeof(sample_t));
    junctionOutputR = (sample_t *)calloc(n + 1, sizeof(sample_t));
    junctionOutputL = (sample_t *)calloc(n + 1, sizeof(sample_t));
    A              = (sample_t *)calloc(n,     sizeof(sample_t));
    maxAmplitude   = (sample_t *)calloc(n,     sizeof(sample_t));

    int nl = this->tractProps->noseLength;
    noseR              = (sample_t *)calloc(nl,     sizeof(sample_t));
    noseL              = (sample_t *)calloc(nl,     sizeof(sample_t));
    noseJunctionOutputR = (sample_t *)calloc(nl + 1, sizeof(sample_t));
    noseJunctionOutputL = (sample_t *)calloc(nl + 1, sizeof(sample_t));
    noseReflection     = (sample_t *)calloc(nl + 1, sizeof(sample_t));
    noseDiameter       = (sample_t *)calloc(nl,     sizeof(sample_t));
    noseA              = (sample_t *)calloc(nl,     sizeof(sample_t));
    noseMaxAmplitude   = (sample_t *)calloc(nl,     sizeof(sample_t));

    for (int i = 0; i < nl; i++) {
        sample_t d2 = 2.0f * ((sample_t)i / (sample_t)nl);
        sample_t d = (d2 < 1.0f) ? (0.4f + 1.6f * d2) : (0.5f + 1.5f * (2.0f - d2));
        noseDiameter[i] = fmin(d, 1.9f);
    }

    newReflectionLeft = newReflectionRight = newReflectionNose = 0.0f;
    this->calculateReflections();
    this->calculateNoseReflections();
    noseDiameter[0] = this->velumTarget;

    memcpy(this->tractProps->tractDiameter, diameter, sizeof(sample_t) * n);
    memcpy(this->tractProps->noseDiameter,  noseDiameter, sizeof(sample_t) * nl);
}

// ── Queries ───────────────────────────────────────────────────────────────────
long Tract::getTractIndexCount()    { return this->tractProps->n; }
long Tract::tongueIndexLowerBound() { return this->tractProps->bladeStart + 2; }
long Tract::tongueIndexUpperBound() { return this->tractProps->tipStart   - 3; }

// ── Transients ────────────────────────────────────────────────────────────────
void Tract::addTransient(int position) {
    if (this->transientCount >= MAX_TRANSIENTS) return;
    t_transient *trans = nullptr;
    for (int i = 0; i < MAX_TRANSIENTS; i++) {
        trans = this->transients + i;
        if (!trans->living) break;
    }
    if (trans) {
        trans->position  = position;
        trans->timeAlive = 0;
        trans->lifeTime  = 0.2f;
        trans->strength  = 0.3f;
        trans->exponent  = 200;
        trans->living    = true;
    }
    this->transientCount++;
}

void Tract::processTransients() {
    for (int i = 0; i < this->transientCount; i++) {
        t_transient *trans = this->transients + i;
        sample_t amplitude = trans->strength * pow(2.0f, -trans->exponent * trans->timeAlive);
        this->R[trans->position] += amplitude / 2.0f;
        this->L[trans->position] += amplitude / 2.0f;
        trans->timeAlive += 1.0f / (this->sampleRate * 2.0f);
    }
    for (int i = this->transientCount - 1; i >= 0; i--) {
        t_transient *trans = this->transients + i;
        if (trans->timeAlive > trans->lifeTime) trans->living = false;
    }
}

// ── Turbulence ────────────────────────────────────────────────────────────────
void Tract::addTurbulenceNoise(sample_t turbulenceNoise, sample_t glottalNoiseModulator) {
    if (this->constrictionIndex < 2.0f || this->constrictionIndex > (sample_t)this->tractProps->n) return;
    if (this->constrictionDiameter <= 0.0f) return;
    this->addTurbulenceNoiseAtIndex(0.66f * turbulenceNoise * this->fricativeIntensity,
                                    this->constrictionIndex,
                                    this->constrictionDiameter,
                                    glottalNoiseModulator);
}

void Tract::addTurbulenceNoiseAtIndex(sample_t turbulenceNoise, sample_t index,
                                       sample_t diameter, sample_t glottalNoiseModulator) {
    long     i       = (long)floor(index);
    sample_t delta   = index - (sample_t)i;
    turbulenceNoise *= glottalNoiseModulator;
    sample_t thinness0 = clamp(8.0f * (0.7f - diameter), 0.0f, 1.0f);
    sample_t openness  = clamp(30.0f * (diameter - 0.3f), 0.0f, 1.0f);
    sample_t noise0 = turbulenceNoise * (1.0f - delta) * thinness0 * openness;
    sample_t noise1 = turbulenceNoise * delta           * thinness0 * openness;
    this->R[i + 1] += noise0 / 2.0f;
    this->L[i + 1] += noise0 / 2.0f;
    this->R[i + 2] += noise1 / 2.0f;
    this->L[i + 2] += noise1 / 2.0f;
}

// ── Reflection calculation ────────────────────────────────────────────────────
void Tract::calculateReflections() {
    int n = this->tractProps->n;
    for (int i = 0; i < n; i++) this->A[i] = this->diameter[i] * this->diameter[i];
    for (int i = 1; i < n; i++) {
        this->reflection[i] = this->newReflection[i];
        if (this->A[i] == 0) this->newReflection[i] = 0.999f;
        else this->newReflection[i] = (this->A[i-1] - this->A[i]) / (this->A[i-1] + this->A[i]);
    }
    this->reflectionLeft  = this->newReflectionLeft;
    this->reflectionRight = this->newReflectionRight;
    this->reflectionNose  = this->newReflectionNose;
    int ns = this->tractProps->noseStart;
    sample_t sum = this->A[ns] + this->A[ns + 1] + this->noseA[0];
    this->newReflectionLeft  = (2.0f * this->A[ns]     - sum) / sum;
    this->newReflectionRight = (2.0f * this->A[ns + 1] - sum) / sum;
    this->newReflectionNose  = (2.0f * this->noseA[0]  - sum) / sum;
}

void Tract::calculateNoseReflections() {
    int nl = this->tractProps->noseLength;
    for (int i = 0; i < nl; i++) this->noseA[i] = this->noseDiameter[i] * this->noseDiameter[i];
    for (int i = 1; i < nl; i++)
        this->noseReflection[i] = (this->noseA[i-1] - this->noseA[i]) / (this->noseA[i-1] + this->noseA[i]);
}

// ── finishBlock ───────────────────────────────────────────────────────────────
void Tract::finishBlock() {
    this->reshapeTract(this->blockTime);
    this->calculateReflections();
    memcpy(this->tractProps->tractDiameter, this->diameter,     sizeof(sample_t) * this->tractProps->n);
    memcpy(this->tractProps->noseDiameter,  this->noseDiameter, sizeof(sample_t) * this->tractProps->noseLength);
}

// ── setRestDiameter / setConstriction ─────────────────────────────────────────
void Tract::setRestDiameter(sample_t tongueIndex, sample_t tongueDiameter) {
    this->tractProps->tongueIndex    = tongueIndex;
    this->tractProps->tongueDiameter = tongueDiameter;
    for (long i = this->tractProps->bladeStart; i < this->tractProps->lipStart; i++) {
        sample_t t = 1.1f * (sample_t)M_PI * (sample_t)(tongueIndex - i)
                     / (sample_t)(this->tractProps->tipStart - this->tractProps->bladeStart);
        sample_t fixedTD = 2.f + (tongueDiameter - 2.f) / 1.5f;
        sample_t curve   = (1.5f - fixedTD + 1.7f) * cos(t);
        if (i == this->tractProps->bladeStart - 2 || i == this->tractProps->lipStart - 1) curve *= 0.8f;
        if (i == this->tractProps->bladeStart     || i == this->tractProps->lipStart - 2) curve *= 0.94f;
        this->restDiameter[i] = 1.5f - curve;
    }
    for (long i = 0; i < this->tractProps->n; i++)
        this->targetDiameter[i] = this->restDiameter[i];
}

void Tract::setConstriction(sample_t cindex, sample_t cdiam, sample_t fricIntensity) {
    this->constrictionIndex    = cindex;
    this->constrictionDiameter = cdiam;
    this->fricativeIntensity   = fricIntensity;

    this->velumTarget = 0.01f;
    if (cindex > this->tractProps->noseStart && cdiam < -this->tractProps->noseOffset)
        this->velumTarget = 0.4f;
    if (cdiam < -0.85f - this->tractProps->noseOffset) return;

    sample_t diameter = cdiam - 0.3f;
    if (diameter < 0) diameter = 0;

    long width;
    if      (cindex < 25)                             width = 10;
    else if (cindex >= this->tractProps->tipStart)    width = 5;
    else    width = (long)(10.f - 5.f * (cindex - 25.f)
                    / ((sample_t)this->tractProps->tipStart - 25.f));

    if (cindex >= 2 && cindex < (sample_t)this->tractProps->n && diameter < 3) {
        long intIndex = (long)round(cindex);
        for (long i = -(long)ceil((float)width) - 1; i < width + 1; i++) {
            if (intIndex + i < 0 || intIndex + i >= this->tractProps->n) continue;
            sample_t relpos = fabs((sample_t)(intIndex + i) - cindex) - 0.5f;
            sample_t shrink;
            if      (relpos <= 0)           shrink = 0;
            else if (relpos > (float)width) shrink = 1;
            else shrink = 0.5f * (1.f - cos((float)M_PI * relpos / (float)width));
            if (diameter < this->targetDiameter[intIndex + i])
                this->targetDiameter[intIndex + i] = diameter + (this->targetDiameter[intIndex + i] - diameter) * shrink;
        }
    }
}

// ── reshapeTract ──────────────────────────────────────────────────────────────
void Tract::reshapeTract(sample_t deltaTime) {
    sample_t amount = deltaTime * this->movementSpeed;
    int newLastObstruction = -1;
    int n  = this->tractProps->n;
    int ns = this->tractProps->noseStart;
    int ts = this->tractProps->tipStart;
    for (int i = 0; i < n; i++) {
        if (this->diameter[i] <= 0) newLastObstruction = i;
        sample_t slowReturn;
        if      (i < ns) slowReturn = 0.6f;
        else if (i >= ts) slowReturn = 1.0f;
        else slowReturn = 0.6f + 0.4f * (i - ns) / (sample_t)(ts - ns);
        this->diameter[i] = moveTowards(this->diameter[i], this->targetDiameter[i],
                                        slowReturn * amount, 2.f * amount);
    }
    if (this->lastObstruction > -1 && newLastObstruction == -1 && this->noseA[0] < 0.05f)
        this->addTransient(this->lastObstruction);
    this->lastObstruction = newLastObstruction;

    sample_t amount2 = deltaTime * this->movementSpeed;
    noseDiameter[0] = moveTowards(noseDiameter[0], velumTarget, amount2 * 0.25f, amount2 * 0.1f);
    tractProps->noseDiameter[0] = noseDiameter[0];
    noseA[0] = noseDiameter[0] * noseDiameter[0];
}

// ── runStep ───────────────────────────────────────────────────────────────────
void Tract::runStep(sample_t glottalOutput, sample_t turbulenceNoise,
                    sample_t lambda, sample_t glottalNoiseModulator) {
    bool updateAmplitudes = ((sample_t)rand() / (sample_t)RAND_MAX) < 0.1f;
    int  n  = this->tractProps->n;
    int  nl = this->tractProps->noseLength;
    int  ns = this->tractProps->noseStart;

    this->processTransients();
    this->addTurbulenceNoise(turbulenceNoise, glottalNoiseModulator);

    this->junctionOutputR[0]  = this->L[0] * this->glottalReflection + glottalOutput;
    this->junctionOutputL[n]  = this->R[n - 1] * this->lipReflection;

    for (int i = 1; i < n; i++) {
        sample_t r = this->reflection[i] * (1 - lambda) + this->newReflection[i] * lambda;
        sample_t w = r * (this->R[i - 1] + this->L[i]);
        this->junctionOutputR[i] = this->R[i - 1] - w;
        this->junctionOutputL[i] = this->L[i]     + w;
    }

    // Junction with nose
    {
        sample_t r;
        r = this->newReflectionLeft  * (1 - lambda) + this->reflectionLeft  * lambda;
        this->junctionOutputL[ns] = r * this->R[ns - 1] + (1 + r) * (this->noseL[0] + this->L[ns]);
        r = this->newReflectionRight * (1 - lambda) + this->reflectionRight * lambda;
        this->junctionOutputR[ns] = r * this->L[ns] + (1 + r) * (this->R[ns - 1] + this->noseL[0]);
        r = this->newReflectionNose  * (1 - lambda) + this->reflectionNose  * lambda;
        this->noseJunctionOutputR[0] = r * this->noseL[0] + (1 + r) * (this->L[ns] + this->R[ns - 1]);
    }

    for (int i = 0; i < n; i++) {
        this->R[i] = this->junctionOutputR[i]     * 0.999f;
        this->L[i] = this->junctionOutputL[i + 1] * 0.999f;
        if (updateAmplitudes) {
            sample_t amp = fabs(this->R[i] + this->L[i]);
            if (amp > this->maxAmplitude[i]) this->maxAmplitude[i] = amp;
            else this->maxAmplitude[i] *= 0.999f;
        }
    }
    this->lipOutput = this->R[n - 1];

    // Nose
    this->noseJunctionOutputL[nl] = this->noseR[nl - 1] * this->lipReflection;
    for (int i = 1; i < nl; i++) {
        sample_t w = this->noseReflection[i] * (this->noseR[i - 1] + this->noseL[i]);
        this->noseJunctionOutputR[i] = this->noseR[i - 1] - w;
        this->noseJunctionOutputL[i] = this->noseL[i]     + w;
    }
    for (int i = 0; i < nl; i++) {
        this->noseR[i] = this->noseJunctionOutputR[i]     * this->fade;
        this->noseL[i] = this->noseJunctionOutputL[i + 1] * this->fade;
        if (updateAmplitudes) {
            sample_t amp = fabs(this->noseR[i] + this->noseL[i]);
            if (amp > this->noseMaxAmplitude[i]) this->noseMaxAmplitude[i] = amp;
            else this->noseMaxAmplitude[i] *= 0.999f;
        }
    }
    this->noseOutput = this->noseR[nl - 1];
}
