#!/usr/bin/env python3
"""Exercise production legacy envelope recall with the shipped chord preset."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


source = (ROOT / 'src/nodes/boxSynths/additiveBox/scAdditiveBox.cpp').read_text()
methods = '\n'.join(block(source, sig) for sig in (
    'void scAdditiveBox::presetRecallAfterSettingParameters(',
    'bool scAdditiveBox::levelEnvelope(',
    'void scAdditiveBox::migrateOldEnvelope(',
    'void scAdditiveBox::migrateOldModulation('))
mod_source = (ROOT / 'src/nodes/boxSynths/shared/scModulation.cpp').read_text()
cpp = r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using ofJson = nlohmann::json;
template<class T> T ofClamp(T x,T lo,T hi){return std::clamp(x,lo,hi);}
std::string ofToString(int x){return std::to_string(x);}
struct Action {
    std::string key;
    std::function<void(const ofJson&)> load;
};
struct scModulation {
    static constexpr int MAX_LFOS=8, MAX_ENVS=4, MAX_EXT=8, SRC_ENV=8;
    int lfoCount=0, envCount=1, extCount=0, maxLfos=8, maxEnvs=4, maxExts=8;
    bool randomShared=false;
    std::vector<float> matrix=std::vector<float>(22*10,0);
    int sourceCount()const{return 22;} int T()const{return 10;}
    static std::vector<float> fitMatrix(std::vector<float> m,int,int){return m;}
    void loadGeneratorState(const ofJson&,const std::string&){} void loadParamsJson(const ofJson&){} void afterLoad(){}
    void load(const ofJson&,const std::string&);
};
struct scAdditiveBox {
    static constexpr int NUM_TARGETS=10;
    struct Drone {
        bool value=false;
        std::string getEscapedName()const{return "Drone";}
        void operator=(bool v){value=v;}
    } drone;
    scModulation mod;
    struct {std::string currentPreset;} snaps;
    float viewHeight=400;
    std::vector<Action> publishActions;
    std::map<std::string,float> values;
    scAdditiveBox(){
        mod.matrix[scModulation::SRC_ENV*NUM_TARGETS+3]=1;
        for(int e=1;e<=4;++e){
            for(const std::string name:{"Attack","Decay","Sustain","Release",
                    "Attack_Curve","Decay_Curve","Release_Curve"}){
                const auto key="Env"+std::to_string(e)+"_"+name;
                values[key]=0;
                publishActions.push_back({key,[this,key](const ofJson& v){values[key]=v.get<float>();}});
            }
        }
    }
    bool isPublished(const std::string&)const{return false;}
    const Action* findPublishAction(const std::string& key)const{
        for(const auto& a:publishActions)if(a.key==key)return &a;
        return nullptr;
    }
    bool levelEnvelope()const;
    void migrateOldEnvelope(const ofJson&);
    void migrateOldModulation(const ofJson&);
    void presetRecallAfterSettingParameters(ofJson&);
};
MOD_LOAD
METHODS
int main(int argc,char** argv){
    assert(argc==2);
    std::ifstream f(argv[1]);ofJson bank;f>>bank;
    auto chord=bank.at("slots").at(1);
    assert(chord.at("name")=="Astres 01 Chord");
    scAdditiveBox box;
    box.presetRecallAfterSettingParameters(chord);
    assert(box.mod.envCount==1 && box.levelEnvelope());
    for(const std::string name:{"Attack","Decay","Sustain","Release"})
        assert(std::abs(box.values.at("Env1_"+name)-chord.at(name).at(0).get<float>())<1e-6f);
    assert(box.values.at("Env1_Sustain")==1);
    // Recalling repeatedly must replace the prior routing, not stack envelopes.
    box.mod.envCount=4;box.mod.lfoCount=8;box.mod.extCount=8;
    box.mod.matrix[0]=.8f;
    box.presetRecallAfterSettingParameters(chord);
    assert(box.mod.envCount==1 && box.mod.lfoCount==0 && box.mod.extCount==0);
    assert(box.mod.matrix[0]==0 && box.values.at("Env1_Sustain")==1);
    // Explicit modern modulation and its zero sustain remain authoritative.
    auto modern=chord;
    modern["Env1_Sustain"]=0;
    modern["additiveBoxModEnvs"]=1;
    modern["additiveBoxModMatrix"]=box.mod.matrix;
    modern["additiveBoxModMatrix"][0]=.25f;
    box.presetRecallAfterSettingParameters(modern);
    assert(box.mod.matrix[0]==.25f && box.mod.envCount==1);
    assert(box.values.at("Env1_Sustain")==0);
    std::cout<<"PASS: legacy chord ADSR replaces default and prior routing; modern envelope preserved\n";
}
'''.replace('MOD_LOAD', block(mod_source, 'void scModulation::load(')).replace('METHODS', methods)

with tempfile.TemporaryDirectory(prefix='additive-envelope-') as work:
    work = Path(work)
    (work / 'test.cpp').write_text(cpp)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT.parents[1] / 'libs/json/include'),
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test'),
                    str(ROOT / 'presets/nodeSnapshots/AdditiveBox/banks/Demo and Astres.json')], check=True)
