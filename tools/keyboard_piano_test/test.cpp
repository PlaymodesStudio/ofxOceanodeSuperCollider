#include "scKeyboardPiano.h"
#include <cassert>
#include <iostream>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f/60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels; int w,h;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    scKeyboardPiano piano;
    ofParameter<std::vector<float>> pitch({60}), gate({1});
    int voices=2, first=0;
    float offset=0;
    auto frame = [&](bool show=true, bool focus=true, bool text=false) {
        ImGui::NewFrame();
        if(show) {
            ImGui::SetNextWindowSize(ImVec2(600,400));
            ImGui::Begin("Synth");
            if(focus) ImGui::SetWindowFocus();
            else ImGui::SetWindowFocus(nullptr);
            io.WantTextInput=text;
            piano.draw(pitch,gate,voices,first,offset);
            ImGui::End();
        }
        piano.finishFrame();
        ImGui::Render();
    };
    auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(k,down);frame();};
    frame();
    assert(gate.get()==std::vector<float>{1}); // idle never takes ownership
    key(ImGuiKey_Q,true);
    assert((pitch.get()==std::vector<float>{60,60}));
    assert((gate.get()==std::vector<float>{1,0}));
    int gateWrites=0; gate.changed=[&](const auto&){++gateWrites;};
    for(int i=0;i<40;++i) frame();
    assert(gateWrites==0); // holding/repeat does not retrigger
    key(ImGuiKey_E,true);
    assert((pitch.get()==std::vector<float>{60,62}));
    assert((gate.get()==std::vector<float>{1,1}));
    key(ImGuiKey_Q,false);
    assert((gate.get()==std::vector<float>{0,1}));
    frame(true,false);
    assert((gate.get()==std::vector<float>{0,0}));
    frame(); assert((gate.get()==std::vector<float>{0,0})); // focus does not replay held key
    key(ImGuiKey_E,false);
    voices=1;
    key(ImGuiKey_Q,true); key(ImGuiKey_W,true);
    assert(pitch.get()[0]==61 && gate.get()[0]==1);
    key(ImGuiKey_W,false);
    assert(pitch.get()[0]==60 && gate.get()[0]==1); // mono previous note returns
    key(ImGuiKey_UpArrow,true); key(ImGuiKey_UpArrow,false);
    assert(pitch.get()[0]==60); // held notes retain their pitch
    key(ImGuiKey_Q,false); key(ImGuiKey_Q,true);
    assert(pitch.get()[0]==72);
    frame(false); assert(gate.get()[0]==0); // hide/collapse releases
    key(ImGuiKey_Q,false);
    io.AddKeyEvent(ImGuiKey_Q,true); frame(true,true,true);
    assert(gate.get()[0]==0); // typing is protected
    key(ImGuiKey_Q,false);
    io.AddKeyEvent(ImGuiMod_Ctrl,true); key(ImGuiKey_Q,true);
    assert(gate.get()[0]==0);
    io.AddKeyEvent(ImGuiMod_Ctrl,false); key(ImGuiKey_Q,false);
    key(ImGuiKey_Semicolon,true); assert(pitch.get()[0]==91); // Spanish ñ
    key(ImGuiKey_Semicolon,false);
    key(ImGuiKey_Period,true); assert(pitch.get()[0]==100);
    key(ImGuiKey_Period,false);
    // SampleBox: channel addressing preserves other pads' values.
    pitch.set({60,60,60}); gate.set({0,0,1}); first=1;
    key(ImGuiKey_Q,true);
    assert(pitch.get()[1]==72 && gate.get()[1]==1 && gate.get()[2]==1);
    first=0; frame(); assert(gate.get()[1]==0); // switching pad releases original
    key(ImGuiKey_Q,false);
    // GrainBox relative semitone mode, and shrinking the voice pool.
    first=0; offset=-60; voices=2;
    key(ImGuiKey_Q,true); key(ImGuiKey_W,true);
    assert(pitch.get()[0]==12 && pitch.get()[1]==13);
    voices=1; frame(); assert(gate.get()[0]==0 && gate.get()[1]==0);
    key(ImGuiKey_Q,false); key(ImGuiKey_W,false);
    ImGui::DestroyContext();
    std::cout << "PASS: chords, mono priority, release, focus, typing, modifiers, octaves, ñ, channels and relative pitch\n";
}
