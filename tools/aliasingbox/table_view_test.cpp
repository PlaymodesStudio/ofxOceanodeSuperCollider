#include "scAliasingTableView.h"
#include "scAliasingFilterView.h"
#include "scEnvelopeCurve.h"
#include <cassert>
#include <iostream>
#include <limits>
int main() {
    ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.DisplaySize=ImVec2(1200,900);io.DeltaTime=1.0f/60;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    for(int voices:{1,5,16}) for(int mode:{0,1}) for(float span:{1.0f,.1f}) {
        std::vector<float> telemetry(scAliasingTableView::channels(voices));
        for(int v=0;v<voices;++v) {
            telemetry[v*6+4]=.2f;telemetry[v*6+5]=.7f;
            for(int b=0;b<256;++b) telemetry[176+v*256+b]=std::sin((b+v*13)*.37f)*.5f+.5f;
        }
        telemetry[176]=std::numeric_limits<float>::quiet_NaN();
        ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(1160,850));
        ImGui::Begin("AliasingBox table");auto a=ImGui::GetCursorScreenPos();
        scAliasingTableView::draw(ImGui::GetWindowDrawList(),a,ImVec2(1100,640),telemetry,voices,mode,0,span);
        ImGui::Dummy(ImVec2(1100,640));ImGui::End();ImGui::Render();
        auto* data=ImGui::GetDrawData();assert(data->TotalVtxCount>0);
        for(int n=0;n<data->CmdListsCount;++n) {
            auto* list=data->CmdLists[n];
            assert(list->VtxBuffer.Size<65536);
            for(const auto& v:list->VtxBuffer) assert(std::isfinite(v.pos.x)&&std::isfinite(v.pos.y));
        }
    }
    // Response shapes match the actual serial four-pole filters and peaking notch.
    using namespace scAliasingFilterView;
    assert(response(0,40,1000,0,0,48000)<-80);
    assert(response(1,10000,1000,0,0,48000)<-70);
    assert(std::abs(response(2,1000,1000,.4,-18,48000)+18)<.001);
    for(float sr:{44100.0f,48000.0f,96000.0f}) for(int type:{0,1,2}) for(float res:{0.0f,.98f}) {
        ImGui::NewFrame();ImGui::Begin("Filter and envelope cards");
        float pitch=type==0 ? -10 : 130,gain=-30;
        draw("##response",250,110,type,pitch,res,gain,sr,true,IM_COL32_WHITE);
        scEnvCurve::ADSR env;env.a=.0001f;
        scEnvCurve::editADSR("##env",ImVec2(250,110),env,IM_COL32_WHITE,IM_COL32_BLACK,false,100,.0001f);
        assert(env.a==.0001f); // Original Reaktor's shortest attack must remain intact.
        ImGui::End();ImGui::Render();
        auto* data=ImGui::GetDrawData();
        for(int n=0;n<data->CmdListsCount;++n) for(const auto& v:data->CmdLists[n]->VtxBuffer)
            assert(std::isfinite(v.pos.x)&&std::isfinite(v.pos.y));
    }
    ImGui::DestroyContext();std::cout<<"PASS: table views at 1/5/16 voices; filter response shapes and geometry; sub-millisecond envelope editing; finite geometry and 16-bit index bounds\n";
}
