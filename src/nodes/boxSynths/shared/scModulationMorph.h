#pragma once
#include "ofMain.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Snapshot schemas differ between Boxes, but route amounts and parameter bases
// have identical audio semantics. Missing routes contribute zero, not a switch.
struct scModulationMorph {
    using Blocked=std::function<bool(const std::string&)>;
    static std::vector<float> numbers(const ofJson& j) {
        if(j.is_array()) {
            std::vector<float> out;
            for(const auto& value:j) if(value.is_number()) out.push_back(value.get<float>());
            return out;
        }
        if(j.is_number()) return {j.get<float>()};
        if(j.is_boolean()) return {j.get<bool>() ? 1.0f : 0.0f};
        // GrainBox serializes ordinary scalar/vector fields as text.
        if(j.is_string()) {
            const auto text=j.get<std::string>(); const char* p=text.c_str();
            std::vector<float> out;
            while(*p) {
                char* end=nullptr;const float value=std::strtof(p,&end);
                if(end!=p) {if(std::isfinite(value))out.push_back(value);p=end;}
                else ++p;
            }
            return out;
        }
        return {};
    }
    static std::vector<float> interpolate(std::vector<float> a,std::vector<float> b,float t,bool zeroMissing=false) {
        const auto n=std::max(a.size(),b.size());
        if(zeroMissing) {a.resize(n,0);b.resize(n,0);}
        else {
            if(a.empty())a=b;if(b.empty())b=a;
            if(a.empty())return {};
            a.resize(n,a.back());b.resize(n,b.back());
        }
        for(size_t i=0;i<n;++i)a[i]=a[i]*(1-t)+b[i]*t;
        return a;
    }
    static ofJson parameters(const ofJson& a,const ofJson& b,float t,
                             const ofJson& controlsA,const ofJson& controlsB,const Blocked& blocked={}) {
        std::map<std::string,ofJson> x,y;
        auto collect=[](const ofJson& j,auto& entries){
            if(j.is_array())for(const auto& e:j)
                if(e.is_object() && e.contains("key") && e["key"].is_string())entries[e["key"].get<std::string>()]=e;
        };
        collect(a,x);collect(b,y);
        std::set<std::string> keys;
        for(const auto& e:x)keys.insert(e.first);for(const auto& e:y)keys.insert(e.first);
        ofJson out=ofJson::array();
        const ofJson empty=ofJson::object();
        for(const auto& key:keys) {
            if(blocked && blocked(key))continue;
            const auto& ea=x.count(key) ? x.at(key) : empty;
            const auto& eb=y.count(key) ? y.at(key) : empty;
            auto base=[&](const ofJson& e,const ofJson& controls){
                if(e.contains("base"))return numbers(e["base"]);
                return controls.contains(key) ? numbers(controls[key]) : std::vector<float>{};
            };
            const auto va=base(ea,controlsA),vb=base(eb,controlsB);
            const auto aa=ea.value("amounts",empty),ab=eb.value("amounts",empty);
            std::set<std::string> sources;
            if(aa.is_object())for(auto i=aa.begin();i!=aa.end();++i)sources.insert(i.key());
            if(ab.is_object())for(auto i=ab.begin();i!=ab.end();++i)sources.insert(i.key());
            ofJson amounts=ofJson::object();
            for(const auto& source:sources) {
                const float av=aa.contains(source) && aa[source].is_number() ? aa[source].get<float>() : 0;
                const float bv=ab.contains(source) && ab[source].is_number() ? ab[source].get<float>() : 0;
                const float value=av*(1-t)+bv*t;
                if(value!=0)amounts[source]=value;
            }
            ofJson entry{{"key",key},{"base",interpolate(va,vb,t)},{"amounts",amounts}};
            // the routes' polarity: the nearer side's (a side without it: an older save)
            const auto& near=(t<0.5f ? x.count(key) : !y.count(key)) ? ea : eb;   // (or the side that has it)
            if(near.contains("pol")){ entry["pol"]=near["pol"]; entry["bipolar"]=near.value("bipolar",ofJson::array()); }
            if(key=="@routes" && !entry.contains("pol"))continue;
            out.push_back(entry);
        }
        return out;
    }
    static ofJson state(const ofJson& a,const ofJson& b,float t,bool switched,const std::string& prefix,const Blocked& blocked={}) {
        t=std::clamp(t,0.0f,1.0f);
        ofJson out=switched ? b : a;
        for(const char* suffix:{"Lfos","Envs","Exts"}) {
            const std::string key=prefix+suffix;
            const int av=a.value(key,0),bv=b.value(key,0);
            out[key]=t<=0 ? av : t>=1 ? bv : std::max(av,bv);
        }
        const std::string matrix=prefix+"Matrix",params=prefix+"Params";
        out[matrix]=interpolate(a.contains(matrix) ? numbers(a[matrix]) : std::vector<float>{},
                                b.contains(matrix) ? numbers(b[matrix]) : std::vector<float>{},t,true);
        out[params]=parameters(a.value(params,ofJson::array()),b.value(params,ofJson::array()),t,a,b,blocked);
        return out;
    }
};
