#pragma once

#include "imgui.h"
#include <algorithm>
#include "ofxOceanodeShared.h"

// A "?" at the right end of a section's title bar: hovering it shows the
// section's explanation, so the windows don't spend space on wrapped notes.
// Draws only (no ImGui item): call it after drawing the bar, with the bar's
// top-left corner, width and height.
inline void scSectionHelp(ImVec2 a, float w, float h, const char* text) {
    if(!text || !*text) return;
    const float z = ofxOceanodeShared::getZoomLevel();
    const float r = std::max(5.0f * z, h * 0.3f);
    const ImVec2 c(a.x + w - r - 6.0f * z, a.y + h * 0.5f);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseHoveringRect(ImVec2(c.x - r - 3 * z, a.y), ImVec2(c.x + r + 3 * z, a.y + h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircle(c, r, hovered ? IM_COL32(230, 230, 240, 255) : IM_COL32(150, 150, 160, 160), 0, 1.2f * z);
    const ImVec2 ts = ImGui::CalcTextSize("?");
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), hovered ? IM_COL32(240, 240, 250, 255) : IM_COL32(170, 170, 180, 200), "?");
    if(hovered){
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The Box synths' shared effects (MixerPro's inserts), one text each
namespace scHelp {
inline constexpr const char* saturation =
    "Warm to hard distortion on the whole output, the first effect in the chain.\n\n"
    "Drive: how hard it is pushed (the level is compensated). Bias: asymmetry, adds even harmonics.\n"
    "Tone: a lowpass after it to tame the fizz. Output: the level after it. Mix: dry / saturated.";
inline constexpr const char* eq =
    "Five bands: low shelf, three peaks, high shelf. Drag a band's point (up / down: gain, left / right: frequency); "
    "the wheel over it sets its Q or slope; right-click it to type values or publish them.";
inline constexpr const char* echo =
    "A filtered feedback delay.\n\n"
    "Sync to tempo: the time as a note value (else seconds). Feedback: how many repeats.\n"
    "Cutoff / Resonance / Filter: a filter inside the loop, so every repeat is darker (or thinner).\n"
    "Ping-pong: the repeats bounce between the speakers. Mix: dry / echoes.";
inline constexpr const char* reverb =
    "A room or hall reverb.\n\n"
    "Mix: dry / reverb. Decay: how long the tail lasts. Size: the room. Predelay: a gap before the tail (keeps attacks clear).\n"
    "Late: early reflections (0) to the diffuse tail (1). Spread: how wide the tail is between the speakers.";
inline constexpr const char* reverbTone =
    "The colour of the reverb's tail.\n\n"
    "Lowpass: its top end. Hi Damp / Lo Damp: highs / lows dying faster in the tail.\n"
    "Mod Rate, Spin, Wander: slow movement inside the tail, against metallic ringing.";
inline constexpr const char* chain = "\n\nThe chain: voices -> Saturation -> EQ -> Echo -> Reverb, at the node's speaker count.";
}
