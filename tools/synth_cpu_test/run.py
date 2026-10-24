#!/usr/bin/env python3
"""Exercise the production math without starting OF, ImGui or an audio server.

Optional --formula-baseline points to a previous formula.h for differential
evaluation and allocation measurements. All generated files live in a temp dir.
"""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--formula-baseline", type=pathlib.Path)
args = parser.parse_args()

prelude = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
static size_t allocations = 0;
void* operator new(size_t n) { ++allocations; if(auto p=std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
template<class T> T ofClamp(T v, T lo, T hi) { return std::max(lo,std::min(hi,v)); }
float ofMap(float v,float a,float b,float c,float d,bool clamp) {
    float r=c+(v-a)/(b-a)*(d-c); return clamp ? ofClamp(r,std::min(c,d),std::max(c,d)) : r;
}
float ofLerp(float a,float b,float t) { return a+(b-a)*t; }
constexpr float PI=3.14159265358979323846f, TWO_PI=2*PI;
using ImU32=unsigned;
#define IM_COL32(r,g,b,a) 0u
template<class T> struct Param { T value{}; const T& get() const { return value; } void set(T v) { value=std::move(v); } };
std::string ofToString(int n) { return std::to_string(n); }
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
NullLog ofLogError(const char*) { return {}; }
bool same(float a,float b) { return std::memcmp(&a,&b,sizeof(a))==0 || (std::isnan(a)&&std::isnan(b)); }
template<class A,class B> void equal(const A& a,const B& b) {
    assert(a.size()==b.size()); for(size_t i=0;i<a.size();++i) assert(same(a[i],b[i]));
}
'''

eq = (ROOT / "src/ui/scEQEditor.h").read_text()
eq = eq[eq.index("class scEQEditor {"):eq.index("    static float logFreqToX")]
eq += "};\n"

mod = (ROOT / "src/nodes/boxSynths/shared/scModulation.cpp").read_text()
mod_header = (ROOT / "src/nodes/boxSynths/shared/scModulation.h").read_text()
helpers = mod[mod.index("float reflectUnit("):mod.index("// 2D Perlin noise")]
helpers = helpers[:helpers.rfind("}")]  # remove anonymous namespace's closing brace
cache = mod_header[mod_header.index("    struct TableCache"):mod_header.index("    struct ParamTarget")]
mod_class = '''class scModulation { public:
static constexpr int MAX_LFOS=8;
bool grainExtras=false;
std::array<Param<std::vector<float>>,8> lfoGain,lfoPow,lfoBiPow,lfoRound,lfoSkew,lfoPw;
std::array<Param<int>,8> lfoSteps;
std::array<Param<bool>,8> lfoNoise,lfoCentered;
std::vector<float> lfoTransfer(int,int) const;
std::vector<float> lfoTable(int,int) const;
''' + cache + "};\n"
transfer = mod[mod.index("std::vector<float> scModulation::lfoTransfer"):mod.index("// Setup")]
transfer = transfer[:transfer.rfind("// ═")]
cycle = mod[mod.index("std::vector<float> scModulation::lfoTable"):mod.index("float scModulation::lfoVoiceOffset")]


def formula_class(source, name):
    tokens = source[source.index("\tstruct Token {"):source.index("\tenum class PortKind")]
    values = source[source.index("\tstruct Value {"):source.index("\t// ===== Dynamic ports")]
    evaluator = source[source.index("\t// ===== Tokenizer ====="):source.rfind("\n};")]
    calculate = source[source.index("\tvoid calculate() {"):source.index("\t// ===== Compiler pipeline")]
    return "class " + name + " { public:\n" + tokens + values + r'''
struct Statement { std::string target; RPN code; };
bool formulaValid=true;
std::vector<Statement> program;
std::map<int,std::shared_ptr<Param<std::vector<float>>>> inputParamRefs,outputParamRefs;
''' + calculate + evaluator + "};\n"


formula_path = ROOT.parent / "ofxSantiNodes/src/formula.h"
formula = formula_class(formula_path.read_text(), "Formula")
baseline = formula_class(args.formula_baseline.read_text(), "Baseline") if args.formula_baseline else ""

polycomb = (ROOT / "src/nodes/boxSynths/polyComb/scPolyComb.cpp").read_text()
osc_listener = polycomb[polycomb.index("    auto toOsc ="):polycomb.index("    auto toAllOsc =")]
transition_test = r'''
class OscillatorTransitions { public:
    struct Part { void* synth=nullptr; bool running=false; float offAt=-1; };
    struct ServerState { std::array<Part,1> osc; } state;
    using ofxSCServer=int;
    bool wanted=false;
    int sends=0, rebuilds=0;
    bool oscWanted(int) { return wanted; }
    template<class F> void forStates(F f) { f(nullptr,state); }
    void sendOsc(ServerState&,int) { ++sends; }
    void updateParts(ofxSCServer*,ServerState&) { ++rebuilds; }
    void edit() {
''' + osc_listener + r'''
        toOsc(0)();
    }
};
void testTransitions() {
    OscillatorTransitions t;
    auto check=[&](bool exists,bool running,float offAt,bool wanted,bool rebuild) {
        t.state.osc[0]={exists ? &t : nullptr,running,offAt};
        t.wanted=wanted; t.sends=t.rebuilds=0; t.edit();
        assert(t.rebuilds==int(rebuild)); assert(t.sends==int(!rebuild));
    };
    check(false,false,-1,true,true); // first activation
    check(true,false,-1,true,true);  // resume
    check(true,true,0,true,true);    // cancel pending stop
    check(true,true,-1,false,true); // schedule stop
    check(true,true,-1,true,false); // level automation
    check(true,true,0,false,false); // another edit while stopping
    check(true,false,-1,false,false);
    check(false,false,-1,false,false);
}
'''

batch = (ROOT / "src/audio/scSynthControlBatch.h").read_text()
batch = batch[batch.index("class scSynthControlBatch"):]
batch_test = r'''
struct OscArg { char type; std::string text; float f=0; int i=0; };
struct ofxOscMessage {
    std::string address; std::vector<OscArg> args;
    void setAddress(const std::string& a) { address=a; }
    void addIntArg(int i) { args.push_back({'i',{},0,i}); }
    void addFloatArg(float f) { args.push_back({'f',{},f,0}); }
    void addStringArg(const std::string& s) { args.push_back({'s',s,0,0}); }
};
struct ofxSCServer {
    std::vector<ofxOscMessage> messages;
    void sendMsg(ofxOscMessage& m) { messages.push_back(m); }
};
struct ofxSCSynth {
    int nodeID=42; bool created=false;
    ofxSCServer server;
    std::vector<std::pair<std::string,OscArg>> stored;
    bool isCreated() const { return created; }
    void set(const std::string& name,float f) { stored.push_back({name,{'f',{},f,0}}); }
    void set(const std::string& name,int i) { stored.push_back({name,{'i',{},0,i}}); }
};
''' + batch + r'''
void testBatch() {
    ofxSCSynth s;
    {
        scSynthControlBatch b(&s,&s.server); b.set("threshold",-18.0f); b.set("grBus",123); b.send();
        assert(s.server.messages.empty()); assert(s.stored.size()==2);
        assert(s.stored[0].first=="threshold" && s.stored[0].second.type=='f' && s.stored[0].second.f==-18);
        assert(s.stored[1].first=="grBus" && s.stored[1].second.type=='i' && s.stored[1].second.i==123);
    }
    s.created=true;
    {
        scSynthControlBatch b(&s,&s.server); b.set("threshold",-18.0f); b.set("grBus",123); b.send(); b.send();
        assert(s.stored.size()==2); assert(s.server.messages.size()==1);
        const auto& msg=s.server.messages[0]; assert(msg.address=="/n_set" && msg.args.size()==5);
        assert(msg.args[0].type=='i' && msg.args[0].i==42);
        assert(msg.args[1].text=="threshold" && msg.args[2].type=='f' && msg.args[2].f==-18);
        assert(msg.args[3].text=="grBus" && msg.args[4].type=='i' && msg.args[4].i==123);
    }
    { scSynthControlBatch empty(&s,&s.server); empty.send(); assert(s.server.messages.size()==1); }
    { scSynthControlBatch missing(nullptr,nullptr); missing.set("on",1.0f); missing.send(); }
}
'''

tests = r'''
void testEq() {
    scEQEditor editor;
    float sr=48000;
    auto check=[&] {
        scEQEditor fresh; fresh.bands=editor.bands; fresh.dbRange=editor.dbRange;
        fresh.recompute(sr); editor.recompute(sr); equal(editor.curveDb,fresh.curveDb);
        editor.recompute(sr); equal(editor.curveDb,fresh.curveDb);
    };
    check();
    for(int i=0;i<5;i++) {
        editor.bands[i].freqHz*=1.37f; check();
        editor.bands[i].gainDb=7-i*3; check();
        editor.bands[i].shape=0.15f+i*0.7f; check();
    }
    sr=44100; check(); sr=96000; check(); editor.dbRange=6; check();
    auto start=std::chrono::steady_clock::now();
    for(int i=0;i<20000;i++) editor.recompute(sr);
    auto cached=std::chrono::steady_clock::now()-start;
    start=std::chrono::steady_clock::now();
    for(int i=0;i<20000;i++) { scEQEditor fresh; fresh.bands=editor.bands; fresh.recompute(sr); }
    auto cold=std::chrono::steady_clock::now()-start;
    std::cout << "EQ cache: " << std::chrono::duration<double,std::milli>(cached).count()
              << " ms warm / " << std::chrono::duration<double,std::milli>(cold).count() << " ms cold (20000 calls)\n";
}
void testLfo() {
    scModulation m;
    for(int li=0;li<8;li++) {
        m.lfoGain[li].value={1}; m.lfoRound[li].value={0.5f}; m.lfoPw[li].value={0.5f};
        auto check=[&] {
            for(int size : {2,97,256,97}) {
                auto a=m.lfoTable(li,size), b=m.lfoTransfer(li,size);
                scModulation fresh=m; fresh.cycleCache={}; fresh.transferCache={};
                equal(a,fresh.lfoTable(li,size)); equal(b,fresh.lfoTransfer(li,size));
                equal(a,m.lfoTable(li,size)); equal(b,m.lfoTransfer(li,size));
                a[0]=-999; assert(m.lfoTable(li,size)[0]!=-999); // callers own their returned tables
            }
        };
        check();
        for(auto* p : {&m.lfoGain[li],&m.lfoPow[li],&m.lfoBiPow[li],&m.lfoRound[li],&m.lfoSkew[li],&m.lfoPw[li]}) {
            p->value={0.27f}; check(); p->value={0.73f}; check();
        }
        m.lfoNoise[li].value=true; check(); m.lfoCentered[li].value=true; check();
        m.grainExtras=true; m.lfoSteps[li].value=7; check(); m.lfoSteps[li].value=3; check();
        m.grainExtras=false; check();
    }
}
template<class F> std::vector<float> evaluate(F& f,const std::string& text) {
    auto p=f.shuntingYard(f.tokenize(text));
    typename F::Env env;
    env["x"]=F::Value::vector({1,2,4,8}); env["y"]=F::Value::vector({2,3});
    auto v=f.evalRPN(p,env); return v.isVec ? v.v : std::vector<float>{v.f};
}
void testFormula() {
    Formula f;
    equal(evaluate(f,"x+y"),std::vector<float>{3,5,7,11});
    equal(evaluate(f,"(x+2)*3"),std::vector<float>{9,12,18,30});
    equal(evaluate(f,"if(x>2,x,y)"),std::vector<float>{2,3,4,8});
    equal(evaluate(f,"if(1,[2,3],[4,5,6])"),std::vector<float>{2,3});
    equal(evaluate(f,"sum(x)"),std::vector<float>{15});
    equal(evaluate(f,"median(x)"),std::vector<float>{3});
    equal(evaluate(f,"at(x,[-1,0,2,9])"),std::vector<float>{1,1,4,8});
    equal(evaluate(f,"sort([3,1,2])"),std::vector<float>{1,2,3});
    equal(evaluate(f,"pairdist([0,3],[0,4])"),std::vector<float>{5});
    bool threw=false; try { evaluate(f,"if(1,x,unknown)"); } catch(...) { threw=true; } assert(threw);
    f.inputParamRefs[1]=std::make_shared<Param<std::vector<float>>>();
    f.inputParamRefs[1]->value={1,2,3};
    f.outputParamRefs[1]=std::make_shared<Param<std::vector<float>>>();
    f.outputParamRefs[2]=std::make_shared<Param<std::vector<float>>>();
    f.program={{"v",f.shuntingYard(f.tokenize("$i1+$1"))},
               {"$o1",f.shuntingYard(f.tokenize("v*2"))},
               {"$o2",f.shuntingYard(f.tokenize("$o1+N"))}};
    f.calculate(); equal(f.outputParamRefs[1]->get(),std::vector<float>{4,8,12});
    equal(f.outputParamRefs[2]->get(),std::vector<float>{7,11,15});
    f.inputParamRefs[1]->value={}; f.calculate();
    equal(f.outputParamRefs[1]->get(),std::vector<float>{0});
    f.formulaValid=false; f.calculate(); equal(f.outputParamRefs[2]->get(),std::vector<float>{0});
}
'''

if baseline:
    tests += r'''
void differential() {
    Formula f; Baseline b;
    std::vector<std::string> expr={"x+y","(x+y)/2","-x","!x","x^y","x%y","x>=y","x&&y",
        "sin(x)","cos(x)","sqrt(x)","exp(y)","log(x)","atan2(x,y)","min(x,y,3)","max(x)",
        "sum(x)","mean(x)","median(x)","rms(x)","std(x)","var(x)","idxmin(x)","idxmax(x)",
        "vec(x,y,3)","concat(x,y)","repeat(3,5)","indices(x)","at(x,y)","sort(x)",
        "clamp(x,2,5)","smoothstep(1,4,x)","step(3,x)","if(x>2,x,y)","pairdist(x,y)"};
    for(auto& e:expr) equal(evaluate(f,e),evaluate(b,e));
    auto benchmark=[](auto& node) {
        using F=std::decay_t<decltype(node)>;
        auto code=node.shuntingYard(node.tokenize("clamp((x+y)*0.5,0,100)+sin(x)"));
        typename F::Env env; env["x"]=F::Value::vector(std::vector<float>(4096,2));
        env["y"]=F::Value::vector(std::vector<float>(4096,3));
        const auto count=allocations; auto start=std::chrono::steady_clock::now();
        for(int i=0;i<200;i++) { auto v=node.evalRPN(code,env); assert(v.v.size()==4096); }
        return std::make_pair(allocations-count,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    };
    auto before=benchmark(b), after=benchmark(f);
    assert(after.first<before.first);
    std::cout << "Formula (4096 elements, 200 evaluations): allocations " << before.first << " -> " << after.first
              << ", time " << before.second << " -> " << after.second << " ms\n";
}
'''

main = "int main() { testEq(); testLfo(); testFormula(); testTransitions(); testBatch(); " + ("differential(); " if baseline else "") + 'std::cout << "PASS: EQ/LFO invalidation, oscillator transitions, OSC batching and Formula semantics\\n"; }'
code = prelude + eq + helpers + '\nfloat first(const std::vector<float>& v,float f=0) { return v.empty()?f:v[0]; }\n' + mod_class + transfer + cycle + formula + baseline + transition_test + batch_test + tests + main
with tempfile.TemporaryDirectory(prefix="synth_cpu_test_") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    executable = pathlib.Path(directory) / "test"
    source.write_text(code)
    subprocess.run(["clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
