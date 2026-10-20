#!/usr/bin/env python3
"""Render the production pitch UI headlessly with ImGui (no app/audio restart).
The adapter supplies parameters; controls and views are extracted from source.
Usage: python3 tools/polycomb/pitch_ui_preview.py [width] [height]
"""
import subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];s=(root/'src/nodes/scPolyComb.cpp').read_text();out=Path('/tmp/polycomb-pitch-ui');out.mkdir(exist_ok=True)
pre=r'''
#include "scPitchMapView.h"
#include "scQuantizedSlider.h"
#include "scPolyCombControlSteps.h"
#include "scPolyCombPitchMap.h"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <cmath>
using std::string;
std::map<std::string,bool>widgetEnabled;
namespace chordbox { struct LibItem {std::string name;std::vector<float> values;};inline const char* const* keyNames(){static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};return names;} }
std::string ofToString(int v){return std::to_string(v);}
std::vector<float> expand(const std::vector<float>& v,int n,float fallback=0){
 if(v.empty())return std::vector<float>(n,fallback);auto out=v;out.resize(n,v.back());return out;
}
float ofClamp(float x,float lo,float hi){return std::clamp(x,lo,hi);}
float first(const std::vector<float>&v,float d=0){return v.empty()?d:v[0];}
template<class T>struct ofParameter {T value{},minimum{},maximum{};string name;T get()const{return value;}T getMin()const{return minimum;}T getMax()const{return maximum;}void setMin(T v){minimum=v;}void setMax(T v){maximum=v;}void set(T v){value=v;}void setWithoutEventNotifications(T v){value=v;}string getEscapedName()const{return name;}void operator=(T v){set(v);}};
struct Drop {std::vector<const char*> names(const string&name){if(name.find("Shape")!=string::npos)return {"Ramp","Mirror","Sine"};if(name.find("Mode")!=string::npos)return {"Semitones","User scale","Snap to scale","Harmonic series"};if(name.find("Quantize")!=string::npos)return {"Nearest","Down","Up"};return {"After the map","In the scale"};}};
class scPolyComb {public: static constexpr int MAX_VOICES=32;
struct MapParams {ofParameter<int>scaleSelection,shape,mode,quantize,indexSymmetry,indexSeed,indexCycleShift,version;ofParameter<bool>indexInvert,indexSeedOnGate;ofParameter<float>indexSkew,indexRoundness,indexPW,indexPow,degreeOffset,octaveJump,indexComb,indexQuantize,indexRandom,indexWaves,first,interval,transpose,bend,offset,root,harmonicBase,span;ofParameter<std::vector<float>> tones,table;};
MapParams oscMap,combMap;ofParameter<bool>scramble;std::vector<int>scrambleOrder;
ofParameter<bool>pitchExternal,combMapOn;ofParameter<int>pitchShiftMode;ofParameter<std::vector<float>>pitch,transpose,ringPitch,ringTranspose,ringRatio,combTranspose;ofParameter<int>ringPitchSource;
std::vector<chordbox::LibItem> scaleLibrary{{"Major",{0,2,4,5,7,9,11}},{"Minor",{0,2,3,5,7,8,10}}},chordLibrary{{"Major triad",{0,4,7}}};
float labelW=70;Drop dropdowns;int voices=16;bool restoring=false;struct ServerState{};struct ofxSCServer{};enum{IComb};template<class F>void forStates(F f){ServerState st;f(nullptr,st);}void sendPitch(ServerState&){}void sendIns(ServerState&,int){};
float zoom(){return 1;}float gapW(){return 14;}int n()const{return voices;}bool internalPitch()const{return !pitchExternal.get();}bool combMapped()const{return internalPitch()&&combMapOn.get();}
void publishMenu(const std::string&key){widgetEnabled[key]=ImGui::GetStyle().Alpha>.99f;}void publishedUnderline(const std::string&){}
void updatePitchIndexLimits();void selectMapScale(MapParams&);void syncMapScale(MapParams&);const chordbox::LibItem*mapScaleAt(int)const;
void drawPitchTab(float w);void drawMapColumn(MapParams&,const char*,float,bool);pcmap::Map mapOf(const MapParams&)const;
std::vector<float>processedPitch()const{return pitch.get();}
std::vector<float>oscillatorPitches()const;std::vector<float>resonatorPitches()const;std::vector<float>ringSourcePitches()const;void drawRingPitchVector(float);void drawResonatorPitchMap(float);
std::vector<float>mapPitches(const MapParams&m)const;
void sectionHeader(const char*,float,ofParameter<bool>* =nullptr,ImU32=0);
void slider(ofParameter<float>&,const char*,float,const char*="%.2f",bool=false);
void sliderInt(ofParameter<int>&,const char*,float);void combo(ofParameter<int>&,const char*,float);void toggle(ofParameter<bool>&,const char*);
void init(){
 pitchExternal.set(false);combMapOn.set(true);combTranspose.set({0});pitch.set({60});transpose.set({0});ringPitch.set({60,64,67});ringTranspose.set({12});ringRatio.set({2});ringPitchSource.set(1);
 int id=0;
 for(auto*m:{&oscMap,&combMap}){
 auto f=[&](auto&p,const char*key,auto v,auto lo,auto hi){p.value=v;p.minimum=lo;p.maximum=hi;p.name=std::to_string(id)+key;};
 f(m->scaleSelection,"Scale",0,0,3);f(m->indexSkew,"Skew",id?-1.f:0.f,-1.f,1.f);f(m->indexRoundness,"Roundness",0.f,0.f,1.f);
 f(m->indexPW,"PW",.5f,.001f,1.f);f(m->indexPow,"Pow",.4f,-1.f,1.f);
 f(m->shape,"Shape",0,0,2);f(m->mode,"Mode",1,0,3);f(m->quantize,"Quantize",0,0,2);
 f(m->first,"First",1.f,0.f,32.f);f(m->interval,"Interval",1.f,-24.f,24.f);f(m->bend,"Bend",1.f,1.f,6.f);
 f(m->root,"Root",0.f,0.f,11.f);f(m->harmonicBase,"Harmonic",36.f,0.f,127.f);
 f(m->transpose,"Transpose",0.f,-72.f,72.f);f(m->offset,"Fine_Tune",0.f,-1.f,1.f);f(m->degreeOffset,"DegreeOffset",0.f,-256.f,256.f);
 f(m->octaveJump,"Octave Jump",1.f,0.f,4.f);f(m->span,"Span",0.f,0.f,48.f);f(m->indexWaves,"NumWaves",1.f,0.f,16.f);f(m->indexComb,"Comb",0.f,0.f,1.f);
 f(m->indexSeedOnGate,"New Seed On Gate",false,false,true);f(m->indexCycleShift,"Cycle Shift",0,-31,31);f(m->indexQuantize,"Group",0.f,0.f,16.f);f(m->indexRandom,"Random",0.f,0.f,16.f);f(m->indexSymmetry,"Symmetry",0,0,32);f(m->indexSeed,"Seed",0,0,9999);
 m->table.set(id?std::vector<float>{0,2,3,7,10}:std::vector<float>{0,4,7});m->tones.set({0,2,4,5,7,9,11});++id;
 }
 oscMap.interval.set(1.f);oscMap.shape.set(1);combMap.indexWaves.set(2);combMap.indexSymmetry.set(1);combMap.interval.set(1.f);
}
};
'''
pal=s[s.index('namespace pal {'):s.index('// ── Routing')]
helper=s[s.index('void scPolyComb::sectionHeader('):s.index('// One slider for every voice')]
helper+=s[s.index('void scPolyComb::sliderInt('):s.index('// The ADSR display:')]
ui=s[s.index('void scPolyComb::drawPitchTab('):s.index('// ── Delta:',s.index('void scPolyComb::drawPitchTab('))]
mapcode=s[s.index('void scPolyComb::updatePitchIndexLimits('):s.index('// ChordBox\'s libraries')]
ringui=s[s.index('void scPolyComb::drawResonatorPitchMap('):s.index('void scPolyComb::drawResonatorTab(')]
main=r'''
float edge(ImVec2 a,ImVec2 b,float x,float y){return (x-a.x)*(b.y-a.y)-(y-a.y)*(b.x-a.x);}
int main(int argc,char**argv){
 int W=argc>1?std::atoi(argv[1]):1150,H=argc>2?std::atoi(argv[2]):850;
 ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(W,H);io.DeltaTime=1.f/60;
 unsigned char*tex;int tw,th,bpp=4;io.Fonts->GetTexDataAsRGBA32(&tex,&tw,&th);io.Fonts->SetTexID((ImTextureID)1);
 ImGui::StyleColorsDark();scPolyComb pc;pc.init();
 // Production inherited pitch helper: independent map data cannot leak into shared mode.
 pc.voices=4;pc.oscMap.indexPow.set(0);pc.combMapOn.set(false);
 pc.ringPitchSource.set(0);if(pc.ringSourcePitches()!=std::vector<float>({60,64,67,67}))return 22;
 pc.ringPitchSource.set(1);pc.transpose.set({12});auto ring=pc.ringSourcePitches();auto osc=pc.oscillatorPitches();
 for(int i=0;i<4;++i)if(ring[i]!=osc[i]+12)return 23;
 pc.ringPitchSource.set(2);if(pc.ringSourcePitches()!=pc.resonatorPitches())return 24;
 pc.transpose.set({0});pc.ringPitchSource.set(1);
 auto shared=pc.resonatorPitches();if(shared!=pc.oscillatorPitches())return 15;
 pc.combMap.transpose.set(7);pc.combMap.offset.set(.137f);auto shifted=pc.resonatorPitches();
 for(int i=0;i<4;++i)if(std::abs(shifted[i]-shared[i]-7.137f)>1e-4)return 16;
 pc.combMapOn.set(true);if(pc.resonatorPitches()!=pc.mapPitches(pc.combMap))return 17;
 pc.combMap.mode.set(pcmap::ModeHarmonic);pc.combMap.transpose.set(12);pc.combMap.offset.set(.137f);
 if(std::abs(pc.resonatorPitches()[0]-48.137f)>1e-4)return 18;
 pc.pitchExternal.set(true);pc.combMap.transpose.set(7);pc.pitch.set({60,64,67});
 if(pc.oscillatorPitches()!=std::vector<float>({60,64,67,67}))return 19;
 shifted=pc.resonatorPitches();for(int i=0;i<4;++i)if(std::abs(shifted[i]-pc.oscillatorPitches()[i]-7.137f)>1e-4)return 20;
 pc.voices=16;pc.init();
 for(int count:{1,2,4,7,8,16,32}){
   pc.voices=count;pc.oscMap.first.set(32);pc.oscMap.indexCycleShift.set(31);pc.updatePitchIndexLimits();
   if(pc.oscMap.first.get()!=count || pc.oscMap.first.getMax()!=count || pc.oscMap.indexCycleShift.get()!=count-1 || pc.oscMap.indexSymmetry.getMax()!=count-1)return 8;
   auto&m=pc.oscMap;m.indexCycleShift.set(0);m.first.set(1);m.indexSkew.set(-1);m.indexRoundness.set(0);m.indexPW.set(.5);
   m.indexPow.set(0);m.indexWaves.set(1);m.indexInvert.set(false);m.indexSymmetry.set(0);m.indexComb.set(0);m.indexQuantize.set(0);m.indexRandom.set(0);
   m.interval.set(1);m.table.set({0,1,2,3,4,5,6,7,8,9,10,11});
   auto notes=pc.mapPitches(m);if(notes.size()!=count)return 9;
   for(int i=0;i<count;++i)if(notes[i]!=60+i)return 10;
   m.indexRandom.set(16);auto map=pc.mapOf(m);
   for(int i=0;i<count;++i){float index=pcmap::voiceIndex(map,i,count);if(index<1 || index>count)return 11;}
 }
 pc.voices=12;pc.init();pc.updatePitchIndexLimits();
 pc.oscMap.mode.set(pcmap::ModeOff);pc.oscMap.indexSkew.set(-1);pc.oscMap.indexPow.set(0);pc.oscMap.indexWaves.set(3);
 if(pc.mapPitches(pc.oscMap)!=std::vector<float>({61,64,67,70,61,64,67,70,61,64,67,70}))return 12;
 pc.oscMap.indexCycleShift.set(1);
 if(pc.mapPitches(pc.oscMap)!=std::vector<float>({61,64,67,70,62,65,68,71,63,66,69,72}))return 13;
 pc.oscMap.indexCycleShift.set(-1);
 if(pc.mapPitches(pc.oscMap)!=std::vector<float>({61,64,67,70,72,63,66,69,71,62,65,68}))return 14;
 pc.voices=16;pc.init();pc.updatePitchIndexLimits();
 pc.oscMap.scaleSelection.set(1);pc.selectMapScale(pc.oscMap);
 if(pc.oscMap.table.get()!=std::vector<float>({0,2,4,5,7,9,11}))return 4;
 pc.oscMap.mode.set(pcmap::ModeTones);pc.oscMap.scaleSelection.set(2);pc.selectMapScale(pc.oscMap);
 if(pc.oscMap.tones.get()!=std::vector<float>({0,2,3,5,7,8,10}))return 5;
 pc.oscMap.mode.set(pcmap::ModeTable);pc.oscMap.scaleSelection.set(3);pc.selectMapScale(pc.oscMap);
 if(pc.oscMap.table.get()!=std::vector<float>({0,4,7}))return 6;
 pc.oscMap.table.set({0,1,6});pc.syncMapScale(pc.oscMap);if(pc.oscMap.scaleSelection.get()!=0)return 7;
 pc.init();
 // Exercise the actual production mapPitches path, including incoming chords.
 pc.pitch.set({60,64,67});auto mapped=pc.mapPitches(pc.oscMap);
 for(int i=1;i<8;++i)if(mapped[i]!=mapped[16-i])return 2;
 pc.scramble.set(true);pc.scrambleOrder={15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
 auto reversed=pc.mapPitches(pc.oscMap);for(int i=0;i<16;++i)if(reversed[i]!=mapped[15-i])return 3;
 pc.scramble.set(false);pc.pitch.set({60});
 if(argc>3){pc.combMapOn.set(false);}
 for(int frame=0;frame<2;++frame){ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(W,H));ImGui::Begin("Pitch mapping",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize);if(argc>3 && std::string(argv[3])=="ring"){pc.drawRingPitchVector(W-20);}else if(argc>3 && std::string(argv[3])=="resonator"){pc.drawResonatorPitchMap(W-20);}else pc.drawPitchTab(W-20);std::cout<<"Used height "<<ImGui::GetCursorPosY()<<'\n';ImGui::End();ImGui::Render();}
 if(argc>3 && std::string(argv[3])!="ring" && std::string(argv[3])!="resonator"){
   if(!widgetEnabled["1Transpose"] || !widgetEnabled["1Fine_Tune"] || widgetEnabled["0Skew"])return 21;
 }
 io.Fonts->GetTexDataAsRGBA32(&tex,&tw,&th);
 std::vector<unsigned char>pixels(W*H*3,18);auto*data=ImGui::GetDrawData();
 for(auto*list:data->CmdLists)for(const auto&cmd:list->CmdBuffer){if(cmd.UserCallback)continue;
  if(cmd.TexRef._TexData){tex=cmd.TexRef._TexData->Pixels;tw=cmd.TexRef._TexData->Width;th=cmd.TexRef._TexData->Height;bpp=cmd.TexRef._TexData->BytesPerPixel;}
  for(unsigned int k=0;k<cmd.ElemCount;k+=3){
   const auto&v0=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+k]+cmd.VtxOffset];const auto&v1=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+k+1]+cmd.VtxOffset];const auto&v2=list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset+k+2]+cmd.VtxOffset];
   const float area=edge(v0.pos,v1.pos,v2.pos.x,v2.pos.y);if(std::abs(area)<1e-8)continue;
   int x0=std::max({0,(int)cmd.ClipRect.x,(int)std::floor(std::min({v0.pos.x,v1.pos.x,v2.pos.x}))});int x1=std::min({W,(int)cmd.ClipRect.z,(int)std::ceil(std::max({v0.pos.x,v1.pos.x,v2.pos.x}))});
   int y0=std::max({0,(int)cmd.ClipRect.y,(int)std::floor(std::min({v0.pos.y,v1.pos.y,v2.pos.y}))});int y1=std::min({H,(int)cmd.ClipRect.w,(int)std::ceil(std::max({v0.pos.y,v1.pos.y,v2.pos.y}))});
   for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
    float a=edge(v1.pos,v2.pos,x+.5,y+.5)/area,b=edge(v2.pos,v0.pos,x+.5,y+.5)/area,c=1-a-b;if(a<0||b<0||c<0)continue;
    const float u=a*v0.uv.x+b*v1.uv.x+c*v2.uv.x,v=a*v0.uv.y+b*v1.uv.y+c*v2.uv.y;
    const float tx=u*tw-.5f,ty=v*th-.5f;int ix=(int)std::floor(tx),iy=(int)std::floor(ty);const float fx=tx-ix,fy=ty-iy;float col[4];
    auto sample=[&](int x,int y,int c){x=std::clamp(x,0,tw-1);y=std::clamp(y,0,th-1);return bpp==1?(c==3?tex[y*tw+x]:255):tex[(y*tw+x)*4+c];};
    for(int j=0;j<4;++j){
      const float t=(1-fy)*((1-fx)*sample(ix,iy,j)+fx*sample(ix+1,iy,j))+fy*((1-fx)*sample(ix,iy+1,j)+fx*sample(ix+1,iy+1,j));
      col[j]=(a*((v0.col>>(j*8))&255)+b*((v1.col>>(j*8))&255)+c*((v2.col>>(j*8))&255))*t/255.f;
    }
    const float alpha=col[3]/255.f;for(int j=0;j<3;++j)pixels[(y*W+x)*3+j]=(unsigned char)std::clamp(col[j]*alpha+pixels[(y*W+x)*3+j]*(1-alpha),0.f,255.f);
   }
  }
 }
 std::ofstream file("/tmp/polycomb-pitch-ui/preview.ppm",std::ios::binary);file<<"P6\n"<<W<<' '<<H<<"\n255\n";file.write((char*)pixels.data(),pixels.size());
}
'''
# A space avoids the *= token in the compact prototype.
pre=pre.replace('const char*="','const char* ="')
(out/'preview.cpp').write_text(pre+pal+helper+mapcode+ui+ringui+main)
(out/'imconfig.h').write_text('#pragma once\n')

imgui=root.parent/'ofxImGui/libs/imgui/src'
subprocess.run(['c++','-std=c++17','-O1','-I'+str(out),'-I'+str(root/'src'),'-I'+str(root/'src/nodes'),'-I'+str(imgui),str(out/'preview.cpp')]+[str(imgui/f) for f in ('imgui.cpp','imgui_draw.cpp','imgui_widgets.cpp','imgui_tables.cpp')]+['-o',str(out/'preview')],check=True)
subprocess.run([str(out/'preview'),sys.argv[1] if len(sys.argv)>1 else '1150',sys.argv[2] if len(sys.argv)>2 else '850']+sys.argv[3:],check=True)
subprocess.run(['sips','-s','format','png',str(out/'preview.ppm'),'--out',str(out/'preview.png')],check=True,stdout=subprocess.DEVNULL)
print(out/'preview.png')
