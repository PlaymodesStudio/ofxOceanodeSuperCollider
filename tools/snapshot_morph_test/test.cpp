#include "nodes/boxSynths/shared/scMorphPolicies.h"
#include <cassert>
#include <cmath>
#include <iostream>

void near(float a, float b) { assert(std::abs(a-b) < 1e-5f); }
int main() {
    for(float t:{0.f,.001f,.499f,.5f,.501f,1.f}){
        auto bypass=scMorphPolicies::neutralMode({1-t,0,t});
        assert(std::abs(bypass.second-t)<1e-5f);
        auto algorithm=scMorphPolicies::neutralMode({0,1-t,t});
        assert(std::abs(algorithm.second-std::abs(1-2*t))<1e-5f);
        if(t!=.5f)assert(algorithm.first==(t<.5f?1:2));
    }

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
    // SteamPipe's checkbox is a float, while polarity has signed endpoints.
    // The UI switches at its configured point; the effective sound must not.
    scMorphPolicies steam;
    ofParameter<float> allpass("Allpass_On",0), pole("Noise_One_Pole",0), polarity("Polarity",-1);
    steam.bypass(allpass); steam.bypass(pole); steam.continuous(polarity);
    const ofJson sa={{"Allpass_On",0},{"Noise_One_Pole",0},{"Polarity",-1}};
    const ofJson sb={{"Allpass_On",1},{"Noise_One_Pole",1},{"Polarity",1}};
    for(float t : {0.0f,0.001f,0.1f,0.499f,0.5f,0.501f,0.9f,1.0f}) {
        steam.begin(sa,sb,t);
        allpass.set(t>=0.5f); pole.set(t>=0.5f); polarity.set(t>=0.5f ? 1 : -1);
        steam.edited(allpass); steam.edited(pole); steam.edited(polarity);
        near(steam.activation(allpass),t); near(steam.activation(pole),t);
        near(steam.value(polarity),2*t-1); steam.end();
    }
    steam.begin(sa,sb,0.2f); steam.end(); auto partial=sa; steam.capture(partial);
    steam.begin(partial,sb,0); near(steam.value(polarity),-0.6f); steam.end();
    steam.begin(sa,sb,0.2f,[](const std::string& key){return key=="Polarity";});
    near(steam.value(polarity),polarity.get()); steam.end();
    polarity.set(-1); steam.edited(polarity); near(steam.value(polarity),-1);
    // Per-voice mode distributions blend evaluated results without sweeping
    // through unrelated enum values; stored intermediate rows remain intact.
    scMorphPolicies vectors;
    ofParameter<std::vector<int>> modes("Modes",{0,1});vectors.mode(modes,4);
    ofJson va={{"Modes",{0,1}}},vb={{"Modes",{3,2}}};
    vectors.begin(va,vb,.25f);vectors.end();
    near(vectors.weights(modes,0)[0],.75f);near(vectors.weights(modes,0)[3],.25f);
    near(vectors.weights(modes,1)[1],.75f);near(vectors.weights(modes,1)[2],.25f);
    near(vectors.evaluate(modes,0,0,[](int mode){return float(mode*mode);}),2.25f);
    auto vc=va;vectors.capture(vc);vectors.begin(vc,vb,0);vectors.end();
    near(vectors.weights(modes,1)[2],.25f);
    // A stateful algorithm switches only at its neutral depth, independent
    // of the user's general Start/Middle/End selector switch preference.
    scMorphPolicies neutral;
    ofParameter<int> algorithm("Algorithm",0);
    ofParameter<float> amount("Amount",1);
    neutral.neutralDepth(algorithm,amount);
    ofJson na={{"Algorithm",0},{"Amount",1.f}},nb={{"Algorithm",3},{"Amount",.5f}};
    for(float t:{0.f,.1f,.499f,.5f,.501f,.9f,1.f}){
        neutral.begin(na,nb,t);auto pa=na,pb=nb;neutral.pinSelectors(pa,pb,t);neutral.end();
        assert(pa["Algorithm"]==pb["Algorithm"] && pa["Algorithm"]==(t<.5f?0:3));
        near(neutral.value(amount),t<.5f ? 1-2*t : (2*t-1)*.5f);
    }
    neutral.begin(na,nb,.25f);neutral.end();auto nc=na;neutral.capture(nc);
    neutral.begin(nc,nb,0);near(neutral.value(amount),.5f);neutral.end();
    algorithm.set(1);neutral.edited(algorithm);near(neutral.value(amount),1);
    // Evaluated tables retain actual samples when storing interrupted morphs.
    scMorphPolicies tables;ofParameter<std::string> table("Table","A");
    tables.resolved(table,[](const ofJson& state)->ofJson{return state.value("Table",std::string("A"))=="A"?ofJson{0.f,1.f}:ofJson{1.f,-1.f};});
    tables.begin({{"Table","A"}},{{"Table","B"}},.25f);tables.end();
    near(tables.resolvedValue(table)[0],.25f);near(tables.resolvedValue(table)[1],.5f);
    ofJson ts={{"Table","A"}};tables.capture(ts);
    tables.begin(ts,{{"Table","B"}},0);tables.end();near(tables.resolvedValue(table)[1],.5f);
    std::cout << "PASS: continuous bypass, zero-depth, direct mode weights, stored/interrupted/reversed morphs, connections, manual edits and legacy schemas\n";
}
