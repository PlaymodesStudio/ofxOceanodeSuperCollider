#include "MacroMixerServerAllocator.h"
#include <cmath>

namespace MacroMixerServerAllocator {
ofxSCServer* choose(const Request& request) {
    if(request.single || request.origin == nullptr) return nullptr;
    const Candidate* best = nullptr;
    // Strictly better: free before busy, then loaded SynthDefs, then lower CPU.
    // Ties keep the earlier server, so the choice is stable.
    const auto better = [](const Candidate& a, const Candidate& b) {
        if(a.busy != b.busy) return !a.busy;
        if(a.loading != b.loading) return !a.loading;
        const float ca = std::isfinite(a.cpu) ? a.cpu : 1e9f, cb = std::isfinite(b.cpu) ? b.cpu : 1e9f;
        return ca < cb;
    };
    for(const auto& candidate : request.candidates) {
        if(candidate.server == nullptr || candidate.server == request.origin || !candidate.reachable) continue;
        if(best == nullptr || better(candidate, *best)) best = &candidate;
    }
    return best ? best->server : nullptr;
}
}
