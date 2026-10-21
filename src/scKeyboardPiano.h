#pragma once

#include "ofParameter.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <vector>

// One instance per instrument (or GrainBox engine). Drives the ordinary note
// parameters, so their existing listeners handle live audio and recording.
class scKeyboardPiano {
public:
    using Notes = ofParameter<std::vector<float>>;

    void draw(Notes& pitch, Notes& gate, int voices, int firstVoice = 0, float pitchOffset = 0) {
        serviced = true;
        const auto& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool enabled = focused && !io.AppFocusLost && !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift;
        if(!enabled) { release(); return; }
        if(gates && (gates != &gate || first != firstVoice || count != voices)) release();
        if(ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) base = std::min(96, base + 12);
        if(ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) base = std::max(0, base - 12);

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
            return !ImGui::IsKeyDown(n.key);
        }), held.end());
        for(const auto& mapping : keys) {
            if(ImGui::IsKeyPressed(mapping.key, false)) held.push_back({mapping.key, float(base + mapping.semitones) + pitchOffset});
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
                if(starting || v >= (int)slots.size() || slots[v].key != next[v].key) g[first + v] = 0;
            if(g != gate.get()) gate.set(g);
            for(int v = 0; v < count; ++v) if(next[v].key != ImGuiKey_None) p[first + v] = next[v].pitch;
            if(p != pitch.get()) pitch.set(p);
            for(int v = 0; v < count; ++v) g[first + v] = next[v].key == ImGuiKey_None ? 0 : 1;
            if(g != gate.get()) gate.set(g);
            slots = std::move(next);
            if(held.empty()) { gates = nullptr; slots.clear(); }
        }
        ImGui::TextDisabled("Keyboard piano: C%d | Up/Down: octave", base / 12 - 1);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("White keys: zxcvbnm,. (Z = C), qwertyuiop (Q = next C)\nBlack keys: s d   g h j   l ñ (semicolon); a/f/k are gaps\nHold keys to play; release to close gates. Chords use the available voices.");
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
    struct Note { ImGuiKey key; float pitch; };
    int base = 60, first = 0, count = 0;
    bool serviced = false;
    Notes* gates = nullptr;
    std::vector<Note> held, slots;
};
