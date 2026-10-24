#!/usr/bin/env python3
"""Validate .afx loading and render the DX7-based fmBox without a sound device."""
import argparse,json,math,pathlib,subprocess,sys,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
BANK=ROOT/'data/Supercollider/Presets/FMBox/DX7.afx'

def production_patches(work):
    s=(ROOT/'src/nodes/scFMBox.cpp').read_text()
    controls=s[s.index('    auto add = [this]'):s.index('    // ── Effects')]
    methods=s[s.index('ofJson scFMBox::dx7Snapshot'):s.index('bool scFMBox::loadAFX')]
    find=s[s.index('ofParameter<float>& scFMBox::soundParam'):s.index('void scFMBox::drawSoundTab')]
    cpp=r'''
#include "scDX7Afx.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using ofJson=nlohmann::json;
std::string ofToString(int i){return std::to_string(i);}
template<class T>struct ofParameter{
 std::string name;T value;
 void set(std::string n,T v,T,T){name=n;value=v;}
 std::string getEscapedName()const{return name;}
 T get()const{return value;}
};
struct SoundControl{ofParameter<float>p;std::string sc,group;};
struct scFMBox{
 struct Control{bool discrete=false;};std::vector<Control>controls;
 struct Dropdowns{void set(const std::string&,const std::vector<std::string>&){}}dropdowns;
 std::vector<std::unique_ptr<SoundControl>>sound;ofJson initial;
 std::vector<scDX7::PresetName>afxNames;
 std::vector<scDX7::PresetBank>afxBanks;int afxBankSelection=0;
 ofParameter<int>afxPreset;
 template<class T,class F>void reg(ofParameter<T>&,F){controls.emplace_back();}
 void sendAll(){}void setAlgorithm(int){}void recallAFX(int){}
 ofParameter<float>&soundParam(const std::string&);
 ofJson dx7Snapshot(const scDX7::Patch&,int)const;
 void setup(){CONTROLS
    for(auto&c:sound)initial[c->p.getEscapedName()]=c->p.get();
 }
};
FIND
METHODS
int main(int argc,char**argv){
 scFMBox box;box.setup();const auto bank=scDX7::load(argv[1]);
 std::vector<int>indices;for(int i=0;i<128;++i)indices.push_back(i);
 for(int algo=0;algo<32;++algo)for(int i=0;i<int(bank.size());++i)if(bank[i][0]==algo){indices.push_back(i);break;}
 for(int i:{1000,16383})indices.push_back(i);
 ofJson out=ofJson::array();
 for(int i:indices){auto snapshot=box.dx7Snapshot(bank[i],i);ofJson values=ofJson::object();
  for(auto&c:box.sound)values[c->sc]=snapshot[c->p.getEscapedName()];
  // The imported sound is self-contained: raw fields survive JSON roundtrip.
  auto restored=ofJson::parse(snapshot.dump());if(restored!=snapshot)return 2;
  out.push_back({{"index",i},{"values",values}});
 }
 std::cout<<out;
}
'''.replace('CONTROLS',controls).replace('FIND',find).replace('METHODS',methods)
    src=work/'patches.cpp';src.write_text(cpp)
    subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src/nodes'),'-I'+str(ROOT.parent.parent/'libs/json/include'),str(src),'-o',str(work/'patches')],check=True)
    return json.loads(subprocess.check_output([str(work/'patches'),str(BANK)],text=True))

def compile_voice(work):
    config=work/'sclang.yaml'
    config.write_text('includePaths:\n'+''.join('    - "'+str(p)+'"\n'for p in (ROOT/'sclang/SCClassLibrary',ROOT/'sclang/Extensions/user/SC3plugins/SkUGens'))+'excludeDefaultPaths: true\n')
    wrapper=work/'compile.scd'
    wrapper.write_text('Archive.archiveDir="'+str(work)+'";try {"'+str(ROOT/'synthdefs/Defaults/FMBox/fmBox.scd')+'".load;} { |e|e.reportError;1.exit; };0.exit;\n')
    r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(wrapper)],text=True,capture_output=True,timeout=30)
    log=r.stdout+r.stderr;(work/'compile.log').write_text(log)
    assert r.returncode==0 and 'FMBox SynthDefs written to 'in log and 'ERROR:'not in log.split('*** Welcome to SuperCollider')[-1],log

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--server',default='/Applications/SuperCollider.app/Contents/Resources/scsynth');p.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/fmbox-dx7-render'));a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fmbox-dx7-')as folder:
        work=pathlib.Path(folder);compile_voice(work);patches=production_patches(work)
        subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'src/nodes'),str(ROOT/'tools/fmbox/afx_test.cpp'),'-o',str(work/'afx_test')],check=True)
        subprocess.run([str(work/'afx_test'),str(BANK)],check=True)
    (a.output/'patches.json').write_text(json.dumps(patches,indent=2))
    defs=ROOT/'synthdefs/Defaults/FMBox/CompiledSynthdefs/FMBox';mod=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation';spread=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb';mixer=ROOT/'synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro'
    paths=[defs/'FMBoxVoice.scsyndef',defs/'FMBoxDX7LFO.scsyndef',spread/'PolyCombSpread2.scsyndef']
    for n in(1,4,16):paths +=[mod/f'{part}{n}.scsyndef'for part in('ModGate','ModLfo','ModEnv','ModRoute')]
    paths +=[mixer/f'mixerPro{part}2.scsyndef'for part in('Sat','EQ','Echo','Rvb')]
    score=bundle(0,[message('/d_load',str(x))for x in paths]+[message('/g_new',1,0,0),message('/c_setn',32,16,*([0.0]*16))])
    targets=['mpitch','mscale','mfeedback','mamp']+['m'+kind+str(i)for kind in('ratio','detune','level')for i in range(6)]
    quiet=sum(([key,32]for key in targets),[])
    def flat(values):return sum(([k,float(v)]for k,v in values.items()if k!='stereo'),[])
    def lfo(values,gatebus=64):return message('/s_new','FMBoxDX7LFO',110,1,1,'gatebus',gatebus,'out',200,*flat({k:v for k,v in values.items()if k.startswith('lfo_')}))
    def voice(values,id=101,v=0,out=0,extra=None):return message('/s_new','FMBoxVoice',id,1,1,'gatebus',64+v,'velbus',v,'nativebus',200,'out',out,'vis',2048,'voice',v,'pitch',60.0,'master',1.0,*quiet,*flat(values),*(extra or []))
    tests=[]
    # First occurrence of every DX7 algorithm, plus 16 sounds spread through the
    # factory bank. They may intentionally be silent: validate safety and report.
    chosen=patches[128:160]+patches[:128:8]+patches[-2:]
    for i,patch in enumerate(chosen):
        t=i*1.6+.01;values=patch['values']
        score+=bundle(t,[message('/s_new','ModGate1',100,1,1,'out',64,'vel',0,'rnd',16,'gate',1.0,'velocity',1.0),lfo(values),voice(values)])
        score+=bundle(t+.9,[message('/n_set',100,'gate',0.0)])+bundle(t+1.57,[message('/n_free',100,110,101)])
        tests.append((f"DX7 {patch['index']+1}",t+.15,t+.8))
    base=len(chosen)*1.6+.01
    # Controlled envelope makes the release assertion independent of arbitrary
    # DX7 patches, which can legitimately have nonzero L4 or very slow release.
    init=dict(patches[0]['values'],algorithm=31,feedback=0,transpose=24,env_time=1,osc_sync=1,lfo_pmd=0,lfo_amd=0)
    for op in range(6):
        pre=f'op{op}_';init.update({pre+'mode':0,pre+'coarse':1,pre+'fine':0,pre+'detune':7,pre+'output':99,pre+'velocity':0,pre+'left_depth':0,pre+'right_depth':0,pre+'rate_scale':0,pre+'amp_sens':0,f'mix{op}':int(op==0)})
        for k in range(4):init[pre+f'r{k}']=0;init[pre+f'l{k}']=99 if k<3 else 0
        for src in range(6):init[f'fm{op}_{src}']=0
    for test,n in enumerate((1,4,16)):
        t=base+test*2.3
        add=[message('/s_new',f'ModGate{n}',100,1,1,'out',64,'vel',0,'rnd',16,'gate',1.0,'velocity',1.0),message('/n_setn',100,'gate',n,*([1.0]*n)),lfo(init),
            message('/s_new',f'ModLfo{n}',120,1,1,'out',512,'freq',2.0),message('/s_new',f'ModEnv{n}',121,1,1,'out',528,'gate',64,'atk',0.01,'dec',0.05,'sus',0.8,'rel',0.05),
            message('/s_new',f'ModRoute{n}',122,1,1,'src',512,'out',560,'amount',0.15,'scale',16.0),message('/s_new',f'ModRoute{n}',123,1,1,'src',528,'out',576,'amount',1.0,'scale',1.0)]
        for v in range(n):add.append(voice(init,300+v,v,80+v,['normalization',1/math.sqrt(n),'mratio0',560+v,'mamp',576+v,'vca',1.0]))
        add.append(message('/s_new','PolyCombSpread2',130,1,1,'in',80,'out',0,'voices',n,'mpan',32,'vis',2200))
        score+=bundle(t,add)+bundle(t+1,[message('/n_setn',100,'gate',n,*([0.0]*n))])+bundle(t+2.1,[message('/n_free',100,110,120,121,122,123,130,*range(300,300+n))])
        tests.append((f'{n} voices',t+.2,t+.8))
    t=base+6.9
    add=[message('/s_new','ModGate1',100,1,1,'out',64,'vel',0,'rnd',16,'gate',1.0,'velocity',1.0),lfo(init),voice(init,101,0,80),message('/s_new','PolyCombSpread2',102,1,1,'in',80,'out',96,'voices',1,'mpan',32,'vis',2200),
        message('/s_new','mixerProSat2',103,1,1,'in',96,'out',112,'drive',12.0,'mix',1.0,'on',1.0),message('/s_new','mixerProEQ2',104,1,1,'in',112,'out',128,'b3gain',6.0),
        message('/s_new','mixerProEcho2',105,1,1,'in',128,'out',144,'delay',0.15,'feed',0.5,'mix',0.4,'on',1.0),message('/s_new','mixerProRvb2',106,1,1,'in',144,'out',0,'decay',0.6,'mix',0.35,'on',1.0)]
    score+=bundle(t,add)+bundle(t+1,[message('/n_set',100,'gate',0.0)])+bundle(t+3,[message('/n_free',100,101,102,103,104,105,106,110)])+bundle(t+3.1,[message('/c_set',32,0.0)])
    path=a.output/'test.osc';path.write_bytes(score);wav=a.output/'test.wav'
    r=subprocess.run([a.server,'-N',str(path),'_',str(wav),'48000','WAV','float','-o','2','-i','0','-z','64','-D','0','-w','8192','-m','262144'],capture_output=True,text=True,timeout=90)
    log=r.stdout+r.stderr;(a.output/'server.log').write_text(log)
    assert r.returncode==0 and not any(x in log for x in('FAILURE','ERROR','not installed')),log
    audio,ch,sr=read_wav(wav);assert all(math.isfinite(v)for v in audio)
    def samples(a,b):return audio[round(a*sr)*ch:round(b*sr)*ch:ch]
    def rms(a,b):
        x=samples(a,b);return math.sqrt(sum(v*v for v in x)/len(x))
    report=[]
    for name,a0,b0 in tests:
        x=samples(a0,b0);peak=max(map(abs,x));report.append(dict(name=name,peak=peak,rms=rms(a0,b0)))
        assert peak<1,(name,'clips',peak)
    assert sum(row['rms']>1e-5 for row in report[:32])>=20,'Unexpectedly many silent algorithms'
    for i,n in enumerate((1,4,16)):
        start=base+i*2.3
        assert rms(start+.2,start+.8)>1e-5,(n,'silent')
        assert max(map(abs,samples(start+1.5,start+1.9)))<1e-5,(n,'did not release')
    assert rms(t+1.4,t+1.8)>1e-6,'Effects tail missing'
    (a.output/'measurements.json').write_text(json.dumps(report,indent=2))
    print('PASS: 32 DX7 algorithms and factory samples render finite/unclipped; 1/4/16 voices with modulation release to silence; effects tails.')
    print('Output:',a.output)
if __name__=='__main__':main()
