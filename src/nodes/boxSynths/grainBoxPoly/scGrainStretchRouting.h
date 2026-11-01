#pragma once
#include "ofMain.h"
#include "../shared/scModulationMorph.h"
#include <algorithm>
#include <cmath>

// Build factory granular scans as ordinary, editable routes.
// Never call this while loading user snapshots or projects.
// Custom modulation and true tape effects are retained as saved.
inline ofJson scGrainStretchRouting(ofJson sound, int sources, int targets,
                                    const std::string& suffix = "") {
    std::string escaped=suffix;
    std::replace(escaped.begin(),escaped.end(),' ','_');
    auto key=[&](const std::string& name){std::string k=name;std::replace(k.begin(),k.end(),' ','_');return k+escaped;};
    auto number=[&](const std::string& name,float fallback){
        auto it=sound.find(key(name));
        if(it==sound.end())return fallback;
        const auto values=scModulationMorph::numbers(*it);
        return values.empty()?fallback:values[0];
    };
    if(sound.contains("publishedEditorParameters") && sound["publishedEditorParameters"].is_array())
        for(const auto& item:sound["publishedEditorParameters"]) if(item.is_string()) {
            const auto name=item.get<std::string>();
            if(name==key("Motion") || name.rfind(key("Motion")+"_",0)==0) return sound;
        }
    const int mode=int(number("Motion Mode",0));
    if(!number("Motion",0) || !mode || number("Motion Freeze",0) || std::abs(number("Motion Curve",0))>0.001f) return sound;
    const auto prefix="gbpMod"+suffix;
    auto matrix=std::vector<float>(sources*targets,0);
    if(sound.contains(prefix+"Matrix") && sound[prefix+"Matrix"].is_array()) {
        for(const auto& value:sound[prefix+"Matrix"])
            if(value.is_number() && std::abs(value.get<float>())>0.00001f) return sound;
    }
    const float start=number("Motion Start",1),end=number("Motion End",0),time=std::max(0.01f,number("Motion Time",0.5f));
    const bool beats=number("Motion Beats",0)>0.5f,repeat=number("Motion Repeat",0)>0.5f;
    float period=time,span=time,power=1,slope=1-start;
    int trigger=1;
    if(mode==2 && std::abs(start-end)<0.00001f) period=time/std::max(0.00001f,std::abs(slope));
    else if(mode==1 && start==1 && end==0 && !repeat) {trigger=2;span=time*0.5f;power=2;slope=1;}
    else return sound;
    matrix[0]=std::abs(slope)>0.00001f?1:0;
    sound[key("Motion")]=false;
    sound[key("Position Mod Span")]=beats?span:span*1000;
    sound[key("Position Mod Units")]=beats?2:1;
    sound[key("LFO1 Sync")]=beats;
    sound[key("LFO1 Beats")]=std::min(512.0f,period);
    sound[key("LFO1 Hz")]=std::abs(slope)>0.00001f?1.0f/period:0;
    sound[key("LFO1 Trigger Mode")]=trigger;
    sound[key("LFO1 Restart Beats")]=beats && repeat?time:0;
    sound[key("LFO1 Ramp Power")]=power;
    sound[key("LFO1 Round")]=0;
    sound[key("LFO1 Skew")]=slope<0?1:-1;
    sound[key("LFO1 Width")]=0.5f;
    sound[key("LFO1 Phase")]=0;
    sound[key("LFO1 Noise")]=false;
    sound[key("LFO1 Random Type")]=0;
    sound[prefix+"Lfos"]=1;
    sound[prefix+"Matrix"]=matrix;
    return sound;
}
