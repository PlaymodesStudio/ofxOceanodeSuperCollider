#pragma once
#include <algorithm>
#include <cmath>

// Shared Classic LFO waveform and pitch-index geometry. Pure arithmetic.
namespace scmorph {
inline float map(float x,float a,float b,float c,float d,bool clamp) {
    const float t=(x-a)/(b-a);
    return c+(d-c)*(clamp?std::clamp(t,0.f,1.f):t);
}
inline float customPow(float value,float pow) {
    const float k1=2.f*pow*.99999f,k2=k1/(1.f-pow*.999999f);
    return value*(k2+1.f)/std::max(k2*std::abs(value)+1.f,1.e-6f);
}
inline float morph(float ph, float roundness, float skew, float pulseWidth, bool pulseCentered) {
    float lin = std::fmod(ph, 1.0f);
    if(lin < 0.f) lin += 1.f;
    pulseWidth = std::clamp(pulseWidth, 0.001f, 1.f);
    if(pulseWidth < 0.5f){
        if(pulseCentered) lin = map(lin, 0.5f - pulseWidth, 0.5f + pulseWidth, 0.f, 1.f, true);
        else lin = map(lin, 0.f, std::max(pulseWidth * 2.0f, 1.0e-7f), 0.f, 1.f, true);
        if(skew < 0.f && lin >= 1.f - 1.0e-6f) lin = 0.f;
    }else if(pulseWidth >= 1.f - 1.0e-6f){
        lin = map(skew, -1.f, 1.f, 1.f, 0.f, true);
    }else{
        const float mid = map(skew, -1.f, 1.f, 1.f, 0.f, true);
        if(lin < mid) lin = map(lin, 0.f, std::max(map(1.f - pulseWidth, 0.f, 0.5f, 0.f, mid, true), 1.0e-7f), 0.f, mid, true);
        else lin = map(lin, std::min(map(pulseWidth, 0.5f, 1.f, mid, 1.f, true), 1.f - 1.0e-7f), 1.f, mid, 1.f, true);
    }
    const float threshold = std::clamp(0.5f - skew * 0.5f, 0.f, 1.f);
    lin = lin < threshold ? map(lin, 0.f, std::max(threshold, 1.0e-7f), 0.f, 0.5f, true)
                          : map(lin, std::min(threshold, 1.f - 1.0e-7f), 1.f, 0.5f, 1.f, true);
    const float w = lin * 6.283185307179586f;
    const float tri = std::max(0.f, 1.f - std::abs(2.f * lin - 1.f));
    const float cosRaw = std::cos(w + 3.141592653589793f);
    const float cosMapped = map(cosRaw, -1.f, 1.f, 0.f, 1.f, true);
    float v;
    if(roundness >= 1.f) v = (lin >= 0.25f && lin < 0.75f) ? 1.f : 0.f;
    else if(roundness >= 0.5f) v = map(customPow(cosRaw, (roundness - 0.5f) * 2.f), -1.f, 1.f, 0.f, 1.f, true);
    else v = (tri + (cosMapped-tri)*roundness*2.f);
    return std::clamp(v, 0.f, 1.f);
}
// Endpoint-preserving logarithmic/exponential pair; zero is exactly linear.
inline float normalizedPow(float x,float amount) {
    x=std::clamp(x,0.f,1.f);amount=std::clamp(amount,-1.f,1.f);
    if(x==0.f || x==1.f || std::abs(amount)<1.e-6f)return x;
    const float strength=6.f*std::abs(amount),k=std::expm1(strength);
    return amount>0.f?std::log1p(k*x)/strength:std::expm1(strength*x)/k;
}
} // namespace scmorph
