#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdio>

namespace scwidgets {
// Integer ticks with labels in the parameter's units. A float slider renders
// its continuous mouse value before the caller can snap it.
inline bool quantizedSlider(const char* id,float& value,float lo,float hi,float quantum,const char* format) {
    int tick=(int)std::lround(value/quantum);
    const bool changed=ImGui::SliderInt(id,&tick,(int)std::lround(lo/quantum),
        (int)std::lround(hi/quantum),"",ImGuiSliderFlags_NoInput);
    if(changed)value=tick*quantum;
    char text[64];std::snprintf(text,sizeof(text),format,tick*quantum);
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(),size=ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2((a.x+b.x-size.x)*.5f,(a.y+b.y-size.y)*.5f),
                                      ImGui::GetColorU32(ImGuiCol_Text),text);
    return changed;
}
} // namespace scwidgets
