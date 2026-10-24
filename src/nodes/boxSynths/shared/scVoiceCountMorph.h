#pragma once
#include "ofMain.h"
#include <algorithm>
#include <cmath>
#include <vector>

// A count is an audio configuration, not an interpolated integer. A saved
// intermediate sound may contain several configurations after interrupted glides.
struct scVoiceCountMorph {
    static constexpr const char* key = "_voiceCountBlend";
    static std::vector<float> endpoint(const ofJson& sound, int fallback, int maximum) {
        if(sound.contains(key) && sound[key].is_array() && int(sound[key].size()) == maximum) {
            std::vector<float> out(maximum); float sum=0;
            bool valid=true;
            for(int i=0;i<maximum;++i) {
                if(!sound[key][i].is_number()) { valid=false; break; }
                out[i]=sound[key][i].get<float>();
                if(!std::isfinite(out[i]) || out[i]<0) { valid=false; break; }
                sum+=out[i];
            }
            if(valid && std::isfinite(sum) && sum>0) { for(auto& v:out) v/=sum; return out; }
        }
        int count=fallback;
        if(sound.contains("voices") && sound["voices"].is_number()) count=sound["voices"].get<int>();
        std::vector<float> out(maximum,0); out[std::clamp(count,1,maximum)-1]=1;
        return out;
    }
    static std::vector<float> blend(const ofJson& a,const ofJson& b,float t,int fallback,int maximum) {
        auto x=endpoint(a,fallback,maximum), y=endpoint(b,fallback,maximum);
        t=std::clamp(t,0.0f,1.0f);
        for(int i=0;i<maximum;++i) x[i]=(1-t)*x[i]+t*y[i];
        return x;
    }
    static int single(const std::vector<float>& weights) {
        int found=0;
        for(size_t i=0;i<weights.size();++i) if(weights[i]>0) {
            if(found) return 0;
            found=int(i)+1;
        }
        return found;
    }
    static int activeCount(const std::vector<float>& weights) {
        // Expose every contributing voice immediately, even at tiny curved t.
        // Gain is handled by the bank mixer, never by the discrete switch point.
        for(size_t i=weights.size();i>0;--i) if(weights[i-1]>0) return int(i);
        return 1;
    }
    static void capture(ofJson& sound,const std::vector<float>& weights) {
        if(!weights.empty() && !single(weights)) sound[key]=weights;
    }
};
