#pragma once

#include "ofParameter.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <string>

// One instance per instrument (or GrainBox engine). Drives the ordinary note
// parameters, so their existing listeners handle live audio and recording.
class scKeyboardPiano {
public:
    using Notes = ofParameter<std::vector<float>>;

    // Draw the fixed footer in the parent, then give the controls a scrollable
    // child above it. Always pair with endContent(), including early exits.
    void beginContent(Notes& pitch, Notes& gate, int voices, int firstVoice = 0, float pitchOffset = 0) {
        const ImVec2 origin = ImGui::GetCursorPos();
        const float available = ImGui::GetContentRegionAvail().y;
        const float footer = ImGui::GetFrameHeightWithSpacing() + 84.0f;
        const float content = std::max(1.0f, available - footer - ImGui::GetStyle().ItemSpacing.y);
        ImGui::SetCursorPos(ImVec2(origin.x, origin.y + content + ImGui::GetStyle().ItemSpacing.y));
        draw(pitch, gate, voices, firstVoice, pitchOffset);
        ImGui::SetCursorPos(origin);
        ImGui::BeginChild("##pianoContent", ImVec2(0, content), false);
    }
    static void endContent() { ImGui::EndChild(); }

    void draw(Notes& pitch, Notes& gate, int voices, int firstVoice = 0, float pitchOffset = 0) {
        serviced = true;
        const auto& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool enabled = focused && !io.AppFocusLost && !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift;
        // Focus loss releases physical keys, but intentionally latched mouse
        // notes continue until clicked again, Hold is disabled, or the GUI closes.
        if(!enabled) held.erase(std::remove_if(held.begin(), held.end(), [](const Note& n){
            return n.key >= 0 || n.key == mouseKey;
        }), held.end());
        if(gates && (gates != &gate || first != firstVoice || count != voices)) release();
        if(enabled && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) base = std::min(96, base + 12);
        if(enabled && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) base = std::max(0, base - 12);

        // Piano layout: white keys on the bottom/top rows, matching black
        // keys between the bottom-row whites. A/F/K are the gaps at B-C/E-F.
        // Semicolon is the physical key occupied by ñ on a Spanish keyboard.
        struct KeyPitch { ImGuiKey key; int semitones; };
        static constexpr std::array<KeyPitch, 26> keys = {{
            {ImGuiKey_Z, 0}, {ImGuiKey_X, 2}, {ImGuiKey_C, 4},
            {ImGuiKey_V, 5}, {ImGuiKey_B, 7}, {ImGuiKey_N, 9},
            {ImGuiKey_M, 11}, {ImGuiKey_Comma, 12}, {ImGuiKey_Period, 14},
            {ImGuiKey_S, 1}, {ImGuiKey_D, 3}, {ImGuiKey_G, 6},
            {ImGuiKey_H, 8}, {ImGuiKey_J, 10}, {ImGuiKey_L, 13},
            {ImGuiKey_Semicolon, 15},
            {ImGuiKey_Q, 12}, {ImGuiKey_W, 14}, {ImGuiKey_E, 16},
            {ImGuiKey_R, 17}, {ImGuiKey_T, 19}, {ImGuiKey_Y, 21},
            {ImGuiKey_U, 23}, {ImGuiKey_I, 24}, {ImGuiKey_O, 26},
            {ImGuiKey_P, 28}
        }};
        held.erase(std::remove_if(held.begin(), held.end(), [](const Note& n){
            return n.key >= 0 && !ImGui::IsKeyDown(static_cast<ImGuiKey>(n.key));
        }), held.end());
        for(const auto& mapping : keys) {
            if(enabled && ImGui::IsKeyPressed(mapping.key, false)) held.push_back({mapping.key, float(base + mapping.semitones) + pitchOffset});
        }
        ImGui::PushID(this);
        if(ImGui::Checkbox("Hold", &hold) && !hold) {
            held.erase(std::remove_if(held.begin(), held.end(), [](const Note& n){ return n.key < 0; }), held.end());
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Latch clicked notes until clicked again. Turning Hold off releases latched notes.");
        ImGui::SameLine();
        if(ImGui::Button("Release")) release();
        ImGui::SameLine();
        if(ImGui::SmallButton("Oct -")) base = std::max(0, base - 12);
        ImGui::SameLine();
        if(ImGui::SmallButton("Oct +")) base = std::min(96, base + 12);
        ImGui::SameLine();
        ImGui::TextDisabled("C%d | Up/Down: octave", base / 12 - 1);

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float height = 76.0f, whiteW = width / 75.0f, blackW = whiteW * 0.64f;
        ImGui::InvisibleButton("##piano", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDown(0);
        const auto black = [](int midi) {
            const int pc = midi % 12;
            return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        };
        int hit = -1, whiteIndex = 0;
        const ImVec2 mouse = io.MousePos;
        // Test whites first, then blacks (which overlap and have priority).
        for(int pass = 0; pass < 2; ++pass) {
            whiteIndex = 0;
            for(int midi = 0; midi < 128; ++midi) {
                const bool isBlack = black(midi);
                const float x = pos.x + whiteIndex * whiteW - (isBlack ? blackW * 0.5f : 0);
                if(isBlack == (pass == 1) && mouse.x >= x && mouse.x < x + (isBlack ? blackW : whiteW)
                   && mouse.y >= pos.y && mouse.y < pos.y + (isBlack ? height * 0.62f : height)) hit = midi;
                if(!isBlack) ++whiteIndex;
            }
        }
        if(hovered && ImGui::IsMouseClicked(0) && hit >= 0 && hold) {
            const int id = -2 - hit;
            auto note = std::find_if(held.begin(), held.end(), [id](const Note& n){ return n.key == id; });
            if(note != held.end()) held.erase(note);
            else held.push_back({id, float(hit) + pitchOffset});
        }
        if(!hold) {
            auto note = std::find_if(held.begin(), held.end(), [](const Note& n){ return n.key == mouseKey; });
            if(!dragging || !hovered || hit < 0) {
                if(note != held.end()) held.erase(note);
            } else if(note == held.end()) held.push_back({mouseKey, float(hit) + pitchOffset});
            else note->pitch = float(hit) + pitchOffset;
        }
        if(!held.empty() || gates) {
            const bool starting = !gates;
            gates = &gate; first = firstVoice; count = std::max(1, voices);
            std::vector<Note> next(count, {ImGuiKey_None, 0});
            const int begin = std::max(0, (int)held.size() - count);
            // Retain voice assignments for notes that are still held. The newest
            // notes win when all voices are occupied; older held notes return
            // when a newer note is released (also gives mono last-note priority).
            for(int v = 0; v < std::min(count, (int)slots.size()); ++v)
                for(int k = begin; k < (int)held.size(); ++k)
                    if(slots[v].key == held[k].key) next[v] = held[k];
            for(int k = begin; k < (int)held.size(); ++k) {
                if(std::any_of(next.begin(), next.end(), [&](const Note& n){ return n.key == held[k].key; })) continue;
                for(auto& n : next) if(n.key == ImGuiKey_None) { n = held[k]; break; }
            }
            auto p = pitch.get(), g = gate.get();
            const int width = first + count;
            p.resize(std::max(width, (int)p.size()), p.empty() ? 60 : p.back());
            g.resize(std::max(width, (int)g.size()), 0);
            // Close changed voices before moving their pitch, then open them.
            for(int v = 0; v < count; ++v)
                if(starting || v >= (int)slots.size() || slots[v].key != next[v].key || slots[v].pitch != next[v].pitch) g[first + v] = 0;
            if(g != gate.get()) gate.set(g);
            for(int v = 0; v < count; ++v) if(next[v].key != ImGuiKey_None) p[first + v] = next[v].pitch;
            if(p != pitch.get()) pitch.set(p);
            for(int v = 0; v < count; ++v) g[first + v] = next[v].key == ImGuiKey_None ? 0 : 1;
            if(g != gate.get()) gate.set(g);
            slots = std::move(next);
            if(held.empty()) { gates = nullptr; slots.clear(); }
        }
        // Read the actual parameters, so external gates and local notes share
        // the same display. Short vectors repeat cyclically, as the synths do.
        std::array<bool, 128> lit{};
        const auto& pitches = pitch.get();
        const auto& levels = gate.get();
        if(!pitches.empty() && !levels.empty()) {
            for(int v = firstVoice; v < firstVoice + std::max(1, voices); ++v) {
                if(v < 0 || levels[v % levels.size()] <= 0) continue;
                const float midi = pitches[v % pitches.size()] - pitchOffset;
                if(std::isfinite(midi) && midi > -0.5f && midi < 127.5f) lit[int(std::round(midi))] = true;
            }
        }
        auto* dl = ImGui::GetWindowDrawList();
        for(int pass = 0; pass < 2; ++pass) {
            whiteIndex = 0;
            for(int midi = 0; midi < 128; ++midi) {
                const bool isBlack = black(midi);
                if(isBlack == (pass == 1)) {
                    const float x = pos.x + whiteIndex * whiteW - (isBlack ? blackW * 0.5f : 0);
                    const ImVec2 a(x, pos.y), b(x + (isBlack ? blackW : whiteW), pos.y + (isBlack ? height * 0.62f : height));
                    const ImU32 color = lit[midi] ? IM_COL32(65, 185, 220, 255) :
                        ((hovered && hit == midi) ? IM_COL32(140, 165, 180, 255) :
                         (isBlack ? IM_COL32(28, 31, 36, 255) : IM_COL32(225, 228, 232, 255)));
                    dl->AddRectFilled(a, b, color, 2.0f);
                    dl->AddRect(a, b, IM_COL32(55, 60, 68, 255), 2.0f);
                    if(midi % 12 == 0 && whiteW >= 12) {
                        const std::string label = "C" + std::to_string(midi / 12 - 1);
                        dl->AddText(ImVec2(x + 1, b.y - ImGui::GetTextLineHeight()), IM_COL32(50, 55, 60, 255), label.c_str());
                    }
                }
                if(!isBlack) ++whiteIndex;
            }
        }
        if(hovered && hit >= 0) ImGui::SetTooltip("MIDI %d | Click to play%s\nComputer keys: zxcvbnm,. / qwertyuiop; black keys: s d g h j l ;", hit, hold ? " / toggle held note" : "; drag to change note");
        ImGui::PopID();
    }

    // Call after the node's draw, even when hidden/collapsed: missed windows
    // release their notes instead of leaving gates stuck open.
    bool isPlaying() const { return !held.empty(); }
    void finishFrame() { if(!serviced) release(); serviced = false; }
    void release() {
        if(gates) {
            auto g = gates->get();
            for(int i = first; i < std::min(first + count, (int)g.size()); ++i) g[i] = 0;
            if(g != gates->get()) gates->set(g);
        }
        gates = nullptr; held.clear(); slots.clear();
    }
private:
    static constexpr int mouseKey = -1000;
    struct Note { int key; float pitch; };
    int base = 60, first = 0, count = 0;
    bool serviced = false, hold = false;
    Notes* gates = nullptr;
    std::vector<Note> held, slots;
};
