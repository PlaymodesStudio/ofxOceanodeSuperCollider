#pragma once
#include "scChordBoxEngine.h"

// Pure pitch-map arithmetic, shared by the node and its offline checks.
// The libraries and their parser are ChordBox's, including microtonal entries.
namespace polycombPitch {
struct Map {
    int shape=0, quantize=0, first=1, inversion=0;
    float root=60, interval=3, transpose=0, fine=0, bend=1, length=1, offset=0, harmonicRoot=60;
    std::vector<float> scale{0,2,4,5,7,9,11}, chord{0,4,7}, table{0,1,2,3,4,5,6,7,8,9,10,11};
};
inline float degree(const std::vector<float>& set,int i){
    if(set.empty()) return 0;
    const int n=(int)set.size();
    return set[chordbox::wrapIndex(i,n)]+12*(float)chordbox::floorDiv(i,n);
}
inline float nearest(float note,const std::vector<float>& set){
    if(set.empty())return note;
    float best=note, distance=1e9f;
    for(int oct=(int)std::floor(note/12)-1;oct<=(int)std::floor(note/12)+1;oct++)for(float pc:set){
        const float p=12*oct+pc,d=std::abs(p-note);
        if(d<distance || (d==distance && p<best)){best=p;distance=d;}
    }
    return best;
}
inline std::vector<float> generate(const Map& m,int voices,float harmony=0){
    std::vector<float> out;out.reserve(voices);
    for(int i=0;i<voices;i++){
        float position=(float)i+m.first;
        if(m.shape==1)position=voices*.5f-std::abs(position-voices*.5f)+1;
        if(m.shape==2){position=std::fmod((float)i/std::max(1,voices-1)*m.length+m.offset,1.f);if(position<0)position+=1;}
        else position=std::pow(std::max(0.f,position/voices),std::max(.01f,m.bend))*voices;
        float p=m.root+m.transpose+position*m.interval;
        if(m.shape==3)p=m.root+m.transpose+degree(m.chord,i+m.inversion);
        if(m.shape==4)p=m.root+m.transpose+degree(m.scale,i+m.inversion);
        if(m.quantize==1 && !m.table.empty()){
            const float oct=std::floor(p/12);const int pc=(int)std::floor(p-oct*12);
            p=oct*12+m.table[pc%m.table.size()];
        }else if(m.quantize==2)p=nearest(p,m.scale);
        else if(m.quantize==3){
            const double f=std::pow(2.,(p-m.harmonicRoot)/12.);
            p=m.harmonicRoot+12*std::log2(std::max(1.,std::floor(f+.5)));
        }else if(m.quantize==4)p=nearest(p,m.chord);
        out.push_back(p+m.fine+harmony);
    }
    return out;
}
inline float progression(double beat,float beats,const std::vector<float>& steps,const std::vector<float>& scale){
    if(steps.empty())return 0;
    const int index=chordbox::wrapIndex((int)std::floor(beat/std::max(.0625f,beats)),(int)steps.size());
    return degree(scale,(int)std::round(steps[index]));
}
}
