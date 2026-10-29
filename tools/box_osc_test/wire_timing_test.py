#!/usr/bin/env python3
"""Wire-level timing of timeline notes, per synth Box.

Production Box code, ofxSCServer and the real OSC serialiser, ASan/UBSan. Each
Box sends to a real ofxSCServer (latency on, 50 ms) pointed at a UDP socket this
test owns; no scsynth runs. For each Box a note goes through the real scheduling
path (ofxOceanodeScheduling::dispatch -> scScheduledInputs -> ScopedTimetag):
Pitch at due-2 ms, Gate 1 at due, Gate 0 at due+100 ms (due = now + 1 s). The
received packets are decoded the way scsynth executes them -- a nested bundle
runs by its own timetag, timetag 1 (or a bare message) on arrival -- and every
message sent while a scheduled event is handled must run at that event's
instant + latency (1 ms tolerance). Frames in between (update()) and the
timeline's suppressed catch-up set must not rewrite any synth control / control
bus the scheduled events wrote.

Also checks the transport: sendBundle inside a ScopedTimetag and sendBundleAt
send a flat timetagged bundle, plain sendBundle keeps its nested immediate one.

    python3 tools/box_osc_test/wire_timing_test.py [box ...]

Boxes: additiveBox aliasingBox analogBox drumKitBox fmBox grainBox physicalBox
physicalBoxPulse polyComb pulsarBox sampleBox steamPipeBox steamPipeBoxPulse
wavetableBox. Set OCEANODE_TEST_BUILD_DIR to keep the build, WIRE_TEST_CACHE=<dir> to
reuse compiled objects between runs, WIRE_TEST_OLD_SERIALISER=1 to check the test
fails against the pre-fix nested-bundle sendBundle (transport checks skipped).
"""
from pathlib import Path
import os, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT.parent/'ofxOceanode/tests'))
import macro_mixer_polycomb_regression as native
mixer = native.mixer

HEADERS = ['scAdditiveBox.h', 'scAliasingBox.h', 'scAnalogBox.h', 'scDrumKitBox.h', 'scFMBox.h',
           'scGrainBoxPoly.h', 'scPhysicalBox.h', 'scPolyComb.h', 'scPulsarBox.h', 'scSampleBox.h',
           'scSteamPipeBox.h', 'scWavetableBox.h']

# name -> C++ block: builds `box` on server `s`, defines gate/pitch handles, then calls note(...)
BOXES = {
 'additiveBox': r'''auto& box=*new scAdditiveBox(servers);box.setup();box.numOutputs=1;box.partials=32;
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);box.flushDirty();ack(s);
    note("AdditiveBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'aliasingBox': r'''auto& box=*new scAliasingBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("AliasingBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'analogBox': r'''auto& box=*new scAnalogBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("AnalogBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'drumKitBox': r'''auto& box=*new scDrumKitBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("DrumKitBox",box,box.gateHandle,box.pitchIn.handle);''',
 'fmBox': r'''auto& box=*new scFMBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("FMBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'grainBox': r'''auto& box=*new scGrainBoxPoly(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("GrainBox",box,box.p0GateHandle,box.p0PitchHandle);''',
 'physicalBox': r'''auto& box=*new scPhysicalBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("PhysicalBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'physicalBoxPulse': r'''auto& box=*new scPhysicalBox(servers);box.setup();box.pulseEnabled=true;
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("PhysicalBox (Pulse)",box,box.gateIn.handle,box.pitchIn.handle);''',
 'polyComb': r'''auto& box=*new scPolyComb(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("PolyComb",box,box.gateIn.handle,box.pitchIn.handle);''',
 'pulsarBox': r'''auto& box=*new scPulsarBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("PulsarBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'sampleBox': r'''auto& box=*new scSampleBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);
    auto& pad=*box.pads[0];box.nodeSC.at(s).padsPending=false;if(!pad.sc.count(s))box.createPad(s,pad);ack(s);
    note("SampleBox",box,box.triggerHandle,box.pitchIn.handle);''',
 'steamPipeBox': r'''auto& box=*new scSteamPipeBox(servers);box.setup();
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("SteamPipeBox",box,box.gateIn.handle,box.pitchIn.handle);''',
 'steamPipeBoxPulse': r'''auto& box=*new scSteamPipeBox(servers);box.setup();box.pulseEnabled=true;
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);
    note("SteamPipeBox (Pulse)",box,box.gateIn.handle,box.pitchIn.handle);''',
 'wavetableBox': r'''auto& box=*new scWavetableBox(servers);box.setup();
    box.path="";box.startDecode();box.applyTable(box.decodeTask.get());   // built-in table: the oscillators need buffers
    box.buildSynth(s);box.setOutputBus(s,0,0);box.createSynth(s);ack(s);box.swapBuffers(s);ack(s);s->nrtSyncOutstanding=0;
    assert(box.synths.at(s)->isCreated());
    note("WavetableBox",box,box.publishedHandles.at(box.gate.getEscapedName()),box.publishedHandles.at(box.pitch.getEscapedName()));''',
}

CASES = r'''
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <variant>
#include <set>
#include <sstream>

// ---- UDP sink + OSC decoder ---------------------------------------------------
static int sinkFd=-1; static int sinkPort=0;
static void openSink(){
    sinkFd=socket(AF_INET,SOCK_DGRAM,0);assert(sinkFd>=0);
    int big=8<<20;setsockopt(sinkFd,SOL_SOCKET,SO_RCVBUF,&big,sizeof big);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
    assert(::bind(sinkFd,(sockaddr*)&a,sizeof a)==0);socklen_t l=sizeof a;getsockname(sinkFd,(sockaddr*)&a,&l);sinkPort=ntohs(a.sin_port);
}
static std::vector<std::vector<char>> drain(int waitMs=40){
    std::vector<std::vector<char>> out;
    while(true){
        pollfd p{sinkFd,POLLIN,0};if(poll(&p,1,waitMs)<=0)break;
        std::vector<char> b(1<<16);const ssize_t n=recv(sinkFd,b.data(),b.size(),0);if(n<=0)break;
        b.resize(n);out.push_back(std::move(b));
    }
    return out;
}
using Arg=std::variant<int,float,std::string,double>;
struct Msg{std::string address;std::vector<Arg> args;uint64_t tag=1;int depth=0;bool nestedImmediate=false;size_t packet=0;size_t inPacket=0;};
static uint32_t be32(const char* p){return (uint32_t(uint8_t(p[0]))<<24)|(uint32_t(uint8_t(p[1]))<<16)|(uint32_t(uint8_t(p[2]))<<8)|uint8_t(p[3]);}
static uint64_t be64(const char* p){return (uint64_t(be32(p))<<32)|be32(p+4);}
static std::string oscString(const char*& p,const char* end){std::string s(p);size_t n=s.size()+1;p+=(n+3)&~size_t(3);assert(p<=end);return s;}
static void decodeMessage(const char* p,const char* end,uint64_t tag,int depth,bool nestedImm,std::vector<Msg>& out){
    Msg m;m.tag=tag;m.depth=depth;m.nestedImmediate=nestedImm;m.address=oscString(p,end);
    if(p<end && *p==','){std::string types=oscString(p,end);
        for(size_t i=1;i<types.size();++i){char t=types[i];
            if(t=='i'){m.args.push_back(int(int32_t(be32(p))));p+=4;}
            else if(t=='f'){uint32_t u=be32(p);float f;memcpy(&f,&u,4);m.args.push_back(f);p+=4;}
            else if(t=='s'||t=='S'){m.args.push_back(oscString(p,end));}
            else if(t=='h'||t=='t'){m.args.push_back(double(int64_t(be64(p))));p+=8;}
            else if(t=='d'){uint64_t u=be64(p);double d;memcpy(&d,&u,8);m.args.push_back(d);p+=8;}
            else if(t=='b'){uint32_t n=be32(p);p+=4+((n+3)&~3u);m.args.push_back(std::string("<blob>"));}
            else if(t=='T'||t=='F'||t=='N'||t=='I'||t=='['||t==']'){}
            else {std::cerr<<"unknown OSC type "<<t<<"\n";std::abort();}
        }
    }
    out.push_back(std::move(m));
}
// scsynth: a bundle's messages run at that bundle's own timetag; 1 = on arrival
static void decode(const char* p,const char* end,uint64_t tag,int depth,bool nestedImm,std::vector<Msg>& out){
    if(end-p>=16 && std::string(p,7)=="#bundle"){
        const uint64_t own=be64(p+8);p+=16;
        while(p<end){const uint32_t n=be32(p);p+=4;decode(p,p+n,own,depth+1,nestedImm||(depth>=1&&own==1),out);p+=n;}
    } else decodeMessage(p,end,depth==0?1:tag,depth,nestedImm,out);
}
static std::vector<Msg> decodeAll(const std::vector<std::vector<char>>& packets){
    std::vector<Msg> out;
    for(size_t i=0;i<packets.size();++i){const size_t from=out.size();decode(packets[i].data(),packets[i].data()+packets[i].size(),1,0,false,out);
        for(size_t j=from;j<out.size();++j){out[j].packet=i;out[j].inPacket=out.size()-from;}}
    return out;
}
static std::string str(const Arg& a){std::ostringstream o;std::visit([&](auto&& v){o<<v;},a);return o.str();}
static std::string show(const Msg& m,size_t maxArgs=10){std::string s=m.address;for(size_t i=0;i<m.args.size()&&i<maxArgs;++i)s+=" "+str(m.args[i]);if(m.args.size()>maxArgs)s+=" ...";return s;}
static double tagDiffMs(uint64_t a,uint64_t b){return (double)(int64_t)(a-b)*1000.0/4294967296.0;}
static int asInt(const Arg& a){if(auto* i=std::get_if<int>(&a))return *i;if(auto* f=std::get_if<float>(&a))return int(*f);return -999999;}

// The targets a message writes: "n<id>.<key>" for synth controls, "c<bus>" for control buses
static std::vector<std::string> targets(const Msg& m){
    std::vector<std::string> t;auto key=[&](const Arg& a){return std::holds_alternative<std::string>(a)?std::get<std::string>(a):("#"+str(a));};
    const auto& a=m.args;
    if(m.address=="/n_set" && !a.empty()){const int id=asInt(a[0]);for(size_t i=1;i+1<a.size();i+=2)t.push_back("n"+std::to_string(id)+"."+key(a[i]));}
    else if(m.address=="/n_setn" && !a.empty()){const int id=asInt(a[0]);for(size_t i=1;i+1<a.size();){t.push_back("n"+std::to_string(id)+"."+key(a[i]));const int c=asInt(a[i+1]);i+=2+std::max(c,0);}}
    else if(m.address=="/s_new" && a.size()>=4){const int id=asInt(a[1]);for(size_t i=4;i+1<a.size();i+=2)t.push_back("n"+std::to_string(id)+"."+key(a[i]));}
    else if(m.address=="/c_set"){for(size_t i=0;i+1<a.size();i+=2)t.push_back("c"+std::to_string(asInt(a[i])));}
    else if(m.address=="/c_setn"){for(size_t i=0;i+1<a.size();){const int b=asInt(a[i]),c=asInt(a[i+1]);for(int k=0;k<c;++k)t.push_back("c"+std::to_string(b+k));i+=2+std::max(c,0);}}
    else if(m.address=="/n_map"||m.address=="/n_mapn"||m.address=="/n_mapa"||m.address=="/n_mapan"){if(!a.empty())t.push_back("n"+std::to_string(asInt(a[0]))+".map"+(a.size()>1?key(a[1]):""));}
    else if(m.address=="/n_run"||m.address=="/n_free"||m.address=="/n_before"||m.address=="/n_after"){if(!a.empty())t.push_back("n"+std::to_string(asInt(a[0]))+m.address);}
    return t;
}

static constexpr float LATENCY=.05f;
static std::vector<serverManager*> servers;
static ofxSCServer* newServer(){
    auto* s=new ofxSCServer("127.0.0.1",sinkPort,0);   // leaked: the receive thread races the destructor
    s->setBLatency(true);s->setLatency(LATENCY);s->nrtSyncOutstanding=0;drain();return s;
}
static void ack(ofxSCServer* s){
    for(int round=0;round<4;++round){auto nodes=s->nodeFeedbackFunctions;
        for(const auto& e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
            ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);}}
}
static int failures=0;
struct Row{std::string box,path,result;};
static std::vector<Row> rows;

template<class Box>
static void runNote(const std::string& name,Box& box,ofxSCServer* s,std::shared_ptr<ofxOceanodeAbstractParameter> gateH,std::shared_ptr<ofxOceanodeAbstractParameter> pitchH){
    ofEventArgs args;
    // settle setup traffic: a few frames, acks
    for(int i=0;i<3;++i){box.update(args);ack(s);}
    drain(60);
    std::vector<std::string> problems;std::set<std::string> written;
    size_t timedMsgs=0,packets=0,singleMsgPackets=0,multiMsgPackets=0;size_t gateMsgs[2]={0,0};
    const uint64_t due=ofxOceanodeScheduling::steadyNowUs()+1000000;
    auto frame=[&](const char* label,bool suppressed,std::function<void()> set){
        if(suppressed){ofxOceanodeScheduling::ScopedBackendSuppression q;set();box.update(args);}
        else {set();box.update(args);}
        // a frame must not touch what the scheduled events wrote, nor any note control
        static const std::set<std::string> noteKeys{"gate","pitch","mpitch","fire","trig","t_trig","vel","velocity","note","midinote"};
        for(const auto& m:decodeAll(drain()))for(const auto& t:targets(m)){
            const auto dot=t.find('.');const std::string key=dot==std::string::npos?"":t.substr(dot+1);
            if(written.count(t)||noteKeys.count(key)){problems.push_back(std::string(label)+" wrote "+t+" on the frame: "+show(m));break;}
        }
    };
    auto event=[&](const char* label,std::shared_ptr<ofxOceanodeAbstractParameter> h,const std::string& value,uint64_t at,int gateIndex){
        if(!h){problems.push_back(std::string(label)+": no handle");return;}
        ofxOceanodeScheduledParameterEvent e;e.value=value;e.dueSteadyTimeUs=at;
        const uint64_t expected=s->applyLatencyToTimetag(ofxSCServer::timetagForSteadyTimeUs(at));
        const bool taken=ofxOceanodeScheduling::dispatch(h.get(),e);
        const auto raw=drain();const auto msgs=decodeAll(raw);
        if(!taken){problems.push_back(std::string(label)+": dispatch not taken (no scheduled input)");return;}
        packets+=raw.size();
        std::map<size_t,size_t> perPacket;for(const auto& m:msgs)perPacket[m.packet]++;
        for(auto& [p,c]:perPacket)(c==1?singleMsgPackets:multiMsgPackets)++;
        for(const auto& m:msgs){
            for(const auto& t:targets(m))written.insert(t);
            const bool immediate=m.tag<=1;
            const double off=immediate?NAN:tagDiffMs(m.tag,expected);
            if(immediate||std::abs(off)>1.0){
                std::ostringstream o;o<<label<<": "<<show(m)<<" -> ";
                if(immediate)o<<(m.nestedImmediate?"nested immediate bundle (runs on arrival)":"immediate");
                else o<<std::showpos<<std::fixed<<std::setprecision(2)<<off<<" ms vs due+latency";
                problems.push_back(o.str());
            } else ++timedMsgs;
        }
        if(gateIndex>=0)gateMsgs[gateIndex]=msgs.size();
    };
    event("pitch",pitchH,"62",due-2000,-1);
    frame("frame after pitch",false,[]{});
    event("gate on",gateH,"1",due,0);
    frame("frame after gate on",false,[]{});
    frame("timeline catch-up (suppressed)",true,[&]{
        if(pitchH)pitchH->template cast<std::vector<float>>().getParameter()=std::vector<float>{62};
        gateH->template cast<std::vector<float>>().getParameter()=std::vector<float>{1};});
    event("gate off",gateH,"0",due+100000,1);
    frame("frame after gate off",false,[]{});
    frame("timeline catch-up off (suppressed)",true,[&]{gateH->template cast<std::vector<float>>().getParameter()=std::vector<float>{0};});
    if(gateMsgs[0]==0)problems.push_back("gate on sent nothing during its scheduled dispatch");
    if(gateMsgs[1]==0)problems.push_back("gate off sent nothing during its scheduled dispatch");
    // on the wire a one-message sendBundle looks like sendMsg; WIRE_TEST_OLD_SERIALISER tells them apart
    std::string path=multiMsgPackets&&singleMsgPackets?"1-msg + multi-msg bundles":multiMsgPackets?"multi-msg bundles":"1-msg bundles";
    std::string result;
    if(problems.empty())result="PASS ("+std::to_string(timedMsgs)+" msgs in "+std::to_string(packets)+" packets at due+latency)";
    else {++failures;result="FAIL";}
    std::cout<<"=== "<<name<<": "<<result<<"  ["<<path<<"]\n";
    for(const auto& p:problems)std::cout<<"    "<<p<<"\n";
    std::cout<<"    written by scheduled events:";size_t k=0;for(const auto& t:written){if(++k>24){std::cout<<" ...";break;}std::cout<<" "<<t;}std::cout<<"\n";
    rows.push_back({name,path,result});
}

static void transport(){
    auto* s=newServer();
    auto bundle=[]{ofxOscBundle b;ofxOscMessage a;a.setAddress("/n_set");a.addIntArg(1000);a.addStringArg("gate");a.addFloatArg(1);
        ofxOscMessage c=a;b.addMessage(a);b.addMessage(c);return b;};
    const uint64_t due=ofxOceanodeScheduling::steadyNowUs()+1000000;
    const uint64_t tag=ofxSCServer::timetagForSteadyTimeUs(due),expected=s->applyLatencyToTimetag(tag);
    assert(tagDiffMs(expected,tag)>49.9 && tagDiffMs(expected,tag)<50.1);   // latency once
    auto check=[&](const char* label,bool flat){
        const auto raw=drain();assert(raw.size()==1);const auto msgs=decodeAll(raw);assert(msgs.size()==2);
        for(const auto& m:msgs){
            if(flat){assert(m.depth==1 && !m.nestedImmediate && std::abs(tagDiffMs(m.tag,expected))<1);}
            else    {assert(m.depth==2 && m.nestedImmediate && m.tag==1);}
        }
        std::cout<<"transport: "<<label<<(flat?" -> flat bundle at due+latency\n":" -> nested immediate bundle\n");
    };
    {ofxSCServer::ScopedTimetag scoped(tag);auto b=bundle();s->sendBundle(b);}check("sendBundle in ScopedTimetag",true);
    {auto b=bundle();s->sendBundleAt(b,tag);}check("sendBundleAt",true);
    {auto b=bundle();s->sendBundle(b);}check("plain sendBundle",false);
    // negative control: the pre-fix scoped path (nested immediate bundle) is what the Box audit flags
    {auto b=bundle();s->osc.sendBundle(b,expected);}check("old scoped serialiser (osc.sendBundle)",false);
    {ofxSCServer::ScopedTimetag scoped(tag);ofxOscMessage m;m.setAddress("/n_set");m.addIntArg(1);m.addStringArg("gate");m.addFloatArg(1);s->sendMsg(m);}
    {const auto msgs=decodeAll(drain());assert(msgs.size()==1 && msgs[0].depth==1 && std::abs(tagDiffMs(msgs[0].tag,expected))<1);}
    std::cout<<"transport: sendMsg in ScopedTimetag -> bundle at due+latency\n";
}

int main(int argc,char** argv){
    assert(argc==3);std::filesystem::current_path(argv[2]);ofDisableDataPath();ofSeedRandom();
    auto window=std::make_shared<ofAppNoWindow>();ofGetMainLoop()->setCurrentWindow(window);
    ofSetLogLevel(OF_LOG_WARNING);
    openSink();
    if(!std::getenv("WIRE_TEST_OLD_SERIALISER"))transport();
BODY
    std::cout<<"\nBox | path | result\n";
    for(const auto& r:rows)std::cout<<r.box<<" | "<<r.path<<" | "<<r.result<<"\n";
    if(failures){std::cout<<failures<<" Box(es) FAILED wire timing\n";std::cout<<std::flush;std::_Exit(1);}
    std::cout<<"PASS: every scheduled Box note runs at its due instant + latency on the wire\n";
    std::cout<<std::flush;std::_Exit(0);   // skip static/server teardown (receive thread race)
}
'''

def main():
    names = sys.argv[1:] or list(BOXES)
    body = ''
    for n in names:
        body += ' {ofxSCServer* s=newServer();\n  auto note=[&](const std::string& name,auto& box,auto gateH,auto pitchH){runNote(name,box,s,gateH,pitchH);};\n  ' + BOXES[n] + '\n }\n'
    mixer.PRELUDE += ''.join(f'#include "{h}"\n' for h in HEADERS) + '#include <iomanip>\n'
    mixer.CASES = CASES.replace('BODY', body)
    sources = [str(mixer.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
    sources += [str(p) for p in sorted((ROOT/'src').rglob('*.cpp')) if 'registration' not in str(p) and not p.name.startswith('ofxOceanodeSuperCollider')]
    sources += [str(p) for p in sorted((native.SUPERCOLLIDER/'src').glob('*.cpp'))]
    sources += [str(p) for p in sorted((native.OSC/'src').glob('*.cpp'))]
    sources += [str(p) for p in sorted((native.OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
    includes = [ROOT/'src', *sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),
                native.SUPERCOLLIDER/'src', native.OSC/'src', native.OSC/'libs/oscpack/src',
                *sorted({p.parent for p in (native.OSC/'libs/oscpack/src').rglob('*.h')})]
    os.environ.setdefault('OCEANODE_TEST_JOBS', '8')
    with tempfile.TemporaryDirectory(prefix='box-wire-') as folder:
        folder = Path(folder)
        # private members (handles, states) are read directly
        wrapper = folder/'cxx'
        cache = os.environ.get('WIRE_TEST_CACHE')   # optional object cache (keyed on flags + source mtime; ignores header edits)
        if cache:
            wrapper.write_text(f'''#!/usr/bin/env python3
import hashlib,os,shutil,subprocess,sys
a=sys.argv[1:];cmd=["clang++","-fno-access-control",*a]
if "-c" in a and "-o" in a:
    src=a[a.index("-c")+1];out=a[a.index("-o")+1]
    if not src.endswith("regression.cpp"):
        k=hashlib.sha1((" ".join(x for x in a if x!=out)+str(os.path.getmtime(src))).encode()).hexdigest()
        hit=os.path.join({cache!r},k+".o")
        if os.path.exists(hit): shutil.copy(hit,out); sys.exit(0)
        r=subprocess.run(cmd).returncode
        if r==0: os.makedirs({cache!r},exist_ok=True); shutil.copy(out,hit)
        sys.exit(r)
sys.exit(subprocess.run(cmd).returncode)
''')
        else:
            wrapper.write_text('#!/bin/sh\nexec clang++ -fno-access-control "$@"\n')
        wrapper.chmod(0o755)
        os.environ['CXX'] = str(wrapper)
        # known toolchain noise (oscpack misaligned read)
        supp = folder/'ubsan.supp'; supp.write_text('alignment:NetworkingUtils.cpp\nvptr:unique_ptr.h\n')
        os.environ['UBSAN_OPTIONS'] = f'suppressions={supp}'
        os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        if os.environ.get('WIRE_TEST_OLD_SERIALISER'):   # self-check: rebuild ofxSCServer with the pre-fix nested bundle path
            server = native.SUPERCOLLIDER/'src/ofxSCServer.cpp'
            old = folder/'ofxSCServer_old.cpp'
            old.write_text(f'#line 1 "{server}"\n' + server.read_text().replace('osc.sendTimedBundle(', 'osc.sendBundle('))
            sources[sources.index(str(server))] = str(old)
        mixer.main(extra_sources=sources, extra_includes=includes, std='c++20')

if __name__ == '__main__': main()
