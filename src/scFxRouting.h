#pragma once

// /s_new is asynchronous: a wrapper is not proof that the server created its
// synth. Keep the dry route until /n_go (or /n_query recovery) confirms it.
// NRT scores have no notifications; their creation and routing are ordered.
template<class Synth, class Server>
bool scFxCanRoute(Synth* synth, Server* server) {
    return synth && (synth->isCreated() || (server && server->isNRTCapturing()));
}

// A running server can still be reading /d_loadDir. Its /sync reply is the
// point at which it is safe to send /s_new for a newly required definition.
template<class Managers, class Server>
bool scFxDefinitionsReady(const Managers& managers, Server* server) {
    if(!server) return false;
    if(server->isNRTCapturing()) return true;
    for(auto* manager : managers)
        if(manager && manager->getServer() == server && manager->areRequiredSynthdefsLoading()) return false;
    return true;
}

// Called from the node's frame update. Start deferred stages, or promote a
// newly acknowledged stage, without resending all controls on every frame.
template<class Stages, class Server, class Enabled>
bool scFxAwaitingRoute(const Stages& stages, Server* server, Enabled enabled, bool definitionsReady = true) {
    for(unsigned s = 0; s < stages.size(); ++s) {
        const auto& stage = stages[s];
        if(!enabled(s)) continue;
        if(!stage.synth && definitionsReady) return true;
        if(!stage.running && scFxCanRoute(stage.synth, server)) return true;
    }
    return false;
}
