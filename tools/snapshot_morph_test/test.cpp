#include "nodes/boxSynths/shared/scMorphPolicies.h"
#include <cassert>
#include <cmath>
#include <iostream>

void near(float a, float b) { assert(std::abs(a-b) < 1e-5f); }
int main() {
    scMorphPolicies m;
    ofParameter<bool> fx("Effect_On", false);
    ofParameter<int> source("FM_Source", 0), type("Type", 0);
    ofParameter<std::vector<float>> depth("Depth", {2,4});
    m.bypass(fx);
    m.mode(source, 4);
    m.mode(type, 4);
    m.enabledDepth(source, depth);
    ofJson a={{"Effect_On",false},{"FM_Source",0},{"Depth",{9.0f,15.0f}},{"Type",0}};
    ofJson b={{"Effect_On",true},{"FM_Source",3},{"Depth",{2.0f,4.0f}},{"Type",3}};
    for(float t : {0.0f, 0.1f, 0.49f, 0.5f, 0.51f, 0.9f, 1.0f}) {
        m.begin(a,b,t);
        // A UI-side switch/listener must never discard fractional audio state.
        fx.set(t>=0.5f); m.edited(fx);
        near(m.activation(fx),t);
        auto d=m.value(depth); near(d[0],2*t); near(d[1],4*t);
        const auto w=m.weights(type);
        near(w[0],1-t); near(w[3],t); near(w[1],0); near(w[2],0);
        m.end();
    }
    // Store halfway, interrupt with a third preset, and reverse it: start from
    // the actual partial bypass, modulation depth and mixed mode distribution.
    m.begin(a,b,0.4f); fx.set(false); source.set(0); type.set(0); m.end();
    ofJson current=a; m.capture(current);
    assert(current.contains(scMorphPolicies::metadata));
    ofJson c=a; c["Type"]=1;
    m.begin(current,c,0); near(m.activation(fx),0.4f); near(m.value(depth)[0],0.8f);
    near(m.weights(type)[3],0.4f); m.end();
    m.begin(current,c,0.5f); near(m.activation(fx),0.2f); near(m.value(depth)[0],0.4f);
    auto w=m.weights(type); near(w[0],0.3f); near(w[1],0.5f); near(w[3],0.2f); m.end();
    m.begin(b,a,0.75f); near(m.activation(fx),0.25f); near(m.value(depth)[0],0.5f); m.end();
    // Connected controls override snapshots. Manual edits resume ordinary use.
    fx.set(true);
    m.begin(a,b,0.2f,[](const std::string& key){ return key=="Effect_On"; });
    near(m.activation(fx),1); m.end();
    m.begin(a,b,0.2f); m.end(); fx.set(false); m.edited(fx); near(m.activation(fx),0);
    bool driven=false;
    m.begin(a,b,0.3f,[&](const std::string& key){ return driven && key=="Effect_On"; }); m.end();
    near(m.activation(fx),0.3f); driven=true; near(m.activation(fx),0);
    // Endpoint snapshots need no extra metadata; intermediate captures retain it.
    driven=false; fx.set(true); type.set(3); source.set(3); depth.set({2,4});
    m.begin(b,b,1); m.end(); auto end=b; m.capture(end);
    assert(!end.contains(scMorphPolicies::metadata));
    // Nested AnalogBox and serialized Sample/Grain scalar values.
    scMorphPolicies nested;
    ofParameter<bool> on("On", true);
    nested.bypass(on,scMorphPolicies::field("On","controls"));
    nested.begin({{"controls",{{"On","0"}}}},{{"controls",{{"On","1"}}}},0.3f);
    near(nested.activation(on),0.3f); nested.end();
    // Missing legacy fields use stable registration defaults, not the last mix.
    nested.begin(ofJson::object(),{{"controls",{{"On",false}}}},0.8f); nested.end();
    nested.begin(ofJson::object(),{{"controls",{{"On",false}}}},0.2f);
    near(nested.activation(on),0.8f); nested.end();
    std::cout << "PASS: continuous bypass, zero-depth, direct mode weights, stored/interrupted/reversed morphs, connections, manual edits and legacy schemas\n";
}
