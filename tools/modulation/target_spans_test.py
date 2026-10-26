#!/usr/bin/env python3
"""Built-in targets span their controls: older saves rescaled, newer ones kept, polarity carried by morphs."""
import pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text()
methods=[]
for name in ['savedWithFullSpans','rescaleSavedMatrix']:
 a=s.index('scModulation::'+name+'(');a=s.rfind('\n',0,a)+1;b=s.index('\n}',a)+2;methods.append(s[a:b])
cpp=r'''
#include "nodes/boxSynths/shared/scModulationMorph.h"
#include <cassert>
#include <iostream>
template<class T>T ofClamp(T x,T a,T b){return std::clamp(x,a,b);}
struct scModulation{
 struct Target{std::string name;float range;std::string unit;bool unipolar=false;float legacyRange=0.0f;};
 std::vector<Target> targetList;
 int sourceCount()const{return 2;} int T()const{return (int)targetList.size();}
 static bool savedWithFullSpans(const ofJson&);
 void rescaleSavedMatrix(std::vector<float>&,const ofJson&)const;
};
METHODS
void near(float a,float b){assert(std::abs(a-b)<1e-5);}
int main(){
 scModulation m;
 // Cutoff 48 st -> 135 (its control's span); Reso unchanged; Drive 10 -> 30
 m.targetList={{"Cutoff",135,"st",false,48},{"Reso",1,""},{"Drive",30,"",false,10}};
 // an older save: the same semitones as before (amount x range unchanged)
 std::vector<float> old={1.0f,.5f,-.6f, .25f,0.f,1.f};
 auto v=old;m.rescaleSavedMatrix(v,ofJson::array());
 for(int i=0;i<6;i++){const auto& t=m.targetList[i%3];near(v[i]*t.range,old[i]*(t.legacyRange>0?t.legacyRange:t.range));}
 // a newer save (the @span marker) is left as saved
 auto keep=old;m.rescaleSavedMatrix(keep,ofJson::array({{{"key","@routes"},{"pol",1}},{{"key","@span"}}}));assert(keep==old);
 // a wrong-sized matrix is left alone
 std::vector<float> small={1.f};m.rescaleSavedMatrix(small,ofJson::array());near(small[0],1);
 // morphs carry the nearer side's polarity; an older side has none (its LFO routes stay bipolar)
 ofJson newer=ofJson::array({{{"key","@routes"},{"pol",1},{"bipolar",{3}}},{{"key","@span"}}});
 ofJson older=ofJson::array();
 auto near0=scModulationMorph::parameters(newer,older,.2f,{},{});
 auto near1=scModulationMorph::parameters(newer,older,.8f,{},{});
 bool routes0=false,routes1=false,span1=false;
 for(auto& e:near0) if(e["key"]=="@routes"){routes0=true;assert(e["bipolar"]==ofJson::array({3}));}
 for(auto& e:near1){ if(e["key"]=="@routes") routes1=true; if(e["key"]=="@span") span1=true; }
 assert(routes0 && !routes1 && span1);
 std::cout<<"PASS: older saves rescaled to the full-span ranges, newer saves kept, morph polarity from the nearer side.\n";
}
'''.replace('METHODS','\n'.join(methods))
with tempfile.TemporaryDirectory(prefix='target-spans-') as d:
 w=pathlib.Path(d);(w/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src'),'-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),
  '-I'+str(ROOT.parents[1]/'libs/json/include'),str(w/'test.cpp'),'-o',str(w/'test')],check=True)
 subprocess.run([str(w/'test')],check=True)
