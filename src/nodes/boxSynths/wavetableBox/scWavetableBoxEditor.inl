bool scWavetableBox::sliderFloat(ofParameter<float>& p,const char* format,bool logarithmic){
    float v=p.get();const bool changed=ImGui::SliderFloat(p.getName().c_str(),&v,p.getMin(),p.getMax(),format,logarithmic?ImGuiSliderFlags_Logarithmic:0);
    if(changed)p=v;controlMenu(p.getEscapedName());return changed;
}
bool scWavetableBox::checkbox(ofParameter<bool>& p){
    bool v=p.get();const bool changed=ImGui::Checkbox(p.getName().c_str(),&v);if(changed)p=v;controlMenu(p.getEscapedName());return changed;
}
void scWavetableBox::drawHeader(){
    ImGui::SetNextItemWidth(135);sliderInt(voices);ImGui::SameLine();
    ImGui::SetNextItemWidth(135);sliderInt(outChannels);ImGui::SameLine();
    ImGui::SetNextItemWidth(160);sliderFloat(master);ImGui::SameLine();checkbox(drone);
    ImGui::SameLine();ImGui::TextDisabled(drone?"droning":"gated");
    ImGui::SameLine();if(ImGui::Button("Reset LFOs"))resetLFOs.trigger();
}
void scWavetableBox::drawPanning(){
    ImGui::SetNextItemWidth(160);comboInt(placement,{"Spread","Channels","Manual"});
    ImGui::SameLine();ImGui::SetNextItemWidth(160);comboInt(speakerLayout,{"Ring","Line"});
    if(placement==0){sliderFloat(spreadWidth);sliderFloat(spreadCenter);}
    if(placement==2)sliderVector(pan);
    ImGui::TextDisabled("Drag a voice to position it. Voices and output channels are independent.");
    const float w=std::max(100.0f,ImGui::GetContentRegionAvail().x),h=210;
    const ImVec2 start=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##panmap",ImVec2(w,h));
    auto* dl=ImGui::GetWindowDrawList();
    const bool ring=outChannels>2&&speakerLayout==0;
    auto point=[&](float p){return ring?ImVec2(start.x+w*0.5f+std::sin(p*TWO_PI)*80,start.y+h*0.5f-std::cos(p*TWO_PI)*80):ImVec2(start.x+20+std::clamp(p,0.0f,1.0f)*(w-40),start.y+h*0.5f);};
    if(ring)dl->AddCircle(ImVec2(start.x+w*0.5f,start.y+h*0.5f),80,ImGui::GetColorU32(ImGuiCol_Border),64);
    else dl->AddLine(point(0),point(1),ImGui::GetColorU32(ImGuiCol_Border),2);
    for(int c=0;c<outChannels;++c){const float p=outChannels==1?0.5f:float(c)/(ring?outChannels.get():outChannels.get()-1);auto q=point(p);dl->AddRectFilled(ImVec2(q.x-6,q.y-6),ImVec2(q.x+6,q.y+6),ImGui::GetColorU32(ImGuiCol_TextDisabled));dl->AddText(ImVec2(q.x-4,q.y+12),ImGui::GetColorU32(ImGuiCol_Text),ofToString(c+1).c_str());}
    auto positions=voicePositions();
    for(int i=0;i<voices;++i){auto q=point(positions[i]);q.y-=ring?0:25;dl->AddCircleFilled(q,7,ImGui::GetColorU32(ImGuiCol_PlotLines));dl->AddText(ImVec2(q.x-4,q.y-20),ImGui::GetColorU32(ImGuiCol_Text),ofToString(i+1).c_str());}
    if(ImGui::IsItemActive()){
        const auto mouse=ImGui::GetIO().MousePos;
        float p=ring?std::atan2(mouse.x-(start.x+w*0.5f),-(mouse.y-(start.y+h*0.5f)))/TWO_PI:(mouse.x-start.x-20)/(w-40);
        if(ring&&p<0)p+=1;p=ofClamp(p,0,1);
        static int dragged=0;
        if(ImGui::IsMouseClicked(0)){float best=1e20;for(int i=0;i<voices;++i){auto q=point(positions[i]);const float d=(q.x-mouse.x)*(q.x-mouse.x)+(q.y-mouse.y)*(q.y-mouse.y);if(d<best){best=d;dragged=i;}}}
        positions.resize(voices.get());positions[std::clamp(dragged,0,voices.get()-1)]=p;
        restoring=true;pan=positions;placement=2;restoring=false;snaps.morph.edited(pan);snaps.morph.edited(placement);updateOutputs();
    }
}
void scWavetableBox::drawEffects(){
    ImGui::TextDisabled("Compressor > EQ > Blur > Echo > Reverb");
    for(int s=0;s<NumFx;++s){
        ImGui::PushID(s);checkbox(fxOn[s]);
        if(fxOn[s]){
            ImGui::Indent();
            if(s==FxComp){sliderFloat(compThreshold,"%.1f dB");sliderFloat(compRatio);sliderFloat(compAttack,"%.1f ms",true);sliderFloat(compRelease,"%.0f ms",true);sliderFloat(compMakeup,"%.1f dB");sliderFloat(compMix);}
            else if(s==FxEq){
                for(int b=0;b<5;++b)eqEditor.bands[b]={eqFreq[b].get(),eqGain[b].get(),eqShape[b].get()};
                eqEditor.dbRange=24;eqEditor.gainLimit=24;eqEditor.slopeMax=1;eqEditor.qMax=12;
                float sr=48000;for(auto* sm:servers)if(sm&&sm->getServer()){sr=serverManager::getSampleRateForServer(sm->getServer());break;}
                eqEditor.recompute(sr>0?sr:48000);
                if(eqEditor.draw(std::max(1.0f,ImGui::GetContentRegionAvail().x),160,scEQEditor::defaultColors(),true,nullptr,0,"##wtEq")){
                    restoring=true;for(int b=0;b<5;++b){eqFreq[b]=eqEditor.bands[b].freqHz;eqGain[b]=eqEditor.bands[b].gainDb;eqShape[b]=eqEditor.bands[b].shape;}restoring=false;
                    for(int b=0;b<5;++b){snaps.morph.edited(eqFreq[b]);snaps.morph.edited(eqGain[b]);snaps.morph.edited(eqShape[b]);}updateOutputs();
                }
                if(ImGui::BeginTable("##eq",3,ImGuiTableFlags_SizingStretchSame)){
                    for(int b=0;b<5;++b){ImGui::TableNextColumn();sliderFloat(eqGain[b],"%.1f dB");ImGui::TableNextColumn();sliderFloat(eqFreq[b],"%.0f Hz",true);ImGui::TableNextColumn();sliderFloat(eqShape[b]);}ImGui::EndTable();
                }
            }
            else if(s==FxBlur){sliderFloat(blurTime,"%.1f ms",true);sliderFloat(blurAmount);sliderFloat(blurDensity);sliderFloat(blurSpread);sliderFloat(blurLowCut,"%.0f Hz",true);sliderFloat(blurHighCut,"%.0f Hz",true);sliderFloat(blurMix);}
            else if(s==FxEcho){checkbox(echoBeats);if(echoBeats)sliderFloat(echoBeatVal,"%.3f beats");else sliderFloat(echoDelay,"%.3f s",true);sliderFloat(echoFeed);comboInt(echoFilter,{"Low Pass","High Pass","Band Pass","Notch"});sliderFloat(echoCutoff,"%.1f MIDI");sliderFloat(echoResonance);sliderFloat(echoPingPong);sliderFloat(echoMix);}
            else{sliderFloat(reverbMix);sliderFloat(reverbDecay,"%.2f s",true);sliderFloat(reverbSize);sliderFloat(reverbPredelay,"%.1f ms");sliderFloat(reverbLate);sliderFloat(reverbSpread);sliderFloat(reverbLowpass,"%.0f Hz",true);}
            ImGui::Unindent();
        }ImGui::Separator();ImGui::PopID();
    }
}
