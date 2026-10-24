#pragma once
// Reusable, node-independent pitch views. The host owns parameters and audio.
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace scPitchMapView {
inline const char* pitchClass(int note) {
    static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return names[((note%12)+12)%12];
}
inline bool blackKey(int note) { const int p=((note%12)+12)%12; return p==1||p==3||p==6||p==8||p==10; }
inline std::string noteName(float note) {
    const int midi=(int)std::lround(note); char text[32];
    std::snprintf(text,sizeof(text),"%s%d",pitchClass(midi),(int)std::floor(midi/12.0)-1);
    return text;
}
inline void smallText(ImDrawList* dl,ImVec2 p,ImU32 color,const std::string& text,float z=1) {
    dl->AddText(ImGui::GetFont(),10*z,p,color,text.c_str());
}
inline void frame(ImDrawList* dl,ImVec2 a,ImVec2 b,float z) {
    dl->AddRectFilled(a,b,IM_COL32(18,22,29,255),3*z);
    dl->AddRect(a,b,IM_COL32(63,73,88,255),3*z);
}
// Twelve discrete slider positions, with exact entry and N distinct offsets.
inline bool scaleEditor(std::vector<float>& notes,int root,float width,float height,float z,ImU32 color) {
    if(notes.empty()) notes={0};
    const int count=(int)notes.size(); bool changed=false;
    ImGui::PushID("scale-editor");
    const ImVec2 a=ImGui::GetCursorScreenPos(),b(a.x+width,a.y+height);
    auto* dl=ImGui::GetWindowDrawList(); frame(dl,a,b,z);
    const float keys=30*z,row=height/12,cell=(width-keys)/count;
    for(int i=0;i<12;++i){
        const float y=b.y-(i+1)*row;
        const bool black=blackKey(root+i);
        dl->AddRectFilled(ImVec2(a.x,y),ImVec2(a.x+keys-2*z,y+row-1),black?IM_COL32(47,52,61,255):IM_COL32(197,204,215,255));
        smallText(dl,ImVec2(a.x+3*z,y+(row-10*z)*.5f),black?IM_COL32(220,225,234,255):IM_COL32(25,29,36,255),pitchClass(root+i),z);
        dl->AddLine(ImVec2(a.x+keys,y),ImVec2(b.x,y),i==0?IM_COL32(102,121,142,150):IM_COL32(83,97,116,65));
    }
    auto set=[&](int i,int value){
        value=std::clamp(value,0,11);
        if((int)notes[i]==value) return;
        auto other=std::find(notes.begin(),notes.end(),(float)value);
        if(other!=notes.end()) *other=notes[i];
        notes[i]=(float)value;changed=true;
    };
    for(int i=0;i<count;++i){
        ImGui::PushID(i);
        const float x=a.x+keys+i*cell,center=x+cell*.5f,y=b.y-(notes[i]+.5f)*row;
        dl->AddLine(ImVec2(center,a.y+3*z),ImVec2(center,b.y-3*z),IM_COL32(87,104,125,90),2*z);
        dl->AddRectFilled(ImVec2(x+2*z,y),ImVec2(x+cell-2*z,b.y-1), (color&0xffffff)|0x32000000);
        dl->AddRectFilled(ImVec2(x+2*z,y-row*.35f),ImVec2(x+cell-2*z,y+row*.35f),color,2*z);
        ImGui::SetCursorScreenPos(ImVec2(x,a.y));
        ImGui::InvisibleButton("note",ImVec2(cell,height));
        if(ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            set(i,(int)std::floor((b.y-ImGui::GetIO().MousePos.y)*12/height));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Scale note %d: %s  /  offset %d\nDrag vertically; right-click to type 0-11.\nChoosing an existing note swaps the two sliders.",i+1,pitchClass(root+(int)notes[i]),(int)notes[i]);
        if(ImGui::BeginPopupContextItem("exact")){
            ImGui::Text("Offset from %s (0-11)",pitchClass(root));
            int value=(int)notes[i];ImGui::SetNextItemWidth(125*z);
            if(ImGui::InputInt("##note-value",&value)) set(i,value);
            ImGui::EndPopup();
        }
        char label[20];std::snprintf(label,sizeof(label),"%s",pitchClass(root+(int)notes[i]));
        const float font=std::min(10*z,std::max(7*z,cell*.55f));
        dl->AddText(ImGui::GetFont(),font,ImVec2(x+2*z,b.y+3*z),IM_COL32(224,230,239,255),label);
        std::snprintf(label,sizeof(label),"%d",(int)notes[i]);
        dl->AddText(ImGui::GetFont(),font,ImVec2(x+2*z,b.y+14*z),IM_COL32(144,158,178,255),label);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x,b.y+26*z));ImGui::Dummy(ImVec2(width,0));
    ImGui::PopID();return changed;
}
// Intermediate integer positions: the geometry can be read before hearing it.
inline void indices(const std::vector<float>& values,float width,float height,float z,ImU32 color) {
    if(values.empty())return;
    const ImVec2 a=ImGui::GetCursorScreenPos(),b(a.x+width,a.y+height);
    auto* dl=ImGui::GetWindowDrawList();frame(dl,a,b,z);
    const auto mm=std::minmax_element(values.begin(),values.end());
    const float lo=std::min(0.f,*mm.first),range=std::max(1.f,*mm.second-lo),cell=width/values.size();
    for(int i=0;i<(int)values.size();++i){
        const float x=a.x+i*cell,y=b.y-4*z-(values[i]-lo)/range*(height-8*z);
        dl->AddRectFilled(ImVec2(x+1,y),ImVec2(x+cell-1,b.y-2*z),(color&0xffffff)|0x40000000);
        dl->AddLine(ImVec2(x+1,y),ImVec2(x+cell-1,y),color,2*z);
    }
    ImGui::InvisibleButton("##index-geometry",ImVec2(width,height));
    if(ImGui::IsItemHovered()){
        const int i=std::clamp((int)((ImGui::GetIO().MousePos.x-a.x)/cell),0,(int)values.size()-1);
        ImGui::SetTooltip("Voice %d -> integer index %.0f",i+1,values[i]);
    }
}
// Compact pitch vector: source bars and nominal carrier markers after tuning
// and frequency ratio. The view needs no audio polling or per-frame OSC.
inline void pitchVector(const std::vector<float>& source,const std::vector<float>& transpose,
                        const std::vector<float>& ratio,float width,float height,float z,ImU32 color) {
    if(source.empty())return;
    auto at=[](const std::vector<float>& v,size_t i,float fallback){return v.empty()?fallback:v[std::min(i,v.size()-1)];};
    std::vector<float> carrier(source.size()),hz(source.size());
    float lo=source.front(),hi=source.front();
    for(size_t i=0;i<source.size();++i){
        const float tuned=std::clamp(source[i]+at(transpose,i,0),0.f,135.f);
        const float r=std::max(0.f,at(ratio,i,1));
        hz[i]=440.f*std::pow(2.f,(tuned-69.f)/12.f)*r;
        carrier[i]=r>0?tuned+12.f*std::log2(r):source[i];
        lo=std::min(lo,std::min(source[i],carrier[i]));hi=std::max(hi,std::max(source[i],carrier[i]));
    }
    lo=std::floor(lo/12.f)*12.f;hi=std::max(lo+12.f,std::ceil(hi/12.f)*12.f);
    ImGui::PushID("pitch-vector");
    ImGui::TextDisabled("Pitch vector / nominal");
    const auto a=ImGui::GetCursorScreenPos();const ImVec2 b(a.x+width,a.y+height);
    auto* dl=ImGui::GetWindowDrawList();frame(dl,a,b,z);
    const float gutter=43*z,cell=std::max(1.f,(width-gutter)/source.size());
    auto y=[&](float note){return b.y-8*z-(note-lo)/(hi-lo)*(height-18*z);};
    const ImU32 sourceColor=IM_COL32(116,131,153,160);
    for(float note=lo;note<=hi;note+=12){
        const float yy=y(note);dl->AddLine(ImVec2(a.x+gutter,yy),ImVec2(b.x-2*z,yy),IM_COL32(80,96,116,65));
    }
    smallText(dl,ImVec2(a.x+3*z,a.y+3*z),IM_COL32(160,173,192,255),noteName(hi),z);
    smallText(dl,ImVec2(a.x+3*z,b.y-15*z),IM_COL32(160,173,192,255),noteName(lo),z);
    for(size_t i=0;i<source.size();++i){
        const float x=a.x+gutter+(i+.5f)*cell;
        dl->AddRectFilled(ImVec2(x-cell*.3f,y(source[i])),ImVec2(x+cell*.3f,b.y-3*z),sourceColor);
        if(hz[i]>0)dl->AddCircleFilled(ImVec2(x,y(carrier[i])),2.5f*z,color);
        else{dl->AddLine(ImVec2(x-2*z,b.y-7*z),ImVec2(x+2*z,b.y-3*z),color);dl->AddLine(ImVec2(x-2*z,b.y-3*z),ImVec2(x+2*z,b.y-7*z),color);}
    }
    ImGui::InvisibleButton("##vector",ImVec2(width,height));
    if(ImGui::IsItemHovered()){
        const size_t i=(size_t)std::clamp((int)((ImGui::GetIO().MousePos.x-a.x-gutter)/cell),0,(int)source.size()-1);
        if(hz[i]>0)ImGui::SetTooltip("Voice %d\nSource: %s / MIDI %.3f\nTranspose: %+.3f st / Ratio: x%.3f\nCarrier: %s / MIDI %.3f / %.2f Hz\nNominal pitch before live modulation and frequency limiting.",
            (int)i+1,noteName(source[i]).c_str(),source[i],at(transpose,i,0),at(ratio,i,1),noteName(carrier[i]).c_str(),carrier[i],hz[i]);
        else ImGui::SetTooltip("Voice %d\nSource: %s / MIDI %.3f\nCarrier: 0 Hz (ratio zero)",(int)i+1,noteName(source[i]).c_str(),source[i]);
    }
    ImGui::TextDisabled("Source bars / Carrier dots (%d voices)",(int)source.size());
    ImGui::PopID();
}

// Minimal read-only pitch bars for the instrument tabs. No axis, labels or grid.
inline void pitchBars(const std::vector<float>& notes,float width,float height,float z,ImU32 color) {
    if(notes.empty())return;
    ImGui::PushID("compact-pitches");
    const auto a=ImGui::GetCursorScreenPos();const ImVec2 b(a.x+width,a.y+height);
    auto* dl=ImGui::GetWindowDrawList();frame(dl,a,b,z);
    const auto mm=std::minmax_element(notes.begin(),notes.end());
    const float lo=*mm.first-1.f,range=std::max(12.f,*mm.second-lo+1.f),cell=width/notes.size();
    for(size_t i=0;i<notes.size();++i){
        const float x=a.x+i*cell,y=b.y-4*z-(notes[i]-lo)/range*(height-8*z);
        dl->AddRectFilled(ImVec2(x+1,y),ImVec2(x+cell-1,b.y-2*z),(color&0xffffff)|0x35000000);
        dl->AddRectFilled(ImVec2(x+1,y-1*z),ImVec2(x+cell-1,y+1*z),color,z);
    }
    ImGui::InvisibleButton("##bars",ImVec2(width,height));
    if(ImGui::IsItemHovered()){
        const int i=std::clamp((int)((ImGui::GetIO().MousePos.x-a.x)/cell),0,(int)notes.size()-1);
        ImGui::SetTooltip("Voice %d  |  %s  |  MIDI %.3f\nNominal pitch before live modulation and detune.",i+1,noteName(notes[i]).c_str(),notes[i]);
    }
    ImGui::PopID();
}

// A read-only piano roll with slider-shaped bars. A bar is a generated voice,
// not an independent pitch override: edits belong to the scale/index controls.
inline void result(const std::vector<float>& notes,const std::vector<float>& degrees,float width,float height,float z,ImU32 color,bool compact=false) {
    ImGui::PushID("generated-notes");
    if(notes.empty()){ImGui::TextDisabled("No voices");ImGui::PopID();return;}
    const auto mm=std::minmax_element(notes.begin(),notes.end());
    const float lo=std::floor(*mm.first/12)*12;
    const float hi=std::max(lo+12,std::ceil(*mm.second/12)*12);
    const ImVec2 a=ImGui::GetCursorScreenPos(),b(a.x+width,a.y+height);
    auto* dl=ImGui::GetWindowDrawList();frame(dl,a,b,z);
    const float keys=(compact?58:76)*z,row=height/(hi-lo+1),cell=(width-keys)/notes.size();
    auto y=[&](float p){return b.y-(p-lo+.5f)*height/(hi-lo+1);};
    for(int note=(int)lo;note<=(int)hi;++note){
        const float center=y((float)note),top=center-row*.5f,bottom=center+row*.5f;
        if(top<a.y||bottom>b.y)continue;
        const bool black=blackKey(note);
        dl->AddRectFilled(ImVec2(a.x,top),ImVec2(a.x+(black?24:30)*z,bottom),black?IM_COL32(55,62,74,255):IM_COL32(195,203,215,255));
        if(note%12==0||row>=7*z)
            dl->AddLine(ImVec2(a.x+keys,center),ImVec2(b.x,center),note%12==0?IM_COL32(122,143,170,100):IM_COL32(88,105,128,40));
    }
    // Draw labels after all keys so adjacent key rectangles cannot cover them.
    for(int note=(int)lo;note<=(int)hi;++note){
        if(note%12!=0&&row<11*z)continue;
        const float center=y((float)note);
        if(center-5*z<a.y||center+5*z>b.y)continue;
        smallText(dl,ImVec2(a.x+32*z,center-5*z),IM_COL32(210,220,235,255),noteName((float)note)+" "+std::to_string(note),z);
    }
    // The top/bottom MIDI numbers remain visible even over wide ranges.
    char text[32];std::snprintf(text,sizeof(text),"%.0f",hi);smallText(dl,ImVec2(a.x+31*z,a.y+2*z),IM_COL32(159,174,195,255),text,z);
    std::snprintf(text,sizeof(text),"%.0f",lo);smallText(dl,ImVec2(a.x+31*z,b.y-12*z),IM_COL32(159,174,195,255),text,z);
    const bool fractional=std::any_of(notes.begin(),notes.end(),[](float note){return std::abs(note-std::round(note))>1.e-4f;});
    const int labelEvery=std::max(1,(int)std::ceil((fractional?36:28)*z/std::max(1.f,cell)));
    for(int i=0;i<(int)notes.size();++i){
        const float x=a.x+keys+i*cell,yy=y(notes[i]);
        dl->AddRectFilled(ImVec2(x+1,yy),ImVec2(x+cell-1,b.y-1),(color&0xffffff)|0x35000000);
        dl->AddRectFilled(ImVec2(x+1,yy-2*z),ImVec2(x+cell-1,yy+2*z),color,1*z);
        if(i%labelEvery==0||i==(int)notes.size()-1){
            std::snprintf(text,sizeof(text),fractional?"%.2f":"%.0f",notes[i]);smallText(dl,ImVec2(x+1,std::max(a.y+2*z,yy-13*z)),IM_COL32(231,236,244,255),text,z);
            std::snprintf(text,sizeof(text),"%d",i+1);smallText(dl,ImVec2(x+1,b.y+3*z),IM_COL32(145,160,182,255),text,z);
        }
    }
    ImGui::InvisibleButton("##notes",ImVec2(width,height));
    if(ImGui::IsItemHovered()){
        const int i=std::clamp((int)((ImGui::GetIO().MousePos.x-a.x-keys)/cell),0,(int)notes.size()-1);
        if(compact)ImGui::SetTooltip("Voice %d  |  %s  |  MIDI %.3f\nNominal resonator pitch before live modulation and detune.",i+1,noteName(notes[i]).c_str(),notes[i]);
        else ImGui::SetTooltip("Voice %d  |  %s  |  MIDI %.3f\nMapped position %.2f\nGenerated from the scale and index controls.",i+1,noteName(notes[i]).c_str(),notes[i],degrees.empty()?0.f:degrees[std::min((size_t)i,degrees.size()-1)]);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x,b.y+17*z));ImGui::Dummy(ImVec2(width,0));
    if(!compact){
    ImGui::Text("First %s / %.2f   Last %s / %.2f",noteName(notes.front()).c_str(),notes.front(),noteName(notes.back()).c_str(),notes.back());
    ImGui::TextDisabled("Range %s / %.1f to %s / %.1f   |   %d voices",noteName(*mm.first).c_str(),*mm.first,noteName(*mm.second).c_str(),*mm.second,(int)notes.size());
    }
    ImGui::PopID();
}
}
