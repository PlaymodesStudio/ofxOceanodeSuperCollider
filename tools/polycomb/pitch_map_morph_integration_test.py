#!/usr/bin/env python3
"""Real PolyComb/MacroMixer code: inspect captured OSC pitch targets, no audio device.

Uses the neighboring Oceanode native fixture and rebuilds all translation units
so stale objects cannot hide header or ownership changes.
"""
from pathlib import Path
import json,math,os,struct,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT.parent/'ofxOceanode/tests'))
import macro_mixer_polycomb_regression as native

native.mixer.PRELUDE+=r'''
#include "ofxOceanodeConnection.h"
class PitchCaptureServer:public ofxSCServer {
public:
    PitchCaptureServer():ofxSCServer("127.0.0.1",57110,0){}
    void acknowledge(){
        std::vector<ofxSCNode*> nodes;
        for(const auto& entry:nodeFeedbackFunctions)nodes.push_back(entry.first);
        for(auto* node:nodes)if(!node->isCreated()){
            ofxOscMessage message;message.setAddress("/n_go");message.addIntArg(node->nodeID);
            node->feedbackListener(message);
        }
    }
};
'''
CASE=r'''
    {
        PitchCaptureServer server;server.beginNRTCapture(true);
        scPolyComb audio(std::vector<serverManager*>{});audio.setup();
        audio.buildSynth(&server);audio.setOutputBus(&server,0,0);audio.createSynth(&server);server.acknowledge();
        const auto reference=ofLoadJson(std::getenv("SC_POLYCOMB_MORPH_REFERENCE"));
        const auto& pair=reference.at("pairs").at("map_modes_and_inheritance");
        auto a=saved(audio);a["Voices"]=pair.at("from").at("voices");
        a["polyComb"].update(pair.at("from").at("controls"));
        auto b=a;b["Voices"]=pair.at("to").at("voices");
        b["polyComb"].update(pair.at("to").at("controls"));
        std::string reason;
        assert(audio.morphBegin(a,reason));audio.morphApply(1,true);audio.morphEnd(true);server.acknowledge();
        assert(audio.morphBegin(b,reason));
        for(float t:{0.f,.00101f,.25f,.4999f,.5001f,.75f,1.f}){
            server.setNRTTime(1.+t);audio.morphApply(t,t>=.5f);server.acknowledge();
        }
        audio.morphEnd(true);
        // Re-enter the same blend and change its played root without touching
        // morph progress. The map must remain live, including the resonators.
        server.setNRTTime(2.1);assert(audio.morphBegin(a,reason));audio.morphApply(1,true);audio.morphEnd(true);
        assert(audio.morphBegin(b,reason));audio.morphApply(.25f,false);
        server.setNRTTime(2.2);audio.getParameter<std::vector<float>>("Pitch")={61.f};server.acknowledge();
        auto c=a;c["polyComb"]["Osc_Map_Interval"]=6.f;
        server.setNRTTime(2.3);assert(audio.morphBegin(c,reason));audio.morphApply(0,false);server.acknowledge();
        server.setNRTTime(2.4);audio.morphApply(.5f,true);server.acknowledge();
        server.setNRTTime(2.5);audio.morphEnd(false);server.acknowledge();
        ofJson publish={{"polyCombPublished",{"Osc_Map_Interval"}}};audio.loadBeforeConnections(publish);
        {
            ofParameter<float> value;value.set("Map Driver",4.f,-24.f,24.f);
            ofxOceanodeParameter<float> driver;driver.bindParameter(value);
            auto& sink=static_cast<ofxOceanodeAbstractParameter&>(audio.getParameterGroup().get("Osc Map Interval")).cast<float>();
            server.setNRTTime(2.6);ofxOceanodeConnection<float,float> cable(driver,sink,true);
            server.setNRTTime(2.7);value.set(2.f);server.acknowledge();
            server.setNRTTime(2.8);
        }
        // Manual editing leaves the frozen endpoint description and uses the
        // editor's current map, as an ordinary edit outside morphing does.
        server.setNRTTime(3.);audio.getParameter<float>("Osc Map Interval")=2.f;server.acknowledge();
        server.setNRTTime(3.1);assert(audio.morphBegin(a,reason));audio.morphApply(1,true);audio.morphEnd(true);
        // The count renderer must receive the endpoint descriptions, not just
        // the dropdown/numeric values currently shown on the parent node.
        auto wide=b;wide["Voices"]=12;
        assert(audio.morphBegin(wide,reason));server.setNRTTime(4.);audio.morphApply(.25f,false);server.acknowledge();
        const auto output=std::string(std::getenv("SC_PITCH_MORPH_SCORE_DIR"));
        ofJson ids={{"osc",server.findNRTNodeID("PCPitch4")},{"comb",server.findNRTNodeID("PCCombR4")},
                    {"wide",server.findNRTNodeID("PCPitch12")}};
        assert(ids["osc"].get<int>()>0&&ids["comb"].get<int>()>0&&ids["wide"].get<int>()>0);
        ofSaveJson(output+"/ids.json",ids);
        assert(server.writeNRTScore(output+"/pitch.osc",5.));
        audio.morphEnd(false);audio.free(&server);server.endNRTCapture();
    }
'''
anchor='    std::cout<<"polyComb node morph passed:'
assert anchor in native.mixer.CASES
native.mixer.CASES=native.mixer.CASES.replace(anchor,CASE+'\n'+anchor)

def string(data,offset):
    end=data.index(0,offset)
    return data[offset:end].decode(),(end+4)//4*4

def packets(data,time=0):
    if data.startswith(b'#bundle\0'):
        seconds,fraction=struct.unpack_from('>II',data,8);time=seconds+fraction/2**32
        offset=16
        while offset<len(data):
            size=struct.unpack_from('>I',data,offset)[0];offset+=4
            yield from packets(data[offset:offset+size],time);offset+=size
        return
    address,offset=string(data,0);tags,offset=string(data,offset);values=[]
    for tag in tags[1:]:
        if tag=='s':value,offset=string(data,offset)
        elif tag in 'if':value=struct.unpack_from('>'+tag,data,offset)[0];offset+=4
        elif tag in '[]':continue
        else:raise AssertionError((address,tag))
        values.append(value)
    yield time,address,values

def validate(work):
    ids=json.loads((work/'ids.json').read_text());data=(work/'pitch.osc').read_bytes();offset=0;events=[]
    while offset<len(data):
        size=struct.unpack_from('>I',data,offset)[0];offset+=4
        events.extend(packets(data[offset:offset+size]));offset+=size
    def control(node,key,time):
        found=None
        for stamp,address,args in events:
            if stamp>time+2e-6 or not args or args[0]!=node:continue
            i=1
            if address=='/n_setn':
                while i<len(args):
                    name,count=args[i:i+2];value=args[i+2:i+2+count];i+=2+count
                    if name==key:found=value
            elif address=='/n_set':
                while i<len(args):
                    name,value=args[i:i+2];i+=2
                    if name==key:found=[value]
            elif address=='/n_fill':
                name,count,value=args[1:4]
                if name==key:found=[value]*count
        assert found is not None,(node,key,time)
        return found
    def expected(t,n,root=60):
        x=[63+3*i+root-60 for i in range(n)]
        y=[36+12*math.log2(i+1)+root-60 for i in range(n)]
        r=[48+12*math.log2(i+1)+root-60 for i in range(n)]
        return [a*(1-t)+b*t for a,b in zip(x,y)],[a*(1-t)+b*t for a,b in zip([v-7 for v in x],r)]
    def near(actual,wanted):
        assert len(actual)==len(wanted)
        assert max(abs(a-b)for a,b in zip(actual,wanted))<4e-5,(actual,wanted)
    for t in (0,.00101,.25,.4999,.5001,.75,1):
        osc,res=expected(t,4);time=1+t
        actual=control(ids['osc'],'pitch',time);near(actual,osc)
        own=control(ids['comb'],'combmode',time)[0];assert abs(own-t)<1e-6
        residual=control(ids['comb'],'combpitch',time)
        near([p+(1-own)*o for p,o in zip(residual,actual)],res)
    osc,res=expected(.25,4,61);near(control(ids['osc'],'pitch',2.2),osc)
    own=control(ids['comb'],'combmode',2.2)[0];residual=control(ids['comb'],'combpitch',2.2)
    near([p+(1-own)*o for p,o in zip(residual,osc)],res)
    near(control(ids['osc'],'pitch',2.3),osc)
    near(control(ids['osc'],'pitch',2.4),[(x+67+6*i)*.5 for i,x in enumerate(osc)])
    near(control(ids['osc'],'pitch',2.5),osc)
    near(control(ids['osc'],'pitch',2.7),[.75*(63+2*i)+.25*(49+12*math.log2(i+1))for i in range(4)])
    assert abs(control(ids['comb'],'combmode',2.7)[0]-.25)<1e-6
    near(control(ids['osc'],'pitch',3),[63+2*i for i in range(4)])
    near(control(ids['wide'],'pitch',4),expected(.25,12,61)[0])
    # Map-only changes retain the original pitch/comb synths.
    assert sum(address=='/s_new' and args[0]=='PCPitch4' for _,address,args in events)==1
    assert sum(address=='/s_new' and args[0]=='PCCombR4' for _,address,args in events)==1
    print('PASS: real PolyComb OSC targets interpolate before 0.0011 and across 0.5; inherited map, live root, manual edit, MacroMixer hooks and 12-voice peer retain exact pitches without map-only synth duplication')

def main():
    with tempfile.TemporaryDirectory(prefix='polycomb-pitch-targets-') as directory:
        work=Path(directory);os.environ['SC_PITCH_MORPH_SCORE_DIR']=str(work)
        os.environ['SC_POLYCOMB_MORPH_REFERENCE']=str(ROOT/'tools/polycomb/fixtures/morph_reference.json')
        sources=[str(native.mixer.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
        sources+=[str(ROOT/'src'/p)for p in ('scNode.cpp','serverManager.cpp','scTimelineMenu.cpp','nodes/scPolyComb.cpp','nodes/scModulation.cpp')]
        sources+=[str(p)for p in sorted((native.SUPERCOLLIDER/'src').glob('*.cpp'))]
        sources+=[str(p)for p in sorted((native.OSC/'src').glob('*.cpp'))]
        sources+=[str(p)for p in sorted((native.OSC/'libs/oscpack/src').rglob('*.cpp'))if 'win32' not in str(p)]
        includes=[ROOT/'src',*sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),native.SUPERCOLLIDER/'src',native.OSC/'src',native.OSC/'libs/oscpack/src',*sorted({p.parent for p in (native.OSC/'libs/oscpack/src').rglob('*.h')})]
        native.mixer.main(extra_sources=sources,extra_includes=includes,std='c++20')
        validate(work)
if __name__=='__main__':main()
