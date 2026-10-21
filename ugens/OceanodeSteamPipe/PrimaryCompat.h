// Primary DSP compatibility math. Kept separate from generated voice code.
// Float operations and interpolation order follow Reaktor 6.5's Primary modules.
#pragma once
#include <array>
#include <cmath>
#include <algorithm>
inline double f32(double value) { return static_cast<float>(value); }
struct PrimaryExpTable {
    std::array<float,2401> step{};
    std::array<float,2400> slope{};
    PrimaryExpTable() {
        for(int i=0;i<=2400;++i) step[i]=std::exp2((float(i)-1200.f)/60.f);
        for(int i=0;i<2400;++i) slope[i]=step[i+1]-step[i];
    }
};
// Constructed when the extension is loaded, never allocated on the audio thread.
static const PrimaryExpTable primaryExpTable;
inline float niExp(float index) {
    const int i=static_cast<int>(index);
    if(i>=2400) return primaryExpTable.step[2400];
    if(i<0) return 0;
    return primaryExpTable.step[i]+(index-float(i))*primaryExpTable.slope[i];
}
inline double niPitch(double pitch) {
    return niExp(float(pitch)*5.f+1381.881591796875f);
}
inline double niTimeRate(double time,double rate) {
    return niExp(float(time)*-9.965784072875977f+1797.947021484375f)*float(1./rate);
}
#include "PrimaryFilterGrid.h"
inline double niFilterGrid(double pitch,double resonance,double rate,const float* grid) {
    float x=niExp(float(pitch)*5.f+1381.881591796875f)*float(2./rate)*10.f;
    float y=float(resonance)*10.f;
    int ix=std::clamp(int(x),0,9), iy=std::clamp(int(y),0,9);
    float fx=std::clamp(x-float(ix),0.f,1.f),fy=std::clamp(y-float(iy),0.f,1.f);
    const int i=iy*11+ix;
    float a=(1.f-fy)*(1.f-fx)*grid[i];
    a=a+(1.f-fy)*fx*grid[i+1];
    a=a+fy*(1.f-fx)*grid[i+11];
    return a+fy*fx*grid[i+12];
}
inline double niFilterF(double p,double r,double sr) { return niFilterGrid(p,r,sr,primaryFilterF); }
inline double niFilterD(double p,double r,double sr) { return niFilterGrid(p,r,sr,primaryFilterD); }

inline double niDelay(double pitch,double correction,double rate) {
    const float delay=1000.f/float(niPitch(pitch));
    const float sr=float(rate);
    const float offset=(float(correction)*(1.f/sr))*1000.f;
    return (delay+offset)*(sr*.001f);
}
