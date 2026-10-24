#!/usr/bin/env python3
"""Production map-output morph arithmetic, metadata and live roots (ASan/UBSan)."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
TEST=r'''
#include "nodes/boxSynths/polyComb/scPolyCombPitchMorph.h"
#include <cassert>
#include <fstream>
#include <iostream>
void near(float a,float b,float tolerance=3e-5f){assert(std::isfinite(a)&&std::abs(a-b)<tolerance);}
ofJson sound(int osc,int comb,bool own=true){return {{"controls",{
    {"Osc_Map_Mode",osc},{"Osc_Map_Version",2},{"Osc_Map_Skew",-1.f},{"Osc_Map_Interval",osc==3?1.f:3.f},
    {"Osc_Map_Table",{0.f,4.f,7.f}},{"Osc_Map_Tones",{0.f,2.f,7.f}},{"Osc_Map_Harmonic_Base",36.f},
    {"Comb_Map_Mode",comb},{"Comb_Map_Version",2},{"Comb_Map_Skew",-1.f},{"Comb_Map_Interval",comb==3?2.f:5.f},
    {"Comb_Map_Table",{0.f,3.f,7.f,10.f}},{"Comb_Map_Harmonic_Base",43.f},{"Comb_Map_On",own}
}}};}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream stream(argv[1]);ofJson reference;stream>>reference;
    const auto& pair=reference.at("pairs").at("map_modes_and_inheritance");
    scPolyCombPitchMorph referenceMorph;referenceMorph.begin(pair.at("from"),pair.at("to"),.25f,ofJson::object());
    const auto quarter=referenceMorph.evaluate(4,pair.at("quarter").at("root").get<float>());
    for(int i=0;i<4;++i){
        near(quarter.oscillator[i],pair.at("quarter").at("oscillator")[i].get<float>());
        near(quarter.resonator[i],pair.at("quarter").at("resonator")[i].get<float>());
    }
    const auto defaults=ofJson::object();
    // Every algorithm pair, oscillator/resonator independence and every bank
    // width. Expected values evaluate each endpoint separately, never blended
    // intervals, rounded indices, modes or scale tables.
    for(int n=1;n<=32;++n)for(int ma=0;ma<4;++ma)for(int mb=0;mb<4;++mb){
        const auto a=sound(ma,(ma+1)%4),b=sound(mb,(mb+2)%4);
        scPolyCombPitchMorph pa,pb,m;pa.begin(a,a,1,defaults);pb.begin(b,b,1,defaults);
        const auto x=pa.evaluate(n,60),y=pb.evaluate(n,60);
        for(float t:{0.f,1e-9f,.00101f,.1f,.4999f,.5001f,.75f,1.f}){
            m.begin(a,b,t,defaults);const auto z=m.evaluate(n,60);
            for(int i=0;i<n;++i){
                near(z.oscillator[i],x.oscillator[i]+t*(y.oscillator[i]-x.oscillator[i]));
                near(z.resonator[i],x.resonator[i]+t*(y.resonator[i]-x.resonator[i]));
            }
        }
    }
    auto a=sound(0,1,false),b=sound(3,2,true);
    a["controls"]["Comb_Map_Transpose"]=-7.f;b["controls"]["Comb_Map_Transpose"]=5.f;
    scPolyCombPitchMorph m;m.begin(a,b,.25f,defaults);
    auto z=m.evaluate(12,60);near(z.own,.25f);
    // The actual DSP receives residual + (1-own)*the live oscillator bus.
    // Preserve the inherited share of oscillator transpose/detune, too.
    for(int i=0;i<12;++i){
        const float residual=z.resonator[i]-(1-z.own)*z.oscillator[i];
        near(residual+(1-z.own)*(z.oscillator[i]+3.f),z.resonator[i]+.75f*3.f);
    }
    auto current=ofJson{{"controls",ofJson::object()}};m.capture(current);
    assert(current.contains(scPolyCombPitchMorph::key));
    auto c=sound(1,3);scPolyCombPitchMorph interrupted;interrupted.begin(current,c,.4f,defaults);
    scPolyCombPitchMorph target;target.begin(c,c,1,defaults);const auto dest=target.evaluate(12,60);
    auto next=interrupted.evaluate(12,60);
    for(int i=0;i<12;++i)near(next.oscillator[i],.6f*z.oscillator[i]+.4f*dest.oscillator[i]);
    // Recall/reverse the saved blend, and respond to a newly played root.
    interrupted.begin(current,current,1,defaults);auto recall=interrupted.evaluate(12,60);
    for(int i=0;i<12;++i)near(recall.oscillator[i],z.oscillator[i]);
    const auto shifted=interrupted.evaluate(12,61);
    for(int i=0;i<12;++i)near(shifted.oscillator[i],z.oscillator[i]+1);
    // A connected mode/root overrides both endpoint descriptions immediately.
    const ofJson override={{"Osc_Map_Mode",0},{"Osc_Map_Interval",2.f},{"Osc_Map_Transpose",7.f},{"Comb_Map_On",false}};
    const auto driven=m.evaluate(12,60,override);near(driven.own,0);
    for(int i=0;i<12;++i)near(driven.oscillator[i],69.f+2*i);
    // Scale changes with the same dropdown mode must interpolate output notes,
    // not create intermediate rounded pitch classes or table lengths.
    a=sound(1,1);b=a;b["controls"]["Osc_Map_Table"]={0.f,2.f,3.f,7.f,10.f};
    b["controls"]["Osc_Map_Num_Waves"]=3.f;b["controls"]["Osc_Map_Invert"]=true;
    b["controls"]["Scramble"]=true;
    scPolyCombPitchMorph left,right;left.begin(a,a,1,defaults);right.begin(b,b,1,defaults);
    m.begin(a,b,.2f,defaults);z=m.evaluate(12,62);
    const auto l=left.evaluate(12,62),r=right.evaluate(12,62);
    for(int i=0;i<12;++i)near(z.oscillator[i],.8f*l.oscillator[i]+.2f*r.oscillator[i]);
    // Quantization stays inside each endpoint; played roots remain live even
    // when a saved endpoint asks for transposition before scale quantization.
    b["controls"]["Pitch_Shift"]=1;b["controls"]["Osc_Map_Mode"]=2;
    m.begin(a,b,.2f,defaults);right.begin(b,b,1,defaults);
    z=m.evaluate(12,65);const auto quantized=right.evaluate(12,65),played=left.evaluate(12,65);
    for(int i=0;i<12;++i)near(z.oscillator[i],.8f*played.oscillator[i]+.2f*quantized.oscillator[i]);
    // Ordinary endpoints/same-map morphs release the override. Invalid optional
    // metadata falls back to the ordinary sound instead of breaking recall.
    m.begin(a,a,1,defaults);m.finish();assert(!m.active());
    auto bad=a;bad[scPolyCombPitchMorph::key]={{{"weight",-1},{"controls",ofJson::object()}}};
    m.begin(bad,a,.5f,defaults);assert(m.parts.size()==1);
    m.clear();assert(!m.active());
    std::cout<<"PASS: all pitch-map modes/widths, early interpolation, scales/geometry, inherited resonators, live roots/connections and stored/interrupted blends\n";
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb-pitch-map-morph-') as work:
    work=Path(work);(work/'test.cpp').write_text(TEST)
    subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
        '-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT/'src'),
        '-I'+str(ROOT.parents[1]/'libs/json/include'),str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test'),str(ROOT/'tools/polycomb/fixtures/morph_reference.json')],check=True)
