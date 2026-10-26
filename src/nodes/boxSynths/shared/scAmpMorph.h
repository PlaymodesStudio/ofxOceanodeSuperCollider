#pragma once

#include "scMorphPolicies.h"
#include <algorithm>
#include <functional>

// Drone and "an envelope owns Amp" (\vca) select different amplitude paths in
// the Box voices. Across a snapshot morph their effective amounts blend and
// the voice SynthDefs crossfade the two paths, so neither switches at Morph
// Switch. At 0 and 1 the voices compute exactly what the old Select did.
//
// - droneAmount(): Drone's activation (0..1), sent as the voice's \drone.
// - gateDrone(): the gate synth holds its gates open only once the blend has
//   fully arrived; until then the voice's own crossfade covers it.
// - envAmpScale(): an envelope -> Amp route fades out while Drone fades in,
//   instead of being skipped at the midpoint (routes skip it at gateDrone()).
// - vcaAmount(live): the blended endpoint VCA states during and after a morph,
//   the live state once the user edits the routing or Drone.
class scAmpMorph {
public:
    using Endpoint = std::function<bool(const ofJson&)>;
    // readDrone: where a snapshot keeps Drone (default: its key at the top level)
    void setup(scMorphPolicies& policies, ofParameter<bool>& droneParameter, Endpoint endpointVca,
               scMorphPolicies::Read readDrone = {}) {
        morph = &policies;
        drone = &droneParameter;
        vca.set("Amp Envelope Path", 0.0f, 0.0f, 1.0f);
        policies.bypass(droneParameter, std::move(readDrone));
        policies.resolved(vca, [endpointVca](const ofJson& j) { return ofJson(endpointVca(j) ? 1.0f : 0.0f); });
    }
    // Boxes whose Drone only holds the gate synth's gates: the VCA blend alone
    void setupVca(scMorphPolicies& policies, Endpoint endpointVca) {
        morph = &policies;
        vca.set("Amp Envelope Path", 0.0f, 0.0f, 1.0f);
        policies.resolved(vca, [endpointVca](const ofJson& j) { return ofJson(endpointVca(j) ? 1.0f : 0.0f); });
    }
    // Boxes that send after the apply (a dirty flag): its live VCA state, at its end
    void markApplied(bool live) const { appliedLive = live; applied = true; }
    float droneAmount() const {
        if(!drone) return 0.0f;
        return morph ? std::clamp(morph->activation(*drone), 0.0f, 1.0f) : (drone->get() ? 1.0f : 0.0f);
    }
    bool gateDrone() const { return droneAmount() >= 0.9999f; }
    float envAmpScale() const {
        const float d = droneAmount();
        return d > 0.0f && d < 1.0f ? 1.0f - d : 1.0f;
    }
    // `live`: the node's own rule (an envelope routed to Amp, and not droning)
    float vcaAmount(bool live) const {
        if(!morph || !morph->hasEffective(vca)) return live ? 1.0f : 0.0f;
        if(morph->applying()) markApplied(live);
        else if(!applied || live != appliedLive) return live ? 1.0f : 0.0f;
        return std::clamp(morph->value(vca), 0.0f, 1.0f);
    }
private:
    scMorphPolicies* morph = nullptr;
    ofParameter<bool>* drone = nullptr;
    ofParameter<float> vca;
    mutable bool applied = false, appliedLive = false;
};
