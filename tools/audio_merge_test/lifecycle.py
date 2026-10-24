#!/usr/bin/env python3
"""Exercise production preset/dimension/request methods without an audio device."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/nodes/synthdefs/scSynthdef.cpp').read_text()
def block(start):
    begin = source.index(start)
    opening = source.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

methods = '\n'.join(block('void scSynthdef::' + name + '(') for name in
                    ('loadBeforeConnections', 'resizeAudioInputs', 'waitForSynthdef', 'revertToBuiltSynthdef', 'buildSynth', 'createSynth'))
listener = block('listeners.push(numChannels.newListener([this](int &i)')
listener = listener[listener.index('{') + 1:-1]
initial = 'if(!isSynthdefCompiled(getSynthdefFilename())) waitForSynthdef(getSynthdefFilename());'
assert initial in block('void scSynthdef::setup(')
code = r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace std;
using ofJson = nlohmann::json;
constexpr int MAX_NODE_CHANNELS=128;
int ofToInt(const string& s){return stoi(s);}
template<class T> string ofToString(const T& v){return to_string(v);}
template<class T> T ofClamp(T v,T a,T b){return clamp(v,a,b);}
template<class T> struct ofParameter {
    string name; T value=1,lo=1,hi=128;
    function<void(T&)> changed;
    operator T() const{return value;}
    string getEscapedName()const {auto k=name;replace(k.begin(),k.end(),' ','_');return k;}
    T getMin()const{return lo;} T getMax()const{return hi;}
    void setWithoutEventNotifications(T v){value=v;}
    void set(T v){value=v;if(changed)changed(value);}
    ofParameter& operator=(T v){set(v);return *this;}
    ofParameter& operator=(const ofParameter& p){set(p.value);return *this;}
};
struct ofxSCServer{};
struct ofxSCSynth {
    static inline int created=0;
    ofxSCSynth(string,ofxSCServer*){}
    void run(bool){}
    void createAndRun(int,int,bool){++created;}
};
namespace ofFilePath {string getEnclosingDirectory(const string&,bool){return "/defs";}}
struct scSynthdefCompiler {
    struct Target {string synthName,defName,synthFolder;int numChannels;vector<int> variables;};
    enum class Result {Ready, Declined, Failed};
    struct Request {Target target;function<void(Result)> callback;};
    map<void*,Request> pending;
    static scSynthdefCompiler& get(){static scSynthdefCompiler x;return x;}
    void cancel(void* owner){pending.erase(owner);}
    void request(void* owner,Target target,function<void(Result)> callback){pending[owner]={target,callback};}
    void finish(void* owner,Result result){auto callback=pending.at(owner).callback;pending.erase(owner);callback(result);}
};
struct scSynthdef {
    struct Description{string filepath="/defs/AudioMerge.txarcmeta";vector<pair<string,int>> variables={{"nInputs",32}};} synthDescription;
    string synthdefName="AudioMerge", builtSynthdef="AudioMerge1_1";
    int oldNumChannels=1,builtNumChannels=1,replacements=0;
    bool variableChanged=false, waitingForSynthdef=false,suppressInputRefresh=false;
    unsigned sendCacheEpoch=0;
    ofParameter<int> numChannels,nInputs;
    map<string,int> lastVariableValues={{"nInputs",1}},builtVariables=lastVariableValues;
    vector<string> inputs={"In1"};
    map<ofxSCServer*,ofxSCSynth*> synths;
    set<string> compiled={"AudioMerge1_1"};
    struct Event{void notify(){} void notify(void*){}}resendParams,parameterGroupChanged;
    struct Group{vector<string> order;void reorder(const vector<string>& names){order=names;}}group;
    Group& getParameterGroup(){return group;}
    bool synthOn=true;bool getActive(){return true;}
    scSynthdef(){numChannels.name="N Chan";nInputs.name="nInputs";nInputs.hi=32;
        numChannels.changed=[this](int& i){LISTENER};
        nInputs.changed=[this](int& i){if(lastVariableValues["nInputs"]!=i){lastVariableValues["nInputs"]=i;variableChanged=true;numChannels=numChannels;}};
    }
    ~scSynthdef(){scSynthdefCompiler::get().cancel(this);for(auto& p:synths)delete p.second;}
    template<class T> ofParameter<T>& getParameter(const string&){return nInputs;}
    string getSynthdefFilename(){return "AudioMerge"+to_string(numChannels.value)+"_"+to_string(nInputs.value);}
    bool isSynthdefCompiled(const string& n)const{return compiled.count(n);}
    void addInput(string s){inputs.push_back(s);}
    void removeInput(int i){inputs.erase(inputs.begin()+i);}
    void replaceSynths(){++replacements;builtSynthdef=getSynthdefFilename();builtNumChannels=numChannels;builtVariables=lastVariableValues;}
    void creationCheck(){INITIAL_CHECK}
    void loadBeforeConnections(ofJson&);
    void resizeAudioInputs();
    void waitForSynthdef(const string&);
    void revertToBuiltSynthdef();
    void buildSynth(ofxSCServer*);
    void createSynth(ofxSCServer*);
};
METHODS
int main(){
    using Result=scSynthdefCompiler::Result;
    auto& compiler=scSynthdefCompiler::get();
    // Preset dimensions are strings or numbers. All pins must exist before
    // the container attempts to connect them, even before user approval.
    scSynthdef n;
    ofJson preset={{"N_Chan","4"},{"nInputs","6"}};
    n.loadBeforeConnections(preset);
    assert(n.inputs.size()==6 && n.inputs[5]=="In6");
    assert(n.waitingForSynthdef && compiler.pending.size()==1);
    assert(compiler.pending.at(&n).target.defName=="AudioMerge4_6");
    assert(compiler.pending.at(&n).target.variables==vector<int>{6});
    n.compiled.insert("AudioMerge4_6");
    compiler.finish(&n,Result::Ready);
    assert(!n.waitingForSynthdef && n.builtSynthdef=="AudioMerge4_6");
    // Existing builds need no confirmation; repeated preset values don't
    // create intermediate channel/input requests.
    n.compiled.insert("AudioMerge2_3");
    preset={{"N_Chan",2},{"nInputs",3}};n.loadBeforeConnections(preset);
    assert(n.inputs.size()==3 && compiler.pending.empty());
    n.nInputs=8;
    assert(n.inputs.size()==8 && compiler.pending.at(&n).target.defName=="AudioMerge2_8");
    compiler.finish(&n,Result::Declined);
    assert(n.inputs.size()==3 && n.nInputs.value==3 && !n.waitingForSynthdef);
    assert(compiler.pending.empty());
    // A newly created node whose default is missing also asks; do not send
    // /s_new for a definition the server cannot load. Decline must not loop.
    scSynthdef fresh;fresh.compiled.clear();fresh.creationCheck();
    assert(compiler.pending.at(&fresh).target.defName=="AudioMerge1_1");
    ofxSCServer server;fresh.buildSynth(&server);fresh.createSynth(&server);
    assert(ofxSCSynth::created==0);
    compiler.finish(&fresh,Result::Declined);
    assert(compiler.pending.empty() && fresh.waitingForSynthdef);
    fresh.nInputs=2;
    assert(compiler.pending.at(&fresh).target.defName=="AudioMerge1_2");
    // Changing channels uses the same confirmation path and replaces the
    // owner's previous request, as the real compiler does.
    fresh.numChannels=7;
    assert(compiler.pending.at(&fresh).target.defName=="AudioMerge7_2");
}
'''
code = code.replace('LISTENER', listener).replace('INITIAL_CHECK', initial).replace('METHODS', methods)
with tempfile.TemporaryDirectory(prefix='audio-merge-lifecycle-') as directory:
    cpp = Path(directory) / 'test.cpp'; binary = Path(directory) / 'test'
    cpp.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(ROOT.parent.parent / 'libs/json/include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('SynthDef lifecycle passed: preset pins before wiring, exact build prompt, approval, decline, missing defaults and channel changes.')
