#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

namespace pccontrol {
inline float step(const std::string& key) {
    if(key=="Osc_Map_Transpose" || key=="Comb_Map_Transpose" || key=="Sub_Transpose")return 1.f;
    if(key=="Transpose" || key.find("_Transpose")!=std::string::npos)return .25f;
    const bool map=key.rfind("Osc_Map_",0)==0 || key.rfind("Comb_Map_",0)==0;
    if(!map)return 0.f;
    const auto field=key.substr(key.find("_Map_")+5);
    if(field=="Num_Waves" || field=="Octave_Jump")return .25f;
    if(field=="Interval" || field=="First" || field=="Degree_Offset" || field=="Index_Quantize")return 1.f;
    return 0.f;
}
inline float snap(float value,float quantum,float lo,float hi) {
    if(!std::isfinite(value))value=0;
    return std::clamp(std::round(value/quantum)*quantum,lo,hi);
}
template<class T> T normalize(const std::string& key,const T& value,const T& lo,const T& hi) {
    const float quantum=step(key);
    if constexpr(std::is_same_v<T,float>){
        if(quantum>0)return snap(value,quantum,lo,hi);
        if(key=="Osc_Map_Index_Random" || key=="Comb_Map_Index_Random" || key=="Osc_Map_Fine_Tune" || key=="Comb_Map_Fine_Tune" || key=="Sub_Fine_Tune")return std::clamp(value,lo,hi);
    }else if constexpr(std::is_same_v<T,std::vector<float>>){
        if(quantum>0){
            auto out=value;
            for(size_t i=0;i<out.size();++i)
                out[i]=snap(out[i],quantum,lo.empty()?-1.e9f:lo[std::min(i,lo.size()-1)],hi.empty()?1.e9f:hi[std::min(i,hi.size()-1)]);
            return out;
        }
    }else if constexpr(std::is_same_v<T,int>){
        if(key=="Osc_Map_Symmetry" || key=="Comb_Map_Symmetry" || key=="Osc_Map_Cycle_Shift" || key=="Comb_Map_Cycle_Shift")return std::clamp(value,lo,hi);
    }
    return value;
}
} // namespace pccontrol
