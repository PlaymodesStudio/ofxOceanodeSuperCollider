//
//  Glottis.cpp
//  PinkTrombone - VST3
//
//  Created by Samuel Tarakajian on 8/28/19.
//  Vendored into ofxSantiNodes — no modifications.
//

#include "Glottis.hpp"
#include <math.h>
#include "noise.hpp"
#include "util.h"

Glottis::Glottis(double sampleRate) :
    timeInWaveform(0),
    oldFrequency(140), newFrequency(140), smoothFrequency(140), targetFrequency(140),
    oldTenseness(0.6), newTenseness(0.6), targetTenseness(0.6),
    totalTime(0.0),
    intensity(0), loudness(1),
    vibratoAmount(VIBRATO_AMOUNT), vibratoFrequency(VIBRATO_FREQUENCY),
    autoWobble(false), isTouched(false), alwaysVoice(true)
{
    this->sampleRate = (sample_t)sampleRate;
    this->setupWaveform(0);
}

Glottis::~Glottis() {}

void Glottis::setupWaveform(sample_t lambda) {
    this->frequency  = this->oldFrequency * (1 - lambda) + this->newFrequency * lambda;
    sample_t tenseness = this->oldTenseness * (1 - lambda) + this->newTenseness * lambda;
    this->Rd = 3.f * (1.f - tenseness);
    this->waveformLength = 1.f / this->frequency;

    sample_t Rd = this->Rd;
    if (Rd < 0.5f) Rd = 0.5f;
    if (Rd > 2.7f) Rd = 2.7f;

    sample_t Ra = -0.01f + 0.048f * Rd;
    sample_t Rk = 0.224f + 0.118f * Rd;
    sample_t Rg = (Rk / 4.f) * (0.5f + 1.2f * Rk) / (0.11f * Rd - Ra * (0.5f + 1.2f * Rk));

    sample_t Ta = Ra;
    sample_t Tp = 1.f / (2.f * Rg);
    sample_t Te = Tp + Tp * Rk;

    sample_t epsilon = 1.f / Ta;
    sample_t shift   = exp(-epsilon * (1.f - Te));
    sample_t Delta   = 1.f - shift;

    sample_t RHSIntegral = (1.f / epsilon) * (shift - 1.f) + (1.f - Te) * shift;
    RHSIntegral /= Delta;

    sample_t totalLowerIntegral = -(Te - Tp) / 2.f + RHSIntegral;
    sample_t totalUpperIntegral = -totalLowerIntegral;

    sample_t omega = (sample_t)M_PI / Tp;
    sample_t s     = sin(omega * Te);
    sample_t y     = -(sample_t)M_PI * s * totalUpperIntegral / (Tp * 2.f);
    sample_t z     = log(y);
    sample_t alpha = z / (Tp / 2.f - Te);
    sample_t E0    = -1.f / (s * exp(alpha * Te));

    this->alpha   = alpha;
    this->E0      = E0;
    this->epsilon = epsilon;
    this->shift   = shift;
    this->Delta   = Delta;
    this->Te      = Te;
    this->omega   = omega;
}

sample_t Glottis::getNoiseModulator() {
    sample_t voiced = 0.1f + 0.2f * fmax(0.f, sin((float)M_PI * 2.f * this->timeInWaveform / this->waveformLength));
    return this->targetTenseness * this->intensity * voiced
           + (1.f - this->targetTenseness * this->intensity) * 0.3f;
}

void Glottis::setTargetFrequency(sample_t frequency) { this->targetFrequency = frequency; }
void Glottis::setTargetTenseness(sample_t tenseness)  { this->targetTenseness = tenseness; }

void Glottis::finishBlock() {
    sample_t vibrato = 0;
    vibrato += this->vibratoAmount * sin(2.f * (sample_t)M_PI * this->totalTime * this->vibratoFrequency);
    vibrato += 0.02f * simplex1(this->totalTime * 4.07f);
    vibrato += 0.04f * simplex1(this->totalTime * 2.15f);
    if (this->autoWobble) {
        vibrato += 0.2f * simplex1(this->totalTime * 0.98f);
        vibrato += 0.4f * simplex1(this->totalTime * 0.5f);
    }
    if (this->targetFrequency > this->smoothFrequency)
        this->smoothFrequency = fmin(this->smoothFrequency * 1.1f, this->targetFrequency);
    if (this->targetFrequency < this->smoothFrequency)
        this->smoothFrequency = fmax(this->smoothFrequency / 1.1f, this->targetFrequency);

    this->oldFrequency  = this->newFrequency;
    this->newFrequency  = this->smoothFrequency * (1.f + vibrato);
    this->oldTenseness  = this->newTenseness;
    this->newTenseness  = this->targetTenseness
                        + 0.1f * simplex1(this->totalTime * 0.46f)
                        + 0.05f * simplex1(this->totalTime * 0.36f);
    if (!this->isTouched && this->alwaysVoice)
        this->newTenseness += (3.f - this->targetTenseness) * (1.f - this->intensity);

    if (this->isTouched || this->alwaysVoice) this->intensity += 0.13f;
    else                                       this->intensity -= 0.05f;
    this->intensity = clamp(this->intensity, 0.f, 1.f);
}

sample_t Glottis::normalizedLFWaveform(sample_t t) {
    sample_t output;
    if (t > this->Te)
        output = (-exp(-this->epsilon * (t - this->Te)) + this->shift) / this->Delta;
    else
        output = this->E0 * exp(this->alpha * t) * sin(this->omega * t);
    return output * this->intensity * this->loudness;
}

sample_t Glottis::runStep(sample_t lambda, sample_t noiseSource) {
    sample_t timeStep = 1.f / this->sampleRate;
    this->timeInWaveform += timeStep;
    this->totalTime      += timeStep;
    if (this->timeInWaveform > this->waveformLength) {
        this->timeInWaveform -= this->waveformLength;
        this->setupWaveform(lambda);
    }
    sample_t out = this->normalizedLFWaveform(this->timeInWaveform / this->waveformLength);
    sample_t aspiration = this->intensity * (1.f - sqrt(this->targetTenseness))
                        * this->getNoiseModulator() * noiseSource;
    aspiration *= 0.2f + 0.02f * simplex1(this->totalTime * 1.99f);
    return out + aspiration;
}
