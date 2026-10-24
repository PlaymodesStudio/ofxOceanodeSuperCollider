#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Server telemetry layout, shared by the editor and the offline GUI check.
// Keep in sync with aliasingBox.scd. Table rows are normalized 0..1 values.
// SCOPE..PAN is unused since the voice's scope went (nothing drew it).
struct scAliasingTableView {
    static constexpr int BINS = 256, METERS = 0, SCOPE = 96, PAN = 160, TABLE = 176;
    static int filterEnv(int voices) { return TABLE + std::clamp(voices,1,16) * BINS; }
    static int channels(int voices) { return filterEnv(voices) + std::clamp(voices,1,16); }
    static float value(const std::vector<float>& data, int offset) {
        return offset >= 0 && offset < int(data.size()) && std::isfinite(data[offset]) ? data[offset] : 0.0f;
    }
    static ImU32 intensity(float v) {
        v=std::clamp(v,0.0f,1.0f);
        const int lo[3]={8,10,35},mid[3]={63,101,77},hi[3]={208,255,150};
        const float t=v<0.5f ? v*2 : (v-0.5f)*2;
        const int* a=v<0.5f?lo:mid;const int* b=v<0.5f?mid:hi;
        return IM_COL32(int(a[0]+(b[0]-a[0])*t),int(a[1]+(b[1]-a[1])*t),int(a[2]+(b[2]-a[2])*t),255);
    }
    static void draw(ImDrawList* dl, ImVec2 a, ImVec2 size, const std::vector<float>& data,
                     int voices, int mode, float start, float span, float scale=1) {
        voices=std::clamp(voices,1,16);span=std::clamp(span,1.0f/BINS,1.0f);start=std::clamp(start,0.0f,1-span);
        const float row=size.y/voices,label=25*scale,left=a.x+label,width=std::max(1.0f,size.x-label);
        const int first=int(start*BINS),last=std::min(BINS,int(std::ceil((start+span)*BINS)));
        dl->AddRectFilled(a,ImVec2(a.x+size.x,a.y+size.y),IM_COL32(1,4,7,255));
        dl->PushClipRect(a,ImVec2(a.x+size.x,a.y+size.y),true);
        for(int voice=0;voice<voices;++voice) {
            float top=a.y+voice*row,bottom=top+row;
            char labelText[8];std::snprintf(labelText,sizeof(labelText),"%02d",voice+1);
            dl->AddText(ImVec2(a.x+3*scale,top+std::max(0.0f,(row-ImGui::GetFontSize())*0.5f)),IM_COL32(109,143,116,255),labelText);
            dl->PushClipRect(ImVec2(left,top),ImVec2(left+width,bottom),true);
            std::vector<ImVec2> points;
            if(mode==0) points.reserve((last-first)*2+2);
            for(int bin=first;bin<last;++bin) {
                float v=std::clamp(value(data,TABLE+voice*BINS+bin),0.0f,1.0f);
                float x1=left+(float(bin)/BINS-start)/span*width;
                float x2=left+(float(bin+1)/BINS-start)/span*width;
                if(mode==1) dl->AddRectFilled(ImVec2(x1,top),ImVec2(x2,bottom),intensity(v));
                else {
                    float y=bottom-2*scale-v*std::max(1.0f,row-4*scale);
                    points.emplace_back(x1,y);points.emplace_back(x2,y);
                }
            }
            if(!points.empty()) dl->AddPolyline(points.data(),int(points.size()),IM_COL32(207,255,173,255),0,scale);
            for(int head=0;head<2;++head) {
                float pos=value(data,METERS+voice*6+4+head);
                if(pos<start || pos>start+span) continue;
                float x=left+(pos-start)/span*width;
                dl->AddLine(ImVec2(x,top),ImVec2(x,bottom),head?IM_COL32(165,154,242,220):IM_COL32(206,255,223,190),scale);
            }
            dl->PopClipRect();
            if(mode==0 && voice+1<voices) dl->AddLine(ImVec2(left,bottom),ImVec2(left+width,bottom),IM_COL32(75,102,77,35));
        }
        dl->PopClipRect();
        dl->AddRect(a,ImVec2(a.x+size.x,a.y+size.y),IM_COL32(72,99,75,255));
    }
};
