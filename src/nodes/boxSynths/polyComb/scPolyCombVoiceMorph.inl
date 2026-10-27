// Temporary count-dependent voice banks. Included by scPolyComb.cpp so projects
// need no additional translation unit. Ordinary playback never enters this path.
void scPolyComb::prepareVoiceMorph(const ofJson& a,const ofJson& b,float t) {
    if(voicesHandle && voicesHandle->hasInConnection()) return;
    const int fallback=userVoices>0 ? userVoices : voices.get();
    const auto wa=scVoiceCountMorph::endpoint(a,fallback,MAX_VOICES);
    const auto wb=scVoiceCountMorph::endpoint(b,fallback,MAX_VOICES);
    if(voiceMorphWeights.empty() && wa==wb && scVoiceCountMorph::single(wa)) return;
    bool running=false;
    for(const auto& item:states) running|=item.second.created;
    if(!running) return; // offline loading needs no temporary graph
    if(voiceMorphWeights.empty()) morphRenderVoices=n();
    morphDomainVoices=n();
    for(int i=0;i<MAX_VOICES;++i) if(wa[i]>0 || wb[i]>0) morphDomainVoices=std::max(morphDomainVoices,i+1);
    auto weights=scVoiceCountMorph::blend(a,b,t,fallback,MAX_VOICES);
    if(weights!=voiceMorphWeights) {
        voiceHandoff=0; voiceEndpointSince=-1;
        voiceMorphWeights=std::move(weights);
    }
    updatePitchIndexLimits(); // domain changes must not clamp parameters at 0.5
}

void scPolyComb::configureVoiceMix(ServerState& st) {
    if(!st.voiceMix) return;
    const bool capture=st.server->isNRTCapturing() && st.server->isNRTCaptureOnly();
    if(!capture && !st.voiceMix->isCreated()) return;
    if(st.voiceMixReady<0) st.voiceMixReady=ofGetElapsedTimef()+0.02f;
    std::vector<float> inputs(MAX_VOICES+1,st.scratch ? st.scratch->index : 0);
    std::vector<float> weights(MAX_VOICES+1,0);
    inputs[0]=st.voiceMainBus->index;
    for(int count=1;count<=MAX_VOICES;++count) {
        if(st.voicePeerBuses[count-1]) inputs[count]=st.voicePeerBuses[count-1]->index;
        // During promotion both mixer slots read the same running bank. Its
        // coefficient moves home without changing the waveform or envelopes.
        if(voiceAliasCount==count && count==n()) inputs[count]=st.voiceMainBus->index;
        if(voiceHandoff==2) continue;
        const float gain=voiceMorphWeights[count-1];
        if(count==n()) weights[0]+=gain;
        else if(voicePeers.count(count)) weights[count]=gain;
    }
    if(voiceHandoff==2) weights[0]=1;
    if(inputs!=st.voiceMixInputs) {
        st.voiceMix->set("inputs",inputs); st.voiceMixInputs=inputs;
    }
    // Never fade into a bank whose /n_go has not arrived. The initial mixer
    // coefficient stays at the live original bank until all contributors exist.
    bool contributorsReady=true;
    if(!capture) for(const auto& item:voicePeers) if(weights[item.first]>0) {
        auto it=item.second.node->states.find(st.server);
        if(it==item.second.node->states.end() || !it->second.spread || !it->second.spread->isCreated()) contributorsReady=false;
    }
    // Keep the original direct path until the paused/configured mixer is live.
    if(contributorsReady && ofGetElapsedTimef()>=st.voiceMixReady && weights!=st.voiceMixWeights) {
        st.voiceMix->set("weights",weights); st.voiceMixWeights=weights;
    }
    if(!st.voiceMix->isCreated()) st.voiceMix->resendStoredArgs();
    routeChain(st);
}

void scPolyComb::syncVoiceMorph() {
    if(voiceRenderer || syncingVoiceMorph || applyingSnapshot || voiceMorphWeights.empty()) return;
    syncingVoiceMorph=true;
    // Scheduled gate/pitch values and their timetag remain in effect while
    // forwarding a note to each temporary bank, including audio-rate cables.
    auto inEffect=noteInputs.inEffectNow();
    const float now=ofGetElapsedTimef();
    const auto sound=captureSnapshot();
    for(int count=1;count<=MAX_VOICES;++count) {
        const bool wanted=voiceMorphWeights[count-1]>0 && count!=n();
        if(!wanted) continue;
        auto& peer=voicePeers[count];
        peer.unusedSince=-1;
        if(!peer.node) {
            peer.node=std::make_unique<scPolyComb>(servers,true);
            peer.node->transposeOwner=this;
            peer.node->setup();
            peer.node->voices.setWithoutEventNotifications(count);
            peer.node->userVoices=count;
            peer.node->outChannels.setWithoutEventNotifications(outChannels.get());
            peer.node->updatePitchIndexLimits(); peer.node->updateScramble();
            peer.node->modulation.voicesChanged();
        }
    }
    // A held source endpoint needs no mixer or peer at all.
    if(voicePeers.empty()) {
        for(auto& [server,st]:states) if(st.created) configureVoiceMix(st);
        syncingVoiceMorph=false; return;
    }
    for(auto& item:voicePeers) {
        const int count=item.first; auto& peer=item.second; auto& node=*peer.node;
        node.gateIn.port=gateIn.port; node.pitchIn.port=pitchIn.port;
        node.gateIn.handle=gateIn.handle; node.pitchIn.handle=pitchIn.handle;
        node.gateIn.buses=gateIn.buses; node.pitchIn.buses=pitchIn.buses;
        node.spreadLayout.set(spreadLayout.get());
        // Live external modulators are deliberately excluded from snapshots.
        // Forward their current values without recalling any snapshot controls.
        for(const auto& control:controls) if(control.key.rfind("Ext_",0)==0) {
            auto target=std::find_if(node.controls.begin(),node.controls.end(),[&](const Control& c){return c.key==control.key;});
            if(target!=node.controls.end()) {
                const auto value=control.save();
                if(value!=target->save()) target->load(value);
            }
        }
        node.gate.set(gate.get()); node.pitch.set(pitch.get()); node.master.set(master.get());
        if(node.currentBpm!=currentBpm) node.setBpm(currentBpm);
        auto copy=sound;
        copy.erase(scVoiceCountMorph::key);
        copy["voices"]=count;
        if(copy!=peer.lastSound) {
            node.applyState(copy,copy,1,true);
            peer.lastSound=std::move(copy);
        }
        for(auto& [server,st]:states) {
            if(!st.created) continue;
            if(!st.voiceMix) {
                st.voiceMainBus=new ofxSCBus(RATE_AUDIO,outChannels.get(),server);
                st.voiceMix=new ofxSCSynth("PCVoiceMorph"+ofToString(outChannels.get()),server);
                st.voiceMixReady=(server->isNRTCapturing() && server->isNRTCaptureOnly()) ? now+0.04f : -1;
                scStartConfiguredSynth(st.voiceMix,server,1,st.groups[SlSpread]->nodeID,[&](){
                    st.voiceMix->set("out",st.scratch ? st.scratch->index : 0);
                    std::vector<float> inputs(MAX_VOICES+1,st.scratch ? st.scratch->index : 0);
                    inputs[0]=st.voiceMainBus->index;
                    st.voiceMix->set("inputs",inputs); st.voiceMixInputs=inputs;
                    std::vector<float> initial(MAX_VOICES+1,0); initial[0]=1;
                    st.voiceMix->set("weights",initial); st.voiceMixWeights=initial;
                });
            }
            if(!st.voicePeerBuses[count-1])
                st.voicePeerBuses[count-1]=new ofxSCBus(RATE_AUDIO,outChannels.get(),server);
            auto& ps=node.states[server];
            if(!ps.created) {
                node.rendererBefore[server]=st.voiceMix->nodeID;
                node.buildSynth(server);
                node.setOutputBus(server,0,st.voicePeerBuses[count-1]->index);
                node.createSynth(server);
            }
        }
    }
    for(auto& [server,st]:states) if(st.created) configureVoiceMix(st);
    syncingVoiceMorph=false;
}

bool scPolyComb::adoptVoiceBank(int count) {
    auto found=voicePeers.find(count);
    if(found==voicePeers.end()) return false;
    auto& peer=*found->second.node;
    for(const auto& [server,st]:states) if(st.created) {
        auto it=peer.states.find(server);
        if(it==peer.states.end() || !it->second.created || !peer.modulation.hasVoiceBank(server,count)) return false;
        if(!(server->isNRTCapturing() && server->isNRTCaptureOnly()) && !it->second.spread->isCreated()) return false;
    }
    syncingVoiceMorph=true;
    morphRenderVoices=count;
    voices.setWithoutEventNotifications(count);
    rebuildAt=-1;
    for(auto& [server,st]:states) {
        if(!st.created) continue;
        auto& incoming=peer.states.at(server);
        const bool latency=server->getBLatency(); server->setBLatency(true);
        // The endpoint bank is already audible. Move its groups out of the
        // temporary root BEFORE freeing the old spread slot containing it.
        std::array<ofxSCGroup*,SlSpread+1> oldGroups{};
        for(int slot=SlGate;slot<=SlSpread;++slot) {
            oldGroups[slot]=st.groups[slot];
            incoming.groups[slot]->moveBefore(st.groups[SlComp]->nodeID);
            st.groups[slot]=std::exchange(incoming.groups[slot],nullptr);
        }
        auto fx=st.fx; st.fx={}; freeSynths(st); st.fx=fx;
        modulation.adoptServerState(peer.modulation,server);
        st.modCreated=incoming.modCreated; incoming.modCreated=false;
        auto drop=[&](ofxSCBus*& bus,bool audio) {
            if(bus && (audio ? liveAudioBus(server,bus) : liveControlBus(server,bus))) bus->free();
            delete bus; bus=nullptr;
        };
        for(auto* bus:{&st.gateBus,&st.envBus,&st.fmBus,&st.oscBus,&st.voiceBus,&st.scratch}) drop(*bus,true);
        for(auto*& bus:st.insBus) drop(bus,true);
        drop(st.noteBus,false); drop(st.visBlock,false);
        st.gateS=std::exchange(incoming.gateS,nullptr); st.ampEnv=std::exchange(incoming.ampEnv,nullptr);
        st.pitchS=std::exchange(incoming.pitchS,nullptr); st.amp=std::exchange(incoming.amp,nullptr);
        st.spread=std::exchange(incoming.spread,nullptr); st.delta=std::exchange(incoming.delta,nullptr);
        st.fm=std::exchange(incoming.fm,Part{}); st.sub=std::exchange(incoming.sub,Part{});
        for(int i=0;i<NumOsc;++i) st.osc[i]=std::exchange(incoming.osc[i],Part{});
        for(int i=0;i<NumIns;++i) st.ins[i]=std::exchange(incoming.ins[i],Part{});
        st.gateBus=std::exchange(incoming.gateBus,nullptr); st.envBus=std::exchange(incoming.envBus,nullptr);
        st.fmBus=std::exchange(incoming.fmBus,nullptr); st.oscBus=std::exchange(incoming.oscBus,nullptr);
        st.voiceBus=std::exchange(incoming.voiceBus,nullptr); st.scratch=std::exchange(incoming.scratch,nullptr);
        st.noteBus=std::exchange(incoming.noteBus,nullptr); st.visBlock=std::exchange(incoming.visBlock,nullptr);
        for(int i=0;i<NumIns;++i) st.insBus[i]=std::exchange(incoming.insBus[i],nullptr);
        st.builtVoices=incoming.builtVoices; st.spreadLanes=incoming.spreadLanes;
        // This bank's spreader already writes here. Keep that bus as the main
        // output, then fade two aliases of it to a single mixer coefficient.
        std::swap(st.voiceMainBus,st.voicePeerBuses[count-1]);
        // Keep the mixer outside the old spread slot. Silent peers can now go;
        // all audible endpoint groups have already moved into the main root.
        st.voiceMix->moveBefore(st.groups[SlComp]->nodeID);
        for(auto& item:voicePeers) if(item.first!=count) item.second.node->free(server);
        if(incoming.root) incoming.root->free();
        for(auto*& group:oldGroups) if(group){group->free();delete group;group=nullptr;}
        delete incoming.root; incoming.root=nullptr;
        peer.freeAll(server,incoming);
        server->setBLatency(latency);
    }
    scrambleOrder=peer.scrambleOrder;
    previousMapGate=peer.previousMapGate;
    voicePeers.clear();
    voiceAliasCount=count;
    syncingVoiceMorph=false;
    return true;
}

void scPolyComb::releaseVoiceMorphServer(ofxSCServer* server,ServerState& st) {
    if(voiceRenderer) return;
    st.voiceMixInputs.clear(); st.voiceMixWeights.clear();
    for(auto& item:voicePeers) item.second.node->free(server);
    if(st.voiceMix) { st.voiceMix->free(); delete st.voiceMix; st.voiceMix=nullptr; }
    auto drop=[&](ofxSCBus*& bus) {
        if(bus && liveAudioBus(server,bus)) bus->free();
        delete bus; bus=nullptr;
    };
    drop(st.voiceMainBus);
    for(auto*& bus:st.voicePeerBuses) drop(bus);
}

void scPolyComb::releaseVoiceMorph() {
    for(auto& [server,st]:states) {
        releaseVoiceMorphServer(server,st);
        if(st.created) routeChain(st);
    }
    voicePeers.clear(); voiceMorphWeights.clear(); morphRenderVoices=0; voiceAliasCount=0;
    voiceHandoff=0; voiceEndpointSince=-1; voiceHandoffAt=-1; morphDomainVoices=0;
    updatePitchIndexLimits();
}

void scPolyComb::updateVoiceMorph(ofEventArgs& args) {
    if(voiceRenderer || voiceMorphWeights.empty()) return;
    // Incoming Voices has precedence. Transition to its value before cleanup.
    if(voicesHandle && voicesHandle->hasInConnection()) {
        std::vector<float> driven(MAX_VOICES,0); driven[voices.get()-1]=1;
        if(driven!=voiceMorphWeights) {
            voiceMorphWeights=std::move(driven); voiceEndpointSince=-1; voiceHandoff=0;
        }
    }
    syncVoiceMorph();
    for(auto& item:voicePeers) item.second.node->update(args);
    const float now=ofGetElapsedTimef();
    // Peer routes fade for 30 ms; wait substantially longer before freeing any.
    for(auto it=voicePeers.begin();it!=voicePeers.end();) {
        const int count=it->first;
        const bool audible=voiceHandoff!=2 && voiceMorphWeights[count-1]>0 && count!=n();
        if(audible) { it->second.unusedSince=-1; ++it; continue; }
        if(it->second.unusedSince<0) it->second.unusedSince=now;
        if(now-it->second.unusedSince<0.2f) { ++it; continue; }
        it=voicePeers.erase(it);
        for(auto& [server,st]:states) {
            auto*& bus=st.voicePeerBuses[count-1];
            if(bus && liveAudioBus(server,bus)) bus->free();
            delete bus; bus=nullptr;
        }
    }
    const int endpoint=scVoiceCountMorph::single(voiceMorphWeights);
    for(const auto& [server,st]:states) if(st.created && st.voiceMix &&
        (st.voiceMixReady<0 || now<st.voiceMixReady || (endpoint>0 &&
         (st.voiceMixWeights.empty() || st.voiceMixWeights[endpoint==n() ? 0 : endpoint]<0.999f)))) {
        voiceEndpointSince=-1; return;
    }
    if(!endpoint) { voiceEndpointSince=-1; return; }
    if(voiceEndpointSince<0) voiceEndpointSince=now;
    if(voiceHandoff==0 && now-voiceEndpointSince>=0.2f) {
        if(endpoint==n()) { releaseVoiceMorph(); return; }
        // Promote the running endpoint graph, including modulation and
        // resonator/envelope state. No second voice rebuild or note retrigger.
        if(adoptVoiceBank(endpoint)) {
            voiceHandoff=2; voiceHandoffAt=now;
            syncVoiceMorph();
        }
    } else if(voiceHandoff==2 && now-voiceHandoffAt>=0.2f) releaseVoiceMorph();
}
