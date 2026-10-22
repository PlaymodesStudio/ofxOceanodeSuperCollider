#pragma once
#include <algorithm>
#include <cmath>
// Reaktor Core factory "2 Pole SV C": FIR, WarpF, WarpD, two Euler
// integrators, and compensated HP/BP/LP outputs. Port order HP/BP/LP.
struct SVFState {
    float previous=0, s1=0, s2=0;
    float tick(float x,float hz,float resonance,int type,float sr) {
        const float w=std::clamp(float(6.28318548)*hz/sr,0.f,1.89999998f);
        const float d=std::max(.02f,2.f*(1.f-resonance));
        const float wd=((2.f+w)*(2.f-w)*d)/(2.f*((2.f-w)+w*d));
        const float fir=(x+previous)*.25f;
        previous=x;
        const float b=(fir-(s1*wd+s2))*w+s1;
        const float l=b*w+s2;
        const float lp=l+s2, bp=b+b, hp=x-(b*wd+lp);
        s1=b;s2=l;
        if(!std::isfinite(s1)||!std::isfinite(s2)) {s1=s2=0;return 0;}
        return type==0?hp:type==1?bp:lp;
    }
};
// Core Interpol RT polynomial, with taps at delay floor-1, floor, +1, +2.
inline float coreCubic(float f,float a,float b,float c,float d) {
    const float slope=(c-a)*.5f;
    const float t=slope+(b-c);
    const float q=(d-b)*.5f+t+(b-c);
    return b+f*(slope+f*(f*q-(t+q)));
}
// XFade(Par), as opposed to the library's linear/equal-power alternatives.
inline float coreFade(float a,float b,float mix) {
    mix=std::clamp(mix,0.f,1.f);
    return a*(1.f-mix*mix)+b*mix*(2.f-mix);
}
