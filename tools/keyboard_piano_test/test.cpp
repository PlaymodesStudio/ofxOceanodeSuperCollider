#include "nodes/boxSynths/shared/scKeyboardPiano.h"
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
    ImVec2 controls, keysMin, keysMax;
    int highlighted = 0;
    bool footer = false;
    float contentBottom=0, contentHeight=0, litTop=0, litBottom=0, windowBottom=0;
    ImVec2 footerHeader;
    auto frame = [&](bool show=true, bool focus=true, bool text=false) {
        ImGui::NewFrame();
        if(show) {
            ImGui::SetNextWindowSize(ImVec2(600,400));
            ImGui::Begin("Synth");
            if(focus) ImGui::SetWindowFocus();
            else ImGui::SetWindowFocus(nullptr);
            io.WantTextInput=text;
            controls = ImGui::GetCursorScreenPos();
            controls.y += ImGui::GetFrameHeightWithSpacing();
            if(footer) {
                windowBottom = ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y;
                piano.beginContent(pitch,gate,voices,first,offset);
                contentHeight = ImGui::GetWindowSize().y;
                contentBottom = ImGui::GetWindowPos().y + contentHeight;
                footerHeader = ImVec2(ImGui::GetWindowPos().x+12, contentBottom+ImGui::GetStyle().ItemSpacing.y+8);
                ImGui::TextUnformatted("Scrollable synth controls");
                scKeyboardPiano::endContent();
            } else {
                piano.draw(pitch,gate,voices,first,offset);
                keysMin = ImGui::GetItemRectMin();
                keysMax = ImGui::GetItemRectMax();
            }
            highlighted = 0; litTop=10000; litBottom=0;
            for(const auto& vertex : ImGui::GetWindowDrawList()->VtxBuffer)
                if(vertex.col == IM_COL32(65,185,220,255)) {
                    ++highlighted;
                    litTop = std::min(litTop, vertex.pos.y);
                    litBottom = std::max(litBottom, vertex.pos.y);
                }
            ImGui::End();
        }
        piano.finishFrame();
        ImGui::Render();
    };
    auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(k,down);frame();};
    frame();
    assert(gate.get()==std::vector<float>{1}); // idle never takes ownership
    key(ImGuiKey_Z,true);
    assert((pitch.get()==std::vector<float>{60,60}));
    assert((gate.get()==std::vector<float>{1,0}));
    int gateWrites=0; gate.changed=[&](const auto&){++gateWrites;};
    for(int i=0;i<40;++i) frame();
    assert(gateWrites==0); // holding/repeat does not retrigger
    key(ImGuiKey_C,true);
    assert((pitch.get()==std::vector<float>{60,64}));
    assert((gate.get()==std::vector<float>{1,1}));
    key(ImGuiKey_Z,false);
    assert((gate.get()==std::vector<float>{0,1}));
    frame(true,false);
    assert((gate.get()==std::vector<float>{0,0}));
    frame(); assert((gate.get()==std::vector<float>{0,0})); // focus does not replay held key
    key(ImGuiKey_C,false);
    voices=1;
    key(ImGuiKey_Z,true); key(ImGuiKey_X,true);
    assert(pitch.get()[0]==62 && gate.get()[0]==1);
    key(ImGuiKey_X,false);
    assert(pitch.get()[0]==60 && gate.get()[0]==1); // mono previous note returns
    key(ImGuiKey_UpArrow,true); key(ImGuiKey_UpArrow,false);
    assert(pitch.get()[0]==60); // held notes retain their pitch
    key(ImGuiKey_Z,false); key(ImGuiKey_Z,true);
    assert(pitch.get()[0]==72);
    frame(false); assert(gate.get()[0]==0); // hide/collapse releases
    key(ImGuiKey_Z,false);
    io.AddKeyEvent(ImGuiKey_Z,true); frame(true,true,true);
    assert(gate.get()[0]==0); // typing is protected
    key(ImGuiKey_Z,false);
    io.AddKeyEvent(ImGuiMod_Ctrl,true); key(ImGuiKey_Z,true);
    assert(gate.get()[0]==0);
    io.AddKeyEvent(ImGuiMod_Ctrl,false); key(ImGuiKey_Z,false);
    key(ImGuiKey_Semicolon,true); assert(pitch.get()[0]==87); // Spanish ñ
    key(ImGuiKey_Semicolon,false);
    key(ImGuiKey_Period,true); assert(pitch.get()[0]==86);
    key(ImGuiKey_Period,false);
    // SampleBox: channel addressing preserves other pads' values.
    pitch.set({60,60,60}); gate.set({0,0,1}); first=1;
    key(ImGuiKey_Z,true);
    assert(pitch.get()[1]==72 && gate.get()[1]==1 && gate.get()[2]==1);
    first=0; frame(); assert(gate.get()[1]==0); // switching pad releases original
    key(ImGuiKey_Z,false);
    // GrainBox relative semitone mode, and shrinking the voice pool.
    first=0; offset=-60; voices=2;
    key(ImGuiKey_Z,true); key(ImGuiKey_X,true);
    assert(pitch.get()[0]==12 && pitch.get()[1]==14);
    voices=1; frame(); assert(gate.get()[0]==0 && gate.get()[1]==0);
    key(ImGuiKey_Z,false); key(ImGuiKey_X,false);
    // White keys on both rows and only matching black keys in between.
    offset=0;
    const ImGuiKey whiteKeys[] = {
        ImGuiKey_Z, ImGuiKey_X, ImGuiKey_C, ImGuiKey_V, ImGuiKey_B,
        ImGuiKey_N, ImGuiKey_M, ImGuiKey_Comma, ImGuiKey_Period,
        ImGuiKey_Q, ImGuiKey_W, ImGuiKey_E, ImGuiKey_R, ImGuiKey_T,
        ImGuiKey_Y, ImGuiKey_U, ImGuiKey_I, ImGuiKey_O, ImGuiKey_P
    };
    const int whiteOffsets[] = {0,2,4,5,7,9,11,12,14,12,14,16,17,19,21,23,24,26,28};
    for(int i=0; i<19; ++i) {
        key(whiteKeys[i],true);
        assert(pitch.get()[0]==72+whiteOffsets[i] && gate.get()[0]==1);
        key(whiteKeys[i],false);
        assert(gate.get()[0]==0);
    }
    const ImGuiKey blackKeys[] = {
        ImGuiKey_S, ImGuiKey_D, ImGuiKey_G, ImGuiKey_H,
        ImGuiKey_J, ImGuiKey_L, ImGuiKey_Semicolon
    };
    const int blackOffsets[] = {1,3,6,8,10,13,15};
    for(int i=0; i<7; ++i) {
        key(blackKeys[i],true);
        assert(pitch.get()[0]==72+blackOffsets[i] && gate.get()[0]==1);
        key(blackKeys[i],false);
        assert(gate.get()[0]==0);
    }
    for(auto gap : {ImGuiKey_A, ImGuiKey_F, ImGuiKey_K}) {
        const auto previousPitch = pitch.get();
        key(gap,true);
        assert(pitch.get()==previousPitch && gate.get()[0]==0 && !piano.isPlaying());
        key(gap,false);
    }
    // Real mouse events: momentary playing, glissando, black-key hit priority,
    // latch toggles, focus loss, and hidden-window cleanup.
    voices=3; first=0; offset=0;
    pitch.set({60,64,67}); gate.set({1,0,1}); frame();
    assert(highlighted > 0 && !piano.isPlaying());
    const int twoLit = highlighted;
    gate.set({1,1,1}); frame(); assert(highlighted > twoLit);
    gate.set({0}); frame(); assert(highlighted == 0);
    auto moveToNote = [&](int midi, bool upper=false) {
        int whites=0;
        for(int n=0;n<midi;++n) {
            int pc=n%12;
            if(pc!=1 && pc!=3 && pc!=6 && pc!=8 && pc!=10) ++whites;
        }
        const int pc=midi%12;
        const bool black=pc==1 || pc==3 || pc==6 || pc==8 || pc==10;
        const float ww=(keysMax.x-keysMin.x)/75;
        io.AddMousePosEvent(keysMin.x+(whites+(black?0:0.5f))*ww,
                            keysMin.y+(upper?15:65));
        frame(); // establish hover before pressing
    };
    auto mouse = [&](bool down){ io.AddMouseButtonEvent(0,down); frame(); };
    auto click = [&](){ mouse(true); mouse(false); };
    moveToNote(60); mouse(true);
    assert(pitch.get()[0]==60 && gate.get()[0]==1 && highlighted>0);
    moveToNote(62); assert(pitch.get()[0]==62 && gate.get()[0]==1);
    mouse(false); assert(!piano.isPlaying() && gate.get()[0]==0);
    moveToNote(61,true); mouse(true);
    assert(pitch.get()[0]==61); mouse(false);
    auto toggleHold = [&](){
        io.AddMousePosEvent(controls.x+9,controls.y+9); frame(); click();
    };
    toggleHold();
    moveToNote(60); click(); assert(piano.isPlaying() && gate.get()[0]==1);
    moveToNote(64); click(); assert(gate.get()[0]==1 && gate.get()[1]==1);
    frame(true,false); assert(gate.get()[0]==1 && gate.get()[1]==1);
    frame(); moveToNote(60); click();
    assert(gate.get()[0]==0 && gate.get()[1]==1);
    toggleHold(); assert(!piano.isPlaying() && gate.get()[1]==0);
    toggleHold(); moveToNote(67); click(); frame(false);
    assert(!piano.isPlaying() && gate.get()[0]==0);
    // Fill repeats notes into every voice, including uneven chord/voice counts.
    frame(); toggleHold(); // back to momentary mouse playing
    auto toggleFill = [&](){
        io.AddMousePosEvent(controls.x+9, controls.y+ImGui::GetFrameHeightWithSpacing()+9);
        frame(); click();
    };
    voices=5; pitch.set({60}); gate.set({0});
    toggleFill();
    assert(!piano.isPlaying() && gate.get()==std::vector<float>{0});
    key(ImGuiKey_Z,true);
    assert((pitch.get()==std::vector<float>{72,72,72,72,72}));
    assert((gate.get()==std::vector<float>{1,1,1,1,1}));
    key(ImGuiKey_X,true);
    assert((pitch.get()==std::vector<float>{72,74,72,74,72}));
    gateWrites=0; frame(); frame(); assert(gateWrites==0);
    key(ImGuiKey_Z,false);
    assert((pitch.get()==std::vector<float>{74,74,74,74,74}));
    assert((gate.get()==std::vector<float>{1,1,1,1,1}));
    toggleFill();
    assert((gate.get()==std::vector<float>{1,0,0,0,0}));
    toggleFill();
    assert((gate.get()==std::vector<float>{1,1,1,1,1}));
    key(ImGuiKey_X,false);
    assert((gate.get()==std::vector<float>{0,0,0,0,0}));
    moveToNote(60); mouse(true);
    assert((pitch.get()==std::vector<float>{60,60,60,60,60}));
    assert((gate.get()==std::vector<float>{1,1,1,1,1}));
    mouse(false); assert((gate.get()==std::vector<float>{0,0,0,0,0}));
    toggleHold(); moveToNote(64); click();
    assert((pitch.get()==std::vector<float>{64,64,64,64,64}));
    frame(false); assert((gate.get()==std::vector<float>{0,0,0,0,0}));
    frame(); toggleFill(); voices=3;
    // Repeated short gate vectors must illuminate every sounding pitch.
    pitch.set({60,64,67}); gate.set({1}); frame();
    assert(highlighted > twoLit);
    // Footer child pairs correctly and keeps a populated parent draw list.
    footer=true; frame(); assert(highlighted>0);
    assert(litTop > contentBottom && litBottom <= windowBottom); // piano is below controls
    const float expandedHeight=contentHeight;
    auto fold = [&](){ io.AddMousePosEvent(footerHeader.x,footerHeader.y); frame(); click(); frame(); };
    fold(); assert(highlighted==0 && contentHeight>expandedHeight+76);
    const auto externalGates=gate.get(); frame(); assert(gate.get()==externalGates);
    key(ImGuiKey_Z,true); assert(piano.isPlaying() && gate.get()[0]==1);
    key(ImGuiKey_Z,false); assert(!piano.isPlaying() && gate.get()[0]==0);
    gate.set({1}); fold(); assert(highlighted>0 && contentHeight==expandedHeight);
    assert(litTop > contentBottom && litBottom <= windowBottom);
    ImGui::DestroyContext();
    std::cout << "PASS: white/black key layout, chords, mono priority, release, focus, typing, modifiers, octaves, ñ, channels, relative pitch, external highlights, mouse notes, glissando, Hold, bottom placement and folding\n";
}
