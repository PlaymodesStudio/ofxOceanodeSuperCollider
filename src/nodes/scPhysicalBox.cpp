#include "scPhysicalBox.h"
#include "scTypedValue.h"
#include "scSynthControlBatch.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "../../ugens/OceanodePhysical/PhysicalModes.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(222, 170, 105, 255);   // brass
const ImU32 exciter    = IM_COL32(245, 140,  90, 255);   // the contact
const ImU32 pickup     = IM_COL32(106, 212, 196, 255);   // the pickups
const ImU32 waveguide  = IM_COL32(150, 165, 255, 255);
const ImU32 body       = IM_COL32(222, 170, 105, 255);
}
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return std::max(1.0f, (w - gapW() * (n - 1)) / n); }
void nextCol() { ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup(); }
bool liveBus(ofxSCServer* s, ofxSCBus* b, bool audio) {
    if(!s || !b || b->index < 0) return false;
    auto& buses = audio ? s->audioBusses : s->controlBusses;
    return b->index < int(buses.size()) && buses[b->index] == b;
}
void dropBus(ofxSCServer* s, ofxSCBus*& b, bool audio) {
    if(liveBus(s, b, audio)) b->free();
    delete b; b = nullptr;
}
float first(const std::vector<float>& v, int i, float fallback) {
    return v.empty() ? fallback : v[i % v.size()];
}
ofJson blend(const ofJson& a, const ofJson& b, float t, bool sw, bool discrete) {
    if(!discrete && a.is_number() && b.is_number()) return ofLerp(a.get<float>(), b.get<float>(), t);
    if(!discrete && a.is_array() && b.is_array() && !a.empty() && !b.empty()) {
        ofJson out = ofJson::array();
        for(size_t i = 0; i < std::max(a.size(), b.size()); ++i) out.push_back(blend(a[i % a.size()], b[i % b.size()], t, sw, false));
        return out;
    }
    return sw ? b : a;
}
ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    t = ofClamp(t, 0, 1);
    auto ch = [&](int s) { return int(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
    return IM_COL32(ch(0), ch(8), ch(16), ch(24));
}
// The factory sounds: PhysicalVoice inputs that differ from the defaults
// (the Phase 0 prototype's presets, synthdefs/WIP/PhysicalBox/physicalbox.scd).
struct Factory { const char* name; std::vector<std::pair<const char*, float>> values; };
const std::vector<Factory>& factorySounds() {
    static const std::vector<Factory> f = {
        {"init", {}},
        {"Struck string", {{"excite", 0}, {"listen", 0}, {"adecay", 3}, {"hardness", 0.6f}, {"symp", 0.2f}, {"sympset", 4}, {"sympdecay", 4}, {"level", -4.0f}}},
        {"Box guitar", {{"excite", 0}, {"listen", 0}, {"adecay", 3}, {"hardness", 0.7f}, {"pos", 0.16f}, {"radiator", 3}, {"radmix", 0.55f}, {"damper", 0.25f}, {"symp", 0.25f}, {"sympset", 3}, {"sympdecay", 3}, {"level", -2.0f}}},
        {"Stiff string", {{"excite", 0}, {"listen", 0}, {"astiff", 0.35f}, {"adecay", 5}, {"hardness", 0.75f}, {"abright", 0.8f}, {"mass", 0.4f}, {"radiator", 3}, {"radmix", 0.3f}, {"damper", 0.6f}, {"symp", 0.3f}, {"sympset", 4}, {"sympdecay", 5}, {"sympshimmer", 2}, {"level", -2.5f}}},
        {"Sitar", {{"excite", 0}, {"listen", 0}, {"adecay", 5}, {"hardness", 0.85f}, {"pos", 0.08f}, {"radiator", 2}, {"radmix", 0.5f}, {"material", 1}, {"abright", 0.9f}, {"jawari", 0.8f}, {"symp", 0.7f}, {"sympset", 0}, {"sympdecay", 6}, {"sympbright", 0.7f}, {"sympshimmer", 3}}},
        {"Tanpura", {{"excite", 0}, {"listen", 0}, {"adecay", 9}, {"hardness", 0.5f}, {"mass", 0.5f}, {"pos", 0.12f}, {"radiator", 2}, {"radmix", 0.6f}, {"material", 1}, {"abright", 0.85f}, {"jawari", 1}, {"symp", 0.5f}, {"sympset", 3}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 2}, {"level", -3.0f}}},
        {"Veena", {{"excite", 0}, {"listen", 0}, {"adecay", 7}, {"hardness", 0.6f}, {"mass", 0.4f}, {"pos", 0.15f}, {"radiator", 2}, {"radmix", 0.7f}, {"material", 0.8f}, {"abright", 0.75f}, {"jawari", 0.5f}, {"symp", 0.5f}, {"sympset", 0}, {"sympdecay", 6}, {"sympbright", 0.5f}, {"sympshimmer", 2}, {"level", -3.5f}}},
        {"Jawari guitar", {{"excite", 0}, {"listen", 0}, {"adecay", 3}, {"hardness", 0.75f}, {"pos", 0.16f}, {"radiator", 3}, {"radmix", 0.55f}, {"material", 0.7f}, {"abright", 0.85f}, {"jawari", 0.5f}, {"symp", 0.3f}, {"sympset", 3}, {"sympdecay", 3}, {"level", -1.0f}}},
        {"Bowed tanpura", {{"strike", 0}, {"bow", 0.35f}, {"bowpress", 0.5f}, {"bowspeed", 0.3f}, {"atk", 0.4f}, {"rel", 2}, {"excite", 0}, {"listen", 0}, {"adecay", 6}, {"pos", 0.1f}, {"radiator", 2}, {"radmix", 0.6f}, {"material", 1}, {"abright", 0.85f}, {"jawari", 1}, {"symp", 0.6f}, {"sympset", 0}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 3}, {"level", -7.0f}}},
        {"Shimmer piano", {{"excite", 0}, {"listen", 0}, {"astiff", 0.3f}, {"adecay", 6}, {"hardness", 0.7f}, {"abright", 0.8f}, {"mass", 0.45f}, {"radiator", 3}, {"radmix", 0.3f}, {"symp", 0.6f}, {"sympset", 4}, {"sympdecay", 7}, {"sympbright", 0.6f}, {"sympshimmer", 10}, {"level", -4.5f}}},
        {"Cluster harp", {{"excite", 0}, {"listen", 0}, {"adecay", 5}, {"hardness", 0.55f}, {"pos", 0.3f}, {"abright", 0.75f}, {"radiator", 3}, {"radmix", 0.4f}, {"symp", 0.7f}, {"sympset", 5}, {"sympdecay", 6}, {"sympbright", 0.5f}, {"sympshimmer", 6}, {"level", -5.0f}}},
        {"Bowed string", {{"strike", 0}, {"bow", 1}, {"excite", 0}, {"listen", 0}, {"adecay", 2}, {"pos", 0.12f}, {"atk", 0.15f}, {"radiator", 3}, {"radmix", 0.45f}, {"symp", 0.35f}, {"sympset", 3}, {"sympdecay", 4}, {"level", -8.5f}}},
        {"Scratchy bow", {{"strike", 0}, {"bow", 1}, {"excite", 0}, {"listen", 0}, {"adecay", 2}, {"pos", 0.12f}, {"bowpress", 1.1f}, {"rosin", 0.8f}, {"atk", 0.1f}, {"level", -3.0f}}},
        {"Flute", {{"strike", 0}, {"blow", 1}, {"blowtype", 0}, {"abore", 1}, {"abright", 0.5f}, {"adecay", 1}, {"breath", 0.9f}, {"jetratio", 0.5f}, {"pos", 0.02f}, {"turb", 0.25f}, {"air", 1}, {"excite", 0}, {"listen", 0}, {"level", -10.0f}}},
        {"Pan flute", {{"strike", 0}, {"blow", 1}, {"blowtype", 0}, {"abore", 0.5f}, {"abright", 0.25f}, {"adecay", 1}, {"breath", 0.8f}, {"pos", 0.02f}, {"turb", 0.25f}, {"air", 0.4f}, {"excite", 0}, {"listen", 0}, {"level", -6.5f}}},
        {"Clarinet", {{"strike", 0}, {"blow", 1}, {"blowtype", 1}, {"abore", 0.5f}, {"pos", 0.02f}, {"adecay", 0.5f}, {"excite", 0}, {"listen", 0}, {"level", -6.5f}}},
        {"Sympathetic strings", {{"excite", 0}, {"listen", 0}, {"adecay", 4}, {"hardness", 0.6f}, {"abright", 0.8f}, {"material", 0.8f}, {"symp", 0.9f}, {"sympset", 0}, {"sympdecay", 7}, {"sympbright", 0.6f}, {"sympshimmer", 4}, {"level", -6.0f}}},
        {"Marimba", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0.75f}, {"material", 0}, {"bdecay", 4}, {"hardness", 0.45f}, {"mass", 0.4f}, {"pos", 0.25f}, {"radiator", 1}, {"radmix", 0.8f}, {"damper", 0.2f}, {"level", 4.0f}}},
        {"Xylophone", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0.25f}, {"material", 0}, {"bdecay", 3}, {"hardness", 0.85f}, {"pos", 0.25f}, {"radiator", 1}, {"radmix", 0.5f}, {"level", 5.5f}}},
        {"Vibraphone", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0.75f}, {"material", 1}, {"bdecay", 6}, {"hardness", 0.55f}, {"pos", 0.25f}, {"radiator", 1}, {"radmix", 0.8f}, {"tremrate", 5}, {"tremdepth", 0.6f}, {"damper", 0.6f}, {"symp", 0.2f}, {"sympset", 4}, {"sympdecay", 6}, {"sympbright", 0.5f}, {"level", -4.5f}}},
        {"Glockenspiel", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"hardness", 0.95f}, {"mass", 0.15f}, {"pos", 0.5f}, {"bbright", 0.7f}, {"symp", 0.2f}, {"sympset", 4}, {"sympdecay", 4}, {"sympbright", 0.8f}, {"sympshimmer", 4}, {"level", 2.0f}}},
        {"Wood bar", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0}, {"material", 0}, {"bdecay", 2}, {"hardness", 0.5f}, {"level", 7.0f}}},
        {"Gamelan saron", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0.5f}, {"material", 1}, {"bdecay", 4}, {"hardness", 0.7f}, {"beat", 5}, {"damper", 0.5f}, {"symp", 0.4f}, {"sympset", 1}, {"sympdecay", 4}, {"sympbright", 0.7f}, {"sympshimmer", 8}, {"level", 1.0f}}},
        {"Kalimba", {{"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 1}, {"material", 1}, {"bdecay", 2.5f}, {"hardness", 0.95f}, {"mass", 0.05f}, {"bbright", 0.95f}, {"pos", 0.97f}, {"radiator", 3}, {"radmix", 0.5f}, {"symp", 0.25f}, {"sympset", 3}, {"sympdecay", 2}, {"sympbright", 0.8f}, {"sympshimmer", 5}, {"level", -5.5f}}},
        {"Bowed metal bar", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0}, {"material", 1}, {"symp", 0.4f}, {"sympset", 7}, {"sympdecay", 3}, {"sympbright", 1}, {"sympshimmer", 20}, {"level", -10.0f}}},
        {"Cristal Baschet", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0}, {"material", 0.5f}, {"bdecay", 3}, {"bowpress", 0.25f}, {"bowspeed", 0.6f}, {"atk", 0.25f}, {"radiator", 5}, {"radmix", 0.7f}, {"damper", 0.3f}, {"symp", 0.4f}, {"sympset", 4}, {"sympdecay", 5}, {"sympbright", 0.7f}, {"sympshimmer", 4}, {"level", -7.5f}}},
        {"Wine glass", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0}, {"material", 0.5f}, {"bdecay", 4}, {"hardness", 0.9f}, {"split", 2}, {"symp", 0.25f}, {"sympset", 1}, {"sympdecay", 4}, {"sympbright", 0.8f}, {"level", 3.0f}}},
        {"Singing glass", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0}, {"material", 0.5f}, {"bdecay", 3}, {"pos", 0.5f}, {"bowpress", 0.2f}, {"bowspeed", 0.7f}, {"atk", 0.3f}, {"split", 1}, {"symp", 0.2f}, {"sympset", 4}, {"sympdecay", 4}, {"sympbright", 0.7f}, {"level", -6.5f}}},
        {"Singing bowl", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.25f}, {"material", 1}, {"bdecay", 8}, {"hardness", 0.4f}, {"mass", 0.6f}, {"split", 1.5f}, {"drift", 0.3f}, {"symp", 0.5f}, {"sympset", 1}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 3}, {"level", -5.5f}}},
        {"Bowed bowl", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.25f}, {"material", 1}, {"bdecay", 8}, {"pos", 0.05f}, {"bowpress", 0.3f}, {"bowspeed", 0.4f}, {"atk", 0.3f}, {"symp", 0.3f}, {"sympset", 1}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 3}, {"level", 1.0f}}},
        {"Hand bell", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.5f}, {"material", 1}, {"bdecay", 3}, {"hardness", 0.8f}, {"damper", 0.4f}, {"symp", 0.2f}, {"sympset", 1}, {"sympdecay", 3}, {"sympbright", 0.8f}, {"level", -3.5f}}},
        {"Church bell", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.75f}, {"material", 1}, {"bdecay", 6}, {"hardness", 0.8f}, {"mass", 0.7f}, {"split", 1}, {"symp", 0.35f}, {"sympset", 4}, {"sympdecay", 6}, {"sympbright", 0.6f}, {"sympshimmer", 2}, {"level", -2.0f}}},
        {"Gong ageng", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 1}, {"material", 1}, {"bdecay", 8}, {"hardness", 0.3f}, {"mass", 0.8f}, {"beat", 3}, {"symp", 0.4f}, {"sympset", 1}, {"sympdecay", 8}, {"sympbright", 0.5f}, {"sympshimmer", 6}, {"level", 2.5f}}},
        {"Opera gong", {{"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 1}, {"material", 1}, {"bdecay", 3}, {"hardness", 0.6f}, {"tension", -0.8f}, {"level", -0.5f}}},
        {"Blown bell drone", {{"strike", 0}, {"blow", 1}, {"blowtype", 0}, {"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.75f}, {"material", 1}, {"atk", 0.5f}, {"symp", 0.5f}, {"sympset", 3}, {"sympdecay", 6}, {"sympbright", 0.6f}, {"sympshimmer", 3}, {"level", -11.5f}}},
        {"Glass drone", {{"strike", 0}, {"bow", 0.6f}, {"blow", 0.4f}, {"excite", 0.6f}, {"listen", 0.6f}, {"bfamily", 2}, {"bshape", 0}, {"drift", 1}, {"freeze", 0.5f}, {"atk", 1}, {"rel", 3}, {"split", 3}, {"symp", 0.4f}, {"sympset", 5}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 8}, {"level", -3.0f}}},
        {"Handpan", {{"excite", 1}, {"listen", 1}, {"bfamily", 3}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"hardness", 0.6f}, {"mass", 0.4f}, {"split", 6}, {"bbright", 0.95f}, {"pos", 0.3f}, {"symp", 0.9f}, {"sympset", 1}, {"sympdecay", 4}, {"sympbright", 0.7f}, {"sympshimmer", 6}, {"level", -7.5f}}},
        {"Handpan Kurd", {{"excite", 1}, {"listen", 1}, {"bfamily", 3}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"hardness", 0.5f}, {"mass", 0.45f}, {"split", 6}, {"bbright", 0.9f}, {"pos", 0.3f}, {"symp", 0.8f}, {"sympset", 2}, {"sympdecay", 5}, {"sympbright", 0.6f}, {"sympshimmer", 4}, {"level", -6.0f}}},
        {"Steel pan", {{"excite", 1}, {"listen", 1}, {"bfamily", 3}, {"bshape", 1}, {"material", 1}, {"bdecay", 2.5f}, {"hardness", 0.7f}, {"split", 3}, {"symp", 0.4f}, {"sympset", 1}, {"sympdecay", 2}, {"sympbright", 0.8f}, {"level", -5.0f}}},
        {"Steel drum", {{"excite", 1}, {"listen", 1}, {"bfamily", 3}, {"bshape", 1}, {"material", 1}, {"bdecay", 2.5f}, {"hardness", 0.75f}, {"bbright", 0.85f}, {"split", 8}, {"beat", 2}, {"symp", 0.7f}, {"sympset", 1}, {"sympdecay", 2.5f}, {"sympbright", 0.8f}, {"sympshimmer", 9}, {"level", -6.0f}}},
        {"Tabla rim", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 0}, {"material", 0.4f}, {"bdecay", 1.5f}, {"hardness", 0.7f}, {"tension", 0.3f}, {"pos", 0.85f}, {"radiator", 4}, {"radmix", 0.4f}, {"symp", 0.15f}, {"sympset", 4}, {"sympdecay", 1.5f}, {"sympbright", 0.6f}, {"level", 9.0f}}},
        {"Tabla centre", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 0}, {"material", 0.4f}, {"bdecay", 0.8f}, {"hardness", 0.4f}, {"mass", 0.6f}, {"tension", 0.5f}, {"pos", 0.05f}, {"radiator", 4}, {"radmix", 0.4f}, {"level", 2.0f}}},
        {"Timpani", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 0.5f}, {"material", 0.4f}, {"bdecay", 3}, {"hardness", 0.3f}, {"mass", 0.7f}, {"pos", 0.75f}, {"tension", 0.15f}, {"damper", 0.5f}, {"symp", 0.2f}, {"sympset", 3}, {"sympdecay", 3}, {"sympbright", 0.4f}, {"level", 10.5f}}},
        {"Tom", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 1}, {"material", 0.4f}, {"tension", 0.8f}, {"bdecay", 1}, {"hardness", 0.4f}, {"pos", 0.5f}, {"radiator", 4}, {"radmix", 0.3f}, {"level", 8.5f}}},
        {"Talking drum", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 1}, {"material", 0.3f}, {"tension", 1}, {"bdecay", 1.5f}, {"hardness", 0.5f}, {"pos", 0.6f}, {"level", 8.0f}}},
        {"Frame drum", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 1}, {"material", 0.2f}, {"bdecay", 0.8f}, {"hardness", 0.25f}, {"mass", 0.6f}, {"pos", 0.8f}, {"radiator", 4}, {"radmix", 0.5f}, {"level", 19.0f}}},
        {"Frame drum centre", {{"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 1}, {"material", 0.2f}, {"bdecay", 0.6f}, {"hardness", 0.2f}, {"mass", 0.8f}, {"pos", 0.08f}, {"radiator", 4}, {"radmix", 0.5f}, {"level", 8.5f}}},
        {"Wood block", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 1}, {"material", 0}, {"bdecay", 1}, {"hardness", 0.9f}, {"level", 5.5f}}},
        {"Crash cymbal", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"bmodes", 48}, {"bbright", 1}, {"hardness", 0.95f}, {"mass", 0.25f}, {"cascade", 0.8f}, {"split", 18}, {"beat", 5}, {"pos", 0.8f}, {"posy", 0.7f}, {"symp", 0.9f}, {"sympset", 6}, {"sympdecay", 3}, {"sympbright", 1}, {"sympshimmer", 25}, {"level", -4.0f}}},
        {"Ride cymbal", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 5}, {"bmodes", 48}, {"bbright", 0.9f}, {"hardness", 0.98f}, {"mass", 0.08f}, {"cascade", 0.3f}, {"split", 10}, {"beat", 3}, {"pos", 0.35f}, {"posy", 0.3f}, {"symp", 0.5f}, {"sympset", 6}, {"sympdecay", 4}, {"sympbright", 1}, {"sympshimmer", 15}, {"level", 1.5f}}},
        {"Brushed cymbal", {{"strike", 0}, {"scrape", 1}, {"scrapespeed", 0.7f}, {"rough", 0.95f}, {"grit", 0.5f}, {"chatter", 0.3f}, {"atk", 0.08f}, {"rel", 0.4f}, {"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 3}, {"bmodes", 48}, {"bbright", 1}, {"cascade", 0.5f}, {"split", 15}, {"pos", 0.7f}, {"symp", 0.8f}, {"sympset", 6}, {"sympdecay", 2}, {"sympbright", 1}, {"sympshimmer", 25}, {"EQ_On", 1}, {"EQ_High", 6.0f}, {"level", -8.5f}}},
        {"Hi-hat", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 0.35f}, {"bmodes", 48}, {"bbright", 1}, {"hardness", 1}, {"mass", 0.05f}, {"split", 30}, {"beat", 12}, {"pos", 0.6f}, {"posy", 0.4f}, {"damper", 0.8f}, {"symp", 0.8f}, {"sympset", 7}, {"sympdecay", 0.3f}, {"sympbright", 1}, {"sympshimmer", 40}, {"level", -1.0f}}},
        {"Sizzle cymbal", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 5}, {"bmodes", 48}, {"bbright", 1}, {"hardness", 0.9f}, {"mass", 0.3f}, {"cascade", 0.6f}, {"split", 15}, {"beat", 4}, {"pos", 0.75f}, {"posy", 0.6f}, {"rattle", 0.5f}, {"rattleth", 0.03f}, {"symp", 0.8f}, {"sympset", 6}, {"sympdecay", 4}, {"sympbright", 1}, {"sympshimmer", 20}, {"level", -2.5f}}},
        {"Metal shimmer pad", {{"strike", 0}, {"bow", 1}, {"bowpress", 0.35f}, {"bowspeed", 0.5f}, {"atk", 1.2f}, {"rel", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"bmodes", 48}, {"bbright", 0.9f}, {"split", 12}, {"beat", 2}, {"drift", 0.5f}, {"symp", 0.8f}, {"sympset", 7}, {"sympdecay", 5}, {"sympbright", 0.9f}, {"sympshimmer", 30}, {"level", -7.5f}}},
        {"Tam-tam bloom", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 4}, {"cascade", 0.8f}, {"hardness", 0.8f}, {"mass", 0.8f}, {"symp", 0.5f}, {"sympset", 6}, {"sympdecay", 5}, {"sympbright", 0.8f}, {"sympshimmer", 15}, {"level", -2.0f}}},
        {"Thunder sheet", {{"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 3}, {"hardness", 0.9f}, {"pos", 0.06f}, {"posy", 0.08f}, {"cascade", 0.5f}, {"rattle", 0.3f}, {"rattleth", 0.1f}, {"bbright", 0.8f}, {"symp", 0.4f}, {"sympset", 7}, {"sympdecay", 3}, {"sympbright", 1}, {"sympshimmer", 25}, {"level", 9.5f}}},
        {"Scraped plate", {{"strike", 0}, {"scrape", 1}, {"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"chatter", 0.5f}, {"symp", 0.5f}, {"sympset", 7}, {"sympdecay", 2}, {"sympbright", 1}, {"sympshimmer", 20}, {"level", 2.0f}}},
        {"Harsh scrape", {{"strike", 0}, {"scrape", 1}, {"rough", 0.9f}, {"grit", 0.8f}, {"chatter", 1}, {"cascade", 0.6f}, {"rattle", 0.4f}, {"rattleth", 0.1f}, {"material", 1}, {"bfamily", 5}, {"bshape", 0}, {"excite", 1}, {"listen", 1}, {"level", -7.0f}}},
        {"Rain on glass", {{"Gate_Mode", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0}, {"material", 0.5f}, {"bdecay", 3}, {"hardness", 0.95f}, {"mass", 0.1f}, {"split", 2}, {"Pulse_Rate", 6}, {"Pulse_Chaos", 1}, {"Pulse_Link", 0}, {"Pulse_Velocity", 0.5f}, {"Pulse_Velocity_Jitter", 0.9f}, {"symp", 0.3f}, {"sympset", 4}, {"sympdecay", 3}, {"sympbright", 0.8f}, {"sympshimmer", 4}, {"level", -0.5f}}},
        {"Gamelan clock", {{"Gate_Mode", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 1}, {"bshape", 0.5f}, {"material", 1}, {"bdecay", 4}, {"hardness", 0.7f}, {"beat", 5}, {"damper", 0.4f}, {"Pulse_Sync", 1}, {"Pulse_Beats", 0.5f}, {"Pulse_Chance", 0.7f}, {"Pulse_Link", 0.5f}, {"Pulse_Length", 0.8f}, {"symp", 0.4f}, {"sympset", 1}, {"sympdecay", 4}, {"sympbright", 0.7f}, {"sympshimmer", 8}, {"level", 1.0f}}},
        {"Bowl circle", {{"Gate_Mode", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 2}, {"bshape", 0.25f}, {"material", 1}, {"bdecay", 8}, {"hardness", 0.4f}, {"mass", 0.6f}, {"split", 1.5f}, {"drift", 0.3f}, {"Pulse_Rate", 0.4f}, {"Pulse_Chaos", 0.3f}, {"Pulse_Link", 0}, {"Pulse_Phasing", 0.4f}, {"symp", 0.4f}, {"sympset", 1}, {"sympdecay", 8}, {"sympbright", 0.6f}, {"sympshimmer", 3}, {"level", -4.5f}}},
        {"Taraf rain", {{"Gate_Mode", 3}, {"excite", 0}, {"listen", 0}, {"adecay", 5}, {"hardness", 0.85f}, {"pos", 0.08f}, {"radiator", 2}, {"radmix", 0.5f}, {"material", 1}, {"abright", 0.9f}, {"jawari", 0.8f}, {"symp", 0.8f}, {"sympset", 0}, {"sympdecay", 7}, {"sympbright", 0.7f}, {"sympshimmer", 3}, {"Pulse_Rate", 3}, {"Pulse_Chaos", 0.8f}, {"Pulse_Link", 0}, {"Pulse_Chance", 0.8f}, {"Pulse_Velocity_Jitter", 0.7f}, {"level", -0.5f}}},
        {"Geiger hats", {{"Gate_Mode", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 5}, {"bshape", 0}, {"material", 1}, {"bdecay", 0.35f}, {"bmodes", 48}, {"bbright", 1}, {"hardness", 1}, {"mass", 0.05f}, {"split", 30}, {"beat", 12}, {"pos", 0.6f}, {"posy", 0.4f}, {"damper", 0.8f}, {"symp", 0.8f}, {"sympset", 7}, {"sympdecay", 0.3f}, {"sympbright", 1}, {"sympshimmer", 40}, {"Pulse_Rate", 9}, {"Pulse_Chaos", 1}, {"Pulse_Cluster", 0.6f}, {"Pulse_Link", 0}, {"Pulse_Length", 0.1f}, {"Pulse_Velocity_Jitter", 0.8f}, {"level", -1.0f}}},
        {"Drum circle", {{"Gate_Mode", 3}, {"excite", 1}, {"listen", 1}, {"bfamily", 4}, {"bshape", 1}, {"material", 0.2f}, {"bdecay", 0.8f}, {"hardness", 0.3f}, {"mass", 0.6f}, {"pos", 0.7f}, {"radiator", 4}, {"radmix", 0.5f}, {"Pulse_Sync", 1}, {"Pulse_Beats", 0.25f}, {"Pulse_Chance", 0.6f}, {"Pulse_Cluster", 0.3f}, {"Pulse_Link", 0.3f}, {"Pulse_Velocity_Jitter", 0.6f}, {"level", 15.5f}}},
    };
    return f;
}
// "Marimba", or "Marimba > Kalimba 0.40" between two models
std::string shapeLabel(float family, float shape) {
    const auto p = physical::place(family, shape);
    std::string s = physical::modelName(p.a);
    if(p.f > 0.04) s += " > " + std::string(physical::modelName(p.b)) + " " + ofToString(p.f, 2);
    return s;
}
std::vector<std::string> familyNames() {
    std::vector<std::string> out;
    for(int f = 0; f < physical::NFAMILIES; ++f) out.push_back(physical::familyName(f));
    return out;
}
}

scPhysicalBox::scPhysicalBox(std::vector<serverManager*> s) : scNode("physicalBox"), servers(std::move(s)) {
    description = "Physical modelling: strike, bow, blow, scrape or feed audio into a waveguide (string to tube) "
                  "and a modal body (bars, glasses, bowls, bells, gongs, handpan, steel pan, drums, plates, blocks), coupled, with rattle, tension and "
                  "cascade, heard by two pickups. Every voice is its own instrument, so LFOs and envelopes "
                  "can move each one separately. Gate is a level (the velocity); In feeds the Ext exciter. "
                  "Gate Mode Pulse plays the voices by itself, from a clock to a Geiger counter, together or each its own. "
                  "Show opens the editor; right-click a control there to publish, modulate or add it to the timeline.";
}
scPhysicalBox::~scPhysicalBox() {
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scPhysicalBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
}

void scPhysicalBox::addSound(const std::string& name, const std::string& sc, float def, float lo, float hi,
                             const std::string& group, const char* fmt, bool log, bool discrete) {
    auto c = std::make_unique<SoundControl>();
    c->p.set(name, def, lo, hi);
    c->sc = sc; c->group = group; c->fmt = fmt; c->log = log; c->discrete = discrete;
    c->voiceInput = sc != "width" && sc != "spread";
    reg(c->p, [this]() { sendAll(); });
    controls.back().discrete = discrete;
    soundControls.push_back(std::move(c));
}

void scPhysicalBox::setup() {
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(voices.set("Voices", 4, 1, 16));
    addParameter(channels.set("Channels", 2, 1, 16));
    // Gate (levels: a gate rising to 0.2 plays at 0.2) and Pitch: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {48.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 0.6f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    addInput("In");
    addOutput("Out");
    // Auto: a drone until Gate is connected, timelined or gets a note; Pulse: the automatic gates
    gateMode.set("Gate Mode", 2, 0, 3);
    reg(gateMode, [this](){ sendAll(); });
    dropdowns.set(gateMode.getEscapedName(), {"Auto", "Gated", "Drone", "Pulse"});
    {
        scPulseGate::Host ph;
        ph.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
        ph.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
        ph.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
        ph.sendAll = [this](){ sendAll(); };
        ph.bpm = [this](){ return bpm; };
        ph.voices = [this](){ return voices.get(); };
        ph.section = [this](const char* t, float w, const char* help){ sectionHeader(t, w, nullptr, pal::pickup, help); };
        ph.slider = [this](ofParameter<float>& p, const char* l, float w, const char* f, bool log){ slider(p, l, w, f, log); };
        ph.toggle = [this](ofParameter<bool>& p, const char* l){ toggle(p, l); };
        ph.mark = [this](const std::string& k){ mark(k); };
        ph.menu = [this](const std::string& k){ menu(k); };
        ph.labelW = &labelW;
        pulse.setup(ph);
    }
    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });
    for(auto* p : {&gate, &pitch}) {
        listeners.push(p->newListener([this, p](std::vector<float>& v) {
            // the timeline's frame set of a value it already sent timetagged: nothing to do
            if(scScheduling::isBackendSendSuppressed()) return;
            if(p == &gate) for(float value : v) if(value > 0) gateTouched = true;
            for(auto& [s, st] : states) sendNotes(s, st);
        }));
    }
    listeners.push(voices.newListener([this](int&) { structureDirty = true; }));
    listeners.push(channels.newListener([this](int&) { structureDirty = true; }));
    listeners.push(master.newListener([this](float&) { if(!restoring) sendAll(); }));
    listeners.push(resetLFOs.newListener([this]() { mod.resetLfos(); pulse.restart = true; }));

    // ── Sound: the PhysicalVoice inputs (defaults: PhysicalModes.h) ─────────
    auto d = [](const char* sc) {
        static const std::map<std::string, float> defs = {
#define PHYSICAL_DEFAULT(name, def) {#name, float(def)},
            PHYSICAL_PARAMS(PHYSICAL_DEFAULT)
#undef PHYSICAL_DEFAULT
        };
        return defs.at(sc);
    };
    addSound("Strike", "strike", d("strike"), 0, 1, "Strike");
    addSound("Hardness", "hardness", d("hardness"), 0, 1, "Strike");
    addSound("Mass", "mass", d("mass"), 0, 1, "Strike");
    addSound("Bounce", "bounce", d("bounce"), 0, 0.9f, "Strike");
    addSound("Bow", "bow", d("bow"), 0, 1, "Bow");
    addSound("Bow Pressure", "bowpress", d("bowpress"), 0, 1.2f, "Bow");
    addSound("Bow Speed", "bowspeed", d("bowspeed"), 0, 1, "Bow");
    addSound("Rosin", "rosin", d("rosin"), 0, 1, "Bow");
    addSound("Blow", "blow", d("blow"), 0, 1, "Blow");
    addSound("Breath", "breath", d("breath"), 0, 1.2f, "Blow");
    addSound("Jet Reed", "blowtype", d("blowtype"), 0, 1, "Blow");
    addSound("Jet Ratio", "jetratio", d("jetratio"), 0.1f, 0.8f, "Blow");
    addSound("Turbulence", "turb", d("turb"), 0, 1, "Blow");
    addSound("Air", "air", d("air"), 0, 1, "Blow");
    addSound("Scrape", "scrape", d("scrape"), 0, 1, "Scrape");
    addSound("Scrape Speed", "scrapespeed", d("scrapespeed"), 0, 1, "Scrape");
    addSound("Roughness", "rough", d("rough"), 0, 1, "Scrape");
    addSound("Grit", "grit", d("grit"), 0, 1, "Scrape");
    addSound("Chatter", "chatter", d("chatter"), 0, 1, "Scrape");
    addSound("Position", "pos", d("pos"), 0, 1, "Contact");
    addSound("Position Y", "posy", d("posy"), 0, 1, "Contact");
    addSound("Ext In", "ext", d("ext"), 0, 1, "Contact");
    addSound("Attack", "atk", d("atk"), 0.001f, 4, "Contact", "%.3f s", true);
    addSound("Release", "rel", d("rel"), 0.01f, 8, "Contact", "%.2f s", true);
    addSound("Hold", "hold", d("hold"), 0, 1, "Contact", "%.0f", false, true);
    addSound("Damper", "damper", d("damper"), 0, 1, "Contact");
    addSound("A Tune", "atune", d("atune"), -24, 24, "A", "%+.2f st");
    addSound("A Decay", "adecay", d("adecay"), 0.02f, 20, "A", "%.2f s", true);
    addSound("A Brightness", "abright", d("abright"), 0, 1, "A");
    addSound("Bore", "abore", d("abore"), 0, 1, "A");
    addSound("Stiffness", "astiff", d("astiff"), 0, 1, "A");
    addSound("Jawari", "jawari", d("jawari"), 0, 1, "A");
    addSound("B Tune", "btune", d("btune"), -24, 24, "B", "%+.2f st");
    addSound("B Decay", "bdecay", d("bdecay"), 0.02f, 20, "B", "%.2f s", true);
    addSound("Family", "bfamily", d("bfamily"), 0, physical::NFAMILIES - 1, "B", "%.0f", false, true);
    dropdowns.set(soundParam("bfamily").getEscapedName(), familyNames());
    addSound("Shape", "bshape", d("bshape"), 0, 1, "B");
    addSound("Stretch", "bstretch", d("bstretch"), -1, 1, "B", "%+.2f");
    addSound("Modes", "bmodes", d("bmodes"), 1, 48, "B", "%.0f");
    addSound("B Brightness", "bbright", d("bbright"), 0, 1, "B");
    addSound("Split", "split", d("split"), 0, 30, "B", "%.1f cents");
    addSound("Beat", "beat", d("beat"), 0, 12, "B", "%.2f Hz");
    addSound("Material", "material", d("material"), 0, 1, "Couple");
    addSound("Excite", "excite", d("excite"), 0, 1, "Couple");
    addSound("Listen", "listen", d("listen"), 0, 1, "Couple");
    addSound("Couple", "couple", d("couple"), 0, 1, "Couple");
    addSound("Mutual", "mutual", d("mutual"), 0, 1, "Couple");
    addSound("Pickup 1", "pick1", d("pick1"), 0, 1, "Couple");
    addSound("Pickup 2", "pick2", d("pick2"), 0, 1, "Couple");
    addSound("Pickup 1 Y", "pick1y", d("pick1y"), 0, 1, "Couple");
    addSound("Pickup 2 Y", "pick2y", d("pick2y"), 0, 1, "Couple");
    addSound("Rattle", "rattle", d("rattle"), 0, 1, "Break");
    addSound("Rattle Threshold", "rattleth", d("rattleth"), 0.01f, 1, "Break", "%.3f", true);
    addSound("Tension", "tension", d("tension"), -1, 1, "Break", "%+.2f");
    addSound("Cascade", "cascade", d("cascade"), 0, 1, "Break");
    addSound("Drift", "drift", d("drift"), 0, 1, "Life");
    addSound("Drift Rate", "driftrate", d("driftrate"), 0, 1, "Life");
    addSound("Freeze", "freeze", d("freeze"), 0, 1, "Life");
    // sympathetic strings: a set tuned from the note, ringing on after it
    addSound("Sympathetic", "symp", d("symp"), 0, 1, "Symp");
    addSound("Strings", "sympset", d("sympset"), 0, physical::NSYMSETS - 1, "Symp", "%.0f", false, true);
    {
        std::vector<std::string> names;
        for(int k = 0; k < physical::NSYMSETS; ++k) names.push_back(physical::sympSetName(k));
        dropdowns.set(soundParam("sympset").getEscapedName(), names);
    }
    addSound("Symp Decay", "sympdecay", d("sympdecay"), 0.05f, 30, "Symp", "%.2f s", true);
    addSound("Symp Brightness", "sympbright", d("sympbright"), 0, 1, "Symp");
    addSound("Shimmer", "sympshimmer", d("sympshimmer"), 0, 50, "Symp", "%.1f cents");
    addSound("Resonator", "radiator", d("radiator"), 0, physical::NRADIATORS - 1, "Res", "%.0f", false, true);
    {
        std::vector<std::string> names;
        for(int r = 0; r < physical::NRADIATORS; ++r) names.push_back(physical::radiatorName(r));
        dropdowns.set(soundParam("radiator").getEscapedName(), names);
    }
    addSound("Res Mix", "radmix", d("radmix"), 0, 1, "Res");
    addSound("Res Tune", "radtune", d("radtune"), -24, 24, "Res", "%+.2f st");
    addSound("Tremolo", "tremrate", d("tremrate"), 0, 12, "Res", "%.2f Hz");
    addSound("Trem Depth", "tremdepth", d("tremdepth"), 0, 1, "Res");
    addSound("Level", "level", 0, -40, 24, "Output", "%+.1f dB");
    addSound("Width", "width", 0.5f, 0, 1, "Pan");
    addSound("Spread", "spread", 0.6f, 0, 1, "Pan");
    // Panning: Spread (Spread, Center), each voice on a speaker, or each where it is put
    {
        auto toSpread = [this]() { sendAll(); };
        placement.set("Placement", 0, 0, 2);                 reg(placement, toSpread);
        spreadCenter.set("Spread Center", 0.0f, -1.0f, 1.0f); reg(spreadCenter, toSpread);
        panP.set("Pan", {0.5f}, {0.0f}, {1.0f});             reg(panP, toSpread);
        speakerLayout.set("Speaker Layout", 0, 0, 1);        reg(speakerLayout, toSpread);
        dropdowns.set(placement.getEscapedName(), {"Spread", "Channels", "Manual"});
        dropdowns.set(speakerLayout.getEscapedName(), {"Ring", "Line"});
    }

    // ── Effects: MixerPro's inserts, then MiVerb ─────────────────────────────
    auto toStages = [this]() { for(auto& [sv, st] : states) if(st.created) updateStages(sv, st); };
    satOn.set("Saturation On", false);                      reg(satOn, toStages);
    satDrive.set("Saturation Drive", 6.0f, 0.0f, 36.0f);    reg(satDrive, [this](){ sendStageAll(StSat); });
    satBias.set("Saturation Bias", 0.0f, 0.0f, 1.0f);       reg(satBias, [this](){ sendStageAll(StSat); });
    satTone.set("Saturation Tone", 20000.0f, 500.0f, 20000.0f); reg(satTone, [this](){ sendStageAll(StSat); });
    satOutput.set("Saturation Output", 0.0f, -24.0f, 12.0f); reg(satOutput, [this](){ sendStageAll(StSat); });
    satMix.set("Saturation Mix", 1.0f, 0.0f, 1.0f);         reg(satMix, [this](){ sendStageAll(StSat); });
    eqOn.set("EQ On", false);                                reg(eqOn, toStages);
    {
        const char* names[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
        const float freqs[5] = {80, 250, 1000, 4000, 12000};
        for(int b = 0; b < 5; b++){
            const bool shelf = scEQEditor::isShelf(b);
            eqGain[b].set(std::string("EQ ") + names[b], 0.0f, -24.0f, 24.0f);
            eqFreq[b].set(std::string("EQ ") + names[b] + " Freq", freqs[b], 20.0f, 20000.0f);
            eqShape[b].set(std::string("EQ ") + names[b] + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
            for(auto* p : {&eqGain[b], &eqFreq[b], &eqShape[b]}) reg(*p, [this]() { sendStageAll(StEq); });
        }
    }
    echoOn.set("Echo On", false);             reg(echoOn, toStages);
    echoDelay.set("Echo Delay", 0.375f, 0.005f, 4.0f);
    echoBeats.set("Echo Sync", true);
    echoBeatVal.set("Echo Beats", 0.75f, 0.0625f, 4.0f);
    echoFeed.set("Echo Feed", 0.45f, 0.0f, 0.98f);
    echoCutoff.set("Echo Cutoff", 84.0f, 12.0f, 130.0f);
    echoResonance.set("Echo Resonance", 0.2f, 0.0f, 0.9f);
    echoFilter.set("Echo Filter", 0, 0, 3);
    echoPingPong.set("Echo Ping-pong", 0.5f, 0.0f, 1.0f);
    echoMix.set("Echo Mix", 0.3f, 0.0f, 1.0f);
    for(auto* p : {&echoDelay, &echoBeatVal, &echoFeed, &echoCutoff, &echoResonance, &echoPingPong, &echoMix})
        reg(*p, [this]() { sendStageAll(StEcho); });
    reg(echoBeats, [this]() { sendStageAll(StEcho); });
    reg(echoFilter, [this]() { sendStageAll(StEcho); });
    dropdowns.set(echoFilter.getEscapedName(), {"LowPass", "HighPass", "BandPass", "PeakEQ"});
    rvbOn.set("Reverb On", false);            reg(rvbOn, toStages);
    rvbMix.set("Reverb Mix", 0.3f, 0.0f, 1.0f);
    rvbDecay.set("Reverb Decay", 3.0f, 0.05f, 30.0f);
    rvbSize.set("Reverb Size", 30.0f, 0.0f, 60.0f);
    rvbPredelay.set("Reverb Predelay", 20.0f, 0.0f, 500.0f);
    rvbLate.set("Reverb Late", 0.8f, 0.0f, 1.0f);
    rvbSpread.set("Reverb Spread", 0.15f, -1.0f, 1.0f);
    rvbLowpass.set("Reverb Lowpass", 10000.0f, 20.0f, 20000.0f);
    rvbHiDamp.set("Reverb Hi Damp", 6.0f, 0.0f, 60.0f);
    rvbLoDamp.set("Reverb Lo Damp", 0.0f, 0.0f, 60.0f);
    rvbModRate.set("Reverb Mod Rate", 0.2f, 0.001f, 20.0f);
    rvbSpin.set("Reverb Spin", 1.0f, 0.0f, 20.0f);
    rvbWander.set("Reverb Wander", 0.25f, 0.0f, 20.0f);
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass,
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander})
        reg(*p, [this]() { sendStageAll(StRvb); });
    // Space: MiVerb, on by default (the prototype's room)
    spaceOn.set("Space On", true);                   reg(spaceOn, toStages);
    spaceMix.set("Space Mix", 0.25f, 0.0f, 1.0f);    reg(spaceMix, [this](){ sendStageAll(StSpace); });
    spaceTime.set("Space Time", 0.8f, 0.0f, 1.2f);   reg(spaceTime, [this](){ sendStageAll(StSpace); });
    spaceDamp.set("Space Damp", 0.5f, 0.0f, 1.0f);   reg(spaceDamp, [this](){ sendStageAll(StSpace); });
    spaceDiff.set("Space Diffusion", 0.7f, 0.0f, 1.0f); reg(spaceDiff, [this](){ sendStageAll(StSpace); });
    spaceLowCut.set("Space Low Cut", 0.05f, 0.0f, 1.0f); reg(spaceLowCut, [this](){ sendStageAll(StSpace); });
    spaceFreeze.set("Space Freeze", false);          reg(spaceFreeze, [this](){ sendStageAll(StSpace); });

    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) reg(*p, [](){});
    scModulation::Host h;
    h.node = this;
    h.voiceParamsChanged = [this](){ if(!restoring) sendAll(); };
    h.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
    h.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
    h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
    h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ reg(p, f); };
    h.control = [this](const std::string& k) -> ofAbstractParameter* { for(auto& c : controls) if(c.key == k && !scSnapshots::isOwnKey(k)) return c.param; return nullptr; };
    h.publishMenu = [this](const std::string& k){ menu(k); };
    h.publishedUnderline = [this](const std::string& k){ mark(k); };
    h.unpublish = [this](const std::string& k){ unpublish(k); };
    h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
    h.voices = [this](){ return voices.get(); };
    mod.setup(h, {{"Pitch", 24, "st"}, {"Material", 1, ""}, {"Excite", 1, ""}, {"Position", 1, ""},
        {"Hardness", 1, ""}, {"Bow Pressure", 1, ""}, {"Bow Speed", 1, ""}, {"Breath", 1, ""},
        {"Scrape Speed", 1, ""}, {"Brightness", 1, ""}, {"Decay", 4, "oct"}, {"Shape", 1, ""},
        {"Couple", 1, ""}, {"Cascade", 1, ""}, {"Amp", 1, ""}, {"Pan", 1, ""}}, true, true, true);
    mod.lfoCount = mod.envCount = mod.extCount = 0;
    {
        static const std::map<std::string, int> targets = {{"material", Material}, {"excite", Excite}, {"pos", Position},
            {"hardness", Hardness}, {"bowpress", BowPressure}, {"bowspeed", BowSpeed}, {"breath", Breath},
            {"scrapespeed", ScrapeSpeed}, {"abright", Brightness}, {"bbright", Brightness}, {"adecay", Decay},
            {"bdecay", Decay}, {"bshape", Shape}, {"couple", Couple}, {"cascade", Cascade}};
        for(const auto& c : soundControls) {
            auto it = targets.find(c->sc);
            if(it != targets.end()) mod.targetOfKey[c->p.getEscapedName()] = it->second;
        }
        mod.targetOfKey[panP.getEscapedName()] = Pan;
    }
    mod.changed = [this](){ if(!restoring) sendAll(); };
    for(const auto& c : soundControls) if(c->voiceInput) mod.addVoiceParam(c->p);
    initial = capture();
    scSnapshots::Host sh;
    sh.folder = "PhysicalBox"; sh.title = "PhysicalBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a, b, t, sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    snaps.setup(sh);
    requireSynthdefs();
}

// ── Server side ──────────────────────────────────────────────────────────────
void scPhysicalBox::sendNotes(ofxSCServer* server, State& st) {
    if(!st.gate) return;
    auto now = scheduled.inEffectNow();   // not a scheduled send: the notes as they stand now
    const int n = st.builtVoices;
    // Pulse: ModGate takes the generator's gates, Gate (Follow Gate) holds its voices
    const int pbus = pulse.bus(server);
    std::vector<float> g(n), ones(n, 1.0f);
    for(int i = 0; i < n; ++i) g[i] = pbus >= 0 ? 0.0f : first(gate.get(), i, 0);
    st.gate->set("gate", g);
    st.gate->set("velocity", ones);       // the gate's level is the velocity
    st.gate->set("out", st.gates->index);
    st.gate->set("vel", mod.sourceBus(server, scModulation::SRC_VEL));
    st.gate->set("rnd", mod.sourceBus(server, scModulation::SRC_RND));
    st.droneSent = droning() ? 1 : 0;
    st.gate->set("drone", float(st.droneSent));
    const int gbus = gateIn.bus(server);
    st.gate->set("gate_sel", gbus >= 0 || pbus >= 0 ? 1.0f : 0.0f);
    if(pbus >= 0) st.gate->mapan("gate_ar", pbus, n);
    else if(gbus >= 0) st.gate->mapan("gate_ar", gbus, n);
    pulse.enable(server, gate.get(), n, gbus);
    const int bus = pitchIn.bus(server);
    for(int i = 0; i < n; ++i) if(auto* v = st.voices[i]) {
        v->set("pitch", first(pitch.get(), i, 48));
        v->set("pitch_sel", bus >= 0 ? 1.0f : 0.0f);
        if(bus >= 0) v->mapan("pitch_ar", bus + i, 1);
    }
}

void scPhysicalBox::sendSound(ofxSCServer* server, State& st) {
    if(!st.created) return;
    static const char* names[NUM_TARGETS - 1] = {"mpitch", "mmat", "mexc", "mpos", "mhard", "mpress", "mspeed",
        "mbreath", "mscrape", "mbright", "mdecay", "mshape", "mcouple", "mcascade", "mamp"};
    bool vca = false;
    for(int e = 0; e < mod.envCount; ++e) if(std::abs(mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + Amp]) > 0.0001f) vca = true;
    const float comp = 1.0f / std::sqrt(std::max(1.0f, float(st.builtVoices) / st.builtChannels));
    const int inBus = st.in >= 0 ? st.in : st.silent->index;
    for(int i = 0; i < st.builtVoices; ++i) if(auto* v = st.voices[i]) {
        auto set = [&](const std::string& name, float value) {
            auto& sent = st.sent[i]; auto it = sent.find(name);
            if(it != sent.end() && it->second == value) return;
            sent[name] = value; v->set(name, value);
        };
        for(const auto& c : soundControls) if(c->voiceInput) set(c->sc, c->sc == "bmodes" ? std::round(mod.voiceValue(c->p, i)) : mod.voiceValue(c->p, i));
        set("gatebus", st.gates->index + i);
        set("out", st.audio->index + 2 * i);
        set("in", inBus + (st.in >= 0 ? i % std::max(1, st.inChannels) : 0));
        set("voice", i);
        set("master", master.get());
        set("normalization", 1.0f / (st.builtVoices * comp));
        set("vca", vca ? 1.0f : 0.0f);
        set("vis", st.vis->index);
        for(int t = 0; t < NUM_TARGETS - 1; ++t) set(names[t], mod.targetBus(server, t) + i);
    }
    if(st.spread) {
        st.spread->set("in", st.audio->index);
        st.spread->set("voices", st.builtVoices);
        std::vector<float> pans = panPositions();
        pans.resize(16, 0.5f);
        st.spread->set("pan", pans);
        st.spread->set("line", speakerLayout.get() == 1 ? 1.0f : 0.0f);
        st.spread->set("width", soundParam("width").get());
        st.spread->set("mpan", mod.targetBus(server, Pan));
        st.spread->set("vis", st.vis->index + VIS_PAN);
    }
    updateStages(server, st);
}
void scPhysicalBox::sendAll() { for(auto& [s, st] : states) { sendNotes(s, st); sendSound(s, st); } pulse.sendAll(); }

// Pulse: a new generator at the head of the gate group and a fresh ModGate
// after it (a level held from Oceanode gate values played before would floor
// every pulse's velocity)
void scPhysicalBox::startPulse(ofxSCServer* server, State& st) {
    if(!st.created || !st.groups[SlGate] || !liveBus(server, st.vis, false)) return;
    if(!pulse.start(server, st.groups[SlGate]->nodeID, st.vis->index + VIS_PULSE, st.builtVoices)) return;
    if(st.gate) {
        st.gate->free(); delete st.gate;
        st.gate = new ofxSCSynth("ModGate" + ofToString(st.builtVoices), server);
        sendNotes(server, st);
        st.gate->createAndRun(1, st.groups[SlGate]->nodeID, true);
    }
}

void scPhysicalBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server]; release(server, st); st.root = new ofxSCGroup(server);
}
void scPhysicalBox::createSynth(ofxSCServer* server) {
    auto it = states.find(server); if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    if(placeBefore >= 0) st.root->create(2, placeBefore); else st.root->create(0, 1);
    for(auto*& g : st.groups) { g = new ofxSCGroup(server); g->create(1, st.root->nodeID); }
    st.gates = new ofxSCBus(RATE_AUDIO, 16, server);
    st.audio = new ofxSCBus(RATE_AUDIO, 32, server);
    st.silent = new ofxSCBus(RATE_AUDIO, 1, server);
    st.vis = new ofxSCBus(RATE_CONTROL, VIS_SIZE, server);
    if(!liveBus(server, st.gates, true) || !liveBus(server, st.audio, true) || !liveBus(server, st.silent, true) || !liveBus(server, st.vis, false)) {
        release(server, st); return;
    }
    st.builtVoices = voices.get(); st.builtChannels = channels.get(); st.created = true;
    mod.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gates->index);
    if(pulsing()) startPulse(server, st);
    st.gate = new ofxSCSynth("ModGate" + ofToString(st.builtVoices), server);
    for(int i = 0; i < st.builtVoices; ++i) st.voices[i] = new ofxSCSynth("PhysicalBoxVoice", server);
    st.spread = new ofxSCSynth("PhysicalBoxSpread" + ofToString(st.builtChannels), server);
    sendNotes(server, st); sendSound(server, st);
    st.gate->createAndRun(1, st.groups[SlGate]->nodeID, true);
    for(int i = 0; i < st.builtVoices; ++i) st.voices[i]->createAndRun(1, st.groups[SlVoice]->nodeID, true);
    st.spread->createAndRun(1, st.groups[SlSpread]->nodeID, true);
    routeChain(st);
    if(!getActive()) st.root->run(false);
}
void scPhysicalBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server, false);
    for(auto& f : st.fx) { delete f.synth; f.synth = nullptr; dropBus(server, f.in, true); f.running = false; f.offAt = -1; }
    delete st.gate; st.gate = nullptr; delete st.spread; st.spread = nullptr;
    pulse.release(server);
    for(auto*& v : st.voices) { delete v; v = nullptr; }
    for(auto& cache : st.sent) cache.clear();
    for(auto*& g : st.groups) { delete g; g = nullptr; }
    delete st.root; st.root = nullptr; st.created = false; st.droneSent = -1;
    dropBus(server, st.gates, true); dropBus(server, st.audio, true); dropBus(server, st.silent, true); dropBus(server, st.vis, false);
}
void scPhysicalBox::free(ofxSCServer* s) { auto it = states.find(s); if(it != states.end()) { release(s, it->second); it->second.out = -1; } }
// A new voice / speaker count: everything again, but in the old root's place
// (the graph is not recomputed for the same topology: a root at the head would
// run before the nodes feeding Gate / Pitch / In and read silence).
void scPhysicalBox::rebuild() {
    structureDirty = false; mod.voicesChanged();
    for(auto& [s, st] : states) if(st.created) {
        const bool wasLatency = s->getBLatency(); s->setBLatency(true);
        ofxSCGroup* old = st.root; st.root = nullptr;
        release(s, st);
        st.root = new ofxSCGroup(s);
        placeBefore = old->nodeID; createSynth(s); placeBefore = -1;
        old->free(); delete old;
        s->setBLatency(wasLatency);
    }
    refreshOutputs();
}
void scPhysicalBox::moveSynthBefore(ofxSCServer* s, int id) { auto it = states.find(s); if(it != states.end() && it->second.created) it->second.root->moveBefore(id); }
int scPhysicalBox::getLastSynthID(ofxSCServer* s) { auto it = states.find(s); return it != states.end() && it->second.created ? it->second.root->nodeID : -1; }
void scPhysicalBox::setOutputBus(ofxSCServer* s, int, int bus) { auto& st = states[s]; st.out = bus; if(st.created) routeChain(st); }
int scPhysicalBox::getOutputBusIndex(ofxSCServer* s, int) { auto it = states.find(s); return it == states.end() ? -1 : it->second.out; }
// a Gate / Pitch cable (told apart by its source's bus), else the audio input
void scPhysicalBox::setInputBus(ofxSCServer* s, scNode* node, int bus) {
    auto& st = states[s];
    if(gateIn.take(s, node, bus) | pitchIn.take(s, node, bus)) { sendNotes(s, st); return; }
    st.in = bus;
    st.inChannels = node ? std::clamp(node->getOutputNumChannels(0), 1, 16) : 1;
    for(auto& cache : st.sent) cache.erase("in");
    sendSound(s, st);
}
void scPhysicalBox::resetInputBusses(ofxSCServer* s, int) {
    gateIn.reset(s); pitchIn.reset(s);
    auto& st = states[s];
    st.in = -1; st.inChannels = 1;     // the voices read their own silent bus
    for(auto& cache : st.sent) cache.erase("in");
    sendNotes(s, st); sendSound(s, st);
}
void scPhysicalBox::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(true); }
void scPhysicalBox::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }
void scPhysicalBox::setBpm(float v) { bpm = std::max(1.0f, v); mod.setBpm(bpm); sendAll(); }
void scPhysicalBox::resendParametersForNRT() { for(auto& [s, st] : states) for(auto& cache : st.sent) cache.clear(); mod.resend(); sendAll(); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scPhysicalBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate = gateIn.handle.get(); out.pitch = pitchIn.handle.get(); out.voices = voices.get();
    return true;
}
#endif

// ── Effects ──────────────────────────────────────────────────────────────────
void scPhysicalBox::updateStages(ofxSCServer* server, State& st) {
    if(!st.created) return;
    for(int s = 0; s < NumStages; s++){
        auto& f = st.fx[s];
        if(stageOn(s)){
            if(!liveBus(server, f.in, true)){
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, channels.get(), server);
                if(!liveBus(server, f.in, true)){ f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth){
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", std::max(0, st.out));
                sendStage(s, f.synth, server);
                f.synth->createAndRun(1, st.groups[SlFx + s]->nodeID, true);
            }else{
                sendStage(s, f.synth, server);
                if(!f.running) f.synth->run(true);
            }
            f.running = true;
            f.offAt = -1.0f;
        }else if(f.synth && f.running && f.offAt < 0.0f){
            if(s != StEq) f.synth->set("on", 0.0f);
            f.offAt = ofGetElapsedTimef() + stageTail(s);
        }
    }
    routeChain(st);
}
// the spreader -> the running effects -> out
void scPhysicalBox::routeChain(State& st) {
    int next = std::max(0, st.out);
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}
bool scPhysicalBox::stageOn(int s) const {
    switch(s){
        case StSat: return satOn.get();
        case StEq: return eqOn.get();
        case StEcho: return echoOn.get();
        case StRvb: return rvbOn.get();
        default: return spaceOn.get();
    }
}
std::string scPhysicalBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "mixerProEcho", "mixerProRvb", "PhysicalBoxSpace"};
    return names[s] + ofToString(channels.get());
}
float scPhysicalBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, bpm), 0.005f, 4.0f);
}
float scPhysicalBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    if(s == StSpace) return spaceFreeze.get() ? 0.1f : 12.0f;
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}
void scPhysicalBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch c(x, server);
    const float on = stageOn(s) ? 1.0f : 0.0f;
    if(s == StSat){
        c.set("drive", satDrive.get()); c.set("bias", satBias.get()); c.set("tone", satTone.get());
        c.set("autogain", 1.0f); c.set("output", satOutput.get()); c.set("mix", satMix.get()); c.set("on", on);
    }else if(s == StEq){
        // mixerProEQ: gains dB, freqs Hz, shelves bNslope (= 1/S), peaks bNrq (= 1/Q)
        for(int b = 0; b < 5; b++){
            const std::string k = "b" + ofToString(b + 1);
            c.set(k + "gain", eqGain[b].get());
            c.set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) c.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
            else c.set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    }else if(s == StEcho){
        c.set("delay", echoDelaySec());
        c.set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        c.set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
        c.set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
        c.set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
        c.set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
        c.set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
        c.set("on", on);
    }else if(s == StRvb){
        c.set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
        c.set("decay", std::max(0.05f, rvbDecay.get()));
        c.set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
        c.set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
        c.set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
        c.set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
        c.set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
        c.set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
        c.set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
        c.set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
        c.set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
        c.set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
        c.set("on", on);
    }else{
        c.set("mix", spaceMix.get()); c.set("time", spaceTime.get()); c.set("damp", spaceDamp.get());
        c.set("diff", spaceDiff.get()); c.set("hp", spaceLowCut.get()); c.set("freeze", spaceFreeze.get() ? 1.0f : 0.0f);
        c.set("on", on);
    }
    c.send();
}
void scPhysicalBox::sendStageAll(int stage) {
    for(auto& [server, st] : states) if(st.created && st.fx[stage].synth) sendStage(stage, st.fx[stage].synth, server);
}

// ── Publishing, snapshots, presets ───────────────────────────────────────────
bool scPhysicalBox::connected(const std::string& k) const {
    auto it = published.find(k); return it != published.end() && (it->second->hasInConnection() || it->second->hasOutConnections());
}
void scPhysicalBox::publish(const std::string& k) {
    if(published.count(k) || getParameterGroup().contains(k)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; }); if(it == controls.end()) return;
    if(!separator){ addSeparator("Published", ofColor(200)); separator = true; }
    published[k] = it->add(); dropdowns.apply(k, published[k]); publishedKeys.push_back(k); parameterGroupChanged.notify(this);
}
void scPhysicalBox::unpublish(const std::string& k) {
    auto it = published.find(k); if(it == published.end() || connected(k)) return;
    removeParameter(it->second->getName()); published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), k), publishedKeys.end());
    if(published.empty() && separator){ removeSeparator("Published"); separator = false; }
    parameterGroupChanged.notify(this);
}
void scPhysicalBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k) && !timeline.isHidden(k)) {
        auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, IM_COL32(106, 212, 196, 255), 2);
    }
}
void scPhysicalBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##phys" + k).c_str())) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; });
    if(it != controls.end()) scTyped::jsonField(k, it->save, it->load);
    if(timeline.isHidden(k)){ if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k); }
    else if(published.count(k)){
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    }else if(ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k); mod.modulateMenu(k); ImGui::EndPopup();
}
ofJson scPhysicalBox::capture() const {
    ofJson j = ofJson::object();
    for(const auto& c : controls) if(!scSnapshots::isOwnKey(c.key) && c.key != speakerLayout.getEscapedName()) j[c.key] = c.save();
    mod.patchBases(j); mod.save(j, "physicalMod"); return j;
}
void scPhysicalBox::apply(const ofJson& a, const ofJson& b, float t, bool sw) {
    restoring = true;
    // a sound saved before the body models had Shape 0..5 (harmonic, bar, glass,
    // bell, plate, membrane) and no Family: the same body in the new scheme
    const std::string famKey = soundParam("bfamily").getEscapedName(), shapeKey = soundParam("bshape").getEscapedName();
    auto value = [&](const ofJson& j, const std::string& key) -> ofJson {
        if((key == famKey || key == shapeKey) && !j.contains(famKey) && j.contains(shapeKey) && j[shapeKey].is_number()) {
            static const float fam[6] = {0, 1, 2, 2, 5, 4}, shp[6] = {0, 0, 0, 0.75f, 0, 1};
            const int old = std::clamp(int(std::lround(j[shapeKey].get<float>())), 0, 5);
            return key == famKey ? fam[old] : shp[old];
        }
        return j.contains(key) ? j[key] : initial[key];
    };
    for(const auto& c : controls) if(!scSnapshots::isOwnKey(c.key) && c.key != speakerLayout.getEscapedName()
                                     && !(published.count(c.key) && published.at(c.key)->hasInConnection()))
        c.load(blend(value(a, c.key), value(b, c.key), t, sw, c.discrete));
    const auto& src = sw ? b : a;
    if(modSource != &src || t >= 1) { modSource = &src; mod.load(src, "physicalMod"); mod.afterLoad(); }
    auto count = [&](const char* key, int max) {
        const int av = a.value(key, 0), bv = b.value(key, 0);
        return std::clamp(t <= 0 ? av : t >= 1 ? bv : std::max(av, bv), 0, max);
    };
    const int nl = count("physicalModLfos", scModulation::MAX_LFOS), ne = count("physicalModEnvs", scModulation::MAX_ENVS),
              nx = count("physicalModExts", scModulation::MAX_EXT);
    const bool counts = nl != mod.lfoCount || ne != mod.envCount || nx != mod.extCount;
    mod.lfoCount = nl; mod.envCount = ne; mod.extCount = nx;
    auto ma = a.value("physicalModMatrix", std::vector<float>(scModulation::NUM_SOURCES * NUM_TARGETS, 0));
    auto mb = b.value("physicalModMatrix", std::vector<float>(scModulation::NUM_SOURCES * NUM_TARGETS, 0));
    ma.resize(mod.matrix.size(), 0); mb.resize(mod.matrix.size(), 0);
    bool dirty = false;
    for(size_t i = 0; i < mod.matrix.size(); ++i) { const float value = ofLerp(ma[i], mb[i], t); if(value != mod.matrix[i]) { mod.matrix[i] = value; dirty = true; } }
    if(counts) mod.refresh(); else if(dirty) mod.sendMatrix();
    restoring = false; sendAll();
}
std::vector<ofJson> scPhysicalBox::factory() const {
    std::vector<ofJson> out;
    for(const auto& f : factorySounds()) {
        ofJson j = initial; j["name"] = f.name;
        for(const auto& [sc, value] : f.values) {
            bool found = false;
            for(const auto& c : soundControls) if(c->sc == sc) { j[c->p.getEscapedName()] = value; found = true; }
            // any other control by its key (Gate_Mode, Pulse_Rate, ...), as its own type
            if(!found && initial.contains(sc)) {
                const auto& def = initial[sc];
                j[sc] = def.is_boolean() ? ofJson(value > 0.5f) : def.is_number_integer() ? ofJson(int(std::lround(value))) : ofJson(value);
            } else if(!found) ofLogWarning("scPhysicalBox") << "factory sound " << f.name << ": no control " << sc;
        }
        out.push_back(j);
    }
    return out;
}
void scPhysicalBox::presetSave(ofJson& j) {
    mod.saveLfoOutputs(j);
    j["physicalBox"] = capture();
    // Snapshot controls belong in a patch, but never inside a sound snapshot.
    for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key)) j["physicalBox"][c.key] = c.save();
    j["physicalBox"][speakerLayout.getEscapedName()] = speakerLayout.get();   // the room, not the sound
    j["physicalBoxPublished"] = publishedKeys; timeline.save(j, "physicalBoxTimeline");
}
void scPhysicalBox::loadBeforeConnections(ofJson& j) {
    mod.loadLfoOutputs(j);
    std::vector<std::string> wanted;
    if(j.contains("physicalBoxPublished") && j["physicalBoxPublished"].is_array())
        for(const auto& k : j["physicalBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys; for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k); timeline.load(j, "physicalBoxTimeline");
}
void scPhysicalBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(!j.contains("physicalBox")) return;
    auto state = j["physicalBox"];
    // a loaded patch starts from Auto's own rule: droning unless Gate is
    // connected or timelined (a value it gets from here on switches it to gated)
    gateTouched = false;
    apply(state, state, 1, true);
    restoring = true; for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key) && state.contains(c.key)) c.load(state[c.key]); restoring = false;
    if(state.contains(speakerLayout.getEscapedName())) speakerLayout = state.value(speakerLayout.getEscapedName(), 0);
}
void scPhysicalBox::requireSynthdefs() {
    for(const auto& entry : std::vector<std::pair<std::string, std::string>>{
        {"PhysicalBoxVoice.scsyndef", "PhysicalBox"}, {"ModGate1.scsyndef", "Modulation"}, {"PulseGate.scsyndef", "Modulation"}, {"mixerProEQ2.scsyndef", "MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(data)) for(const auto& f : std::filesystem::recursive_directory_iterator(data, std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file() && f.path().filename() == entry.first) { folder = f.path().parent_path(); break; }
            if(folder.empty()) folder = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "synthdefs/Defaults" / entry.second / "CompiledSynthdefs" / (entry.second == "MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder / entry.first)) { ofLogError("physicalBox") << "Compile synthdefs/Defaults/" << entry.second << " first"; continue; }
            for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e) { ofLogError("physicalBox") << e.what(); }
    }
}
void scPhysicalBox::update(ofEventArgs&) {
    timeline.update(); snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto names = snaps.slotNames();
        for(const auto& k : snaps.slotKeys()) dropdowns.set(k, names, published.count(k) ? published[k] : nullptr);
    }
    if(structureDirty) rebuild();
    for(auto& [server, st] : states) if(st.created && st.droneSent != (droning() ? 1 : 0)) sendNotes(server, st);
    // Pulse on / off / again (a new Seed, Reset)
    for(auto& [server, st] : states) if(st.created) {
        const bool want = pulsing();
        if(want && (!pulse.running(server) || pulse.restart)) startPulse(server, st);
        else if(!want && pulse.running(server)) { pulse.stop(server); sendNotes(server, st); }
    }
    pulse.restart = false;
    mod.showing = show.get(); mod.update();
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states) if(st.created) {
        for(auto& f : st.fx) {
            if(f.offAt < 0 || now < f.offAt) continue;
            f.offAt = -1; f.running = false; routeChain(st); if(f.synth) f.synth->run(false);
        }
    }
    if(!show.get()) return;
    for(auto& [s, st] : states) if(st.created && liveBus(s, st.vis, false)) {
        live = st.vis->readValues;
        if(now >= nextRead) { nextRead = now + 1.0f / (pulsing() ? 60 : 25); s->requestControlValues(st.vis->index, VIS_SIZE); }
        break;
    }
    if(pulsing()) pulse.read(live, VIS_PULSE);
}
bool scPhysicalBox::droning() const {
    if(pulsing()) return false;
    if(gateMode.get() == 2) return true;
    if(gateMode.get() == 1) return false;
    return !(gateTouched || gateIn.connected() || (gateIn.handle && gateIn.handle->isTimelined()));
}

// ── Window ───────────────────────────────────────────────────────────────────
scPhysicalBox::SoundControl* scPhysicalBox::sound(const std::string& sc) {
    for(auto& c : soundControls) if(c->sc == sc) return c.get();
    return nullptr;
}
ofParameter<float>& scPhysicalBox::soundParam(const std::string& sc) { return sound(sc)->p; }

void scPhysicalBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1180 * z, 860 * z), ImGuiCond_FirstUseEver);
    bool open = show.get();
    if(!ImGui::Begin(getWindowTitle().c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End(); if(!open) show = false; return;
    }
    if(!open) { show = false; ImGui::End(); return; }
    keyboardPiano.draw(pitch, gate, voices.get());
    const float w = ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    drawDisplay(w);
    if(pulsing()) pulse.drawPanel(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##physTabs")) {
        auto tab = [&](const char* name, std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##physTab") + name).c_str(), ImVec2(0, 0), false);
            ImGui::Spacing();
            const float tw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            labelW = 70;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Sound", [this](float tw){ drawSoundTab(tw); });
        tab("Modulation", [this](float tw){ mod.drawEnvelopeCards(tw, {}); ImGui::Spacing(); mod.drawLfos(tw); });
        tab("Routing", [this](float tw){ mod.drawRouting(tw); });
        tab("Panning", [this](float tw){ drawPanTab(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void scPhysicalBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
    keyboardPiano.finishFrame();
}

void scPhysicalBox::drawHeader() {
    const float z = zoom();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    auto next = [&](float width, float gap = 12.0f) {
        if(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + gap * z + width <= right) ImGui::SameLine(0, gap * z);
    };
    labelW = 54;
    ImGui::BeginGroup(); sliderInt(voices, "Voices", 170 * z); ImGui::EndGroup();
    next(170 * z);
    ImGui::BeginGroup(); sliderInt(channels, "Channels", 170 * z); ImGui::EndGroup();
    next(170 * z);
    ImGui::BeginGroup(); slider(master, "Master", 170 * z, "%.2f"); ImGui::EndGroup();
    next(190 * z); labelW = 64;
    ImGui::BeginGroup(); combo(gateMode, "Gate Mode", 190 * z); ImGui::EndGroup();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Auto: drone until Gate is connected, timelined or receives a note.\nGated: follow Gate. Drone: every voice always plays.\nPulse: the voices play by themselves (the Pulse panel).");
    next(ImGui::CalcTextSize("pulsing").x, 6);
    ImGui::AlignTextToFramePadding();
    if(pulsing()) ImGui::TextColored(ImVec4(.42f, .83f, .77f, 1), "pulsing");
    else ImGui::TextColored(droning() ? ImVec4(.96f, .73f, .45f, 1) : ImVec4(.59f, .65f, 1, 1), droning() ? "droning" : "gated");
    next(ImGui::CalcTextSize("Strike").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button("Strike")) for(auto& [s, st] : states) for(int i = 0; i < st.builtVoices; ++i) if(st.voices[i]) st.voices[i]->set("strk", 1.0f);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Strike every voice now (also while droning or held)");
    next(ImGui::CalcTextSize("Reset LFOs").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button(pulsing() ? "Reset" : "Reset LFOs")) resetLFOs.trigger();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(pulsing() ? "Restart the LFOs and the pulse pattern" : "Restart the LFOs");
    labelW = 70;
}

void scPhysicalBox::drawDisplay(float w) {
    const float z = zoom();
    const float h = 176 * z, pad = h, meters = std::max(60.0f * z, voices.get() * 11.0f * z);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    drawPad(a, pad);
    drawObject(ImVec2(a.x + pad + gapW(), a.y), ImVec2(w - pad - meters - 2 * gapW(), h));
    drawMeters(ImVec2(a.x + w - meters, a.y), ImVec2(meters, h));
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 4 * z));
    ImGui::Dummy(ImVec2(w, 0));
}

// Material (x: wood .. glass .. metal) and Excite + Listen (y: up the body, down the waveguide)
void scPhysicalBox::drawPad(ImVec2 a, float s) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + s, a.y + s);
    const ImU32 wood = IM_COL32(120, 78, 44, 255), glass = IM_COL32(70, 120, 130, 255), metal = IM_COL32(120, 125, 140, 255);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + s * 0.5f, b.y), wood, glass, glass, wood);
    dl->AddRectFilledMultiColor(ImVec2(a.x + s * 0.5f, a.y), b, glass, metal, metal, glass);
    dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
    for(int i = 1; i < 4; ++i) {
        dl->AddLine(ImVec2(a.x + s * i / 4, a.y), ImVec2(a.x + s * i / 4, b.y), pal::grid);
        dl->AddLine(ImVec2(a.x, a.y + s * i / 4), ImVec2(b.x, a.y + s * i / 4), pal::grid);
    }
    dl->AddRect(a, b, pal::border, 3 * z);
    auto label = [&](ImVec2 p, const char* t) { dl->AddText(p, IM_COL32(255, 255, 255, 150), t); };
    label(ImVec2(a.x + 4 * z, b.y - 16 * z), "wood");
    label(ImVec2(a.x + s * 0.5f - ImGui::CalcTextSize("glass").x * 0.5f, b.y - 16 * z), "glass");
    label(ImVec2(b.x - ImGui::CalcTextSize("metal").x - 4 * z, b.y - 16 * z), "metal");
    label(ImVec2(a.x + 4 * z, a.y + 3 * z), "body");
    label(ImVec2(a.x + 4 * z, b.y - 32 * z), "waveguide");
    auto& mat = soundParam("material");
    auto& exc = soundParam("excite");
    const ImVec2 p(a.x + mat.get() * s, b.y - exc.get() * s);
    dl->AddCircle(p, 7 * z, IM_COL32(255, 255, 255, 230), 0, 2 * z);
    dl->AddCircleFilled(p, 3 * z, pal::exciter);
    // listen apart from excite: its own ring
    const float lis = soundParam("listen").get();
    if(std::abs(lis - exc.get()) > 0.01f) dl->AddCircle(ImVec2(p.x, b.y - lis * s), 5 * z, pal::pickup, 0, 1.5f * z);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##materialPad", ImVec2(s, s));
    if(ImGui::IsItemActive()) {
        const auto m = ImGui::GetMousePos();
        mat = ofClamp((m.x - a.x) / s, 0.0f, 1.0f);
        const float e = ofClamp((b.y - m.y) / s, 0.0f, 1.0f);
        exc = e; soundParam("listen") = e;
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Material %.2f  (wood > glass > metal: how fast highs die, how long it rings)\n"
                          "Excite / Listen %.2f / %.2f  (0 waveguide, 1 modal body)\nRight-click: publish / modulate Material", mat.get(), exc.get(), lis);
    menu(mat.getEscapedName());
}

// The waveguide's excited shape with the contact and pickups (drag them), and
// the body's modes as the voice computes them (PhysicalModes.h).
void scPhysicalBox::drawObject(ImVec2 a, ImVec2 size) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, pal::canvas, 3 * z);
    dl->AddRect(a, b, pal::border, 3 * z);
    const float exc = soundParam("excite").get(), lis = soundParam("listen").get();
    const float family = soundParam("bfamily").get(), shape = soundParam("bshape").get();
    // a drum or plate body: its own 2-D view at the right of the waveguide's
    const int kind2 = physical::bodyKind(family, shape);
    const float view = kind2 != physical::K_LINE ? size.y * 0.55f - 10 * z : 0;
    const float x0 = a.x + 18 * z, x1 = b.x - 18 * z - (view > 0 ? view + 18 * z : 0);
    const float rowA = a.y + size.y * 0.27f, ampA = size.y * 0.16f;
    auto& pos = soundParam("pos"); auto& p1 = soundParam("pick1"); auto& p2 = soundParam("pick2");
    const float bore = soundParam("abore").get();
    const char* kind = bore < 0.25f ? "string" : bore < 0.75f ? "closed tube" : "open tube";
    const float pitch0 = first(pitch.get(), 0, 48) + soundParam("atune").get();
    const auto noteName = [](float note) {
        static const char* n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int k = int(std::round(note));
        return std::string(n[((k % 12) + 12) % 12]) + ofToString(k / 12 - 1);
    };
    dl->AddText(ImVec2(a.x + 6 * z, a.y + 4 * z), mixColor(pal::textDim, pal::waveguide, 1 - std::min(exc, lis)),
                ("A  " + std::string(kind) + "   " + noteName(pitch0) + "  " + ofToString(440 * std::pow(2.0f, (pitch0 - 69) / 12), 1) + " Hz").c_str());
    // the waveguide (a tube drawn as two walls)
    const ImU32 wg = mixColor(IM_COL32(150, 165, 255, 70), pal::waveguide, 1 - exc);
    if(bore >= 0.25f) {
        dl->AddLine(ImVec2(x0, rowA - ampA - 4 * z), ImVec2(x1, rowA - ampA - 4 * z), IM_COL32(150, 165, 255, 60), 1.5f * z);
        dl->AddLine(ImVec2(x0, rowA + ampA + 4 * z), ImVec2(x1, rowA + ampA + 4 * z), IM_COL32(150, 165, 255, 60), 1.5f * z);
    }
    dl->AddLine(ImVec2(x0, rowA - ampA), ImVec2(x0, rowA + ampA), pal::headerText, 2 * z);
    dl->AddLine(ImVec2(x1, rowA - ampA), ImVec2(x1, rowA + ampA), pal::headerText, 2 * z);
    // its shape just after a pluck / strike at the contact: sum sin(k pi pos) sin(k pi x) / k^2
    std::vector<ImVec2> line;
    const int steps = 96;
    for(int i = 0; i <= steps; ++i) {
        const float x = float(i) / steps;
        float y = 0;
        for(int k = 1; k <= 16; ++k) y += std::sin(k * PI * pos.get()) * std::sin(k * PI * x) / (k * k);
        line.push_back(ImVec2(ofLerp(x0, x1, x), rowA - y * ampA * 1.15f));
    }
    dl->AddPolyline(line.data(), int(line.size()), wg, 0, 2 * z);
    // the markers: contact (triangle) and pickups (circles); drag them
    struct Marker { ofParameter<float>* p; ImU32 color; const char* name; };
    Marker markers[3] = {{&pos, pal::exciter, "Contact"}, {&p1, pal::pickup, "Pickup 1"}, {&p2, pal::pickup, "Pickup 2"}};
    for(int m = 0; m < 3; ++m) {
        const float x = ofLerp(x0, x1, markers[m].p->get());
        if(m == 0) dl->AddTriangleFilled(ImVec2(x, rowA + ampA + 2 * z), ImVec2(x - 6 * z, rowA + ampA + 12 * z), ImVec2(x + 6 * z, rowA + ampA + 12 * z), pal::exciter);
        else {
            dl->AddCircleFilled(ImVec2(x, rowA - ampA - 7 * z), 4.5f * z, markers[m].color);
            dl->AddText(ImVec2(x - 3 * z, rowA - ampA - 22 * z), markers[m].color, m == 1 ? "1" : "2");
        }
    }
    static int dragging = -1;
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y));
    ImGui::InvisibleButton("##object", ImVec2(view > 0 ? x1 - a.x + 9 * z : size.x, size.y * 0.55f));
    const auto mouse = ImGui::GetMousePos();
    const float mx = ofClamp((mouse.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    if(ImGui::IsItemActivated()) {
        float best = 1e9f; dragging = -1;
        for(int m = 0; m < 3; ++m) {
            const float d = std::abs(ofLerp(x0, x1, markers[m].p->get()) - mouse.x) + (m == 0 ? (mouse.y > rowA ? 0 : 6 * z) : (mouse.y < rowA ? 0 : 6 * z));
            if(d < best) { best = d; dragging = m; }
        }
    }
    if(ImGui::IsItemActive() && dragging >= 0) *markers[dragging].p = mx;
    if(!ImGui::IsItemActive()) dragging = -1;
    if(ImGui::IsItemHovered() && dragging < 0)
        ImGui::SetTooltip("Drag the contact (where it is struck, bowed, blown) or a pickup (where it is heard).\n"
                          "Contact %.2f   Pickup 1 %.2f   Pickup 2 %.2f", pos.get(), p1.get(), p2.get());
    if(view > 0) drawSurface(ImVec2(b.x - 10 * z - view, a.y + 6 * z), view, kind2, std::max(exc, lis));

    // the body's modes: log frequency, height = how much the contact and pickups get of them
    const float rowTop = a.y + size.y * 0.58f, rowBottom = b.y - 8 * z;
    std::string shapeName = std::string(physical::familyName(int(std::lround(family)))) + ": " + shapeLabel(family, shape);
    dl->AddText(ImVec2(a.x + 6 * z, rowTop - 16 * z), mixColor(pal::textDim, pal::body, std::max(exc, lis)), ("B  " + shapeName).c_str());
    const float lo = std::log2(0.4f), hi = std::log2(64.0f);
    auto xOf = [&](float ratio) { return ofLerp(x0, x1, (std::log2(std::max(ratio, 0.01f)) - lo) / (hi - lo)); };
    for(int k = 1; k <= 64; k *= 2) {
        const float x = xOf(float(k));
        dl->AddLine(ImVec2(x, rowTop), ImVec2(x, rowBottom), pal::grid);
    }
    const float fb0 = 440 * std::pow(2.0f, (first(pitch.get(), 0, 48) + soundParam("btune").get() - 69) / 12);
    physical::Body body;
    body.family = family; body.shape = shape; body.stretch = soundParam("bstretch").get();
    body.material = soundParam("material").get(); body.bdecay = soundParam("bdecay").get();
    body.freeze = soundParam("freeze").get(); body.bright = soundParam("bbright").get(); body.f0 = fb0;
    const int modes = std::min(std::clamp(int(std::round(soundParam("bmodes").get())), 1, physical::MAXM), physical::modeCount(body));
    physical::Spots spots;
    spots.cx = pos.get(); spots.cy = soundParam("posy").get();
    spots.p1x = p1.get(); spots.p1y = soundParam("pick1y").get();
    spots.p2x = p2.get(); spots.p2y = soundParam("pick2y").get();
    int hovered = -1; float hoverD = 6 * z;
    std::vector<physical::Mode> ms(modes);
    for(int k = 0; k < modes; ++k) {
        ms[k] = physical::mode(k, body);
        const auto& m = ms[k];
        if(!m.on || m.ratio > 64 || fb0 * m.ratio > 20000) continue;
        const auto wt = physical::weights(m, spots);
        const float e = float(std::abs(wt.e));
        const float l = 0.5f * float(std::abs(wt.p1) + std::abs(wt.p2));
        const float hgt = ofClamp(float(e * l * m.amp), 0.03f, 1.0f);
        const float x = xOf(m.ratio);
        const float ring = ofClamp(std::log10(m.t60 * 10) / 2.5f, 0.0f, 1.0f);   // 0.1 s .. 30 s
        const ImU32 col = mixColor(IM_COL32(120, 80, 50, 200), IM_COL32(255, 230, 170, 255), ring);
        dl->AddLine(ImVec2(x, rowBottom), ImVec2(x, rowBottom - hgt * (rowBottom - rowTop)), col, 2 * z);
        if(std::abs(mouse.x - x) < hoverD && mouse.y > rowTop && mouse.y < rowBottom) { hovered = k; hoverD = std::abs(mouse.x - x); }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, rowTop));
    ImGui::InvisibleButton("##modes", ImVec2(size.x, rowBottom - rowTop));
    if(ImGui::IsItemHovered() && hovered >= 0)
        ImGui::SetTooltip("Mode %d: x%.3f  %.1f Hz  rings %.2f s", hovered + 1, ms[hovered].ratio, fb0 * ms[hovered].ratio, ms[hovered].t60);
    else if(ImGui::IsItemHovered())
        ImGui::SetTooltip("The body's modes (log frequency, octave lines): height = how much the contact\n"
                          "and pickups get of each, colour = how long it rings (Material, B Decay).\n"
                          "Split and Beat make each mode a beating pair (warble, gamelan ombak).");
}

// A drum head (radius and angle) or a plate (x, y) seen from above, with the
// contact and the pickups on it; drag them. The nodal lines of the lowest
// modes are drawn faintly, so it shows where a mode cannot be struck or heard.
void scPhysicalBox::drawSurface(ImVec2 a, float side, int kind, float presence) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const bool drum = kind == physical::K_MEMBRANE;
    const float w = drum ? side : side * 0.618f, h = side;
    const ImVec2 o(a.x + (side - w) * 0.5f, a.y), c(a.x + side * 0.5f, a.y + side * 0.5f);
    const float R = side * 0.5f;
    const ImU32 edge = mixColor(IM_COL32(222, 170, 105, 60), pal::body, presence);
    struct Spot { ofParameter<float>* x; ofParameter<float>* y; ImU32 color; const char* name; };
    Spot spots[3] = {{&soundParam("pos"), &soundParam("posy"), pal::exciter, "Contact"},
                     {&soundParam("pick1"), &soundParam("pick1y"), pal::pickup, "1"},
                     {&soundParam("pick2"), &soundParam("pick2y"), pal::pickup, "2"}};
    auto at = [&](float x, float y) {
        if(drum) { const float t = 2 * PI * y; return ImVec2(c.x + R * x * std::cos(t), c.y - R * x * std::sin(t)); }
        return ImVec2(o.x + x * w, o.y + y * h);
    };
    if(drum) {
        dl->AddCircleFilled(c, R, IM_COL32(222, 170, 105, 14), 48);
        dl->AddCircle(c, R, edge, 48, 1.5f * z);
        // nodal circles of (0,2), (0,3) and diameters of (1,1), (2,1), relative to the contact's angle
        for(float rr : {2.4048f / 5.5201f, 2.4048f / 8.6537f, 5.5201f / 8.6537f}) dl->AddCircle(c, R * rr, IM_COL32(255, 255, 255, 18), 40, 1 * z);
        const float ref = spots[0].y->get();
        for(int m : {1, 2}) for(int i = 0; i < m; ++i) {
            const float t = 2 * PI * (ref + (0.25f + i * 0.5f) / m);
            dl->AddLine(ImVec2(c.x - R * std::cos(t), c.y + R * std::sin(t)), ImVec2(c.x + R * std::cos(t), c.y - R * std::sin(t)), IM_COL32(255, 255, 255, 14), 1 * z);
        }
    } else {
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(222, 170, 105, 14));
        dl->AddRect(o, ImVec2(o.x + w, o.y + h), edge, 0, 0, 1.5f * z);
        for(float t : {1 / 3.0f, 0.5f, 2 / 3.0f}) {
            dl->AddLine(ImVec2(o.x + w * t, o.y), ImVec2(o.x + w * t, o.y + h), IM_COL32(255, 255, 255, 14), 1 * z);
            dl->AddLine(ImVec2(o.x, o.y + h * t), ImVec2(o.x + w, o.y + h * t), IM_COL32(255, 255, 255, 14), 1 * z);
        }
    }
    for(int i = 0; i < 3; ++i) {
        const ImVec2 p = at(spots[i].x->get(), spots[i].y->get());
        if(i == 0) dl->AddTriangleFilled(ImVec2(p.x, p.y - 6 * z), ImVec2(p.x - 5.5f * z, p.y + 4 * z), ImVec2(p.x + 5.5f * z, p.y + 4 * z), pal::exciter);
        else {
            dl->AddCircleFilled(p, 4.5f * z, spots[i].color);
            dl->AddText(ImVec2(p.x + 5 * z, p.y - 14 * z), spots[i].color, spots[i].name);
        }
    }
    static int dragging = -1;
    ImGui::SetCursorScreenPos(ImVec2(a.x - 6 * z, a.y - 6 * z));
    ImGui::InvisibleButton("##surface", ImVec2(side + 12 * z, side + 12 * z));
    const auto mouse = ImGui::GetMousePos();
    if(ImGui::IsItemActivated()) {
        float best = 1e9f; dragging = -1;
        for(int i = 0; i < 3; ++i) {
            const ImVec2 p = at(spots[i].x->get(), spots[i].y->get());
            const float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
            if(d < best) { best = d; dragging = i; }
        }
    }
    if(ImGui::IsItemActive() && dragging >= 0) {
        auto& sp = spots[dragging];
        if(drum) {
            const float dx = mouse.x - c.x, dy = c.y - mouse.y;
            *sp.x = ofClamp(std::hypot(dx, dy) / R, 0.0f, 1.0f);
            float t = std::atan2(dy, dx) / (2 * PI);
            *sp.y = t < 0 ? t + 1 : t;
        } else {
            *sp.x = ofClamp((mouse.x - o.x) / w, 0.0f, 1.0f);
            *sp.y = ofClamp((mouse.y - o.y) / h, 0.0f, 1.0f);
        }
    }
    if(!ImGui::IsItemActive()) dragging = -1;
    if(ImGui::IsItemHovered() && dragging < 0)
        ImGui::SetTooltip(drum ? "The drum head from above: drag the contact or a pickup.\n"
                                 "Centre: only the round modes (a thud); towards the rim: every mode (ringing).\n"
                                 "A pickup on a mode's nodal diameter (faint lines) does not hear it."
                               : "The plate from above: drag the contact or a pickup.\n"
                                 "The centre strikes only the odd-odd modes; near the edges and corners, all of them.");
}

void scPhysicalBox::drawMeters(ImVec2 a, ImVec2 size) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, pal::canvas, 3 * z);
    dl->AddRect(a, b, pal::border, 3 * z);
    const int n = voices.get();
    const float cw = (size.x - 8 * z) / std::max(1, n);
    for(int v = 0; v < n; ++v) {
        const size_t o = size_t(v * VIS_VOICE);
        const float level = o < live.size() ? std::sqrt(ofClamp(live[o] * 4, 0.0f, 1.0f)) : 0;
        const bool on = o + 1 < live.size() && live[o + 1] > 0.0001f;
        const float x = a.x + 4 * z + v * cw;
        const float top = a.y + 14 * z, bottom = b.y - 6 * z;
        dl->AddRectFilled(ImVec2(x + 1 * z, top), ImVec2(x + cw - 1 * z, bottom), IM_COL32(255, 255, 255, 10));
        dl->AddRectFilled(ImVec2(x + 1 * z, bottom - level * (bottom - top)), ImVec2(x + cw - 1 * z, bottom), mixColor(pal::waveguide, pal::accent, level));
        dl->AddCircleFilled(ImVec2(x + cw * 0.5f, a.y + 7 * z), 2.5f * z, on ? pal::exciter : IM_COL32(255, 255, 255, 30));
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##meters", size);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Voices: level, gate dot (each voice is its own instrument)");
}

void scPhysicalBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent, const char* help, int icon) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f * z;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent ? accent : pal::accent, 1.5f * z);
    scSectionHelp(cur, w, bh, help);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f * z, cur.y + 3.0f * z));
    if(on){
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) *on = v;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off", title);
        menu(on->getEscapedName());
        ImGui::PopID();
        ImGui::SameLine(0, 4.0f * z);
    }
    if(icon >= 0){   // the section's picture, before its title
        const float s = ImGui::GetFrameHeight();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImU32 col = accent ? accent : pal::accent;
        if(icon == BODY_ICON){
            const auto pl = physical::place(soundParam("bfamily").get(), soundParam("bshape").get());
            physIcons::drawModel(dl, ImVec2(at.x - 2 * z, at.y), ImVec2(at.x + s * 1.3f, at.y + s), pl.f < 0.5f ? pl.a : pl.b, soundParam("material").get());
        }else physIcons::drawIcon(dl, ImVec2(at.x + s * 0.5f, at.y + s * 0.5f), s * 0.62f, icon, col);
        ImGui::Dummy(ImVec2(icon == BODY_ICON ? s * 1.3f : s, s));
        ImGui::SameLine(0, 4.0f * z);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::headerText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f * z));
    ImGui::Dummy(ImVec2(w, 0.0f));
}
void scPhysicalBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(v);
    menu(key);
    ImGui::PopID();
}
void scPhysicalBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    mark(p.getEscapedName());
    menu(p.getEscapedName());
}
void scPhysicalBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    menu(key);
    ImGui::PopID();
}
void scPhysicalBox::combo(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    const auto items = dropdowns.names(key);
    if(items.empty()) return;
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = std::clamp(p.get(), 0, (int)items.size() - 1);
    if(ImGui::Combo("##v", &v, items.data(), (int)items.size())) p.set(v);
    menu(key);
    ImGui::PopID();
}

// one sound control: a slider (Modes: whole numbers, Hold: a checkbox)
void scPhysicalBox::soundSlider(const std::string& sc, const char* label, float w) {
    auto* c = sound(sc);
    if(!c) return;
    const std::string key = c->p.getEscapedName();
    if(dropdowns.has(key)) {   // Family, Resonator: a combo; published, an Oceanode dropdown
        const float z = zoom();
        const auto items = dropdowns.names(key);
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); mark(key);
        ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
        int v = std::clamp(int(std::lround(c->p.get())), 0, int(items.size()) - 1);
        if(ImGui::Combo("##v", &v, items.data(), int(items.size()))) c->p = float(v);
        menu(key);
        ImGui::PopID();
        return;
    }
    if(c->discrete) {
        ImGui::PushID(key.c_str());
        bool on = c->p.get() > 0.5f;
        if(ImGui::Checkbox(label, &on)) c->p = on ? 1 : 0;
        mark(key); menu(key);
        ImGui::PopID();
        return;
    }
    if(sc == "bmodes") {
        const float z = zoom();
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); mark(key);
        ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
        int v = int(std::round(c->p.get()));
        if(ImGui::SliderInt("##v", &v, 1, physical::MAXM)) c->p = float(v);
        menu(key);
        ImGui::PopID();
        return;
    }
    std::string fmt = c->fmt;
    if(sc == "bshape") {
        fmt = shapeLabel(soundParam("bfamily").get(), c->p.get());
        for(size_t i = fmt.find('%'); i != std::string::npos; i = fmt.find('%', i + 2)) fmt.insert(i, "%");
    }
    slider(c->p, label, w, fmt.c_str(), c->log);
}
void scPhysicalBox::soundGroup(const std::string& group, float w) {
    for(auto& c : soundControls) if(c->group == group) {
        std::string label = c->p.getName();
        for(const char* strip : {"A ", "B "}) if(label.rfind(strip, 0) == 0 && group.size() == 1) label.erase(0, 2);
        soundSlider(c->sc, label.c_str(), w);
    }
}

void scPhysicalBox::drawSoundTab(float w) {
    const float cw = colW(w, 5);
    ImGui::BeginGroup();
    labelW = 78;
    sectionHeader("Strike", cw, nullptr, pal::exciter,
        "A hit: a short contact pulse on each new note (the gate's level is how hard).\n\n"
        "Strike: how much of it reaches the object (blend it with the other exciters).\n"
        "Hardness: soft mallet (dull, round) to hard (bright, clicky); harder notes also get harder.\n"
        "Mass: a light stick (short contact) to a heavy mallet (long contact, darker).\n"
        "Bounce: the mallet bounces back for more, quicker and quieter hits (a drum roll at the end of a stroke).", physIcons::Mallet);
    soundGroup("Strike", cw);
    ImGui::Spacing();
    sectionHeader("Bow", cw, nullptr, pal::exciter,
        "A bow drawn across the contact while the gate is open: stick-slip friction keeps the object singing.\n\n"
        "Bow: how much bowing.\nBow Pressure: light pressure whistles (flautando, harmonics), around 1 it sings, "
        "past 1 it goes raucous and scratchy.\nBow Speed: louder and brighter, and how easily the note speaks.\n"
        "Rosin: the friction's grip, smooth to gritty.", physIcons::Bow);
    soundGroup("Bow", cw);
    nextCol();
    sectionHeader("Blow", cw, nullptr, pal::exciter,
        "Air into the waveguide, as a flute or a reed, while the gate is open.\n\n"
        "Blow: how much blowing. Breath: the pressure (overblow it for the upper register).\n"
        "Jet Reed: 0 a jet across an edge (flute), 1 a reed (clarinet). Jet Ratio: where the jet lands; it picks the register.\n"
        "Turbulence: noise in the breath. Air: the breath heard at the mouthpiece, around the tone.\n\n"
        "A flute wants an open Bore (1) and Position near 0. For vibrato, an LFO on Breath.", physIcons::Air);
    soundGroup("Blow", cw);
    ImGui::Spacing();
    sectionHeader("Scrape", cw, nullptr, pal::exciter,
        "Something dragged over the surface while the gate is open: a stream of tiny impacts.\n\n"
        "Scrape Speed: how fast it is dragged. Roughness: how many bumps (sparse to a dense rasp).\n"
        "Grit: how hard each bump is. Chatter: the object's own motion makes it skip and bounce (squeals, chatter).", physIcons::Rasp);
    soundGroup("Scrape", cw);
    nextCol();
    sectionHeader("Contact", cw, nullptr, pal::exciter,
        "Where and how the exciters touch the object.\n\n"
        "Position: where it is struck, bowed or blown along the object (0 one end, 1 the other); near an end is thin and "
        "bright, the middle is full. On a drum it is the radius (centre 0, rim 1) and Position Y the angle; on a plate X and Y.\n"
        "Ext In: the node's In played through the object (an exciter of its own).\n"
        "Attack / Release: how the sustained exciters (bow, blow, scrape) start and stop. Hold keeps them on without a gate.\n"
        "Damper: chokes the ringing when the gate closes (a hand, a pedal); 0 lets it ring.", physIcons::Contact);
    soundGroup("Contact", cw);
    ImGui::Spacing();
    sectionHeader("Output", cw, nullptr, 0, "The voices' level. Where the voices and their two pickups sit on the speakers: the Panning tab.", physIcons::Speaker);
    soundGroup("Output", cw);
    ImGui::Spacing();
    sectionHeader("Resonator", cw, nullptr, pal::body,
        "A resonating body after the pickups.\n\n"
        "Tube: a tube under the bar tuned to the note, as in a marimba or vibraphone (Res Tune from the note; "
        "Tremolo and Trem Depth are a vibraphone's fan).\n"
        "Gourd, Box, Shell, Cone: fixed bodies with their own pitch (Res Tune moves it).\n"
        "Res Mix: how much of the resonator is heard.", physIcons::ResOff + std::clamp(int(std::round(soundParam("radiator").get())), 0, 5));
    soundGroup("Res", cw);
    nextCol();
    labelW = 70;
    sectionHeader("A  Waveguide", cw, nullptr, pal::waveguide,
        "Resonator A: a waveguide, a travelling wave in a string or a tube. Harmonic, singing, it carries the pitch.\n\n"
        "Tune: from the note. Decay: how long it rings. Brightness: how fast the highs die.\n"
        "Bore: 0 string, 0.5 closed tube (odd harmonics, a clarinet), 1 open tube (a flute).\n"
        "Stiffness: a stiff string or wire; the overtones go sharp (piano, bells on wire).\n"
        "Jawari: a sitar's curved bridge. The string buzzes against it, every cycle: a bright, harmonic buzz that "
        "blooms after the attack and sweeps down as the note dies (sitar, tanpura).\n\n"
        "Excite and Listen (Material & Coupling) choose how much of A and B is played and heard.", soundParam("abore").get() < 0.25f ? physIcons::String : physIcons::Pipe);
    soundGroup("A", cw);
    ImGui::Spacing();
    sectionHeader("B  Body", cw, nullptr, pal::body,
        "Resonator B: a modal body, a set of resonances (modes) like a bar, a bell, a plate or a drum. "
        "Inharmonic, metallic or woody.\n\n"
        "Family and Shape: which kind of object (the display shows its modes). Stretch: the modes squeezed or spread.\n"
        "Modes: how many resonances (more is richer and costs more). Tune / Decay / Brightness as for A.\n"
        "Split and Beat: each mode doubled and slightly detuned, the slow beating of real bells and gongs.", BODY_ICON);
    drawBodyPicture(cw);
    soundGroup("B", cw);
    nextCol();
    labelW = 78;
    sectionHeader("Material & Coupling", cw, nullptr, 0,
        "What it is made of, and how the two resonators meet.\n\n"
        "Material: wood (highs die fast, short) to glass to metal (long, bright ring). The square pad above sets it with Excite / Listen.\n"
        "Excite: where the exciters play, 0 into the waveguide (A), 1 into the body (B). Listen: which one is heard.\n"
        "Couple: energy passing between A and B (a string on a soundboard, sympathetic ringing). Mutual: both ways or one way.\n"
        "Pickup 1 / 2: where the two pickups listen (drag them on the display); their Y on drums and plates.", physIcons::Couple);
    soundGroup("Couple", cw);
    ImGui::Spacing();
    sectionHeader("Break", cw, nullptr, 0,
        "Nonlinear behaviour: the object misbehaves when hit hard.\n\n"
        "Rattle: something loose buzzing against it (a snare, a sitar's bridge) once it moves more than Rattle Threshold.\n"
        "Tension: +, the pitch jumps up with the hit and falls back (drums, big gongs); -, it starts low and rises.\n"
        "Cascade: energy spills from low modes into high ones: a gong or a tam-tam blooming after the hit.", physIcons::Rattle);
    soundGroup("Break", cw);
    ImGui::Spacing();
    sectionHeader("Life", cw, nullptr, 0,
        "Slow movement so long notes don't sound frozen.\n\n"
        "Drift: the tuning and the modes wander a little (Drift Rate: how fast).\n"
        "Freeze: the ringing stops decaying and sustains forever (a drone from a single hit).", physIcons::Drift);
    soundGroup("Life", cw);
    ImGui::Spacing();
    sectionHeader("Sympathetic", cw, nullptr, pal::waveguide,
        "Strings that nobody plays, ringing along with the note: a sitar's taraf, the other note fields of a handpan, "
        "a cymbal's dense wash. The note drives them; they ring on after it.\n\n"
        "Sympathetic: how much of them is heard (0: off).\n"
        "Strings: their tuning, in steps from the played note. Taraf: a sitar's scale over an octave and a half. "
        "Pan: the note's own fifth and octaves, the handpan's bloom. Kurd: a handpan's other fields. "
        "Fifths, Octaves, Cluster (chromatic). Cymbal: short combs close together, a metallic wash.\n"
        "Symp Decay: how long they ring. Symp Brightness: soft strings to wide open (cymbals want 1).\n"
        "Shimmer: each string slightly out of tune (cents): they beat against the note and each other.", physIcons::Strings);
    soundGroup("Symp", cw);
    ImGui::EndGroup();
    labelW = 70;
}

// the body as the object it is (crossfading between the two models Shape is
// between), tinted by Material; rings spread from it while the voices sound
void scPhysicalBox::drawBodyPicture(float w) {
    const float z = zoom(), h = 78 * z;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    dl->AddRectFilledMultiColor(a, b, IM_COL32(22, 24, 30, 255), IM_COL32(22, 24, 30, 255), IM_COL32(10, 11, 13, 255), IM_COL32(10, 11, 13, 255));
    dl->AddRect(a, b, pal::border, 3 * z);
    const auto pl = physical::place(soundParam("bfamily").get(), soundParam("bshape").get());
    const float material = soundParam("material").get();
    float level = 0;
    for(int v = 0; v < voices.get(); ++v) { const size_t o = size_t(v * VIS_VOICE); if(o < live.size()) level = std::max(level, live[o]); }
    level = std::sqrt(ofClamp(level * 4, 0.0f, 1.0f));
    const ImVec2 pa(a.x + 4 * z, a.y + 4 * z), pb(b.x - 4 * z, b.y - 16 * z);
    dl->PushClipRect(a, b, true);
    if(level > 0.01f) {
        const ImVec2 c((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f);
        const float now = ofGetElapsedTimef(), r0 = (pb.y - pa.y) * 0.45f;
        for(int k = 0; k < 3; ++k) {
            const float t = std::fmod(now * 0.8f + k / 3.0f, 1.0f);
            dl->AddEllipse(c, ImVec2(r0 * (1 + 2.2f * t) * 1.5f, r0 * (1 + 2.2f * t)), mixColor(IM_COL32(222, 170, 105, 0), IM_COL32(255, 220, 160, 110), level * (1 - t)), 0, 48, 1.2f * z);
        }
    }
    if(pl.f < 0.999f) physIcons::drawModel(dl, pa, pb, pl.a, material, 1 - pl.f);
    if(pl.f > 0.001f) physIcons::drawModel(dl, pa, pb, pl.b, material, pl.f);
    dl->PopClipRect();
    std::string name = physical::modelName(pl.a);
    if(pl.f > 0.04f) name += "  >  " + std::string(physical::modelName(pl.b)) + " " + ofToString(pl.f, 2);
    const char* mat = material < 0.25f ? "wood" : material < 0.75f ? "glass" : "metal";
    dl->AddText(ImVec2(a.x + 6 * z, b.y - 15 * z), pal::headerText, name.c_str());
    dl->AddText(ImVec2(b.x - ImGui::CalcTextSize(mat).x - 6 * z, b.y - 15 * z), pal::textDim, mat);
    ImGui::InvisibleButton("##bodyPicture", ImVec2(w, h));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The body: %s (Family, Shape), in %s (Material)", name.c_str(), mat);
}

// ── Panning ──────────────────────────────────────────────────────────────────
// Spread: evenly (ring or line); Channels: voice i on speaker i; Manual: Pan
std::vector<float> scPhysicalBox::panPositions() const {
    const int nv = voices.get(), c = channels.get();
    const bool ring = c > 2 && speakerLayout.get() == 0;
    std::vector<float> p(nv, 0.5f);
    if(placement.get() == 2) {
        for(int i = 0; i < nv; ++i) p[i] = first(panP.get(), i, 0.5f);
        return p;
    }
    const float spread = const_cast<scPhysicalBox*>(this)->soundParam("spread").get();
    for(int i = 0; i < nv; ++i) {
        if(placement.get() == 1) {
            const int ch = i % std::max(1, c);
            p[i] = c <= 1 ? 0.5f : ring ? float(ch) / c : float(ch) / (c - 1);
        } else if(ring) {
            p[i] = float(i) / nv * spread + spreadCenter.get() * 0.5f;
        } else {
            const float t = nv > 1 ? float(i) / (nv - 1) : 0.5f;
            p[i] = 0.5f + (t - 0.5f) * spread + spreadCenter.get() * 0.5f;
        }
    }
    return p;
}
void scPhysicalBox::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    const float sw = std::max(30.0f * z, w - labelW * z);
    const auto& values = p.get();
    if(values.size() > 1) {
        const ImVec2 a = ImGui::GetCursorScreenPos(), size(sw, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##bars", size);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), pal::canvas, 2 * z);
        const float bw = size.x / values.size();
        for(size_t i = 0; i < values.size(); ++i) {
            const float t = ofClamp(values[i], 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x + i * bw + 1, a.y + size.y * (1 - t)), ImVec2(a.x + (i + 1) * bw - 1, a.y + size.y), pal::accent);
        }
        if(ImGui::IsItemActive()) p.set({ofClamp((ImGui::GetMousePos().x - a.x) / size.x, 0.0f, 1.0f)});
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d values, one per voice\nDrag: one value for every voice", int(values.size()));
    } else {
        ImGui::SetNextItemWidth(sw);
        float v = first(values, 0, 0.5f);
        if(ImGui::SliderFloat("##v", &v, 0, 1, fmt)) p.set({v});
    }
    menu(key);
    ImGui::PopID();
}
void scPhysicalBox::drawPanTab(float w) {
    const float z = zoom();
    const int nv = voices.get(), c = channels.get();
    const bool ring = c > 2 && speakerLayout.get() == 0;
    const float cw = colW(w, 3);
    auto voiceCol = [](int v, int alpha) {
        ImColor col = ImColor::HSV(std::fmod(v * 0.61803f, 1.0f), 0.5f, 0.98f);
        col.Value.w = alpha / 255.0f;
        return (ImU32)col;
    };
    const float width = soundParam("width").get();
    ImGui::BeginGroup();
    labelW = 70;
    sectionHeader("Placement", cw, nullptr, pal::pickup,
        "Where each voice sits on the speakers. Each voice is heard by two pickups (the small dots), Pickups apart.\n\n"
        "Spread: evenly across the speakers (Spread: how wide; Center: shifted). Channels: voice 1 on speaker 1, "
        "voice 2 on speaker 2... Manual: each voice where you put it: drag it in the view (Shift: onto a speaker), or "
        "set the Pan vector.\nSpeakers (more than 2): around a ring or along a line.\n\n"
        "To move them, route an LFO (or anything) to Pan in Routing: the filled dots show where each voice is now.", physIcons::Placement);
    combo(placement, "Mode", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Spread: evenly across the speakers\nChannels: voice 1 on speaker 1, voice 2 on speaker 2...\n"
                                                 "Manual: each voice where you put it (drag it, or the Pan vector)");
    if(placement.get() == 0) {
        soundSlider("spread", "Spread", cw);
        slider(spreadCenter, "Center", cw, "%+.2f");
    }
    if(placement.get() == 2) sliderVec(panP, "Pan", cw);
    soundSlider("width", "Pickups", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How far apart each voice's two pickups sit (0: both on the voice)");
    if(c > 2) combo(speakerLayout, "Speakers", cw);
    if(c == 1) ImGui::TextColored(ImColor(pal::exciter), "One channel: nothing to pan.");
    nextCol();
    const float vw = 2 * cw + gapW();
    sectionHeader("Speakers", vw, nullptr, pal::pickup,
        "The voices (numbered rings) on the speakers. A ring is where you placed it, the filled dot where it is now "
        "(with Pan modulation), the small dots its two pickups. Drag a ring to place it (Shift: onto a speaker).", physIcons::Speaker);
    {
        auto* dl = ImGui::GetWindowDrawList();
        const float h = ring ? std::min(vw, 340.0f * z) : std::max(120.0f * z, nv * 18.0f * z + 40 * z);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + vw, a.y + h);
        ImGui::InvisibleButton("##panView", ImVec2(vw, h));
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, pal::canvas, 3 * z);
        dl->AddRect(a, b, pal::border, 3 * z);
        const auto base = panPositions();
        auto now = [&](int v) { const size_t i = size_t(VIS_PAN + v); return i < live.size() ? live[i] : base[v]; };
        const ImVec2 m = ImGui::GetMousePos();
        int hoverVoice = -1;
        std::function<ImVec2(float, int)> place;
        const float pad = 30.0f * z;
        if(ring) {
            const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float R = h * 0.5f - pad;
            dl->AddCircle(ctr, R, IM_COL32(255, 255, 255, 34), 64, 1.0f * z);
            for(int k = 0; k < c; ++k) {
                const float ang = -HALF_PI + TWO_PI * k / c;
                const ImVec2 sp(ctr.x + std::cos(ang) * (R + 14 * z), ctr.y + std::sin(ang) * (R + 14 * z));
                dl->AddRectFilled(ImVec2(sp.x - 7 * z, sp.y - 7 * z), ImVec2(sp.x + 7 * z, sp.y + 7 * z), pal::header, 2 * z);
                dl->AddRect(ImVec2(sp.x - 7 * z, sp.y - 7 * z), ImVec2(sp.x + 7 * z, sp.y + 7 * z), pal::headerText, 2 * z);
                const std::string t = ofToString(k + 1);
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f), pal::headerText, t.c_str());
            }
            place = [=](float p, int v) {
                const float r = R * (0.35f + 0.6f * (nv > 1 ? float(v) / (nv - 1) : 1.0f));
                const float ang = -HALF_PI + TWO_PI * p;
                return ImVec2(ctr.x + std::cos(ang) * r, ctr.y + std::sin(ang) * r);
            };
            for(int v = 0; v < nv; ++v) dl->AddCircle(ctr, R * (0.35f + 0.6f * (nv > 1 ? float(v) / (nv - 1) : 1.0f)), pal::grid, 64);
        } else {
            const float x0 = a.x + pad, x1 = b.x - pad;
            for(int k = 0; k < std::max(1, c); ++k) {
                const float x = c <= 1 ? (x0 + x1) * 0.5f : x0 + (x1 - x0) * k / (c - 1);
                dl->AddLine(ImVec2(x, a.y + 22 * z), ImVec2(x, b.y - 6 * z), pal::grid);
                dl->AddRectFilled(ImVec2(x - 9 * z, a.y + 4 * z), ImVec2(x + 9 * z, a.y + 20 * z), pal::header, 2 * z);
                const std::string t = c == 2 ? (k == 0 ? "L" : "R") : ofToString(k + 1);
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                dl->AddText(ImVec2(x - ts.x * 0.5f, a.y + 12 * z - ts.y * 0.5f), pal::headerText, t.c_str());
            }
            const float rowH = (h - 34 * z) / std::max(1, nv);
            place = [=](float p, int v) { return ImVec2(x0 + (x1 - x0) * ofClamp(p, 0.0f, 1.0f), a.y + 28 * z + (v + 0.5f) * rowH); };
        }
        for(int v = 0; v < nv; ++v) {
            const ImVec2 pb = place(base[v], v), pl = place(now(v), v);
            // the two pickups, Width apart around where the voice is now
            const ImVec2 l = place(now(v) - width * 0.25f, v), r = place(now(v) + width * 0.25f, v);
            if(width > 0.01f && c > 1) {
                dl->AddLine(l, r, voiceCol(v, 60), 1.0f * z);
                dl->AddCircleFilled(l, 2.5f * z, voiceCol(v, 180));
                dl->AddCircleFilled(r, 2.5f * z, voiceCol(v, 180));
            }
            dl->AddLine(pb, pl, voiceCol(v, 90), 1.0f * z);
            dl->AddCircle(pb, 7 * z, voiceCol(v, 230), 0, 2 * z);
            dl->AddCircleFilled(pl, 4.5f * z, voiceCol(v, 255));
            dl->AddText(ImVec2(pb.x + 9 * z, pb.y - 7 * z), voiceCol(v, 200), ofToString(v + 1).c_str());
            if(hovered && std::hypot(m.x - pb.x, m.y - pb.y) < 9 * z) hoverVoice = v;
        }
        // dragging a voice: Manual from here on
        if(ImGui::IsItemActivated() && hoverVoice >= 0) {
            panDragVoice = hoverVoice;
            if(placement.get() != 2) { panP.set(panPositions()); placement = 2; }
        }
        if(ImGui::IsItemActive() && panDragVoice >= 0 && panDragVoice < nv) {
            float p;
            if(ring) {
                const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                p = std::fmod(std::atan2(m.y - ctr.y, m.x - ctr.x) + HALF_PI + TWO_PI, TWO_PI) / TWO_PI;
            } else {
                p = ofClamp((m.x - (a.x + pad)) / (vw - 2 * pad), 0.0f, 1.0f);
            }
            if(ImGui::GetIO().KeyShift && c > 1) {      // onto the nearest speaker
                const float steps = ring ? float(c) : float(c - 1);
                p = std::round(p * steps) / steps;
                if(ring) p = std::fmod(p, 1.0f);
            }
            std::vector<float> v(nv);
            for(int i = 0; i < nv; ++i) v[i] = first(panP.get(), i, 0.5f);
            v[panDragVoice] = p;
            panP.set(v);
        }
        if(ImGui::IsItemDeactivated()) panDragVoice = -1;
        if(hovered && hoverVoice >= 0 && panDragVoice < 0)
            ImGui::SetTooltip("Voice %d at %.2f (now %.2f)\nDrag to place it (Shift: onto a speaker)", hoverVoice + 1, base[hoverVoice], now(hoverVoice));
    }
    ImGui::EndGroup();
}

void scPhysicalBox::drawFxTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 5);
    ImGui::BeginGroup();
    static const std::string satHelp = std::string(scHelp::saturation) + scHelp::chain + " -> Space.";
    sectionHeader("Saturation", cw, &satOn, 0, satHelp.c_str(), physIcons::Saturate);
    if(!satOn.get()) ImGui::BeginDisabled();
    slider(satDrive, "Drive", cw, "%.1f dB");
    slider(satBias, "Bias", cw, "%.2f");
    slider(satTone, "Tone", cw, "%.0f Hz", true);
    slider(satOutput, "Output", cw, "%+.1f dB");
    slider(satMix, "Mix", cw, "%.2f");
    if(!satOn.get()) ImGui::EndDisabled();
    ImGui::Spacing();
    sectionHeader("EQ", cw, &eqOn, 0, scHelp::eq, physIcons::Equalizer);
    for(int b = 0; b < 5; b++) eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f; eqEditor.gainLimit = 24.0f;
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f; eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()){ sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    eqEditor.recompute(sr > 0 ? sr : 48000.0f);
    eqEditor.bandMenu = [this](int b){
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        auto value = [this](ofParameter<float>& p, const char* label){
            float v = p.get();
            ImGui::SetNextItemWidth(160.0f * zoom());
            if(ImGui::InputFloat(label, &v, 0, 0, "%.3f")) p.set(ofClamp(v, p.getMin(), p.getMax()));
            const auto key = p.getEscapedName();
            const bool pub = published.count(key) != 0;
            if(ImGui::MenuItem((std::string(pub ? "Unpublish " : "Publish ") + label).c_str(), nullptr, false, !connected(key))){
                if(pub) unpublish(key); else publish(key);
            }
        };
        value(eqFreq[b], "Freq (Hz)");
        value(eqGain[b], "Gain (dB)");
        value(eqShape[b], scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    if(!eqOn.get()) ImGui::BeginDisabled();
    if(eqEditor.draw(cw, 130.0f * z, scEQEditor::defaultColors(), eqOn.get(), nullptr, 0, "##physEq")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(!eqOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Echo", cw, &echoOn, 0, scHelp::echo, physIcons::Echo);
    if(!echoOn.get()) ImGui::BeginDisabled();
    toggle(echoBeats, "Sync to tempo");
    if(echoBeats.get()){
        static const std::vector<std::pair<const char*, float>> notes = {
            {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/16D", 0.375f},
            {"1/8", 0.5f}, {"1/4T", 2.0f / 3.0f}, {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 4.0f / 3.0f},
            {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/2D", 3.0f}, {"1 bar", 4.0f}};
        int k = 0;
        for(int i = 0; i < (int)notes.size(); i++)
            if(std::abs(notes[i].second - echoBeatVal.get()) < std::abs(notes[k].second - echoBeatVal.get())) k = i;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Delay");
        mark(echoBeatVal.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        if(ImGui::SliderInt("##echoNote", &k, 0, (int)notes.size() - 1, notes[k].first)) echoBeatVal = notes[k].second;
        menu(echoBeatVal.getEscapedName());
    }else{
        slider(echoDelay, "Delay", cw, "%.3f s", true);
    }
    slider(echoFeed, "Feedback", cw, "%.2f");
    slider(echoCutoff, "Cutoff", cw, "%.0f (note)");
    slider(echoResonance, "Resonance", cw, "%.2f");
    combo(echoFilter, "Filter", cw);
    slider(echoPingPong, "Ping-pong", cw, "%.2f");
    slider(echoMix, "Mix", cw, "%.2f");
    if(!echoOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb", cw, &rvbOn, 0, scHelp::reverb, physIcons::Reverb);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbMix, "Mix", cw, "%.2f");
    slider(rvbDecay, "Decay", cw, "%.2f s", true);
    slider(rvbSize, "Size", cw, "%.1f");
    slider(rvbPredelay, "Predelay", cw, "%.0f ms");
    slider(rvbLate, "Late", cw, "%.2f");
    slider(rvbSpread, "Spread", cw, "%+.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb Tone", cw, nullptr, 0, scHelp::reverbTone, physIcons::Reverb);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbLowpass, "Lowpass", cw, "%.0f Hz", true);
    slider(rvbHiDamp, "Hi Damp", cw, "%.1f dB");
    slider(rvbLoDamp, "Lo Damp", cw, "%.1f dB");
    slider(rvbModRate, "Mod Rate", cw, "%.3f Hz", true);
    slider(rvbSpin, "Spin", cw, "%.2f");
    slider(rvbWander, "Wander", cw, "%.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Space", cw, &spaceOn, pal::pickup,
        "MiVerb, the reverb of Mutable Instruments' Clouds and Rings: a lush, diffuse space, the last effect, on by default.\n\n"
        "Mix: dry / space. Time: how long it lasts. Damp: the highs dying in it. Diffusion: smeared (high) or grainy (low).\n"
        "Low Cut: keeps the lows out of it. Freeze: holds the current space forever (a pad from what is playing).", physIcons::Space);
    if(!spaceOn.get()) ImGui::BeginDisabled();
    slider(spaceMix, "Mix", cw, "%.2f");
    slider(spaceTime, "Time", cw, "%.2f");
    slider(spaceDamp, "Damp", cw, "%.2f");
    slider(spaceDiff, "Diffusion", cw, "%.2f");
    slider(spaceLowCut, "Low Cut", cw, "%.2f");
    toggle(spaceFreeze, "Freeze");
    if(!spaceOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
}
