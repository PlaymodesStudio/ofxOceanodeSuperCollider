#!/usr/bin/env python3
"""Use the real MOVING_SAWS state with production snapshot recall/blend code.
Proves the feedback toggle resets off when absent in older snapshots, including
recall after a saturated patch. Also verifies original compiled DSP is unchanged.
"""
import hashlib,json,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2];HERE=pathlib.Path(__file__).resolve().parent
fixture=HERE/'fixtures/MOVING_SAWS.json'
assert hashlib.sha256(fixture.read_bytes()).hexdigest()=='0650ef23fe5868c2105f059a6b586222e95dc2418da87d6a5cc7176424b25652'
expected=json.loads((HERE/'fixtures/legacy-definitions.sha256.json').read_text());defs=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
for name,sha in expected.items():assert hashlib.sha256((defs/name).read_bytes()).hexdigest()==sha,name
code=(ROOT/'src/nodes/scPolyComb.cpp').read_text()
helpers=code[code.index('bool logKey('):code.index('void scPolyComb::applyState(')]
apply=code[code.index('void scPolyComb::applyState('):code.index('// ── Library (shared files)')]
prelude=r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>
using ofJson=nlohmann::json;
float ofLerp(float a,float b,float t){return a+(b-a)*t;}
template<class T>T ofClamp(T v,T a,T b){return std::clamp(v,a,b);}
template<class T>struct Param {
    std::string key;T value;
    std::string getEscapedName()const{return key;}
};
struct Mod {
    int lfoCount=4,envCount=1,extCount=0;
    std::vector<float> matrix=std::vector<float>(322,0);
    ofJson params=ofJson::array();
    void refresh(){} void sendMatrix(){}
    void loadParamsJson(const ofJson& j){params=j;}
};
class scPolyComb {public:
    static constexpr int NUM_LFOS=8,NUM_ENVS=4,NUM_EXT=8,NUM_SOURCES=23,NUM_TARGETS=14;
    Param<bool> combSaturation{"Comb_Saturation",true};
    Param<float> combSatLevel{"Comb_Sat_Level",0};
    ofJson compDefaults=ofJson::object(), subDefaults=ofJson::object();
    Mod modulation;
    struct Control {std::string key;std::function<ofJson()> save;std::function<void(const ofJson&)> load;};
    std::vector<Control> controls;
    ofJson current=ofJson::object();
    bool snapExcluded(const std::string&)const{return false;}
    bool drivenByConnection(const std::string&)const{return false;}
    void applyState(const ofJson&,const ofJson&,float,bool);
    void add(const std::string& k,const ofJson& initial){
        current[k]=initial;
        controls.push_back({k,[this,k](){return current[k];},[this,k](const ofJson& v){
            current[k]=v;
            if(k==combSaturation.key)combSaturation.value=v.get<bool>();
            if(k==combSatLevel.key)combSatLevel.value=v.get<float>();
        }});
    }
};
namespace {
'''
main=r'''
int main(int argc,char** argv){
    std::ifstream in(argv[1]);ofJson moving;in>>moving;
    assert(moving["name"]=="MOVING_SAWS");
    scPolyComb node;
    node.compDefaults=ofJson::parse(R"defaults(COMP_DEFAULTS)defaults");
    node.subDefaults=ofJson::parse(R"defaults(SUB_DEFAULTS)defaults");
    for(auto i=moving["controls"].begin();i!=moving["controls"].end();++i)node.add(i.key(),i.value());
    node.add("Comb_Saturation",true);node.add("Comb_Sat_Level",0.0f);
    for(auto i=node.compDefaults.begin();i!=node.compDefaults.end();++i)node.add(i.key(),i.key()=="Comp_On" ? ofJson(true) : i.value());
    for(auto i=node.subDefaults.begin();i!=node.subDefaults.end();++i)node.add(i.key(),i.key()=="Sub_On" ? ofJson(true) : i.value());
    auto check=[&](){
        for(auto i=node.subDefaults.begin();i!=node.subDefaults.end();++i)assert(node.current[i.key()]==i.value());
        assert(!node.combSaturation.value && node.combSatLevel.value==12.0f);
        for(auto i=node.compDefaults.begin();i!=node.compDefaults.end();++i)assert(node.current[i.key()]==i.value());
        for(auto i=moving["controls"].begin();i!=moving["controls"].end();++i)assert(node.current[i.key()]==i.value());
        assert(ofJson(node.modulation.matrix)==moving["matrix"]);
        assert(node.modulation.lfoCount==moving["lfos"] && node.modulation.envCount==moving["envs"] && node.modulation.extCount==moving["exts"]);
        assert(node.modulation.params==moving["params"]);
    };
    node.applyState(moving,moving,1,true);check();
    auto saturated=moving;
    saturated["controls"]["Comb_Saturation"]=true;
    saturated["controls"]["Comb_Sat_Level"]=6.0f;
    saturated["controls"]["Filter_Type"]=5;
    saturated["controls"]["Sub_On"]=true;
    saturated["controls"]["Sub_Level"]=0.7f;
    saturated["controls"]["Comp_On"]=true;
    saturated["controls"]["Comp_Threshold"]=-48.5f;
    node.applyState(saturated,saturated,1,true);
    assert(node.combSaturation.value && node.combSatLevel.value==6.0f);
    assert(node.current["Comp_On"]==true && node.current["Comp_Threshold"]==-48.5f);
    node.applyState(moving,moving,1,true);check();
    node.applyState(moving,saturated,.25f,false);assert(!node.combSaturation.value);
    node.applyState(moving,saturated,.75f,true);assert(node.combSaturation.value);
    node.applyState(moving,moving,1,true);check();
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb_moving_') as d:
    from import_ensemble import controls_from_source
    comp={k:v['default'] for k,v in controls_from_source().items() if k.startswith('Comp_')}
    assert len(comp)==11
    main=main.replace('COMP_DEFAULTS',json.dumps(comp)).replace('SUB_DEFAULTS',json.dumps({k:v['default'] for k,v in controls_from_source().items() if k.startswith('Sub_')}))
    src=pathlib.Path(d)/'test.cpp';src.write_text(prelude+helpers+apply+main)
    out=pathlib.Path(d)/'test'
    subprocess.run(['clang++','-std=c++17','-O2','-I'+str(ROOT.parent.parent/'libs/json/include'),str(src),'-o',str(out)],check=True)
    subprocess.run([str(out),str(fixture)],check=True)
print(f'PASS: actual MOVING_SAWS values/matrix/counts restore unchanged after saturation, compression and independent sub; option morphing; {len(expected)} original compiled DSP definitions byte-identical.')
