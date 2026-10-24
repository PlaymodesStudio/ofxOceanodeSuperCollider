// Post-voice graph: spread -> optional effects -> master/output.
template<typename T>
void scWavetableBox::registerOutput(ofParameter<T>& p, std::vector<std::string> choices){
    addPublishAction(p, choices);
    auto* parameter=&p;
    nodeListeners.push(p.newListener([this,parameter](T&){
        snaps.morph.edited(*parameter);
        if(!restoring) updateOutputs();
    }));
}

void scWavetableBox::setupOutputControls(){
    master.set("Master",1,0,2); registerOutput(master);
    placement.set("Placement",0,0,2); registerOutput(placement,{"Spread","Channels","Manual"});
    speakerLayout.set("Speakers",0,0,1); registerOutput(speakerLayout,{"Ring","Line"});
    spreadWidth.set("Spread Width",1,0,1); registerOutput(spreadWidth);
    spreadCenter.set("Spread Center",0,-1,1); registerOutput(spreadCenter);
    pan.set("Pan",{0.5f},{0.0f},{1.0f}); registerOutput(pan);
    const char* names[]={"Compressor","EQ","Blur","Echo","Reverb"};
    for(int i=0;i<NumFx;++i){fxOn[i].set(std::string(names[i])+" On",false);registerOutput(fxOn[i]);}
    auto f=[this](ofParameter<float>& p,const char* name,float v,float lo,float hi){p.set(name,v,lo,hi);registerOutput(p);};
    f(compThreshold,"Comp Threshold",-18,-60,0);f(compRatio,"Comp Ratio",3,1,20);
    f(compAttack,"Comp Attack",10,0.1,200);f(compRelease,"Comp Release",150,10,2000);
    f(compMakeup,"Comp Makeup",0,-12,24);f(compMix,"Comp Mix",1,0,1);
    const float freqs[]={80,250,1000,4000,12000};
    for(int b=0;b<5;++b){
        const std::string prefix="EQ "+ofToString(b+1)+" ";
        f(eqGain[b],(prefix+"Gain").c_str(),0,-24,24);
        f(eqFreq[b],(prefix+"Freq").c_str(),freqs[b],20,20000);
        f(eqShape[b],(prefix+(b==0||b==4?"Slope":"Q")).c_str(),1,0.1,b==0||b==4?1:12);
    }
    f(blurTime,"Blur Time",8,0.1,100);f(blurAmount,"Blur Amount",0.75,0,1);
    f(blurDensity,"Blur Density",0.55,0,1);f(blurSpread,"Blur Spread",1,0,1);
    f(blurLowCut,"Blur Low Cut",20,20,20000);f(blurHighCut,"Blur High Cut",20000,20,20000);f(blurMix,"Blur Mix",0.5,0,1);
    f(echoDelay,"Echo Delay",0.2,0.005,4);f(echoFeed,"Echo Feed",0.5,0,0.98);
    f(echoCutoff,"Echo Cutoff",60,12,130);f(echoResonance,"Echo Resonance",0.2,0,0.9);
    f(echoPingPong,"Echo Ping Pong",0,0,1);f(echoMix,"Echo Mix",0.35,0,1);
    f(echoBeatVal,"Echo Beats",0.75,0.03125,8);
    echoBeats.set("Echo Sync",false);registerOutput(echoBeats);
    echoFilter.set("Echo Filter",0,0,3);registerOutput(echoFilter,{"Low Pass","High Pass","Band Pass","Notch"});
    f(reverbMix,"Reverb Mix",0.3,0,1);f(reverbDecay,"Reverb Decay",3,0.05,30);
    f(reverbSize,"Reverb Size",30,0,60);f(reverbPredelay,"Reverb Predelay",20,0,500);
    f(reverbLate,"Reverb Position",0.8,0,1);f(reverbSpread,"Reverb Spread",0.15,-1,1);
    f(reverbLowpass,"Reverb Lowpass",10000,20,20000);
}

void scWavetableBox::migrateLegacy(ofJson& j){
    scDroneMode::migrate(j);
    std::function<void(ofJson&)> visit=[&](ofJson& v){
        if(v.is_object()){
            if(v.contains("N_Chan")){
                int n=1;try{n=v["N_Chan"].is_string()?std::stoi(v["N_Chan"].get<std::string>()):v["N_Chan"].get<int>();}catch(...){}
                n=std::clamp(n,1,MAX_CHANNELS);
                if(!v.contains("Voices"))v["Voices"]=n;
                if(!v.contains("Channels"))v["Channels"]=n;
                if(!v.contains("Placement"))v["Placement"]=1;
                v.erase("N_Chan");
            }
            for(auto& item:v.items())visit(item.value());
        }else if(v.is_array())for(auto& item:v){if(item=="N_Chan")item="Voices";else visit(item);}
    };visit(j);
}

std::vector<float> scWavetableBox::voicePositions() const{
    std::vector<float> p(MAX_CHANNELS,0.5f);
    const int n=voices.get(),c=outChannels.get();
    for(int i=0;i<n;++i){
        if(placement==2)p[i]=valueFor(pan.get(),i,0.5f);
        else if(placement==1)p[i]=c<=1?0.5f:float(i%c)/((c==2||speakerLayout==1)?c-1:c);
        else if(c>2&&speakerLayout==0)p[i]=float(i)/n*spreadWidth.get()+spreadCenter.get()*0.5f;
        else p[i]=0.5f+spreadCenter.get()*0.5f+(n>1?(float(i)/(n-1)-0.5f)*spreadWidth.get():0);
    }return p;
}

std::string scWavetableBox::fxDef(int stage) const{
    static const char* names[]={"mixerProComp","mixerProEQ","PCBlur","mixerProEcho","mixerProRvb"};
    return std::string(names[stage])+ofToString(outChannels.get());
}
void scWavetableBox::requireEffectSynthdefs(){
    fs::path addon=fs::path(__FILE__).parent_path();for(int i=0;i<4;++i)addon=addon.parent_path();
    const std::vector<fs::path> roots={ofToDataPath("Supercollider/Synthdefs",true),addon/"data/Supercollider/Synthdefs",addon/"synthdefs/Defaults"};
    for(const char* def:{"mixerProComp1.scsyndef","PCBlur1.scsyndef"}){
        bool found=false;
        for(const auto& root:roots){
            std::error_code ec;
            for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;!ec&&it!=end;it.increment(ec)){
                if(it->path().filename()==def){for(auto* sm:servers)if(sm)sm->requireSynthdefFolder(it->path().parent_path().string());found=true;break;}
            }if(found)break;
        }
    }
}
void scWavetableBox::sendEffect(ofxSCSynth* x,int s){
    if(!x)return;
    const float on=snaps.morph.activation(fxOn[s]);
    auto set=[&](const std::string& k,float v){x->set(k,v);};
    set("on",on);
    if(s==FxComp){set("threshold",compThreshold);set("ratio",compRatio);set("attack",compAttack);set("release",compRelease);set("makeup",compMakeup);set("mix",compMix);set("knee",6);set("automakeup",0);set("rms",1);set("schpf",20);set("grBus",-1);}
    else if(s==FxEq){for(int b=0;b<5;++b){std::string k="b"+ofToString(b+1);set(k+"gain",eqGain[b].get()*on);set(k+"freq",eqFreq[b]);set(k+(b==0||b==4?"slope":"rq"),1/(scEQEditor::isShelf(b)?scEQEditor::safeShelfSlope(eqShape[b].get(),eqGain[b].get()*on):std::max(0.1f,eqShape[b].get())));}}
    else if(s==FxBlur){set("time",blurTime);set("blur",blurAmount);set("density",blurDensity);set("spread",blurSpread);set("hipass",blurLowCut);set("lowpass",blurHighCut);set("mix",blurMix);}
    else if(s==FxEcho){set("delay",echoBeats?std::clamp(echoBeatVal.get()*60/std::max(1.0f,currentBpm),0.005f,4.0f):echoDelay.get());set("feed",echoFeed);set("cutoff",echoCutoff);set("resonance",echoResonance);set("filtertype",echoFilter);set("pingpong",echoPingPong);set("mix",echoMix);}
    else{set("mix",reverbMix);set("decay",reverbDecay);set("size",reverbSize);set("predelay",reverbPredelay);set("position",reverbLate);set("spread",reverbSpread);set("lowpass",reverbLowpass);}
}
void scWavetableBox::freeOutput(ofxSCServer* server){
    auto it=modStates.find(server);if(it==modStates.end())return;auto& st=it->second;
    auto release=[](ofxSCSynth*& x){if(x){x->free();delete x;x=nullptr;}};
    release(st.spreadSynth);release(st.outputSynth);for(auto*& x:st.effects)release(x);
    st.outputChannels=0;
}
void scWavetableBox::updateOutputs(){
    fxPending=false;
    for(auto& [server,st]:modStates)updateOutput(server);
}
void scWavetableBox::updateOutput(ofxSCServer* server){
    auto it=modStates.find(server);if(it==modStates.end())return;auto& st=it->second;
    const int c=outChannels.get();
    if(st.outputChannels!=c)freeOutput(server);
    bool ready=true;
    if(!server->isNRTCapturing())for(auto* sm:servers)if(sm&&sm->getServer()==server)ready=!sm->areRequiredSynthdefsLoading();
    if(!st.spreadSynth){
        st.spreadSynth=new ofxSCSynth("WavetableBoxSpread"+ofToString(c),server);
        st.spreadSynth->set("in",st.voiceBus->index);st.spreadSynth->set("out",st.panBus->index);
        st.spreadSynth->set("voices",voices.get());st.spreadSynth->set("pos",voicePositions());
        st.spreadSynth->set("mpan",modulation.targetBus(server,TPan));st.spreadSynth->set("line",speakerLayout.get());
        st.spreadSynth->createAndRun(0,st.processing->nodeID,true);
        st.outputSynth=new ofxSCSynth("WavetableBoxOut"+ofToString(c),server);
        st.outputSynth->set("in",st.panBus->index);st.outputSynth->set("out",outputBuses.count(server)?outputBuses.at(server):0);
        st.outputSynth->set("master",master.get());st.outputSynth->createAndRun(1,st.processing->nodeID,true);
        st.outputChannels=c;
    }
    st.spreadSynth->set("voices",voices.get());st.spreadSynth->set("pos",voicePositions());
    st.spreadSynth->set("mpan",modulation.targetBus(server,TPan));st.spreadSynth->set("line",speakerLayout.get());
    int input=st.panBus->index;
    for(int s=0;s<NumFx;++s){
        const bool enabled=snaps.morph.activation(fxOn[s])>0.0001f;
        if(enabled&&!st.effects[s]&&ready){
            auto* x=st.effects[s]=new ofxSCSynth(fxDef(s),server);
            x->set("in",input);x->set("out",st.fxBus[s]->index);sendEffect(x,s);
            x->createAndRun(2,st.outputSynth->nodeID,true);
        }
        if(!enabled&&st.effects[s]){st.effects[s]->free();delete st.effects[s];st.effects[s]=nullptr;}
        if(enabled&&!ready)fxPending=true;
        if(auto* x=st.effects[s]){x->moveBefore(st.outputSynth->nodeID);x->set("in",input);x->set("out",st.fxBus[s]->index);sendEffect(x,s);input=st.fxBus[s]->index;}
    }
    st.outputSynth->set("in",input);st.outputSynth->set("out",outputBuses.count(server)?outputBuses.at(server):0);st.outputSynth->set("master",master.get());
}
