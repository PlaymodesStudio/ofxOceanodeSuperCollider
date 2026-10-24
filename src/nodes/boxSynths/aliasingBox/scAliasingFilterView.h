#pragma once
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>

// PolyComb's log-frequency response canvas, with coefficients matching the
// actual AliasingBox BHiPass4 / BLowPass4 and BPeakEQ rather than a generic Q.
namespace scAliasingFilterView {
inline double response(int type,double f,double fc,double resonance,double gain,double sr) {
    constexpr double pi=3.14159265358979323846;
    fc=std::clamp(fc,10.0,sr*.45);f=std::clamp(f,1.0,sr*.49);
    const double w=2*pi*fc/sr,c=std::cos(w),rq=std::clamp(1-resonance,.02,1.0);
    const double alpha=std::sin(w)*.5*(type==2 ? rq : std::sqrt(rq));
    double b0,b1,b2,a0=1+alpha,a1=-2*c,a2=1-alpha;
    if(type==0){b0=(1+c)*.5;b1=-(1+c);b2=b0;}
    else if(type==1){b0=(1-c)*.5;b1=1-c;b2=b0;}
    else {const double A=std::pow(10.0,gain/40);b0=1+alpha*A;b1=-2*c;b2=1-alpha*A;a0=1+alpha/A;a2=1-alpha/A;}
    const auto z=std::polar(1.0,-2*pi*f/sr);
    const double db=20*std::log10(std::max(1e-12,std::abs((b0+b1*z+b2*z*z)/(a0+a1*z+a2*z*z))));
    return db*(type==2 ? 1 : 2);
}
// Returns true on drag; x edits pitch, y edits resonance (gain on the notch).
inline bool draw(const char* id,float w,float h,int type,float& pitch,float& resonance,float& gain,
                 float sr,bool enabled,ImU32 color,float scale=1) {
    const auto a=ImGui::GetCursorScreenPos();const ImVec2 b(a.x+w,a.y+h);
    auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(a,b,IM_COL32(11,12,14,255),3*scale);dl->AddRect(a,b,IM_COL32(46,50,58,255),3*scale);
    const float high=std::min(20000.0f,sr*.45f),span=std::log(high/20),zero=a.y+h*.35f;
    for(float f:{100.0f,1000.0f,10000.0f}) if(f<high) {
        const float x=a.x+w*std::log(f/20)/span;
        dl->AddLine(ImVec2(x,a.y),ImVec2(x,b.y),IM_COL32(255,255,255,14));
    }
    dl->AddLine(ImVec2(a.x,zero),ImVec2(b.x,zero),IM_COL32(255,255,255,34));
    const float fc=std::clamp(440*std::pow(2.0f,(pitch-69)/12),10.0f,high);
    std::array<ImVec2,129> pts;
    for(int i=0;i<129;++i) {
        const float f=20*std::exp(span*i/128);
        const float db=enabled ? response(type,f,fc,resonance,gain,sr) : 0;
        pts[i]=ImVec2(a.x+w*i/128,std::clamp(zero-db*h/60,a.y+1,b.y-1));
    }
    dl->AddPolyline(pts.data(),pts.size(),enabled ? color : IM_COL32(100,105,115,180),0,2*scale);
    if(enabled && fc>=20) {
        const float x=a.x+w*std::log(fc/20)/span;
        dl->AddLine(ImVec2(x,a.y),ImVec2(x,b.y),color,scale);
    }
    char text[48];std::snprintf(text,sizeof(text),"%.0f Hz / %s",fc,enabled ? "24 dB/oct" : "bypassed");
    if(type==2) std::snprintf(text,sizeof(text),"%.0f Hz / %+.1f dB%s",fc,gain,enabled ? "" : " / bypassed");
    dl->AddText(ImVec2(a.x+4*scale,b.y-ImGui::GetTextLineHeight()-2*scale),IM_COL32(150,150,160,255),text);
    ImGui::InvisibleButton(id,ImVec2(w,h));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(type==2 ? "Drag: cutoff / gain. Right-click: cutoff value, publish or timeline." : "Drag: cutoff / resonance. Right-click: cutoff value, publish or timeline.");
    if(enabled && ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        const auto m=ImGui::GetMousePos();
        const float hz=20*std::exp(span*std::clamp((m.x-a.x)/w,0.0f,1.0f));
        pitch=69+12*std::log2(hz/440);
        if(type==2) gain=std::clamp((zero-m.y)*60/h,-30.0f,20.0f);
        else resonance=std::clamp(1-(m.y-a.y)/h,0.0f,.98f);
        return true;
    }
    return false;
}
}
