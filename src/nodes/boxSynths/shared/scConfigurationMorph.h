#pragma once
#include "ofMain.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

// Discrete graph configurations are rendered only while they contribute. Keep
// the weights in captured sounds so reversing/storing/interruption is continuous.
struct scConfigurationMorph {
    using Configuration=std::vector<int>;
    using Weights=std::map<Configuration,float>;
    struct Field { std::string key; int fallback,maximum; };
    static Weights endpoint(const ofJson& sound,const std::string& metadata,const std::vector<Field>& fields) {
        Weights out;float sum=0;
        if(sound.contains(metadata) && sound[metadata].is_array()) {
            for(const auto& entry:sound[metadata]) {
                if(!entry.is_object() || !entry.contains("configuration") || !entry["configuration"].is_array()
                   || entry["configuration"].size()!=fields.size() || !entry.contains("weight") || !entry["weight"].is_number())continue;
                Configuration c;bool valid=true;
                for(size_t i=0;i<fields.size();++i) {
                    const auto& v=entry["configuration"][i];
                    if(!v.is_number_integer()){valid=false;break;}
                    const int n=v.get<int>();
                    if(n<1 || n>fields[i].maximum){valid=false;break;}c.push_back(n);
                }
                const float w=entry["weight"].get<float>();
                if(valid && std::isfinite(w) && w>0){out[c]+=w;sum+=w;}
            }
        }
        if(std::isfinite(sum) && sum>0){for(auto& e:out)e.second/=sum;return out;}
        Configuration c;
        for(const auto& f:fields) {
            const int n=sound.contains(f.key) && sound[f.key].is_number_integer() ? sound[f.key].get<int>() : f.fallback;
            c.push_back(std::clamp(n,1,f.maximum));
        }
        return {{c,1}};
    }
    static Weights blend(const ofJson& a,const ofJson& b,float t,const std::string& metadata,const std::vector<Field>& fields) {
        t=std::clamp(t,0.f,1.f);Weights out;
        for(const auto& e:endpoint(a,metadata,fields))if(t<1)out[e.first]+=(1-t)*e.second;
        for(const auto& e:endpoint(b,metadata,fields))if(t>0)out[e.first]+=t*e.second;
        return out;
    }
    static void capture(ofJson& sound,const std::string& metadata,const Weights& weights) {
        if(weights.size()<2)return;
        sound[metadata]=ofJson::array();
        for(const auto& e:weights)if(e.second>0)
            sound[metadata].push_back(ofJson{{"configuration",e.first},{"weight",e.second}});
    }
};
