#!/usr/bin/env python3
"""Exercise production filter selection/control branches and legacy migration."""
import pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scPolyComb.cpp').read_text()
a=s.index('        case IFilter:',s.index('void scPolyComb::sendIns'));b=s.index('        case IComb:',a)
branches=s[a:b]
a=s.index('        const char* name=insDefs[i];');b=s.index('        bring(st.ins[i]',a)
selection=s[a:b]
a=s.index('std::pair<int,int> filterChoice(');b=s.index('\n}',a)+2
choice=s[a:b]
code=r'''
#include "scPolyCombMigration.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
template<class T>struct P{T value{};T get()const{return value;}};
struct Synth{std::map<std::string,std::vector<float>>v;void set(std::string key,float value){v[key]={value};}};
struct ServerState{};
CHOICE
struct Node{
 enum{IFilter,IComb,IRing,IPost,IShaper,ZERO_TARGET=14};
 P<int>filterModel,postFilterModel,fType,postType;
 P<float>keyTrack,postKeyTrack;
 P<std::vector<float>>cutoff,res,postCut,postRes;
 Synth storage;ServerState st;
 int mod(ServerState&,int target){return 100+target;}
 void sendVec(Synth*s,std::string key,const std::vector<float>&v){s->v[key]=v;}
 void send(int i){auto*s=&storage;storage.v.clear();switch(i){BRANCHES}}
 std::string definition(int i){
  const char* insDefs[]={"PCFilter","PCCombR","PCRingR","PCFilter","PCShaper"};
  SELECTION
  return name;
 }
};
int main(){
 Node n;
 for(int pre:{0,1})for(int post:{0,1})for(int type=0;type<12;++type){
  n.filterModel.value=pre;n.postFilterModel.value=post;n.fType.value=n.postType.value=type;
  n.keyTrack.value=.25f;n.postKeyTrack.value=.75f;
  n.cutoff.value={60,72};n.postCut.value={80,92};n.res.value={.3f};n.postRes.value={.5f};
  auto expected=[&](int model){return model==1?"PCFilterR":type>=4?"PCFilterSlopes":"PCFilter";};
  assert(n.definition(Node::IFilter)==expected(pre));assert(n.definition(Node::IPost)==expected(post));
  n.send(Node::IFilter);
  assert(n.storage.v["keytrack"][0]==.25f && n.storage.v["cutoff"]==n.cutoff.value);
  assert(n.storage.v["type"][0]==(pre==1?type:filterChoice(type).first));
  n.send(Node::IPost);
  assert(n.storage.v["keytrack"][0]==.75f && n.storage.v["cutoff"]==n.postCut.value && n.storage.v["res"]==n.postRes.value);
  assert(n.storage.v["type"][0]==(post==1?type:filterChoice(type).first));
  if(type>=4 && post==0)assert(n.storage.v["slope"][0]==filterChoice(type).second);
  assert(n.storage.v["mcut"][0]==109);
 }
 using Json=nlohmann::json;
 for(int pre:{0,1}){
  auto legacy=pcmigration::controls(Json{{"Filter_Model",pre}});
  assert(legacy["Post_Filter_Model"]==pre && legacy["Post_Key_Track"]==0.f);
  auto current=pcmigration::controls(Json{{"Filter_Model",pre},{"Post_Filter_Model",1-pre},{"Post_Key_Track",.75f}});
  assert(current["Post_Filter_Model"]==1-pre && current["Post_Key_Track"]==.75f);
  assert(pcmigration::controls(current)==current);
 }
}
'''.replace('CHOICE',choice).replace('BRANCHES',branches).replace('SELECTION',selection)
with tempfile.TemporaryDirectory(prefix='polycomb-post-filter-')as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src/nodes'),'-I'+str(ROOT.parent.parent/'libs/json/include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: independent pre/post models for all 12 types, key tracking, cutoff/resonance, modulation buses and legacy/idempotent migration.')
