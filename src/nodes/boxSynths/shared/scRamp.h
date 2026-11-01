#pragma once
#include <algorithm>
#include <cmath>

// Shared Ramp trajectory, in spans per cycle. Rate mode integrates the shaped
// From -> To rate; it is useful for acceleration, tape braking and scan ramps.
namespace scRamp {
inline double shape(double t,double curve) {
    return std::abs(curve)<0.01?t:std::expm1(curve*t)/std::expm1(curve);
}
inline double integral(double t,double curve) {
    return std::abs(curve)<0.01?t*t*0.5:(std::expm1(curve*t)/curve-t)/std::expm1(curve);
}
// Same rational Pow/BiPow easing as the shared LFO. Shape time rather
// than output, so negative/reversed ramps keep their endpoints and bounds.
inline double tension(double p) {
    p=std::clamp(p,-0.9999,0.9999);
    return 2*p*0.99999/(1-p*0.999999);
}
inline double ease(double t,double pow,double bipow) {
    auto bend=[](double x,double p){const double k=tension(p);return x*(k+1)/(k*std::abs(x)+1);};
    t=bend(t,pow);
    return std::clamp((bend(2*t-1,bipow)+1)*0.5,0.0,1.0);
}
inline double tail(double from,double to,double curve,bool rate,double pow=0,double bipow=0) {
    const double slope=rate?to:(to-from)*(std::abs(curve)<0.01?1:curve*std::exp(curve)/std::expm1(curve));
    return slope/((tension(pow)+1)*(tension(bipow)+1));
}
inline double value(double phase,double from,double to,double curve,double offset,bool rate,int end,int bounds,double pow=0,double bipow=0) {
    double u=std::max(0.0,phase);
    if(end==1) u-=std::floor(u);
    const double t=ease(std::min(1.0,u),pow,bipow);
    double v=offset+(rate?from*t+(to-from)*integral(t,curve):from+(to-from)*shape(t,curve));
    if(end==2) v+=std::max(0.0,u-1)*tail(from,to,curve,rate,pow,bipow);
    if(bounds==1) v=std::clamp(v,0.0,1.0);
    if(bounds==2) v-=std::floor(v);
    return v;
}
}
