#pragma once
#include "ofMain.h"
#include "../shared/scModulationMorph.h"
#include <algorithm>
#include <cmath>

// Translate old Motion snapshots to an ordinary shared Ramp -> Pos route.
// The old keys are input compatibility data only; no Motion DSP remains.
inline ofJson scGrainStretchRouting(ofJson sound,int sources,int targets,const std::string& suffix="",float bpm=120,float regionSeconds=15.99f) {
    std::string escaped=suffix;std::replace(escaped.begin(),escaped.end(),' ','_');
    auto key=[&](std::string name){std::replace(name.begin(),name.end(),' ','_');return name+escaped;};
    auto number=[&](const std::string& name,float fallback){
        auto it=sound.find(key(name));if(it==sound.end())return fallback;
        const auto values=scModulationMorph::numbers(*it);return values.empty()?fallback:values[0];
    };
    if(!sound.contains(key("Motion")))return sound;
    const bool active=number("Motion",0)>0.5f;
    if(active) {
        const auto prefix="gbpMod"+suffix;
        int count=sound.value(prefix+"Lfos",0),slot=-1;
        std::vector<float> matrix(sources*targets,0);
        if(sound.contains(prefix+"Matrix") && sound[prefix+"Matrix"].is_array()) {
            auto old=scModulationMorph::numbers(sound[prefix+"Matrix"]);
            if(old.size()==matrix.size())matrix=old;
            else if(!old.empty() && old.size()%sources==0) {
                const int columns=old.size()/sources;
                for(int s=0;s<sources;++s)for(int t=0;t<std::min(columns,targets);++t)matrix[s*targets+t]=old[s*columns+t];
            }
        }
        // Reuse an unused generator slot; existing LFO/Env routes retain IDs.
        for(int candidate=0;candidate<std::min(8,sources);++candidate) {
            const int i=(std::clamp(count,0,8)+candidate)%8;
            bool free=true;for(int t=0;t<targets;++t)free&=std::abs(matrix[i*targets+t])<0.000001f;
            if(sound.contains(prefix+"Params") && sound[prefix+"Params"].is_array())
                for(const auto& param:sound[prefix+"Params"]) {
                    if(!param.is_object() || !param.contains("amounts") || !param["amounts"].is_object())continue;
                    const auto it=param["amounts"].find(std::to_string(i));
                    if(it!=param["amounts"].end()) {const auto values=scModulationMorph::numbers(*it);if(!values.empty())free&=std::abs(values[0])<0.000001f;}
                }
            if(sound.contains("modulationLfoOutputs") && sound["modulationLfoOutputs"].is_object()) {
                const auto& outputs=sound["modulationLfoOutputs"];
                if(outputs.contains(suffix) && outputs[suffix].is_array())
                    for(const auto& output:outputs[suffix])if(output.is_number_integer() && output.get<int>()==i)free=false;
            }
            if(free){slot=i;break;}
        }
        if(slot<0){sound["rampMigrationError"]="No free modulation slot for the saved Motion effect";return sound;}
        const int model=int(number("Motion Mode",0));
        const float from=number("Motion Start",1),to=number("Motion End",0),time=std::max(0.01f,number("Motion Time",0.5f));
        const bool beats=number("Motion Beats",0)>0.5f,repeat=number("Motion Repeat",0)>0.5f,freeze=number("Motion Freeze",0)>0.5f;
        const std::string L="LFO"+std::to_string(slot+1)+" ";
        sound[key(L+"Ramp")]=true;sound[key(L+"Ramp Enabled")]=true;sound[key(L+"Ramp Integrate")]=true;
        sound[key(L+"Ramp From")]=(freeze?0:1)-from;sound[key(L+"Ramp To")]=(freeze?0:1)-to;
        sound[key(L+"Ramp Curve")]=number("Motion Curve",0);
        sound[key(L+"Pow")]=std::vector<float>{0};sound[key(L+"BiPow")]=std::vector<float>{0};
        sound[key(L+"Ramp Offset")]=freeze?std::max(0.0f,(from+to)*0.5f):0;
        sound[key(L+"Ramp Duration")]=time;sound[key(L+"Sync")]=beats;
        sound[key(L+"Trigger Mode")]=2;sound[key(L+"Restart Beats")]=0;
        sound[key(L+"Ramp End")]=repeat?1:2;sound[key(L+"Ramp Bounds")]=model==2?2:0;
        const float seconds=time*(beats?60.0f/std::max(1.0f,bpm):1.0f);
        const float oldSpan=number("Position Mod Span",1),oldUnit=number("Position Mod Units",0);
        const float oldSeconds=oldUnit==0?oldSpan*regionSeconds:oldUnit==1?oldSpan*0.001f:oldSpan*60.0f/std::max(1.0f,bpm);
        bool usedPos=false;for(int i=0;i<sources;++i)usedPos|=std::abs(matrix[i*targets])>0.000001f;
        const float common=usedPos?std::max(seconds,oldSeconds):seconds;
        for(int i=0;i<sources;++i)matrix[i*targets]*=oldSeconds/std::max(0.001f,common);
        sound[key("Position Mod Span")]=beats?common*bpm/60.0f:common*1000;
        sound[key("Position Mod Units")]=beats?2:1;
        sound[key("Freeze On Gate")]=freeze;sound[key("Follow Scan Pitch")]=model==0;
        if(model==0)sound[key("DelayGlide")]=0;
        matrix[slot*targets]=seconds/std::max(0.001f,common);
        // A reused slot may have retained a bipolar checkbox after its route
        // was cleared. The translated scan is an absolute unipolar offset.
        if(sound.contains(prefix+"Params") && sound[prefix+"Params"].is_array())
            for(auto& param:sound[prefix+"Params"])if(param.is_object() && param.value("key",std::string())=="@routes" && param.contains("bipolar") && param["bipolar"].is_array()) {
                auto& cells=param["bipolar"];
                for(auto it=cells.begin();it!=cells.end();)if(it->is_number_integer() && it->get<int>()==slot*targets)it=cells.erase(it);else ++it;
            }
        sound[prefix+"Lfos"]=std::max(count,slot+1);sound[prefix+"Matrix"]=matrix;
    }
    for(const char* name:{"Motion","Motion Mode","Motion Start","Motion End","Motion Time","Motion Beats","Motion Curve","Motion Repeat","Motion Freeze"})sound.erase(key(name));
    return sound;
}
