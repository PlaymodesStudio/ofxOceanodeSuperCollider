#!/usr/bin/env python3
"""Use the real MOVING_SAWS state with production snapshot recall/blend code.
Proves the feedback toggle resets off when absent in older snapshots, including
recall after a saturated patch. Also checks legacy DSP except the intentionally updated Sub/Amp and smoothed pitch/oscillator definitions, covered by render tests.
"""
import argparse,hashlib,json,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2];HERE=pathlib.Path(__file__).resolve().parent
fixture=HERE/'fixtures/MOVING_SAWS.json'
assert hashlib.sha256(fixture.read_bytes()).hexdigest()=='0650ef23fe5868c2105f059a6b586222e95dc2418da87d6a5cc7176424b25652'
expected=json.loads((HERE/'fixtures/legacy-definitions.sha256.json').read_text());defs=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
# Independent sub/vibrato, tremolo and pitch interpolation are covered by audio render tests.
expected={name:sha for name,sha in expected.items() if not name.startswith(('PCSub','PCAmp','PCPitch','PCFm','PCOsc','PCCombR'))}
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--skip-dsp-hashes',action='store_true',help='Test snapshot behavior independently of the local compiled legacy binary cache')
args=parser.parse_args()
if not args.skip_dsp_hashes:
    for name,sha in expected.items():assert hashlib.sha256((defs/name).read_bytes()).hexdigest()==sha,name
code=(ROOT/'src/nodes/boxSynths/polyComb/scPolyComb.cpp').read_text()
helpers=code[code.index('bool logKey('):code.index('void scPolyComb::applyState(')]
start=code.index('void scPolyComb::applyState(')
apply=code[start:code.index('\n}\n',start)+3]
prelude=r'''
#include <nlohmann/json.hpp>
#include "nodes/boxSynths/polyComb/scPolyCombMigration.h"
#include "nodes/boxSynths/shared/scVoiceCountMorph.h"
#include "nodes/boxSynths/shared/scDroneMode.h"
#include "nodes/boxSynths/polyComb/scPolyCombPitchMorph.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <type_traits>
#include <string>
#include <vector>
using ofJson=nlohmann::json;
float ofLerp(float a,float b,float t){return a+(b-a)*t;}
template<class T>T ofClamp(T v,T a,T b){return std::clamp(v,a,b);}
template<class T>struct Param {
    std::string key;T value;
    std::string getEscapedName()const{return key;}
    T get()const{return value;}
    void set(T v);
};
std::function<void(int)> onVoices;
template<class T> void Param<T>::set(T v){value=v; if constexpr(std::is_same_v<T,int>) if(onVoices) onVoices(v);}
struct Handle { bool hasInConnection()const{return false;} };
struct ofxSCServer{};
struct ServerState{};
struct Mod {
    int lfoCount=4,envCount=1,extCount=0;
    std::vector<float> matrix=std::vector<float>(336,0);   // 23 sources + Delta, 14 targets
    ofJson params=ofJson::array();
    void refresh(){} void sendMatrix(){}
    void loadParamsJson(const ofJson& j){params=j;}
};
class scPolyComb {public:
    static constexpr int NUM_LFOS=8,NUM_ENVS=4,NUM_EXT=8,NUM_SOURCES=23,NUM_TARGETS=14,MAX_VOICES=32;
    Param<int> voices{"Voices",4};
    std::shared_ptr<Handle> voicesHandle=std::make_shared<Handle>();
    int userVoices=4; bool voicesFromSnapshot=false,applyingSnapshot=false,voiceRenderer=false;
    void prepareVoiceMorph(const ofJson&,const ofJson&,float){} // no running server in this recall test
    void syncVoiceMorph(){}
    ofJson pitchDefaults=ofJson::object(), deltaDefaults=ofJson::object();
    scPolyCombPitchMorph pitchMorph;
    Param<bool> scramble{"Scramble",false};
    static constexpr int IComb=0;
    template<class F>void forStates(F){} // no SC server in the recall fixture
    void sendPitch(ServerState&){} void sendIns(ServerState&,int){}
    bool combMapped()const{return false;}
    Param<bool> combSaturation{"Comb_Saturation",true};
    Param<float> combSatLevel{"Comb_Sat_Level",0};
    ofJson compDefaults=ofJson::object(), subDefaults=ofJson::object(), tremDefaults=ofJson::object(), blurDefaults=ofJson::object();
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
    const auto migrated=pcmigration::controls(moving["controls"]);
    auto check=[&](){
        for(auto i=node.subDefaults.begin();i!=node.subDefaults.end();++i)assert(node.current[i.key()]==i.value());
        assert(!node.combSaturation.value && node.combSatLevel.value==12.0f);
        for(auto i=node.compDefaults.begin();i!=node.compDefaults.end();++i)assert(node.current[i.key()]==i.value());
        for(auto i=moving["controls"].begin();i!=moving["controls"].end();++i)assert(node.current[i.key()]==migrated[i.key()]);
        {
            auto m=node.modulation.matrix; auto saved=moving["matrix"].get<std::vector<float>>();
            assert(m.size()==336);
            for(size_t i=0;i<m.size();i++) assert(m[i]==(i<saved.size()?saved[i]:0.0f));
        }
        assert(node.voices.value==node.userVoices);
        for(auto i=node.pitchDefaults.begin();i!=node.pitchDefaults.end();++i)assert(node.current[i.key()]==i.value());
        for(auto i=node.deltaDefaults.begin();i!=node.deltaDefaults.end();++i)assert(node.current[i.key()]==(migrated.contains(i.key()) ? migrated[i.key()] : i.value()));
        assert(node.modulation.lfoCount==moving["lfos"] && node.modulation.envCount==moving["envs"] && node.modulation.extCount==moving["exts"]);
        assert(node.modulation.params==moving["params"]);
    };
    node.pitchDefaults=ofJson::parse(R"defaults(PITCH_DEFAULTS)defaults");
    node.deltaDefaults=ofJson::parse(R"defaults(DELTA_DEFAULTS)defaults");
    for(auto i=node.pitchDefaults.begin();i!=node.pitchDefaults.end();++i)node.add(i.key(),i.value());
    for(auto i=node.deltaDefaults.begin();i!=node.deltaDefaults.end();++i)node.add(i.key(),i.value());
    onVoices=[&](int v){ if(!node.voicesFromSnapshot) node.userVoices=v; };
    node.applyState(moving,moving,1,true);check();
    // an imported sound: the maps, Delta routed, 24 voices; then MOVING_SAWS again
    auto imported=moving;
    imported["controls"]["Pitch_External"]=false;
    imported["controls"]["Comb_Map_On"]=true;
    imported["controls"]["Osc_Map_Mode"]=2;
    imported["controls"]["Osc_Map_Interval"]=5.0f;
    imported["controls"]["Osc_Map_Tones"]=std::vector<float>{0,3,7};
    imported["controls"]["Comb_Map_Harmonic_Base"]=97.0f;
    imported["controls"]["Delta_Gain"]=7.5f;
    imported["controls"]["Delta_Mix_A"]=1.0f;
    imported["controls"]["LFO1_Random_Type"]=2;
    imported["controls"]["LFO3_Random_Smooth"]=0.4f;
    imported["controls"]["Comb_Vib_Amount"]=0.75f;
    {auto m=std::vector<float>(336,0.0f); auto saved=moving["matrix"].get<std::vector<float>>(); std::copy(saved.begin(),saved.end(),m.begin()); m[23*14+0]=0.5f; m[23*14+5]=-0.25f; imported["matrix"]=m;}
    imported["voices"]=24;
    node.applyState(imported,imported,1,true);
    assert(node.current["Comb_Vib_Amount"]==0.75f);
    assert(node.voices.value==24 && node.current["Pitch_External"]==false && node.modulation.matrix[23*14]==0.5f);
    node.applyState(moving,moving,1,true);check();
    node.applyState(imported,moving,.5f,false);assert(node.voices.value==24);   // every contributing count is available
    node.applyState(imported,moving,.5f,true);assert(node.voices.value==24);
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
    assert(!node.combSaturation.value && node.combSatLevel.value==6.0f);
    assert(node.current["Comp_On"]==true && node.current["Comp_Threshold"]==-48.5f);
    node.applyState(moving,moving,1,true);check();
    node.applyState(moving,saturated,.25f,false);assert(!node.combSaturation.value);
    node.applyState(moving,saturated,.75f,true);assert(!node.combSaturation.value);
    node.applyState(moving,moving,1,true);check();
    auto malformed=moving;malformed["lfos"]="invalid count";
    bool rejected=false;
    try{node.applyState(malformed,moving,.5f,false);}catch(...){rejected=true;}
    assert(rejected && !node.applyingSnapshot);
    node.applyState(moving,moving,1,true);check(); // a failed recall must not latch listener suppression
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb_moving_') as d:
    from import_ensemble import controls_from_source
    comp={k:v['default'] for k,v in controls_from_source().items() if k.startswith('Comp_')}
    assert len(comp)==11
    pitch={'Pitch_External':True,'Pitch_Shift':0,'Comb_Map_On':False}
    for pre in ('Osc_Map_','Comb_Map_'):
        pitch.update({pre+'Shape':0,pre+'First':1.0,pre+'Interval':3.0,pre+'Transpose':0.0,pre+'Bend':1.0,pre+'Mode':0,pre+'Quantize':0,pre+'Root':0.0,
                      pre+'Tones':[0,2,4,5,7,9,11],pre+'Table':list(range(12)),pre+'Harmonic_Base':36.0,pre+'Fine_Tune':0.0,pre+'Span':0.0})
    delta={'Delta_LFO_A':0,'Delta_LFO_B':1,'Delta_Mix_A':0.0,'Delta_Mix_B':1.0,'Delta_Window':1000.0,'Delta_Gain':3.0}
    lfo_random={f'LFO{i}_Random_{k}':d for i in range(1,9) for k,d in (('Type',0),('Smooth',0.0))}
    lfo_random.update({f'LFO{i}_Steps':0 for i in range(1,9)})
    # Older snapshots keep the classic filter/echo, but always use the Reaktor comb.
    schema=controls_from_source()
    pitch.update({k:v['default'] for k,v in schema.items() if k.startswith('Comb_Vib_')})
    models={k:schema[k]['default'] for k in ('Ring_Model','Ring_Ratio','Ring_Width','Filter_Model','Comb_Model','Echo_Model','Echo_Hi','Echo_Lo','Echo_Res','Echo_Spread')}
    assert models['Ring_Model']==1 and models['Filter_Model']==0 and models['Comb_Model']==1 and models['Echo_Model']==0
    for k in models: assert f'c.key == "{k}"' in code, k
    lfo_random.update(models)
    # the defaults written in the node's source (no drift between this test and it)
    for k,v in list(pitch.items())+list(delta.items()):
        name=k.replace('Osc_Map_','').replace('Comb_Map_','').replace('_',' ')
        assert name in code, k
    assert 'Random Type' in (ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text() and '_Random_Type' in code
    main=main.replace('PITCH_DEFAULTS',json.dumps(pitch)).replace('DELTA_DEFAULTS',json.dumps({**delta,**lfo_random}))
    main=main.replace('COMP_DEFAULTS',json.dumps(comp)).replace('SUB_DEFAULTS',json.dumps({k:v['default'] for k,v in controls_from_source().items() if k.startswith('Sub_')}))
    (pathlib.Path(d)/'ofJson.h').write_text('#pragma once\n#include <nlohmann/json.hpp>\nusing ofJson=nlohmann::json;\n')
    src=pathlib.Path(d)/'test.cpp';src.write_text(prelude+helpers+apply+main)
    out=pathlib.Path(d)/'test'
    subprocess.run(['clang++','-std=c++17','-O2','-I'+d,'-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT.parent.parent/'libs/json/include'),'-I'+str(ROOT/'src'),str(src),'-o',str(out)],check=True)
    subprocess.run([str(out),str(fixture)],check=True)
audit='local compiled DSP hash audit skipped' if args.skip_dsp_hashes else f'{len(expected)} original compiled DSP definitions byte-identical'
print(f'PASS: MOVING_SAWS values/matrix/counts/voices restore with migrated ring/LFO models after an imported sound (pitch maps, Delta routes, 24 voices), saturation, compression and independent sub; option morphing; {audit}.')
