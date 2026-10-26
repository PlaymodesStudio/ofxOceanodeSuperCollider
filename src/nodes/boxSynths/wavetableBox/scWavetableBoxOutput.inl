// Post-voice graph: spread -> optional effects -> master/output.
template<typename T>
void scWavetableBox::registerOutput(ofParameter<T>& p, std::vector<std::string> choices){
    addPublishAction(p, choices);
    auto* parameter=&p;
    nodeListeners.push(p.newListener([this,parameter](T&){
        snaps.morph.edited(*parameter);
        // the delay a morph blends between Echo Sync's seconds and beats
        if(parameter==static_cast<void*>(&echoDelay) || parameter==static_cast<void*>(&echoBeatVal)) snaps.morph.edited(echoBeats);
        if(!restoring && !snaps.morph.applying()) updateOutputs();
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
    return snaps.morph.evaluateVector(speakerLayout,[&](int layoutMode){
        return snaps.morph.evaluateVector(placement,[&](int placementMode){
    std::vector<float> positions(MAX_CHANNELS, 0.5f);
    const int channels = outChannels.get();
    auto position = [&](int voice, int count){
        if(placementMode == 2) return valueFor(pan.get(), voice, 0.5f);
        if(placementMode == 1) return channels <= 1 ? 0.5f : float(voice % channels) / ((channels == 2 || layoutMode == 1) ? channels - 1 : channels);
        if(channels > 2 && layoutMode == 0) return float(voice) / count * spreadWidth.get() + spreadCenter.get() * 0.5f;
        return 0.5f + spreadCenter.get() * 0.5f + (count > 1 ? (float(voice) / (count - 1) - 0.5f) * spreadWidth.get() : 0);
    };
    for(int voice = 0; voice < renderingVoices(); ++voice){
        if(voiceMorphWeights.empty()){ positions[voice] = position(voice, voices.get()); continue; }
        float sum = 0, weight = 0;
        for(size_t i = size_t(voice); i < voiceMorphWeights.size(); ++i){
            sum += voiceMorphWeights[i] * position(voice, int(i) + 1);
            weight += voiceMorphWeights[i];
        }
        if(weight > 0) positions[voice] = sum / weight;
    }
    return positions;
        });
    });
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
float scWavetableBox::echoDelaySeconds(const ofJson* sound) const{
    auto number=[&](const ofAbstractParameter& p,float fallback){
        if(!sound || !sound->contains(p.getEscapedName())) return fallback;
        const auto& v=(*sound)[p.getEscapedName()];
        return v.is_number() ? v.get<float>() : v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : fallback;
    };
    const bool beats=number(echoBeats,echoBeats.get() ? 1.0f : 0.0f) > 0.5f;
    const float seconds=beats ? number(echoBeatVal,echoBeatVal.get())*60/std::max(1.0f,currentBpm) : number(echoDelay,echoDelay.get());
    return std::clamp(seconds,0.005f,4.0f);
}
float scWavetableBox::effectTail(int s) const{
    if(s==FxComp || s==FxEq) return 0.05f;
    if(s==FxBlur){
        // the eight allpass tails one after another (as PolyComb's Blur)
        const float g=ofLerp(0.25f,0.72f,ofClamp(blurDensity.get(),0.0f,1.0f));
        return std::min(30.0f,blurTime.get()*0.001f*13.43f*std::log(0.001f)/std::log(g)+0.1f);
    }
    if(s==FxEcho){
        const float feed=ofClamp(echoFeed.get(),0.0f,0.98f);
        const float repeats=feed>0.001f ? std::log(0.001f)/std::log(feed) : 0.0f;
        return std::min(30.0f,echoDelaySeconds()*(repeats+1.0f)+0.2f);
    }
    return std::min(30.0f,reverbDecay.get()+reverbPredelay.get()*0.001f+0.3f);
}
void scWavetableBox::sendEffect(ofxSCServer* server,ofxSCSynth* x,int s,scSynthControlBatch::Cache* cache){
    if(!x)return;
    const float on=snaps.morph.activation(fxOn[s]);
    scSynthControlBatch controls(x,server,cache);
    auto set=[&](const std::string& k,float v){controls.set(k,v);};
    set("on",on);
    if(s==FxComp){set("threshold",compThreshold);set("ratio",compRatio);set("attack",compAttack);set("release",compRelease);set("makeup",compMakeup);set("mix",compMix);set("knee",6);set("automakeup",0);set("rms",1);set("schpf",20);set("grBus",-1);}
    else if(s==FxEq){for(int b=0;b<5;++b){std::string k="b"+ofToString(b+1);set(k+"gain",eqGain[b].get()*on);set(k+"freq",eqFreq[b]);set(k+(b==0||b==4?"slope":"rq"),1/(scEQEditor::isShelf(b)?scEQEditor::safeShelfSlope(eqShape[b].get(),eqGain[b].get()*on):std::max(0.1f,eqShape[b].get())));}}
    else if(s==FxBlur){set("time",blurTime);set("blur",blurAmount);set("density",blurDensity);set("spread",blurSpread);set("hipass",blurLowCut);set("lowpass",blurHighCut);set("mix",blurMix);}
    else if(s==FxEcho){
        const float delay=snaps.morph.hasEffective(echoBeats) ? snaps.morph.resolvedValue(echoBeats).get<float>() : echoDelaySeconds();
        set("delay",std::clamp(delay,0.005f,4.0f));set("feed",echoFeed);set("cutoff",echoCutoff);set("resonance",echoResonance);
        set("filtertype",echoFilter);controls.set("filterweights",snaps.morph.weights(echoFilter));set("pingpong",echoPingPong);set("mix",echoMix);
    }
    else{set("mix",reverbMix);set("decay",reverbDecay);set("size",reverbSize);set("predelay",reverbPredelay);set("position",reverbLate);set("spread",reverbSpread);set("lowpass",reverbLowpass);}
    controls.send();
}
void scWavetableBox::freeOutput(ofxSCServer* server){
    auto it=modStates.find(server);if(it==modStates.end())return;auto& st=it->second;
    auto release=[](ofxSCSynth*& x){if(x){x->free();delete x;x=nullptr;}};
    release(st.spreadSynth);release(st.outputSynth);for(auto*& x:st.effects)release(x);
    st.outputChannels=0;st.chain.clear();st.spreadSent.clear();st.outputSent.clear();
    for(auto& sent:st.effectSent)sent.clear();
    st.effectOffAt.fill(0);
}
void scWavetableBox::updateOutputs(){
    fxPending=false;
    for(auto& [server,st]:modStates)updateOutput(server);
}
// Effects switched off keep running (on = 0: dry) until their tail has rung out
void scWavetableBox::retireEffects(){
    const float now=ofGetElapsedTimef();
    for(auto& [server,st]:modStates){
        bool removed=false;
        for(int s=0;s<NumFx;++s){
            if(!st.effects[s] || st.effectOffAt[s]<=0 || now<st.effectOffAt[s])continue;
            st.effects[s]->free();delete st.effects[s];st.effects[s]=nullptr;
            st.effectSent[s].clear();st.effectOffAt[s]=0;removed=true;
        }
        if(removed)updateOutput(server);
    }
}
// Every call sends only what changed (morph frames, BPM, edits); the chain is
// re-ordered only when an effect joins or leaves it.
void scWavetableBox::updateOutput(ofxSCServer* server){
    auto it=modStates.find(server);if(it==modStates.end())return;auto& st=it->second;
    const int c=outChannels.get();
    if(st.outputChannels!=c)freeOutput(server);
    bool ready=true;
    if(!server->isNRTCapturing())for(auto* sm:servers)if(sm&&sm->getServer()==server)ready=!sm->areRequiredSynthdefsLoading();
    const int outBus=outputBuses.count(server)?outputBuses.at(server):0;
    const bool fresh=!st.spreadSynth;
    if(fresh){
        st.spreadSynth=new ofxSCSynth("WavetableBoxSpread"+ofToString(c),server);
        st.outputSynth=new ofxSCSynth("WavetableBoxOut"+ofToString(c),server);
        st.outputChannels=c;
    }
    {
        scSynthControlBatch spread(st.spreadSynth,server,&st.spreadSent);
        spread.set("in",st.voiceBus->index);spread.set("out",st.panBus->index);
        spread.set("voices",MAX_CHANNELS);spread.set("pos",voicePositions());
        spread.set("mpan",modulation.targetBus(server,TPan));spread.set("line",snaps.morph.weights(speakerLayout)[1]);
        spread.send();
    }
    const float now=ofGetElapsedTimef();
    std::array<bool,NumFx> created{};
    for(int s=0;s<NumFx;++s){
        const bool enabled=snaps.morph.activation(fxOn[s])>0.0001f;
        if(enabled){
            st.effectOffAt[s]=0;
            if(!st.effects[s]&&ready){st.effects[s]=new ofxSCSynth(fxDef(s),server);st.effectSent[s].clear();created[s]=true;}
            if(!ready)fxPending=true;
        }else if(st.effects[s]&&st.effectOffAt[s]<=0)st.effectOffAt[s]=now+effectTail(s);
    }
    std::vector<int> chain;
    for(int s=0;s<NumFx;++s)if(st.effects[s])chain.push_back(s);
    const bool rechain=chain!=st.chain;
    if(fresh){
        scSynthControlBatch out(st.outputSynth,server,&st.outputSent);
        out.set("in",chain.empty()?st.panBus->index:st.fxBus[chain.back()]->index);out.set("out",outBus);out.set("master",master.get());out.send();
        st.spreadSynth->createAndRun(0,st.processing->nodeID,true);
        st.outputSynth->createAndRun(1,st.processing->nodeID,true);
    }
    int input=st.panBus->index;
    for(int s:chain){
        auto* x=st.effects[s];
        scSynthControlBatch route(x,server,&st.effectSent[s]);
        route.set("in",input);route.set("out",st.fxBus[s]->index);route.send();
        sendEffect(server,x,s,&st.effectSent[s]);
        if(created[s])x->createAndRun(2,st.outputSynth->nodeID,true);   // before the output, after the effects already placed
        else if(rechain)x->moveBefore(st.outputSynth->nodeID);
        input=st.fxBus[s]->index;
    }
    st.chain=chain;
    scSynthControlBatch out(st.outputSynth,server,&st.outputSent);
    out.set("in",input);out.set("out",outBus);out.set("master",master.get());out.send();
}
