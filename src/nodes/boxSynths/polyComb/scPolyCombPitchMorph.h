#pragma once
#include "ofMain.h"
#include "scPolyCombMigration.h"
#include <numeric>
#include <random>

// Interpolate evaluated MIDI pitches, not the parameters of incompatible map
// algorithms. Weighted endpoint descriptions keep interrupted/stored morphs live
// to played roots and allow every temporary voice bank to use its own width.
class scPolyCombPitchMorph {
public:
    static constexpr const char* key="_pitchMapBlend";
    struct Part { ofJson controls; float weight; };
    struct Notes {
        std::vector<float> oscillator, resonator;
        float own=0;
    };
    std::vector<Part> parts;
    static bool control(const std::string& k) {
        return k.rfind("Osc_Map_",0)==0 || k.rfind("Comb_Map_",0)==0 ||
            k=="Pitch_Shift" || k=="Scramble";
    }
    bool active() const { return !parts.empty(); }
    void clear() { parts.clear(); }
    void finish() { if(parts.size()==1) clear(); }
    static ofJson select(const ofJson& c) {
        ofJson out=ofJson::object();
        if(c.is_object()) for(auto it=c.begin();it!=c.end();++it) if(control(it.key())) out[it.key()]=it.value();
        return out;
    }
    static ofJson complete(const ofJson& c,const ofJson& defaults) {
        auto out=select(defaults);
        if(!c.is_object()) return out;
        for(auto it=c.begin();it!=c.end();++it) if(control(it.key())) out[it.key()]=it.value();
        return out;
    }
    static void validate(const ofJson& c) {
        (void)pcmigration::map(c,"Osc_Map_"); (void)pcmigration::map(c,"Comb_Map_");
        (void)c.value("Comb_Map_On",false); (void)c.value("Scramble",false); (void)c.value("Pitch_Shift",0);
    }
    static std::vector<Part> endpoint(const ofJson& sound,const ofJson& defaults) {
        auto ordinary=[&](){
            auto c=complete(pcmigration::controls(sound.value("controls",ofJson::object())),defaults);
            try { validate(c); } catch(...) { c=select(defaults); }
            return std::vector<Part>{{std::move(c),1}};
        };
        // Optional metadata is all-or-nothing. Invalid data falls back to the
        // ordinary snapshot controls, as older snapshots do.
        if(sound.contains(key) && sound[key].is_array() && !sound[key].empty()) try {
            std::vector<Part> out; float total=0;
            for(const auto& p:sound[key]) {
                const float w=p.at("weight").get<float>();
                if(!std::isfinite(w) || w<0 || !p.at("controls").is_object()) return ordinary();
                auto c=complete(p.at("controls"),defaults);
                // Validate the map fields before accepting the entire blend.
                validate(c);
                if(w>0) out.push_back({std::move(c),w});
                total+=w;
            }
            if(!std::isfinite(total) || total<=0) return ordinary();
            for(auto& p:out) p.weight/=total;
            return out;
        } catch(...) {}
        return ordinary();
    }
    void begin(const ofJson& a,const ofJson& b,float t,const ofJson& defaults) {
        t=std::clamp(t,0.f,1.f);
        auto x=endpoint(a,defaults),y=endpoint(b,defaults);
        parts.clear();
        auto add=[&](const Part& p,float gain){
            const float w=p.weight*gain;
            if(w<=0) return;
            for(auto& old:parts) if(old.controls==p.controls) { old.weight+=w; return; }
            parts.push_back({p.controls,w});
        };
        for(const auto& p:x) add(p,1-t);
        for(const auto& p:y) add(p,t);
    }
    void capture(ofJson& sound,const ofJson& overrides=ofJson::object()) const {
        if(parts.size()<2) return;
        auto state=ofJson::array();
        for(const auto& p:parts) {
            auto c=p.controls; c.update(overrides);
            state.push_back({{"weight",p.weight},{"controls",std::move(c)}});
        }
        sound[key]=std::move(state);
    }
    void reseed(const std::string& prefix,int seed) {
        for(auto& p:parts) if(p.controls.value(prefix+"New_Seed_On_Gate",false)) p.controls[prefix+"Seed"]=seed;
    }
    float ownWeight(const ofJson& overrides=ofJson::object()) const {
        float own=0;
        for(const auto& p:parts) if(overrides.value("Comb_Map_On",p.controls.value("Comb_Map_On",false))) own+=p.weight;
        return std::clamp(own,0.f,1.f);
    }
    static std::vector<float> mapNotes(const ofJson& c,const std::string& prefix,int n,float root) {
        auto map=pcmigration::map(c,prefix);
        // Current runtime maps use the migrated geometry and integer root.
        map.morphGeometry=true; map.root=std::round(map.root);
        const bool inScale=c.value("Pitch_Shift",0)==1;
        std::vector<int> order;
        if(c.value("Scramble",false) && n>1) {
            order.resize(n);std::iota(order.begin(),order.end(),0);
            std::mt19937 rng(0x504f4c59+n*31);std::shuffle(order.begin(),order.end(),rng);
        }
        return pcmap::playedPitches(map,n,root,inScale,order);
    }
    Notes evaluate(int n,float root,const ofJson& overrides=ofJson::object()) const {
        Notes out{std::vector<float>(n,0),std::vector<float>(n,0),0};
        for(const auto& p:parts) {
            auto c=p.controls;c.update(overrides);
            const auto osc=mapNotes(c,"Osc_Map_",n,root);
            const bool own=c.value("Comb_Map_On",false);
            auto comb=own?mapNotes(c,"Comb_Map_",n,root):osc;
            if(!own) for(auto& note:comb) note=std::clamp(note+
                pcmigration::scalar(c,"Comb_Map_Transpose")+pcmigration::scalar(c,"Comb_Map_Fine_Tune"),0.f,135.f);
            for(int i=0;i<n;++i) {
                out.oscillator[i]+=p.weight*osc[i];out.resonator[i]+=p.weight*comb[i];
            }
            if(own) out.own+=p.weight;
        }
        out.own=std::clamp(out.own,0.f,1.f);
        return out;
    }
};
