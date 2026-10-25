#!/usr/bin/env python3
"""Exercise production AFX import/recall and shared external-snapshot application."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/boxSynths/fmBox/scFMBox.cpp').read_text()
harness=(ROOT/'tools/fmbox/render_test.py').read_text().split("    cpp=r'''",1)[1].split("'''.replace('CONTROLS'",1)[0]
a=s.index('    auto add = [this]');b=s.index('    // ── Effects',a)
controls=s[a:b]
a=s.index('ofJson scFMBox::dx7Snapshot');b=s.index('void scFMBox::drawPresets',a)
methods=s[a:b]
find=s[s.index('ofParameter<float>& scFMBox::soundParam'):s.index('void scFMBox::drawSoundTab')]
snapshot=(ROOT/'src/nodes/boxSynths/shared/scSnapshots.h').read_text();a=snapshot.index('    void applyExternal(');b=snapshot.index('\n    }',a)+6
external=snapshot[a:b]
harness=harness[:harness.index('int main(')]
harness=harness.replace('#include <vector>','#include <vector>\n#include <fstream>\n#include <filesystem>\n#include <functional>\n#include <cassert>')
harness=harness.replace('std::string name;T value;','std::string name;T value;\n void setMax(T){}void setWithoutEventNotifications(T v){value=v;}')
harness=harness.replace('struct SoundControl{',r'''
namespace ofFilePath {std::string getAbsolutePath(const std::string&p){return std::filesystem::absolute(p).string();}}
struct Snapshots{
 struct Host{std::function<void(const ofJson&,const ofJson&,float,bool)>apply;}host;
 struct Morph{void capture(ofJson&j){j["_morphAudio"]={{"Reverb On",0.4}};}}morph;
 bool running=true;int active=3;
 void setActive(int n){active=n;}
 EXTERNAL
};
struct SoundControl{'''.replace('EXTERNAL',external))
harness=harness.replace('void sendAll(){}void setAlgorithm(int){}void recallAFX(int){}','''void sendAll(){}void setAlgorithm(int){}
 Snapshots snaps;std::vector<scDX7::Patch>afxBank;std::string afxPath,afxMessage;ofJson current,applied;
 ofJson capture(){return current;}
 void recallAFX(int);bool loadAFX(const std::string&,bool recallFirst=true);
 std::vector<ofJson> factory() const;''')
harness=harness.replace('CONTROLS',controls).replace('FIND',find).replace('METHODS',methods)
harness+=r'''
int main(int argc,char**argv){
 assert(argc==3);scFMBox box;box.setup();
 box.snaps.host.apply=[&](const ofJson&a,const ofJson&,float,bool){box.applied=a;};
 assert(box.loadAFX(argv[1],false));assert(box.afxBank.size()==16384);
 assert(box.afxNames[0].name=="ARP 2600");
 assert(box.dx7Snapshot(box.afxBank[0],0)["name"]=="DX7 1 / ARP 2600");
 const auto factory=box.factory();assert(factory.size()==32);
 assert(factory[0]["name"]=="BRASS 1"&&factory[10]["name"]=="E.PIANO 1"&&factory[31]["name"]=="TAKE OFF");
 for(int slot=0;slot<32;++slot){
  const int index=12544+slot;
  assert(box.afxNames[index].bank=="ROM1A");
  auto expected=box.dx7Snapshot(box.afxBank[index],index);
  expected["name"]=box.afxNames[index].name;
  assert(factory[slot]==expected); // Complete converted sound, in original order.
 }
 assert(factory[0]["Algorithm"]==21&&factory[0]["Transpose"]==24);
 assert(factory[10]["Algorithm"]==4&&factory[10]["Transpose"]==24);
 assert(box.afxNames[12800].bank=="SAX_01");
 const auto originalBank=box.afxBank;
 std::string broken=std::string(argv[2])+"/broken.afx";
 {std::ofstream f(broken);f<<"not an afx bank\n";}
 const auto originalPath=box.afxPath;
 assert(!box.loadAFX(broken));assert(box.afxBank==originalBank&&box.afxPath==originalPath);
 assert(box.afxNames[0].name=="ARP 2600"); // Failed imports keep the catalogue, too.
 box.current=box.initial;box.current["Reverb On"]=true;box.current["Width"]=0.25;
 box.current["Placement"]=2;box.current["Speaker Layout"]=1;box.current["Spread Center"]=-0.2;box.current["Pan"]={0.1,0.8};
 box.current["fmModParams"]=ofJson::array({{{"key","Op 1 Fine"},{"base",{95}},{"amounts",{{"0",0.25}}}}});
 box.recallAFX(0);
 assert(!box.snaps.running&&box.snaps.active==-1);
 assert(box.applied["Reverb On"]==true&&box.applied["Width"]==0.25);
 assert(box.applied["Placement"]==2&&box.applied["Speaker Layout"]==1&&box.applied["Spread Center"]==-0.2);
 assert(box.applied["Pan"]==ofJson::array({0.1,0.8}));
 assert(box.applied["_morphAudio"]["Reverb On"]==0.4);
 assert(box.applied["Op 1 Fine"]==originalBank[0][21]);
 assert(box.applied["fmModParams"][0]["base"][0]==originalBank[0][21]);
 assert(box.applied["fmModParams"][0]["amounts"]["0"]==0.25);
 const auto snapshot=ofJson::parse(box.applied.dump());
 box.afxBank.clear();box.snaps.running=true;box.applied={};
 assert(box.factory().size()==1&&box.factory()[0]["name"]=="DX7 Init");
 box.snaps.applyExternal(snapshot);
 assert(box.applied==snapshot&&!box.snaps.running&&box.snaps.active==-1);
 std::cout<<"PASS: 32 named ROM1A factory snapshots and init fallback; rejected imports retain bank; AFX recall preserves effects/modulation and rebases controls; snapshot glide stops; stored sounds are independent of source files\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fmbox-state-')as folder:
    work=pathlib.Path(folder);src=work/'state.cpp';src.write_text(harness)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'src'),'-I'+str(ROOT.parent.parent/'libs/json/include'),str(src),'-o',str(work/'state')],check=True)
    subprocess.run([str(work/'state'),str(ROOT/'data/Supercollider/Presets/FMBox/DX7.afx'),str(work)],check=True)
