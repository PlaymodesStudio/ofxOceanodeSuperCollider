#pragma once

#include <set>

// Resolve an output through paused processors without changing the visible
// patch. A missing first input or a cycle of paused nodes resolves to silence.
template<typename Port, typename IsEnabled, typename FirstInput>
Port scResolveAudioSource(Port port, IsEnabled isEnabled, FirstInput firstInput){
    std::set<decltype(port.getNodeRef())> visited;
    while(auto* node = port.getNodeRef()){
        if(isEnabled(node)) return port;
        if(!visited.insert(node).second) return Port();
        port = firstInput(node);
    }
    return Port();
}
