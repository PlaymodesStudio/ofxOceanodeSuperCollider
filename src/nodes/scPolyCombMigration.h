#pragma once
#include "scPolyCombPitchMap.h"
#include <nlohmann/json.hpp>

// Shared by runtime recall and the bank migrator. Repeating it is harmless.
namespace pcmigration {
using Json = nlohmann::json;
inline float scalar(const Json& c, const std::string& k, float fallback=0) {
    if(!c.contains(k)) return fallback;
    const auto& v=c[k];
    if(v.is_array()) return v.empty()?fallback:v[0].get<float>();
    return v.is_number()?v.get<float>():fallback;
}
inline pcmap::Map map(const Json& c,const std::string& p) {
    pcmap::Map m;
    m.shape=(int)scalar(c,p+"Shape"); m.first=scalar(c,p+"First",1);
    m.interval=scalar(c,p+"Interval",3); m.transpose=scalar(c,p+"Transpose");
    m.bend=scalar(c,p+"Bend",1); m.offset=scalar(c,p+"Offset",scalar(c,p+"Fine_Tune"));
    m.mode=(int)scalar(c,p+"Mode"); m.quantize=(int)scalar(c,p+"Quantize");
    m.root=scalar(c,p+"Root"); m.span=scalar(c,p+"Span");
    m.harmonicBase=scalar(c,p+"Harmonic_Base",36);
    m.morphGeometry=c.contains(p+"Skew");
    m.skew=scalar(c,p+"Skew",-1);m.roundness=scalar(c,p+"Roundness");
    m.pulseWidth=scalar(c,p+"PW",.5f);m.pow=scalar(c,p+"Pow");
    m.waves=scalar(c,p+"Num_Waves",1);m.cycleShift=(int)scalar(c,p+"Cycle_Shift");
    m.octaveJump=scalar(c,p+"Octave_Jump",1);
    m.degreeOffset=scalar(c,p+"Degree_Offset"); m.invert=c.value(p+"Invert",false);
    m.symmetry=(int)scalar(c,p+"Symmetry"); m.comb=scalar(c,p+"Comb");
    m.indexQuantize=scalar(c,p+"Index_Quantize");m.indexRandom=scalar(c,p+"Index_Random");m.seed=(int)scalar(c,p+"Seed");
    if(c.contains(p+"Tones")) m.tones=c[p+"Tones"].get<std::vector<float>>();
    if(c.contains(p+"Table")) m.table=c[p+"Table"].get<std::vector<float>>();
    return m;
}
inline float legacyIndex(const pcmap::Map& m,int i,int n){
    const float nf=std::max(1,n);float v=m.first+i;
    if(m.shape==1)v=nf*.5f-std::abs(v-nf*.5f)+1;
    if(std::abs(m.bend-1)>1e-6f){const float x=v/nf;v=(x<0?-std::pow(-x,m.bend):std::pow(x,m.bend))*nf;}
    return v;
}
inline std::vector<float> legacyNotes(const pcmap::Map& m,int n) {
    if(m.mode!=pcmap::ModeTable) return pcmap::generate(m,n);
    std::vector<float> out;
    for(int i=0;i<n;++i){
        float rel=m.interval*legacyIndex(m,i,n);
        if(m.span>1e-6f) rel=pcmap::floorMod(rel,m.span);
        const float p=60+m.transpose+rel-m.root;
        const float octave=std::floor(p/12);
        const int index=std::clamp((int)std::floor(p-octave*12),0,11);
        out.push_back(octave*12+(m.table.empty()?0:m.table[std::min((size_t)index,m.table.size()-1)])+m.root+m.offset);
    }
    return out;
}
inline Json controls(Json c) {
    if(!c.is_object()) return c;
    // The post filter used to share the pre-filter's model and had no tracking.
    if(!c.contains("Post_Filter_Model")) c["Post_Filter_Model"]=(int)scalar(c,"Filter_Model",0);
    if(!c.contains("Post_Key_Track")) c["Post_Key_Track"]=0.0f;
    // The resonators used to follow the oscillators' glided note: they keep that glide.
    if(!c.contains("Comb_Glide")){
        auto flag=[&](const char* k,bool fallback){
            if(!c.contains(k)) return fallback;
            return c[k].is_boolean() ? c[k].get<bool>() : scalar(c,k,fallback?1.f:0.f)>.5f;
        };
        const bool internal=!flag("Pitch_External",true);
        const bool follows=internal ? !flag("Comb_Map_On",false) : (int)scalar(c,"Comb_Mode",0)==0;
        c["Comb_Glide"]=follows ? scalar(c,"Glide",0) : 0.0f;
    }
    // Preserve the combined sub tuning when splitting legacy fractional offsets.
    if(c.contains("Sub_Transpose") || c.contains("Sub_Fine_Tune")){
        const float old=scalar(c,"Sub_Transpose",-12.f);
        const float rounded=std::round(old);
        float fine=scalar(c,"Sub_Fine_Tune")+old-rounded;
        const float carry=std::abs(fine)>1.f?std::round(fine):0.f;
        c["Sub_Transpose"]=std::clamp(rounded+carry,-96.f,96.f);
        c["Sub_Fine_Tune"]=std::clamp(fine-carry,-1.f,1.f);
    }
    const bool oldRing=(int)scalar(c,"Ring_Model",0)!=1;
    if(oldRing){
        std::vector<float> ratio;
        auto notes=(int)scalar(c,"Osc_Map_Version",1)<2?legacyNotes(map(c,"Osc_Map_"),32):pcmap::generate(map(c,"Osc_Map_"),32);
        const bool external=c.value("Pitch_External",true);
        auto at=[&](const std::string& k,int i,float fallback){
            if(!c.contains(k)) return fallback;
            const auto& v=c[k];
            return v.is_array() ? (v.empty()?fallback:v[std::min((size_t)i,v.size()-1)].get<float>()) : scalar(c,k,fallback);
        };
        for(int i=0;i<32;++i){
            const float base=std::clamp((external?at("Pitch",i,60):notes[i]+at("Pitch",i,60)-60)+at("Transpose",i,0),0.0f,135.0f);
            const float target=std::clamp(at("Ring_Pitch",i,72)+at("Ring_Transpose",i,0),0.0f,135.0f);
            ratio.push_back(std::clamp(std::pow(2.0f,(target-base)/12),0.0f,32.0f));
        }
        c["Ring_Ratio"]=ratio; c["Ring_Width"]=0.0f;
    }
    c["Ring_Model"]=1; c["Comb_Model"]=1; c["Comb_Saturation"]=false;
    for(const std::string p : {"Osc_Map_","Comb_Map_"}){
        if((int)scalar(c,p+"Version",1)<2){
            auto old=map(c,p);
            if(old.mode==pcmap::ModeTable){
                std::vector<float> scale;
                for(float v:old.table){
                    const float pc=pcmap::floorMod(std::round(v),12);
                    if(std::find(scale.begin(),scale.end(),pc)==scale.end()) scale.push_back(pc);
                }
                if(scale.empty()) scale={0};
                std::sort(scale.begin(),scale.end());
                const auto notes=legacyNotes(old,32);
                // Fit the old mapping in scale-degree space, retaining its
                // voice-index shape. Exact for regular scales; arbitrary old
                // 12-entry remaps can only be approximated by an N-note scale.
                double sx=0,sy=0,sxx=0,sxy=0;
                for(int i=0;i<32;++i){
                    const double x=legacyIndex(old,i,32)-1;
                    const float rel=notes[i]-60-old.root-old.offset;
                    const float octave=std::floor(rel/12),pc=rel-octave*12;
                    int degree=0; float distance=1e9;
                    for(int k=0;k<(int)scale.size();++k)
                        if(std::abs(scale[k]-pc)<distance){distance=std::abs(scale[k]-pc);degree=k;}
                    const double y=octave*scale.size()+degree+0.25;
                    sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
                }
                const double denom=32*sxx-sx*sx;
                const double slope=std::abs(denom)>1e-9?(32*sxy-sx*sy)/denom:0;
                pcmap::Map candidate=old;
                candidate.table=scale;candidate.transpose=0;candidate.span=0;
                candidate.interval=std::clamp((float)slope,-24.0f,24.0f);
                candidate.degreeOffset=std::clamp((float)((sy-slope*sx)/32),-256.0f,256.0f);
                pcmap::Map best=candidate; double bestError=1e30;
                auto score=[&](const pcmap::Map& trial){
                    const auto actual=pcmap::generate(trial,32);
                    double error=0;
                    for(int i=0;i<32;++i){
                        const double d=std::clamp(actual[i],0.0f,135.0f)-std::clamp(notes[i],0.0f,135.0f);
                        error+=std::abs(d)+0.01*d*d;
                    }
                    if(error<bestError-1e-7){bestError=error;best=trial;}
                };
                score(candidate);
                // Discrete degree lookup makes a least-squares line imperfect.
                // Search around it, plus the old semitone-to-degree stride; also
                // retain periodic pitch sets when the old Span was enabled.
                for(float center:{candidate.interval,old.interval*(float)scale.size()/12.0f}){
                    for(int a=-16;a<=16;++a) for(int b=-16;b<=16;++b){
                        candidate.interval=std::clamp(center+a*0.025f,-24.0f,24.0f);
                        candidate.degreeOffset=std::clamp((float)((sy-slope*sx)/32)+b*0.125f,-256.0f,256.0f);
                        candidate.span=0;score(candidate);
                        if(old.span>1e-6f){candidate.span=old.span*scale.size()/12.0f;score(candidate);}
                    }
                }
                c[p+"Table"]=scale;c[p+"Interval"]=best.interval;
                c[p+"Degree_Offset"]=best.degreeOffset;
                c[p+"Transpose"]=0.0f;c[p+"Span"]=best.span;
            }
            c[p+"Version"]=2;
        }
        if(c.contains(p+"Table")){
            auto table=c[p+"Table"].get<std::vector<float>>();
            for(auto& v:table) v=pcmap::floorMod(std::round(v),12);
            c[p+"Table"]=pcmap::userScale(table);
        }
        for(const std::string k:{"Degree_Offset","Comb","Index_Quantize","Index_Random"})
            if(!c.contains(p+k)) c[p+k]=0.0f;
        if(!c.contains(p+"Skew")){
            const int shape=(int)scalar(c,p+"Shape");
            c[p+"Skew"]=shape==0?-1.f:0.f;
            c[p+"Roundness"]=shape==2?.5f:0.f;c[p+"PW"]=.5f;
        }
        if(!c.contains(p+"Pow")){
            // Match the old exponent at mid-height. Exact legacy geometry is
            // intentionally replaced; Pow now acts after every transformation.
            const float target=std::pow(.5f,scalar(c,p+"Bend",1));
            float lo=-1.f,hi=1.f;
            for(int i=0;i<24;++i){const float mid=(lo+hi)*.5f;
                if(scmorph::normalizedPow(.5f,mid)<target)lo=mid;else hi=mid;}
            c[p+"Pow"]=std::abs(target-.5f)<1.e-6f?0.f:(lo+hi)*.5f;
        }
        // Rename the old post-map shift and carry its whole semitones into
        // Transpose. Keep fractional transpositions as continuous fine tuning.
        const float oldTranspose=scalar(c,p+"Transpose");
        float coarse=std::round(oldTranspose);
        float fine=scalar(c,p+"Offset",scalar(c,p+"Fine_Tune"))+(oldTranspose-coarse);
        const float carry=std::abs(fine)>1.f?std::round(fine):0.f;
        coarse+=carry;fine-=carry;
        c[p+"Transpose"]=std::clamp(coarse,-72.f,72.f);
        c[p+"Fine_Tune"]=std::clamp(fine,-1.f,1.f);c.erase(p+"Offset");
        if(!c.contains(p+"Num_Waves")) c[p+"Num_Waves"]=1.0f;
        if(!c.contains(p+"Invert")) c[p+"Invert"]=false;
        if(!c.contains(p+"New_Seed_On_Gate"))c[p+"New_Seed_On_Gate"]=false;
        for(const std::string k:{"Symmetry","Seed","Cycle_Shift"}) if(!c.contains(p+k)) c[p+k]=0;
    }
    for(int i=1;i<=8;++i){
        const std::string p="LFO"+std::to_string(i)+"_";
        if(!c.contains(p+"Model")){
            const int type=(int)scalar(c,p+"Random_Type");
            c[p+"Model"]=type>0?3:c.value(p+"Noise",false)?2:1;
            if(type==2){
                const float period=c.value(p+"Sync",true)?scalar(c,p+"Beats",1)*0.5f:1/std::max(0.001f,scalar(c,p+"Hz",1));
                c[p+"Random_Smooth"]=std::clamp(period*0.25f,0.0f,10.0f);
            }
        }
        if(!c.contains(p+"Value")) c[p+"Value"]=0.0f;
        c[p+"Random_Type"]=(int)scalar(c,p+"Model")==3?1:0;
        c[p+"Noise"]=(int)scalar(c,p+"Model")==2;
    }
    return c;
}
}
