#!/usr/bin/env python3
"""MacroMixer morphs each Box's saved sound on the running node (its morph hooks),
instead of replacing the node with a crossfaded second instance.

Production Box code and MacroMixer, ASan/UBSan, no SuperCollider server. For each
Box: a saved patch, the same patch with one sound value and Voices changed; the
morph must keep the node, report no replacement, switch Voices once, arrive at
the target value, and a cancelled morph must return to the start.

    python3 tools/boxes/macro_morph_test.py [box ...]
"""
from pathlib import Path
import os,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT.parent/'ofxOceanode/tests'))
import macro_mixer_polycomb_regression as native
mixer=native.mixer

# class, header, node name, saved key holding the sound ('': the top level)
BOXES={
    'analogBox':('scAnalogBox','scAnalogBox.h','analogBox','analogBox'),
    'echoBox':('scEchoBox','scEchoBox.h','EchoBox','echoBox'),
    'physicalBox':('scPhysicalBox','scPhysicalBox.h','physicalBox','physicalBox'),
    'steamPipeBox':('scSteamPipeBox','scSteamPipeBox.h','steampipeBox','steampipeBox'),
    'aliasingBox':('scAliasingBox','scAliasingBox.h','aliasingBox','aliasingBox'),
    'fmBox':('scFMBox','scFMBox.h','fmBox','fmBox'),
    'sampleBox':('scSampleBox','scSampleBox.h','SampleBox','sampleBoxPads'),
    'drumKitBox':('scDrumKitBox','scDrumKitBox.h','DrumKitBox','drumKitBoxPads'),
    'slicerBox':('scSlicerBox','scSlicerBox.h','SlicerBox',''),
    'pulsarBox':('scPulsarBox','scPulsarBox.h','PulsarBox',''),
    'wavetableBox':('scWavetableBox','scWavetableBox.h','WavetableBox',''),
    'grainBox':('scGrainBoxPoly','scGrainBoxPoly.h','GrainBox',''),
    'additiveBox':('scAdditiveBox','scAdditiveBox.h','AdditiveBox',''),   # saved flat; Voices blended by its sound
}

def cases(boxes):
    prelude=''.join(f'#include "{BOXES[b][1]}"\n' for b in boxes)
    registry=''.join(f'    registry->registerModel<{BOXES[b][0]}>("SuperCollider",std::vector<serverManager*>{{}});\n' for b in boxes)
    runs=''.join(f'    box(registry,types,transport,parent,scratch,"SC {BOXES[b][2]}","{BOXES[b][3]}");\n' for b in boxes)
    return prelude,r'''
static void near(float a,float b) { if(!std::isfinite(a)||std::abs(a-b)>=.0001f*(1+std::abs(b))) { std::cerr<<a<<" != "<<b<<"\n"; std::abort(); } }
static ofJson saved(ofxOceanodeNodeModel& model) { ofJson j; model.presetSave(j); return j; }
// a saved number, or a number saved as text
static bool numeric(const ofJson& j,float& v) {
    if(j.is_number_float()) { v=j.get<float>(); return true; }
    if(!j.is_string()) return false;
    const auto& t=j.get<std::string>(); char* end=nullptr; v=std::strtof(t.c_str(),&end);
    return !t.empty() && end==t.c_str()+t.size() && t.find('.')!=std::string::npos;
}
static float number(const ofJson& j);
static float number(const ofJson& j) {   // any number, "48" too
    if(j.is_number()) return j.get<float>();
    if(j.is_array() && j.size()==1) return number(j[0]);
    if(j.is_string()) {   // a vector parameter's text: its first value
        const auto& t=j.get<std::string>(); const auto at=t.find_first_of("-0123456789.");
        char* end=nullptr; const float v=at==std::string::npos?NAN:std::strtof(t.c_str()+at,&end);
        if(end && end!=t.c_str()+at) return v;
    }
    return NAN;
}
// The first sound number that is not the node's own snapshot settings
static bool firstNumber(const ofJson& j,ofJson::json_pointer at,ofJson::json_pointer& found) {
    if(j.is_object()) { for(auto it=j.begin();it!=j.end();++it) {
        const auto& k=it.key();
        if(k.rfind("Snapshot",0)==0||k.rfind("Morph",0)==0||k.rfind("Ext_",0)==0||k=="path"||k=="Voices") continue;
        if(firstNumber(it.value(),at/k,found)) return true; } }
    else if(j.is_array()) { for(size_t i=0;i<j.size();++i) if(firstNumber(j[i],at/std::to_string(i),found)) return true; }
    else if(float v; numeric(j,v) && std::abs(v)>1e-3f) { found=at; return true; }
    return false;
}
static void box(std::shared_ptr<ofxOceanodeNodeRegistry> registry,std::shared_ptr<ofxOceanodeTypesRegistry> types,
                std::shared_ptr<ofxOceanodeTransport> transport,ofxOceanodeContainer& parent,const std::string& scratch,const std::string& model,const std::string& key) {
    namespace fs=std::filesystem;
    const std::string stem=ofJoinString(ofSplitString(model," "),"_"),file=stem+"_1.json";
    const auto a=scratch+"/"+stem+"-a",b=scratch+"/"+stem+"-b";
    std::vector<std::string> m0Keys;   // saved keys the node blends itself
    {
        ofxOceanodeContainer c(registry,types,transport); c.setCanvasID(stem+"Source");
        auto& out=c.createNode(std::make_unique<router<float>>("f",0.f,-100.f,100.f),1).getNodeModel();
        out.getParameter<std::string>("Name")="Value";
        m0Keys=c.createNode(registry->create(model),1).getNodeModel().morphKeys().blended;
        assert(c.savePreset(a));
    }
    assert(fs::exists(a+"/"+file)); fs::copy(a,b,fs::copy_options::recursive);
    auto source=ofLoadJson(a+"/"+file); assert(key.empty()||source.contains(key));
    const auto root=key.empty()?ofJson::json_pointer():ofJson::json_pointer("/"+key);
    ofJson::json_pointer at; bool found=false;
    if(!key.empty()) found=firstNumber(source[root],root,at);
    else for(const auto& k:m0Keys) if(!found && source.contains(k)) found=firstNumber(source[k],ofJson::json_pointer("/"+k),at);
    assert(found);
    const float from=number(source[at]),to=from*.75f;
    auto target=source; target[at]=source[at].is_string()?ofJson(ofToString(to)):ofJson(to);
    // GrainBox also saves engine 1's controls under legacy lowercase names: a real save has them agree
    if(const auto legacy=ofToLower(at.to_string().substr(1)); key.empty() && target.contains(legacy) && legacy!=at.to_string().substr(1))
        target[legacy]=to;
    const bool hasVoices=source.contains("Voices"); int fromVoices=0,toVoices=0;
    const bool switched=hasVoices&&std::find(m0Keys.begin(),m0Keys.end(),"Voices")==m0Keys.end();
    if(hasVoices) {   // registered parameters are saved as text
        fromVoices=source["Voices"].is_string()?ofToInt(source["Voices"].get<std::string>()):source["Voices"].get<int>();
        toVoices=fromVoices==1?2:fromVoices-1; target["Voices"]=ofToString(toVoices);
    }
    ofSaveJson(b+"/"+file,target);
    std::cout<<model<<": "<<at.to_string()<<" "<<from<<" -> "<<to<<(hasVoices?", Voices "+ofToString(fromVoices)+" -> "+ofToString(toVoices):"")<<"\n";

    MacroMixerRuntime r(parent); assert(r.importMacro(a)&&r.importMacro(b));
    r.request(a,0,0,true); r.update(0); const auto* graph=r.origin(); assert(graph);
    auto* node=graph->container->getDynamicNodes().at(model).begin()->second.get();
    auto& m=node->getNodeModel();
    auto value=[&]() {   // a registered parameter is saved by its node, not presetSave
        const auto j=saved(m); if(j.contains(at)) { const float v=number(j[at]); if(!std::isfinite(v)) std::cerr<<"saved "<<j[at].dump()<<"\n"; return v; }
        auto& group=m.getParameterGroup(); const auto name=at.to_string().substr(1);
        for(int i=0;i<group.size();++i) if(auto* q=dynamic_cast<ofxOceanodeAbstractParameter*>(&group.get(i)); q && q->getEscapedName()==name) {
            if(q->valueType()==typeid(std::vector<float>).name()) { const auto& v=q->cast<std::vector<float>>().getParameter().get(); return v.empty()?NAN:v[0]; }
            if(q->valueType()==typeid(float).name()) return q->cast<float>().getParameter().get();
            return number(ofJson(q->toString()));
        }
        std::cerr<<"no "<<name<<" in "<<j.dump().substr(0,300)<<"\n"; return NAN;
    };
    near(value(),from);
    const auto details=r.sourceDetails(r.importedMacro(b)->id());
    for(const auto& d:details) if(d.kind=="replaced"||d.kind=="fallback") { std::cerr<<model<<": "<<d.kind<<" "<<d.location<<" "<<d.message<<"\n"; std::abort(); }
    assert(std::any_of(details.begin(),details.end(),[](const auto& d){return d.kind=="node";}));
    r.request(b,1000,0,true); r.update(10); assert(r.morphingInPlace()&&r.origin()==graph);
    r.update(260); if(switched) assert(m.getParameter<int>("Voices")==fromVoices);
    r.update(760); if(switched) assert(m.getParameter<int>("Voices")==toVoices);
    r.finish(); r.update(770); assert(!r.transitioning()&&r.origin()==graph);
    assert(graph->container->getDynamicNodes().at(model).begin()->second.get()==node);
    near(value(),to); if(hasVoices) assert(m.getParameter<int>("Voices")==toVoices);
    // Cancelled half way back: the sound the morph started from
    r.request(a,1000,0,true); r.update(780); r.update(1030);
    const_cast<MacroMixerSource::Graph*>(r.destination())->cancelMorph();
    near(value(),to);
    r.clear();
    std::cout<<model<<": in place, Voices switched once, arrived, cancel restored\n";
}
int main(int argc,char** argv) {
    assert(argc==3); namespace fs=std::filesystem;
    const std::string scratch=argv[2]; fs::current_path(scratch); ofDisableDataPath(); ofSeedRandom();
    auto window=std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
    auto registry=std::make_shared<ofxOceanodeNodeRegistry>(); auto types=std::make_shared<ofxOceanodeTypesRegistry>();
    registry->registerModel<router<float>>("Router","f",0.f,-FLT_MAX,FLT_MAX);
'''+registry+r'''    auto transport=std::make_shared<ofxOceanodeTransport>();
    ofxOceanodeContainer parent(registry,types,transport); parent.setCanvasID("BoxMorph");
'''+runs+r'''    std::cout<<"Box MacroMixer morphs passed.\n";
}
'''

def main():
    boxes=sys.argv[1:] or list(BOXES)
    prelude,case=cases(boxes)
    mixer.PRELUDE+=prelude
    mixer.CASES=case
    sources=[str(mixer.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
    sources+=[str(p) for p in sorted((ROOT/'src').rglob('*.cpp')) if 'registration' not in str(p) and not p.name.startswith('ofxOceanodeSuperCollider')]
    sources+=[str(p) for p in sorted((native.SUPERCOLLIDER/'src').glob('*.cpp'))]
    sources+=[str(p) for p in sorted((native.OSC/'src').glob('*.cpp'))]
    sources+=[str(p) for p in sorted((native.OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
    includes=[ROOT/'src',*sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),
              native.SUPERCOLLIDER/'src',native.OSC/'src',native.OSC/'libs/oscpack/src',
              *sorted({p.parent for p in (native.OSC/'libs/oscpack/src').rglob('*.h')})]
    os.environ.setdefault('OCEANODE_TEST_JOBS','8')
    mixer.main(extra_sources=sources,extra_includes=includes,std='c++20')

if __name__=='__main__': main()
