//
//  scMetroBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scMetroBox.h"

#if OFXOCEANODESC_HAS_TRANSPORT

#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#if OFXOCEANODESC_HAS_TIMELINE
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeTimeline.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <type_traits>

namespace fs = std::filesystem;
using namespace metrobox;

namespace {

// Floats per /b_setn: 8 KB messages, well under the UDP limit
constexpr int floatsPerMessage = 2048;
// Seconds to wait for /b_alloc's /sync before writing anyway
constexpr float allocTimeout = 2.0f;
constexpr int TPB = TICKS_PER_BEAT;
// The playhead sits this far into the view; the rhythm scrolls past it
constexpr double playheadAt = 0.25;

struct DivisionOption { int ticks; const char* label; };
const DivisionOption divisionOptions[] = {
    {192, "1 bar (4 beats)"}, {96, "2 beats"}, {48, "1 beat"}, {24, "1/8"},
    {16, "1/8 triplet"}, {12, "1/16"}, {8, "1/16 triplet"}, {6, "1/32"},
};
const DivisionOption unitOptions[] = {{24, "1/8"}, {12, "1/16"}, {16, "1/8 triplet"}};
const DivisionOption ratchetUnitOptions[] = {{12, "1/16"}, {8, "1/16 triplet"}, {6, "1/32"}, {4, "1/32 triplet"}, {3, "1/64"}};
const std::vector<std::string> generatorNames = {"Grid", "Euclidean", "Weighted durations", "Subdivision", "Markov"};
const std::vector<std::string> durationNames = {"Fixed", "Legato", "Metric"};
const std::vector<std::string> anchorNames = {"None", "Downbeat", "Group starts", "Beats", "Backbeats"};
const std::vector<std::string> relationNames = {"None", "Follow", "Avoid", "Complement", "Unison", "Response"};
const std::vector<std::string> dynamicsNames = {"None", "Duck", "Reinforce"};
const std::vector<std::string> ghostSideNames = {"Before", "After", "Either"};
const std::vector<std::string> ratchetShapeNames = {"Flat", "Crescendo", "Decrescendo"};
const std::vector<std::string> grooveNames = {"Straight", "Swing 16ths", "Swing 8ths", "Laid back", "Pushed"};
const std::vector<std::string> repeatNames = {"Off", "Loop", "Evolve"};

ImU32 layerColor(int i, float alpha = 1.0f) {
    static const float hues[] = {0.58f, 0.02f, 0.13f, 0.33f, 0.78f, 0.48f, 0.90f, 0.20f};
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hues[i % 8], 0.55f, 0.95f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

bool divisionCombo(const char* label, int& ticks, const DivisionOption* options, int count) {
    int current = 0;
    for(int i = 0; i < count; i++) if(options[i].ticks == ticks) current = i;
    bool changed = false;
    if(ImGui::BeginCombo(label, options[current].label)) {
        for(int i = 0; i < count; i++)
            if(ImGui::Selectable(options[i].label, i == current)) { ticks = options[i].ticks; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool stringCombo(const char* label, int& value, const std::vector<std::string>& options) {
    bool changed = false;
    const int current = std::clamp(value, 0, (int)options.size() - 1);
    if(ImGui::BeginCombo(label, options[current].c_str())) {
        for(int i = 0; i < (int)options.size(); i++)
            if(ImGui::Selectable(options[i].c_str(), i == current)) { value = i; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

void heading(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

ofJson opsToJson(const std::vector<Op>& ops) {
    ofJson arr = ofJson::array();
    for(const auto& op : ops) arr.push_back({op.type, op.seed, op.aux});
    return arr;
}

std::vector<Op> opsFromJson(const ofJson& arr) {
    std::vector<Op> ops;
    if(!arr.is_array()) return ops;
    for(const auto& j : arr) {
        if(!j.is_array() || j.size() < 3) continue;
        Op op;
        op.type = std::clamp(j[0].get<int>(), 0, (int)OP_COUNT - 1);
        op.seed = j[1].get<uint32_t>();
        op.aux = j[2].get<uint32_t>();
        ops.push_back(op);
    }
    return ops;
}

}

// ════════════════════════════════════════════════════════════════════════════
// Fields: every lane setting the window shows and the node can publish
// ════════════════════════════════════════════════════════════════════════════

const std::vector<scMetroBox::Field>& scMetroBox::fields() {
    static const std::vector<Field> list = [] {
        std::vector<Field> v;
        auto F = [&](const char* n, float Layer::* m, float lo, float hi, const char* tip = "") {
            Field f; f.name = n; f.kind = F_FLOAT; f.f = m; f.min = lo; f.max = hi; f.tooltip = tip; v.push_back(f);
        };
        auto I = [&](const char* n, int Layer::* m, float lo, float hi, const char* tip = "") {
            Field f; f.name = n; f.kind = F_INT; f.i = m; f.min = lo; f.max = hi; f.tooltip = tip; v.push_back(f);
        };
        auto B = [&](const char* n, bool Layer::* m, const char* tip = "") {
            Field f; f.name = n; f.kind = F_BOOL; f.b = m; f.min = 0; f.max = 1; f.tooltip = tip; v.push_back(f);
        };
        auto E = [&](const char* n, int Layer::* m, const std::vector<std::string>& opts, const char* tip = "") {
            Field f; f.name = n; f.kind = F_ENUM; f.i = m; f.min = 0; f.max = (float)opts.size() - 1; f.options = opts; f.tooltip = tip; v.push_back(f);
        };
        auto T = [&](const char* n, int Layer::* m, float loBeats, float hiBeats, const char* tip = "") {
            Field f; f.name = n; f.kind = F_TICKS; f.i = m; f.min = loBeats; f.max = hiBeats; f.tooltip = tip; v.push_back(f);
        };
        auto L = [&](const char* n, int Layer::* m, const char* tip = "") {
            Field f; f.name = n; f.kind = F_LANE; f.i = m; f.min = 0; f.max = MAX_LAYERS; f.tooltip = tip; v.push_back(f);
        };
        auto A = [&](const char* n, int action, const char* tip = "") {
            Field f; f.name = n; f.kind = F_ACTION; f.action = action; f.tooltip = tip; v.push_back(f);
        };
        const float tick = 1.0f / TPB;
        E("Generator", &Layer::generator, generatorNames);
        I("Cycle", &Layer::cycleBeats, 1, MAX_CYCLE_BEATS, "Cycle length in beats (polymeter when it differs from the bar).");
        I("Layer seed", &Layer::seed, 0, 99999);
        B("Mute", &Layer::mute, "Silences the outputs only: lanes that relate to this one still hear it.");
        F("Density", &Layer::density, 0, 1, "Chance to keep each onset. Raising it only adds hits.");
        T("Shift", &Layer::shift, 0, MAX_CYCLE_BEATS, "Moves every generated onset later, in beats (wraps in the cycle).");
        I("Steps", &Layer::eucSteps, 1, 64);
        I("Hits", &Layer::eucHits, 0, 64);
        I("Rotation", &Layer::eucRotation, 0, 63);
        F("Complexity", &Layer::complexity, 0, 1, "Chance to split a segment into halves or thirds.");
        F("Long on strong", &Layer::longOnStrong, 0, 1, "Strong positions resist splitting: long notes on strong beats.");
        F("Triplets", &Layer::triplets, 0, 1, "Chance a split is in three.");
        B("Feel main bar", &Layer::feelBar, "The timeline bar's strong beats count too (matters when the cycle is not one bar).");
        F("Weighting", &Layer::weighting, 0, 1, "Weak positions are dropped more: 1 = 16ths kept a quarter as often as downbeats.");
        E("Anchor", &Layer::anchorMode, anchorNames, "Positions always played (with the amount as chance).");
        F("Anchor amount", &Layer::anchorAmount, 0, 1);
        F("Push", &Layer::push, 0, 1, "Chance a hit on a beat moves one unit earlier (anticipation).");
        F("Drag", &Layer::drag, 0, 1, "Chance a hit on a beat moves one unit later.");
        F("Cross barline", &Layer::crossBarline, 0, 1, "Chance a push, drag or gate may cross the cycle start.");
        I("Phrase length", &Layer::phraseLen, 1, 16, "Cycles per phrase: the loop length when Repeat is on (2 = a 2-bar loop with 1-bar cycles).");
        E("Repeat", &Layer::repeat, repeatNames,
          "Off: new material every phrase (with the Renew chance).\n"
          "Loop: the phrase playing now repeats forever (Mutate / Re-roll still change it).\n"
          "Evolve: that loop, with a small mutation every few loops.");
        F("Renew", &Layer::renew, 0, 1, "Chance a phrase brings new material (Repeat off).");
        F("B strength", &Layer::bStrength, 0, 1, "How far the form's other letters (B, C...) mutate A.");
        T("Fill length", &Layer::fillTicks, 0, 4, "Fill at the end of each phrase, in beats (0 = none).");
        F("Fill intensity", &Layer::fillIntensity, 0, 1, "Denser, with ratchets and a crescendo.");
        F("Build-up", &Layer::buildUp, 0, 1, "Density grows over the phrase.");
        F("Phrase accent", &Layer::phraseAccent, 0, 1, "The phrase's first downbeat louder.");
        F("Swell", &Layer::swell, 0, 1, "Amplitude grows over the phrase.");
        F("Ghost", &Layer::ghost, 0, 1, "Chance of a quiet hit a 16th before / after a hit.");
        E("Ghost side", &Layer::ghostSide, ghostSideNames);
        F("Ghost level", &Layer::ghostLevel, 0, 1);
        F("Flam", &Layer::flam, 0, 1, "Chance of a grace hit just before a hit.");
        T("Flam offset", &Layer::flamTicks, tick, 8 * tick, "How far before the hit the grace note falls, in beats.");
        F("Ratchet", &Layer::ratchet, 0, 1, "Chance a hit becomes a quick burst.");
        I("Ratchet hits", &Layer::ratchetCount, 2, 6);
        E("Ratchet shape", &Layer::ratchetShape, ratchetShapeNames);
        E("Groove", &Layer::groove, grooveNames, "Timing feel: swing, or every hit slightly late / early.");
        F("Swing", &Layer::swing, 0.5f, 0.75f, "Where the offbeat falls: 0.5 straight, 0.66 triplet, 0.75 dotted.");
        T("Humanize", &Layer::humanize, 0, 4 * tick, "Seeded timing spread, in beats.");
        E("Relation", &Layer::relMode, relationNames,
          "Follow: only where the lane plays. Avoid: not where it plays. Complement: fills its silences. "
          "Unison: adds its hits. Response: answers what it played, after the delay.");
        L("Relation lane", &Layer::relLane);
        F("Relation amount", &Layer::relAmount, 0, 1);
        T("Response delay", &Layer::respDelay, 0, MAX_CYCLE_BEATS, "How long after the other lane the response comes, in beats.");
        L("Choke by", &Layer::chokeLane, "That lane's hits cut this lane's gates (closed hat chokes open hat).");
        E("Dynamics", &Layer::dynMode, dynamicsNames, "Duck: quieter when the lane hits too. Reinforce: louder.");
        L("Dynamics lane", &Layer::dynLane);
        F("Dynamics amount", &Layer::dynAmount, 0, 1);
        E("Duration", &Layer::durMode, durationNames, "Fixed length, a fraction of the gap to the next hit, or by metric strength.");
        T("Length", &Layer::durTicks, tick, 4);
        F("Legato", &Layer::legato, 0.05f, 1);
        T("Weak length", &Layer::durShort, tick, 4, "Gate on the weakest positions, in beats.");
        T("Strong length", &Layer::durLong, tick, 4, "Gate on the downbeat, in beats.");
        I("Long hits", &Layer::longHits, 0, 16, "Euclidean long notes: this many of the cycle's hits, evenly spread, get the long length.");
        T("Long length", &Layer::longTicks, tick, 4);
        F("Max", &Layer::level, 0, 1);
        F("Min", &Layer::ampMin, 0, 1);
        F("Metric", &Layer::ampMetric, 0, 1, "Strong positions louder.");
        F("Variation", &Layer::variation, 0, 1);
        F("Curve", &Layer::curve, 0.25f, 4, "1 = linear, above 1 favours soft hits, below 1 loud ones.");
        I("Tiers", &Layer::tiers, 0, 5, "Snap amplitudes to N levels (3 = ghost / normal / accent). 0 = off.");
        I("Accent hits", &Layer::accentHits, 0, 16, "Euclidean accents: this many of the cycle's hits, evenly spread, are accented.");
        F("Accent amount", &Layer::accentAmount, 0, 1);
        B("Accents are long", &Layer::accentsLong, "The long notes are the accented hits.");
        B("Keep", &Layer::keep, "Locks the lane: no mutation, manual or automatic (and the phrase playing now is frozen).");
        I("Mutations", &Layer::mutations, 1, 6, "Operators one Mutate adds.");
        I("Evolve every", &Layer::autoEvery, 1, 16, "Repeat Evolve: a mutation is considered every N loops.");
        F("Evolve chance", &Layer::autoChance, 0, 1, "Repeat Evolve: chance the mutation happens.");
        A("Mutate", ACT_MUTATE, "Small changes from the next cycle on: split, merge, nudge, tie, accents...");
        A("Re-roll", ACT_REROLL, "A new rhythm with the same rules.");
        A("Undo", ACT_UNDO);
        A("Redo", ACT_REDO);
        A("Like", ACT_LIKE, "Keeps this version in the lane's pool, for crossover.");
        return v;
    }();
    return list;
}

const scMetroBox::Field* scMetroBox::findField(const std::string& name) {
    for(const auto& f : fields()) if(f.name == name) return &f;
    return nullptr;
}

std::string scMetroBox::publishKey(int layer, const std::string& field) {
    return "L" + ofToString(layer + 1) + " " + field;
}

float scMetroBox::fieldValue(int li, const Field& f) const {
    const Layer& L = layers[li];
    switch(f.kind) {
        case F_FLOAT:  return L.*(f.f);
        case F_BOOL:   return L.*(f.b) ? 1.0f : 0.0f;
        case F_TICKS:  return (float)(L.*(f.i)) / TPB;
        case F_ACTION: return 0.0f;
        default:       return (float)(L.*(f.i));
    }
}

void scMetroBox::setFieldValue(int li, const Field& f, float value) {
    Layer& L = layers[li];
    if(f.kind == F_ACTION) { doAction(li, f.action); return; }
    if(f.b == &Layer::keep) {
        engine.setKeep(li, value >= 0.5f, nowCycle(li));
        refreshPublished(li);
        return;
    }
    if(f.i == &Layer::repeat) {
        engine.setRepeat(li, (int)std::lround(value), nowCycle(li));
        refreshPublished(li);
        return;
    }
    switch(f.kind) {
        case F_FLOAT: L.*(f.f) = ofClamp(value, f.min, f.max); break;
        case F_BOOL:  L.*(f.b) = value >= 0.5f; break;
        case F_TICKS: L.*(f.i) = std::max(f.min > 0 ? 1 : 0, (int)std::lround(ofClamp(value, f.min, f.max) * TPB)); break;
        case F_LANE:  L.*(f.i) = std::clamp((int)std::lround(value), 0, MAX_LAYERS); break;
        default:      L.*(f.i) = std::clamp((int)std::lround(value), (int)f.min, (int)f.max); break;
    }
    // Settings that bound others
    L.eucHits = std::clamp(L.eucHits, 0, L.eucSteps);
    L.eucRotation = std::clamp(L.eucRotation, 0, std::max(0, L.eucSteps - 1));
    L.shift = std::clamp(L.shift, 0, std::max(0, L.cycleTicks() - 1));
    if(L.tiers == 1) L.tiers = 0;

    if(f.b == &Layer::mute) {
        for(auto& [server, st] : servers) sendStaticControls(st);
    } else {
        engine.changed(li);
    }
    refreshPublished(li);
}

int64_t scMetroBox::nowCycle(int li) const {
    return engine.cycleAt(li, getFrameTransportState().current.beatPosition);
}

void scMetroBox::doAction(int li, int action) {
    Layer& L = layers[li];
    const int64_t now = nowCycle(li);
    switch(action) {
        case ACT_MUTATE: engine.mutate(li, now, (uint32_t)std::random_device{}()); break;
        case ACT_REROLL: engine.reroll(li, now, (L.seed + 1) % 100000); break;
        case ACT_UNDO:   engine.undo(li, now); break;
        case ACT_REDO:   engine.redo(li, now); break;
        case ACT_LIKE:   engine.like(li); break;
        default: break;
    }
    refreshPublished(li);
}

// A control drawn from its field, with the right-click menu (typed value,
// publish / unpublish). Returns true when the value changed.
bool scMetroBox::drawField(int li, const std::string& name, const char* label) {
    const Field* fp = findField(name);
    if(!fp) return false;
    const Field& f = *fp;
    const std::string text = label ? label : f.name;
    float v = fieldValue(li, f);
    bool changed = false;

    switch(f.kind) {
        case F_FLOAT: {
            changed = ImGui::SliderFloat(text.c_str(), &v, f.min, f.max);
            break;
        }
        case F_INT: {
            int iv = (int)v;
            if(f.i == &Layer::seed) changed = ImGui::InputInt(text.c_str(), &iv);
            else if(f.i == &Layer::tiers) changed = ImGui::SliderInt(text.c_str(), &iv, 0, 5, iv < 2 ? "off" : "%d");
            else if(f.i == &Layer::autoEvery) changed = ImGui::SliderInt(text.c_str(), &iv, 1, 16, iv == 1 ? "every loop" : "every %d loops");
            else changed = ImGui::SliderInt(text.c_str(), &iv, (int)f.min, (int)f.max);
            v = (float)iv;
            break;
        }
        case F_BOOL: {
            bool bv = v >= 0.5f;
            changed = ImGui::Checkbox(text.c_str(), &bv);
            v = bv ? 1.0f : 0.0f;
            break;
        }
        case F_ENUM: {
            int iv = (int)v;
            changed = stringCombo(text.c_str(), iv, f.options);
            v = (float)iv;
            break;
        }
        case F_TICKS: {
            changed = ImGui::DragFloat(text.c_str(), &v, 0.25f / TPB, f.min, f.max, v == 0.0f && f.min == 0.0f ? "off" : "%.3f beats");
            v = std::round(v * TPB) / TPB;
            break;
        }
        case F_LANE: {
            int iv = (int)v;
            std::vector<std::string> opts = {"None"};
            for(int j = 0; j < li && j < numLayers; j++) opts.push_back(ofToString(j + 1) + " " + layers[j].name);
            if(iv >= (int)opts.size()) iv = 0;
            changed = stringCombo(text.c_str(), iv, opts);
            v = (float)iv;
            break;
        }
        case F_ACTION: {
            const bool disabled = (f.action == ACT_UNDO && !engine.canUndo(li)) || (f.action == ACT_REDO && !engine.canRedo(li))
                || ((f.action == ACT_MUTATE || f.action == ACT_REROLL) && layers[li].keep);
            ImGui::BeginDisabled(disabled);
            changed = ImGui::Button(text.c_str());
            ImGui::EndDisabled();
            break;
        }
    }
    if(!f.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", f.tooltip.c_str());
    fieldMenu(li, f);
    if(changed) setFieldValue(li, f, v);
    return changed;
}

void scMetroBox::fieldMenu(int li, const Field& f) {
    const std::string key = publishKey(li, f.name);
    if(published.count(key)) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    }
    if(!ImGui::BeginPopupContextItem(("##ctx_" + f.name).c_str())) return;
    if(f.kind != F_ACTION) {
        static char buffer[64];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) {
            const float v = fieldValue(li, f);
            if(f.kind == F_FLOAT || f.kind == F_TICKS) std::snprintf(buffer, sizeof(buffer), "%g", v);
            else std::snprintf(buffer, sizeof(buffer), "%d", (int)v);
        }
        const char* unitText = f.kind == F_TICKS ? "Value (beats)"
                             : f.kind == F_ENUM ? "Value (index from 0)"
                             : f.kind == F_LANE ? "Value (lane number, 0 = none)"
                             : f.kind == F_BOOL ? "Value (0 / 1)" : "Value";
        ImGui::TextDisabled("%s", unitText);
        ImGui::SetNextItemWidth(160.0f * ofxOceanodeShared::getZoomLevel());
        if(appearing) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##typed", buffer, sizeof(buffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
            setFieldValue(li, f, ofToFloat(buffer));
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(published.count(key)) {
        ImGui::TextDisabled("Published in node as \"%s\"", key.c_str());
        if(hasConnection(key)) ImGui::TextDisabled("Connected: disconnect to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublish(key);
    } else if(ImGui::MenuItem("Publish to Node")) {
        publish(li, f.name);
    }
    ImGui::EndPopup();
}

bool scMetroBox::publish(int li, const std::string& fieldName) {
    const Field* fp = findField(fieldName);
    const std::string key = publishKey(li, fieldName);
    if(!fp || published.count(key) || li < 0 || li >= MAX_LAYERS) return false;
    const Field& f = *fp;
    if(!publishedSeparatorAdded) {
        addSeparator("Published", ofColor(200));
        publishedSeparatorAdded = true;
    }
    auto p = std::make_unique<Published>();
    p->layer = li;
    p->field = fieldName;
    Published* raw = p.get();
    const float v = fieldValue(li, f);
    auto apply = [this, raw](float value) {
        if(raw->updating) return;
        const Field* field = findField(raw->field);
        if(!field) return;
        raw->updating = true;
        setFieldValue(raw->layer, *field, value);
        raw->updating = false;
    };
    if(f.kind == F_ACTION) {
        p->handle = addParameter(p->pv.set(key));
        p->listener = p->pv.newListener([apply]() { apply(1.0f); });
    } else if(f.kind == F_FLOAT || f.kind == F_TICKS) {
        p->handle = addParameter(p->pf.set(key, v, f.min, f.max));
        p->listener = p->pf.newListener([apply](float& x) { apply(x); });
    } else if(f.kind == F_BOOL) {
        p->handle = addParameter(p->pb.set(key, v >= 0.5f));
        p->listener = p->pb.newListener([apply](bool& x) { apply(x ? 1.0f : 0.0f); });
    } else {
        if(f.kind == F_ENUM) p->handle = addParameterDropdown(p->pi, key, (int)v, f.options);
        else p->handle = addParameter(p->pi.set(key, (int)v, (int)f.min, (int)f.max));
        p->listener = p->pi.newListener([apply](int& x) { apply((float)x); });
    }
    published[key] = std::move(p);
    publishedOrder.push_back(key);
    parameterGroupChanged.notify(this);
    return true;
}

bool scMetroBox::hasConnection(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second->handle
        && (it->second->handle->hasInConnection() || it->second->handle->hasOutConnections());
}

bool scMetroBox::unpublish(const std::string& key) {
    auto it = published.find(key);
    if(it == published.end() || hasConnection(key)) return false;
    if(it->second->handle) removeParameter(it->second->handle->getEscapedName());
    published.erase(it);
    publishedOrder.erase(std::remove(publishedOrder.begin(), publishedOrder.end(), key), publishedOrder.end());
    if(published.empty() && publishedSeparatorAdded) {
        removeSeparator("Published");
        publishedSeparatorAdded = false;
    }
    parameterGroupChanged.notify(this);
    return true;
}

void scMetroBox::syncPublished(const std::vector<std::string>& keys) {
    const auto current = publishedOrder;
    for(const auto& key : current)
        if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasConnection(key)) unpublish(key);
    for(const auto& key : keys) {
        if(published.count(key) || key.size() < 4 || key[0] != 'L') continue;
        const size_t space = key.find(' ');
        if(space == std::string::npos) continue;
        publish(ofToInt(key.substr(1, space - 1)) - 1, key.substr(space + 1));
    }
}

void scMetroBox::refreshPublished(int li) {
    for(auto& [key, p] : published) {
        if(p->layer != li || p->updating) continue;
        const Field* f = findField(p->field);
        if(!f || f->kind == F_ACTION) continue;
        const float v = fieldValue(li, *f);
        if(f->kind == F_FLOAT || f->kind == F_TICKS) { if(p->pf.get() != v) p->pf.setWithoutEventNotifications(v); }
        else if(f->kind == F_BOOL) { if(p->pb.get() != (v >= 0.5f)) p->pb.setWithoutEventNotifications(v >= 0.5f); }
        else if(p->pi.get() != (int)v) p->pi.setWithoutEventNotifications((int)v);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Roles
// ════════════════════════════════════════════════════════════════════════════

const std::vector<std::string>& scMetroBox::builtInRoleNames() {
    static const std::vector<std::string> names = {"Kick", "Snare", "Hat", "Perc / Fill", "Free"};
    return names;
}

scMetroBox::Layer scMetroBox::builtInRole(int index) {
    Layer L;
    switch(index) {
        case 0: // Kick: subdivision, always on 1, long notes on the strong beats
            L.name = "Kick";
            L.generator = GEN_SUBDIVISION;
            L.division = 24;
            L.complexity = 0.45f;
            L.longOnStrong = 0.6f;
            L.anchorMode = ANCHOR_DOWNBEAT;
            L.durMode = DUR_METRIC;
            L.durShort = 6;
            L.durLong = 24;
            L.ampMetric = 0.4f;
            L.form = "AAAB";
            L.bStrength = 0.3f;
            L.renew = 0.25f;
            break;
        case 1: // Snare: backbeats, ghost notes before them, a few flams, a fill
            L.name = "Snare";
            L.division = 96;
            L.shift = 48;
            L.durTicks = 8;
            L.ghost = 0.3f;
            L.ghostSide = 0;
            L.ghostLevel = 0.3f;
            L.flam = 0.1f;
            L.fillTicks = 48;
            L.fillIntensity = 0.6f;
            L.phraseAccent = 0.3f;
            break;
        case 2: // Hat: metric-weighted 16ths in three tiers, rare ratchets
            L.name = "Hat";
            L.division = 12;
            L.density = 0.9f;
            L.weighting = 0.5f;
            L.durTicks = 3;
            L.ampMetric = 0.6f;
            L.ampMin = 0.2f;
            L.variation = 0.2f;
            L.tiers = 3;
            L.ratchet = 0.05f;
            L.ratchetCount = 2;
            L.groove = GROOVE_SWING16;
            L.swing = 0.56f;
            break;
        case 3: // Perc / Fill: weighted durations that push, filling the kick's gaps
            L.name = "Perc";
            L.generator = GEN_WEIGHTED;
            L.values = {0.75f, 0.5f, 0.25f};
            L.weights = {1.0f, 1.0f, 0.5f};
            L.durMode = DUR_LEGATO;
            L.push = 0.2f;
            L.crossBarline = 0.5f;
            L.relMode = REL_COMPLEMENT;
            L.relLane = 1;
            L.relAmount = 0.25f;
            L.accentHits = 3;
            L.accentAmount = 0.5f;
            break;
        default:
            L.name = "Free";
            break;
    }
    return L;
}

std::string scMetroBox::rolesDir() const {
    return ofToDataPath("Supercollider/MetroBox/Roles", true);
}

void scMetroBox::scanRoles() {
    userRoles.clear();
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(rolesDir(), ec))
        if(entry.is_regular_file(ec) && entry.path().extension() == ".json") userRoles.push_back(entry.path().stem().string());
    std::sort(userRoles.begin(), userRoles.end());
}

// A role brings rules, not a lane's state: its mute, genome (seed and
// mutations), liked pool and Keep stay.
void scMetroBox::applyRole(int li, const Layer& role) {
    Layer& L = layers[li];
    Layer next = role;
    next.mute = L.mute;
    next.seed = L.seed;
    next.ops = L.ops;
    next.liked = L.liked;
    next.keep = L.keep;
    next.keepPhrase = L.keepPhrase;
    next.repeat = L.repeat;
    L = next;
    engine.changed(li);
    refreshPublished(li);
}

void scMetroBox::saveRole(int li, const std::string& name) {
    if(name.empty()) return;
    std::error_code ec;
    fs::create_directories(rolesDir(), ec);
    ofJson j = layerToJson(layers[li], false);
    j["name"] = name;
    std::ofstream file(rolesDir() + "/" + name + ".json");
    file << j.dump(2);
    scanRoles();
}

ofJson scMetroBox::layerToJson(const Layer& L, bool withState) {
    ofJson j = {
        {"name", L.name}, {"generator", L.generator},
        {"cycleBeats", L.cycleBeats}, {"density", L.density},
        {"shift", L.shift}, {"division", L.division}, {"eucSteps", L.eucSteps},
        {"eucHits", L.eucHits}, {"eucRotation", L.eucRotation},
        {"values", L.values}, {"weights", L.weights},
        {"complexity", L.complexity}, {"longOnStrong", L.longOnStrong}, {"triplets", L.triplets},
        {"examples", L.examples},
        {"grouping", L.grouping}, {"feelBar", L.feelBar}, {"weighting", L.weighting},
        {"anchorMode", L.anchorMode}, {"anchorAmount", L.anchorAmount},
        {"push", L.push}, {"drag", L.drag}, {"dispUnit", L.dispUnit}, {"crossBarline", L.crossBarline},
        {"phraseLen", L.phraseLen}, {"form", L.form}, {"renew", L.renew}, {"bStrength", L.bStrength},
        {"fillTicks", L.fillTicks}, {"fillIntensity", L.fillIntensity}, {"buildUp", L.buildUp},
        {"phraseAccent", L.phraseAccent}, {"swell", L.swell},
        {"ghost", L.ghost}, {"ghostSide", L.ghostSide}, {"ghostLevel", L.ghostLevel},
        {"flam", L.flam}, {"flamTicks", L.flamTicks},
        {"ratchet", L.ratchet}, {"ratchetCount", L.ratchetCount}, {"ratchetUnit", L.ratchetUnit}, {"ratchetShape", L.ratchetShape},
        {"relMode", L.relMode}, {"relLane", L.relLane}, {"relAmount", L.relAmount}, {"respDelay", L.respDelay},
        {"chokeLane", L.chokeLane}, {"dynMode", L.dynMode}, {"dynLane", L.dynLane}, {"dynAmount", L.dynAmount},
        {"groove", L.groove}, {"swing", L.swing}, {"humanize", L.humanize},
        {"durMode", L.durMode}, {"durTicks", L.durTicks}, {"legato", L.legato},
        {"durShort", L.durShort}, {"durLong", L.durLong}, {"longHits", L.longHits}, {"longTicks", L.longTicks},
        {"level", L.level}, {"ampMin", L.ampMin}, {"ampMetric", L.ampMetric},
        {"variation", L.variation}, {"curve", L.curve}, {"tiers", L.tiers},
        {"accentHits", L.accentHits}, {"accentAmount", L.accentAmount}, {"accentsLong", L.accentsLong},
        {"mutations", L.mutations}, {"autoEvery", L.autoEvery}, {"autoChance", L.autoChance},
        {"repeat", L.repeat},
    };
    if(withState) {
        j["mute"] = L.mute;
        j["seed"] = L.seed;
        j["ops"] = opsToJson(L.ops);
        j["keep"] = L.keep;
        j["keepPhrase"] = L.keepPhrase;
        ofJson liked = ofJson::array();
        for(const auto& g : L.liked) liked.push_back({{"seed", g.seed}, {"ops", opsToJson(g.ops)}});
        j["liked"] = liked;
    }
    return j;
}

// Settings missing from the JSON take the plain defaults, so an older preset
// sounds as it was saved.
scMetroBox::Layer scMetroBox::layerFromJson(const ofJson& j) {
    Layer L;
    auto get = [&](const char* key, auto& field) {
        try { if(j.contains(key)) field = j[key].get<std::decay_t<decltype(field)>>(); } catch(...) {}
    };
    get("name", L.name); get("mute", L.mute); get("generator", L.generator); get("cycleBeats", L.cycleBeats);
    get("seed", L.seed); get("density", L.density); get("shift", L.shift); get("division", L.division);
    get("eucSteps", L.eucSteps); get("eucHits", L.eucHits); get("eucRotation", L.eucRotation);
    get("values", L.values); get("weights", L.weights);
    get("complexity", L.complexity); get("longOnStrong", L.longOnStrong); get("triplets", L.triplets);
    get("examples", L.examples);
    get("grouping", L.grouping); get("feelBar", L.feelBar); get("weighting", L.weighting);
    get("anchorMode", L.anchorMode); get("anchorAmount", L.anchorAmount);
    get("push", L.push); get("drag", L.drag); get("dispUnit", L.dispUnit); get("crossBarline", L.crossBarline);
    get("phraseLen", L.phraseLen); get("form", L.form); get("renew", L.renew); get("bStrength", L.bStrength);
    get("fillTicks", L.fillTicks); get("fillIntensity", L.fillIntensity); get("buildUp", L.buildUp);
    get("phraseAccent", L.phraseAccent); get("swell", L.swell);
    get("ghost", L.ghost); get("ghostSide", L.ghostSide); get("ghostLevel", L.ghostLevel);
    get("flam", L.flam); get("flamTicks", L.flamTicks);
    get("ratchet", L.ratchet); get("ratchetCount", L.ratchetCount); get("ratchetUnit", L.ratchetUnit); get("ratchetShape", L.ratchetShape);
    get("relMode", L.relMode); get("relLane", L.relLane); get("relAmount", L.relAmount); get("respDelay", L.respDelay);
    get("chokeLane", L.chokeLane); get("dynMode", L.dynMode); get("dynLane", L.dynLane); get("dynAmount", L.dynAmount);
    get("groove", L.groove); get("swing", L.swing); get("humanize", L.humanize);
    get("durMode", L.durMode); get("durTicks", L.durTicks); get("legato", L.legato);
    get("durShort", L.durShort); get("durLong", L.durLong); get("longHits", L.longHits); get("longTicks", L.longTicks);
    get("level", L.level); get("ampMin", L.ampMin); get("ampMetric", L.ampMetric);
    get("variation", L.variation); get("curve", L.curve); get("tiers", L.tiers);
    get("accentHits", L.accentHits); get("accentAmount", L.accentAmount); get("accentsLong", L.accentsLong);
    get("mutations", L.mutations); get("autoEvery", L.autoEvery); get("autoChance", L.autoChance);
    get("repeat", L.repeat);
    get("keep", L.keep); get("keepPhrase", L.keepPhrase);
    if(j.contains("ops")) L.ops = opsFromJson(j["ops"]);
    if(j.contains("liked") && j["liked"].is_array())
        for(const auto& g : j["liked"]) {
            Genome genome;
            try { genome.seed = g.value("seed", 0); } catch(...) {}
            if(g.contains("ops")) genome.ops = opsFromJson(g["ops"]);
            L.liked.push_back(genome);
        }

    // Ranges
    L.generator    = std::clamp(L.generator, 0, (int)generatorNames.size() - 1);
    L.cycleBeats   = std::clamp(L.cycleBeats, 1, MAX_CYCLE_BEATS);
    L.division     = std::max(1, L.division);
    L.eucSteps     = std::clamp(L.eucSteps, 1, 64);
    L.anchorMode   = std::clamp(L.anchorMode, 0, (int)anchorNames.size() - 1);
    L.dispUnit     = std::max(1, L.dispUnit);
    L.phraseLen    = std::clamp(L.phraseLen, 1, 16);
    L.ghostSide    = std::clamp(L.ghostSide, 0, 2);
    L.flamTicks    = std::max(1, L.flamTicks);
    L.ratchetCount = std::clamp(L.ratchetCount, 2, 6);
    L.ratchetUnit  = std::max(1, L.ratchetUnit);
    L.ratchetShape = std::clamp(L.ratchetShape, 0, 2);
    L.relMode      = std::clamp(L.relMode, 0, (int)relationNames.size() - 1);
    L.relLane      = std::clamp(L.relLane, 0, MAX_LAYERS);
    L.chokeLane    = std::clamp(L.chokeLane, 0, MAX_LAYERS);
    L.dynMode      = std::clamp(L.dynMode, 0, 2);
    L.dynLane      = std::clamp(L.dynLane, 0, MAX_LAYERS);
    L.groove       = std::clamp(L.groove, 0, (int)grooveNames.size() - 1);
    L.durMode      = std::clamp(L.durMode, 0, 2);
    L.durTicks     = std::max(1, L.durTicks);
    L.durShort     = std::max(1, L.durShort);
    L.durLong      = std::max(1, L.durLong);
    L.longTicks    = std::max(1, L.longTicks);
    L.tiers        = std::clamp(L.tiers, 0, 5);
    L.mutations    = std::clamp(L.mutations, 1, 6);
    L.repeat       = std::clamp(L.repeat, 0, (int)repeatNames.size() - 1);
    L.autoEvery    = std::clamp(L.autoEvery, 1, 16);
    if(L.values.empty()) L.values = {1.0f};
    if(L.values.size() > MAX_VALUES) L.values.resize(MAX_VALUES);
    L.weights.resize(L.values.size(), 1.0f);
    return L;
}

// ════════════════════════════════════════════════════════════════════════════
// Construction
// ════════════════════════════════════════════════════════════════════════════

scMetroBox::scMetroBox() : scNode("MetroBox"), layers(engine.layers) {
    anchoredCycleTicks.fill(-1);
    lastPhrase.fill(NO_CYCLE);
    phrasesSinceAuto.fill(0);
    // A starting groove from the roles: kick, snare with ghosts and fills,
    // swung hats that duck under the kick, perc filling the kick's gaps.
    for(int i = 0; i < 4; i++) layers[i] = builtInRole(i);
    layers[2].dynMode = DYN_DUCK;
    layers[2].dynLane = 1;
    layers[2].dynAmount = 0.4f;
    for(int i = 4; i < MAX_LAYERS; i++) layers[i].name = "Layer " + ofToString(i + 1);
    engine.onChanged = [this](int lane) {
        invalidateSlots(lane);
        if(lane >= 0 && lane < MAX_LAYERS && anchoredCycleTicks[lane] != layers[lane].cycleTicks()) follower.requestAnchor();
    };
}

scMetroBox::~scMetroBox() {
    listeners.unsubscribeAll();
    published.clear();
    engine.onChanged = nullptr;
    for(auto& [server, st] : servers) {
        if(st.synth) { st.synth->free(); delete st.synth; }
        if(st.buffer) {
            if(bufferLive(server, st)) st.buffer->free();
            delete st.buffer;
        }
    }
    servers.clear();
}

void scMetroBox::setup() {
    description = "Generative rhythm locked to the transport: up to 8 lanes, each "
                  "with its own cycle (polymeter), generator (Grid, Euclidean, Weighted "
                  "durations, Subdivision, Markov), metric rules, phrases (form, fills, "
                  "build-ups), ornaments, groove and relations to the lanes above. "
                  "Evolve them with Mutate / Re-roll / Keep / Undo / Like / crossover. "
                  "Each lane has mono Trig, Gate and Amp outputs. Seeded per cycle, so "
                  "the window previews exactly what will play. Open it with Show; "
                  "right-click a control there to type a value or publish it here.";

    addParameter(showWindow.set("Show", false));
    addParameter(numLayers.set("Layers", 4, 1, MAX_LAYERS));
    addParameter(globalSeed.set("Seed", 1, 0, 99999));
    addParameter(densityIn.set("Density", {1.0f}, {0.0f}, {1.0f}));
    addParameter(trigAmp.set("Trig Amp", false));

    engine.numLayers = numLayers;
    engine.globalSeed = globalSeed;
    engine.densityScale = densityIn.get();
    mainBarBeats = beatsPerBar();
    engine.mainBarBeats = mainBarBeats;
    syncOutputPorts();
    scanRoles();

    listeners.push(numLayers.newListener([this](int& n) {
        engine.numLayers = n;
        syncOutputPorts();
        engine.changedAll();
        for(auto& [server, st] : servers) if(st.synth) st.synth->set("layers", (float)n);
    }));
    listeners.push(globalSeed.newListener([this](int& s) { engine.globalSeed = s; engine.changedAll(); }));
    listeners.push(densityIn.newListener([this](vector<float>& d) { engine.densityScale = d; engine.changedAll(); }));
    listeners.push(trigAmp.newListener([this](bool&) {
        for(auto& [server, st] : servers) if(st.synth) st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f);
    }));
    listeners.push(resendParams.newListener([this]() {
        for(auto& [server, st] : servers) sendStaticControls(st);
        follower.requestAnchor();
    }));
}

// ════════════════════════════════════════════════════════════════════════════
// Ports
// ════════════════════════════════════════════════════════════════════════════

std::string scMetroBox::busName(int index) {
    static const char* kinds[] = {"trigout", "gateout", "ampout"};
    return std::string(kinds[index % OUTPUTS_PER_LAYER]) + ofToString(index / OUTPUTS_PER_LAYER + 1);
}

void scMetroBox::syncOutputPorts() {
    const int n = std::clamp(numLayers.get(), 1, MAX_LAYERS);
    while(portLayers < n) {
        const std::string lane = ofToString(portLayers + 1);
        scNode::addOutput("Trig " + lane);
        scNode::addOutput("Gate " + lane);
        scNode::addOutput("Amp " + lane);
        portLayers++;
    }
    while(portLayers > n) {
        const int first = (portLayers - 1) * OUTPUTS_PER_LAYER;
        for(int k = OUTPUTS_PER_LAYER - 1; k >= 0; k--) scNode::removeOutput(first + k);
        // A removed port's bus may be handed to another node: forget it, so it
        // is not reused if the lane comes back (the synth silences the lane).
        for(auto& [server, st] : servers)
            for(int k = 0; k < OUTPUTS_PER_LAYER; k++) st.outputBuses.erase(first + k);
        portLayers--;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Repeat Evolve: every N loops, a mutation with some chance
// ════════════════════════════════════════════════════════════════════════════

void scMetroBox::autoEvolve() {
    const auto t = getFrameTransportState().current;
    for(int li = 0; li < numLayers; li++) {
        const Layer& L = layers[li];
        const int64_t now = engine.cycleAt(li, t.beatPosition);
        const int64_t phrase = engine.phraseOf(li, now);
        if(!t.isPlaying || L.repeat != REPEAT_EVOLVE || L.keep) { lastPhrase[li] = phrase; continue; }
        if(lastPhrase[li] != NO_CYCLE && phrase == lastPhrase[li] + 1 && ++phrasesSinceAuto[li] >= L.autoEvery) {
            phrasesSinceAuto[li] = 0;
            const uint32_t r = (uint32_t)hashOf((uint64_t)ofGetElapsedTimeMicros(), ++autoCounter, (uint64_t)li);
            if(unit(r) < L.autoChance) engine.mutate(li, now, r);
        }
        lastPhrase[li] = phrase;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Onset buffer
// ════════════════════════════════════════════════════════════════════════════

bool scMetroBox::bufferLive(ofxSCServer* server, const ServerState& st) const {
    if(!server || !st.buffer || st.buffer->index < 0 || st.buffer->index >= (int)server->buffers.size()) return false;
    return server->buffers[st.buffer->index] == st.buffer;
}

void scMetroBox::invalidateSlots(int li) {
    for(auto& [server, st] : servers) {
        for(int i = 0; i < MAX_LAYERS; i++) {
            if(li >= 0 && i != li) continue;
            st.slotCycle[i].fill(NO_CYCLE);
            st.slotCycleTicks[i] = 0;
        }
    }
}

// The cycles the player may read soon, most urgent first: the current one,
// the next, the previous (the server plays a latency behind this clock), the
// one after. Cycles after the loop end are taken from the loop start.
std::vector<int64_t> scMetroBox::wantedCycles(int li, const ofxOceanodeTransportState& t) const {
    const int cT = layers[li].cycleTicks();
    const int64_t c = floorDiv(tickAt(t.beatPosition), cT);
    auto cycleAtStartOf = [&](int64_t cycle) {
        double beat = (double)cycle * cT / TPB;
        if(t.loopActive() && beat >= t.loopEndBeat - 1e-9) {
            const double len = t.loopEndBeat - t.loopStartBeat;
            beat = t.loopStartBeat + std::fmod(beat - t.loopEndBeat, len);
        }
        return floorDiv(tickAt(beat), cT);
    };
    std::vector<int64_t> out = {c, cycleAtStartOf(c + 1), c - 1, cycleAtStartOf(c + 2)};
    std::vector<int64_t> unique;
    for(int64_t w : out) if(std::find(unique.begin(), unique.end(), w) == unique.end()) unique.push_back(w);
    return unique;
}

void scMetroBox::writeSlot(ServerState& st, int li, int64_t cycle) {
    const int cT = layers[li].cycleTicks();
    const int slot = (int)positiveMod64(cycle, SLOTS);
    std::vector<float> data(cT * 2, 0.0f);
    if(li < numLayers) {
        for(const auto& o : engine.realizeCycle(li, cycle)) {
            if(o.tick < 0 || o.tick >= cT) continue;
            data[o.tick * 2]     = o.amp;
            data[o.tick * 2 + 1] = (float)o.dur;
        }
    }
    const int firstSample = (li * SLOTS * MAX_CYCLE_TICKS + slot * cT) * 2;
    for(int offset = 0; offset < (int)data.size(); offset += floatsPerMessage) {
        const int count = std::min(floatsPerMessage, (int)data.size() - offset);
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(st.buffer->index);
        m.addIntArg(firstSample + offset);
        m.addIntArg(count);
        for(int i = 0; i < count; i++) m.addFloatArg(data[offset + i]);
        st.buffer->server->sendMsg(m);
    }
    st.slotCycle[li][slot] = cycle;
}

void scMetroBox::fillBuffers() {
    const auto t = getFrameTransportState().current;
    for(auto& [server, st] : servers) {
        if(st.allocPending || !bufferLive(server, st)) continue;
        for(int li = 0; li < MAX_LAYERS; li++) {
            const int cT = layers[li].cycleTicks();
            if(st.slotCycleTicks[li] != cT) {
                st.slotCycle[li].fill(NO_CYCLE);
                st.slotCycleTicks[li] = cT;
            }
            std::array<bool, SLOTS> claimed{};
            for(int64_t w : wantedCycles(li, t)) {
                const int slot = (int)positiveMod64(w, SLOTS);
                if(claimed[slot]) continue;
                claimed[slot] = true;
                if(st.slotCycle[li][slot] != w) writeSlot(st, li, w);
            }
        }
    }
}

void scMetroBox::sendStaticControls(ServerState& st) {
    if(!st.synth) return;
    if(st.buffer) st.synth->set("buf", st.buffer->index);
    st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f);
    st.synth->set("layers", (float)numLayers.get());
    std::vector<float> mutes(MAX_LAYERS);
    for(int i = 0; i < MAX_LAYERS; i++) mutes[i] = layers[i].mute ? 1.0f : 0.0f;
    st.synth->set("mute", mutes);
    for(auto& [index, bus] : st.outputBuses)
        if(index >= 0 && index < MAX_LAYERS * OUTPUTS_PER_LAYER) st.synth->set(busName(index), bus);
}

// ════════════════════════════════════════════════════════════════════════════
// Transport
// ════════════════════════════════════════════════════════════════════════════

// As in the Poly Arpeggiator: the player only adds the (small) number of ticks
// elapsed since an anchor to \base, the absolute tick at floor(anchorPos)
// modulo each lane's buffer period, computed here in 64-bit. StepClock keeps
// \anchorPos below its 2048-tick period for float32 precision.
void scMetroBox::sendTransportAnchors() {
    for(int i = 0; i < MAX_LAYERS; i++)
        if(anchoredCycleTicks[i] != layers[i].cycleTicks()) { follower.requestAnchor(); break; }

    const auto anchors = follower.poll(getFrameTransportState());
    if(anchors.empty()) return;
    const int64_t period = (int64_t)std::llround(stepClock.getPeriod());

    for(const auto& a : anchors) {
        const double tickPos = a.beat * TPB;
        const scTransportSync::StepAnchor sa = stepClock.make(tickPos, a.discontinuity, a.playing);
        const bool hard = sa.hard || forceHardAnchor;
        forceHardAnchor = false;

        const int64_t k = (int64_t)std::floor(tickPos);
        int64_t delta = (int64_t)std::floor(sa.pos) - positiveMod64(k, period);
        if(delta > period / 2) delta -= period;
        else if(delta < -period / 2) delta += period;
        const int64_t absTick = k + delta;

        std::vector<float> cycleTicks(MAX_LAYERS), bases(MAX_LAYERS);
        for(int i = 0; i < MAX_LAYERS; i++) {
            const int cT = layers[i].cycleTicks();
            cycleTicks[i] = (float)cT;
            bases[i] = (float)positiveMod64(absTick, (int64_t)cT * SLOTS);
            anchoredCycleTicks[i] = cT;
        }

        anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
        if(hard) anchorHardCounter = anchorHardCounter % (1 << 20) + 1;

        bool pending = false;
        scTransportSync::AnchorScope scope(a);
        for(auto& [server, st] : servers) {
            auto* s = st.synth;
            if(!s) continue;
            // Sets stored on a synth not confirmed yet would go out later
            // without the timetag: anchor it (hard) next frame instead.
            if(!s->isCreated()) { pending = true; continue; }
            s->set("bpm",        a.bpm);
            s->set("run",        a.playing ? 1.0f : 0.0f);
            s->set("anchorPos",  (float)sa.pos);
            s->set("anchorFire", sa.fire ? 1.0f : 0.0f);
            s->set("cycleTicks", cycleTicks);
            s->set("base",       bases);
            if(hard) s->set("anchorHard", (float)anchorHardCounter);
            s->set("anchorId",   (float)anchorIdCounter);   // last: starts the new anchor
        }
        if(pending) { forceHardAnchor = true; follower.requestAnchor(); }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update / draw
// ════════════════════════════════════════════════════════════════════════════

void scMetroBox::update(ofEventArgs&) {
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : servers) {
        if(st.allocPending && (!server->isNRTSyncPending() || now - st.allocTime > allocTimeout)) {
            st.allocPending = false;
            for(int i = 0; i < MAX_LAYERS; i++) { st.slotCycle[i].fill(NO_CYCLE); st.slotCycleTicks[i] = 0; }
        }
    }
    // The main bar's strength map follows the timeline's meter
    const double bpb = beatsPerBar();
    if(std::abs(bpb - mainBarBeats) > 1e-9) {
        mainBarBeats = bpb;
        engine.mainBarBeats = bpb;
        engine.changedAll();
    }
    autoEvolve();
    for(int li = 0; li < MAX_LAYERS; li++) engine.forgetHighlightsBefore(li, nowCycle(li) - 1);
    fillBuffers();
    sendTransportAnchors();
}

void scMetroBox::draw(ofEventArgs&) {
    if(showWindow) drawWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// SC node
// ════════════════════════════════════════════════════════════════════════════

void scMetroBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    ServerState& st = servers[server];
    if(st.synth) { st.synth->free(); delete st.synth; }
    st.synth = new ofxSCSynth("MetroBoxPlayer", server);
    if(!bufferLive(server, st)) {
        delete st.buffer;
        st.buffer = new ofxSCBuffer(MAX_LAYERS * SLOTS * MAX_CYCLE_TICKS, 2, server);
        st.buffer->alloc();
        st.allocPending = true;
        st.allocTime = ofGetElapsedTimef();
        server->requestNRTSync();
        for(int i = 0; i < MAX_LAYERS; i++) { st.slotCycle[i].fill(NO_CYCLE); st.slotCycleTicks[i] = 0; }
    }
}

void scMetroBox::createSynth(ofxSCServer* server) {
    auto it = servers.find(server);
    if(it == servers.end() || !it->second.synth) return;
    resendParams.notify();
    // Hold until the next update() sends a hard anchor
    it->second.synth->set("run", 0.0f);
    forceHardAnchor = true;
    follower.requestAnchor();
    it->second.synth->createAndRun(0, 1, getActive());
}

void scMetroBox::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = servers.find(server);
    if(it == servers.end() || !it->second.synth) return;
    sendStaticControls(it->second);
    it->second.synth->moveBefore(nodeID);
}

// Only the synth goes: the onset buffer (and the cycles written in it) stays
// for the synth the graph builds next, so a rebuild does not leave the player
// reading an empty buffer while a new one is allocated and filled. The bus
// routing is the graph's to give again. The buffer is freed with the node.
void scMetroBox::free(ofxSCServer* server) {
    auto it = servers.find(server);
    if(it == servers.end()) return;
    ServerState& st = it->second;
    if(st.synth) { st.synth->free(); delete st.synth; st.synth = nullptr; }
    st.outputBuses.clear();
}

void scMetroBox::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server || index < 0 || index >= MAX_LAYERS * OUTPUTS_PER_LAYER) return;
    ServerState& st = servers[server];
    st.outputBuses[index] = bus;
    if(st.synth) st.synth->set(busName(index), bus);
}

int scMetroBox::getOutputBusIndex(ofxSCServer* server, int index) {
    auto it = servers.find(server);
    if(it == servers.end()) return -1;
    auto b = it->second.outputBuses.find(index);
    return b != it->second.outputBuses.end() ? b->second : -1;
}

int scMetroBox::getLastSynthID(ofxSCServer* server) {
    auto it = servers.find(server);
    return it != servers.end() && it->second.synth ? it->second.synth->nodeID : -1;
}

void scMetroBox::activate() {
    for(auto& [server, st] : servers) if(st.synth) st.synth->run(true);
    // It was paused: re-position it without firing the missed ticks
    forceHardAnchor = true;
    follower.requestAnchor();
}

void scMetroBox::deactivate() {
    for(auto& [server, st] : servers) if(st.synth) st.synth->run(false);
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scMetroBox::presetSave(ofJson &json) {
    ofJson arr = ofJson::array();
    for(const auto& L : layers) arr.push_back(layerToJson(L));
    json["MetroBoxLayers"] = arr;
    json["MetroBoxPublished"] = publishedOrder;
}

// The published parameters must exist before Oceanode sets their values and
// restores their connections.
void scMetroBox::loadBeforeConnections(ofJson &json) {
    if(!json.contains("MetroBoxPublished") || !json["MetroBoxPublished"].is_array()) return;
    try { syncPublished(json["MetroBoxPublished"].get<std::vector<std::string>>()); } catch(...) {}
}

void scMetroBox::presetRecallBeforeSettingParameters(ofJson &json) {
    loadBeforeConnections(json);
}

void scMetroBox::presetRecallAfterSettingParameters(ofJson &json) {
    if(!json.contains("MetroBoxLayers") || !json["MetroBoxLayers"].is_array()) return;
    const auto& arr = json["MetroBoxLayers"];
    for(int i = 0; i < MAX_LAYERS && i < (int)arr.size(); i++) layers[i] = layerFromJson(arr[i]);
    engine.changedAll();
    for(int i = 0; i < MAX_LAYERS; i++) refreshPublished(i);
    for(auto& [server, st] : servers) sendStaticControls(st);
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

double scMetroBox::beatsPerBar() const {
#if OFXOCEANODESC_HAS_TIMELINE
    if(auto* container = getHostContainer())
        return std::max(0.25, container->getTimelineManager().getBeatsPerBar());
#endif
    return 4.0;
}

void scMetroBox::drawWindow() {
    const std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "MetroBox " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 820.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open)) {
        ImGui::PushID(this);

        int n = numLayers.get();
        ImGui::SetNextItemWidth(120);
        if(ImGui::SliderInt("Layers", &n, 1, MAX_LAYERS)) numLayers = n;
        ImGui::SameLine();
        int seed = globalSeed.get();
        ImGui::SetNextItemWidth(120);
        if(ImGui::InputInt("Seed", &seed)) globalSeed = std::clamp(seed, 0, 99999);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Bars shown", &viewBars, 1, 16);
        ImGui::SameLine();
        bool ta = trigAmp.get();
        if(ImGui::Checkbox("Trig Amp", &ta)) trigAmp = ta;

        drawTimeline(std::max(200.0f, ImGui::GetContentRegionAvail().x));

        ImGui::Separator();
        selectedLayer = std::clamp(selectedLayer, 0, numLayers - 1);
        if(ImGui::BeginChild("##editor", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar))
            drawLayerEditor(selectedLayer);
        ImGui::EndChild();

        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

void scMetroBox::drawTimeline(float width) {
    constexpr float labelW = 150.0f, rowH = 44.0f, rulerH = 18.0f;
    const auto t = getFrameTransportState().current;
    const double bpb = beatsPerBar();
    const double viewBeats = viewBars * bpb;
    // Continuous scroll: the playhead stays put and the rhythm moves left
    const double viewStart = t.beatPosition - viewBeats * playheadAt;
    const double viewEnd = viewStart + viewBeats;
    const int rows = numLayers.get();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float x0 = origin.x + labelW, w = std::max(50.0f, width - labelW);
    const float yRows = origin.y + rulerH;
    const float yEnd = yRows + rows * rowH;
    auto X = [&](double beat) { return x0 + (float)((beat - viewStart) / viewBeats) * w; };

    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 beatLine = ImGui::GetColorU32(ImGuiCol_Border, 0.35f);
    const ImU32 barLine = ImGui::GetColorU32(ImGuiCol_Border, 1.0f);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 textDim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 fillShade = ImGui::GetColorU32(ImGuiCol_Text, 0.06f);
    const ImU32 cycleLine = ImGui::GetColorU32(ImGuiCol_Text, 0.12f);
    const ImU32 cycleLineStrong = ImGui::GetColorU32(ImGuiCol_Text, 0.35f);

    dl->AddRectFilled(ImVec2(x0, yRows), ImVec2(x0 + w, yEnd), bg);
    dl->PushClipRect(ImVec2(x0, origin.y), ImVec2(x0 + w, yEnd), true);

    // Ruler and beat / bar lines
    for(double beat = std::ceil(viewStart); beat <= viewEnd; beat += 1.0) {
        const double inBar = std::fmod(std::fmod(beat, bpb) + bpb, bpb);
        const bool bar = inBar < 1e-6 || bpb - inBar < 1e-6;
        const float x = X(beat);
        dl->AddLine(ImVec2(x, yRows), ImVec2(x, yEnd), bar ? barLine : beatLine);
        if(bar && beat >= 0) dl->AddText(ImVec2(x + 3, origin.y + 1), textDim, ofToString((int)std::lround(beat / bpb) + 1).c_str());
    }

    for(int li = 0; li < rows; li++) {
        const Layer& L = layers[li];
        const float y0 = yRows + li * rowH, yB = y0 + rowH - 4.0f;
        const float dim = L.mute ? 0.3f : 1.0f;
        const int cT = L.cycleTicks();
        const int n = std::max(1, L.phraseLen);
        const int64_t first = floorDiv(tickAt(viewStart), cT);
        const int64_t last = floorDiv(tickAt(viewEnd) - 1, cT);
        for(int64_t c = std::max<int64_t>(first, 0); c <= last && c - first < 256; c++) {
            const double cycleBeat = (double)c * cT / TPB;
            // Cycle starts: thin neutral lines across the row (lane colours
            // are only for pulses); phrase starts stronger, with a notch on top.
            // Fills shaded.
            const bool phraseStart = positiveMod64(c, n) == 0;
            if(cycleBeat >= viewStart) {
                const float cx = X(cycleBeat);
                dl->AddLine(ImVec2(cx, y0 + 1), ImVec2(cx, y0 + rowH - 1), phraseStart ? cycleLineStrong : cycleLine, 1.0f);
                if(phraseStart)
                    dl->AddTriangleFilled(ImVec2(cx - 4, y0 + 1), ImVec2(cx + 4, y0 + 1), ImVec2(cx, y0 + 6), cycleLineStrong);
            }
            if(L.fillTicks > 0 && positiveMod64(c, n) == n - 1) {
                const double fs = cycleBeat + (double)(cT - L.fillTicks) / TPB;
                dl->AddRectFilled(ImVec2(X(fs), y0 + 1), ImVec2(X(cycleBeat + (double)cT / TPB), y0 + rowH - 1), fillShade);
            }
            const auto changed = engine.changedTicks(li, c);
            for(const auto& o : engine.realizeCycle(li, c)) {
                const double ob = cycleBeat + (double)o.tick / TPB;
                if(ob + (double)o.dur / TPB < viewStart || ob >= viewEnd) continue;
                const bool past = ob < t.beatPosition;
                const float alpha = (past ? 0.45f : 1.0f) * dim;
                const float x = X(ob);
                const float xe = X(ob + (double)o.dur / TPB);
                dl->AddRectFilled(ImVec2(x, yB - 4), ImVec2(std::max(x + 1, xe), yB), layerColor(li, 0.35f * alpha));
                const float thick = o.kind == KIND_MAIN ? 2.0f : 1.0f;
                const float top = yB - o.amp * (rowH - 14);
                dl->AddLine(ImVec2(x, yB), ImVec2(x, top), layerColor(li, alpha), thick);
                // Hits a mutation just brought, highlighted for a phrase
                if(std::find(changed.begin(), changed.end(), o.tick) != changed.end())
                    dl->AddCircleFilled(ImVec2(x, top - 3), 3.0f, ImGui::GetColorU32(ImGuiCol_Text, alpha));
            }
        }
        dl->AddLine(ImVec2(x0, y0 + rowH), ImVec2(x0 + w, y0 + rowH), beatLine);
    }

    // Playhead
    const float px = X(t.beatPosition);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, yEnd), ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 1.5f);
    dl->PopClipRect();

    // Lane labels: select the lane, M toggles mute
    for(int li = 0; li < rows; li++) {
        const Layer& L = layers[li];
        const float y0 = yRows + li * rowH;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
        ImGui::PushID(li);
        if(li == selectedLayer) dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x0 - 4, y0 + rowH - 2), ImGui::GetColorU32(ImGuiCol_Header));
        if(ImGui::InvisibleButton("##sel", ImVec2(labelW - 34, rowH - 2))) selectedLayer = li;
        dl->AddRectFilled(ImVec2(origin.x + 4, y0 + 6), ImVec2(origin.x + 8, y0 + rowH - 8), layerColor(li, L.mute ? 0.3f : 1.0f));
        dl->AddText(ImVec2(origin.x + 14, y0 + 6), L.mute ? textDim : text, (ofToString(li + 1) + " " + L.name + (L.keep ? " (kept)" : L.repeat == REPEAT_LOOP ? " (loop)" : L.repeat == REPEAT_EVOLVE ? " (evolving)" : "")).c_str());
        const std::string sub = generatorNames[L.generator].substr(0, 11) + ", " + ofToString(L.cycleBeats) + " beats";
        dl->AddText(ImVec2(origin.x + 14, y0 + 22), textDim, sub.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x0 - 32, y0 + 10));
        if(L.mute) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        if(ImGui::SmallButton("M")) setFieldValue(li, *findField("Mute"), L.mute ? 0.0f : 1.0f);
        if(L.mute) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, yEnd + 6));
    ImGui::Dummy(ImVec2(width, 1));
}

void scMetroBox::drawLayerEditor(int li) {
    Layer& L = layers[li];
    ImGui::PushID(1000 + li);

    // ── Header: name, role, generator, cycle, seed ──
    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", L.name.c_str());
    ImGui::SetNextItemWidth(150);
    if(ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) L.name = nameBuf;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if(ImGui::BeginCombo("Role", "Apply a role...")) {
        const auto& names = builtInRoleNames();
        for(int r = 0; r < (int)names.size(); r++)
            if(ImGui::Selectable(names[r].c_str())) applyRole(li, builtInRole(r));
        if(!userRoles.empty()) ImGui::Separator();
        for(const auto& name : userRoles) {
            if(ImGui::Selectable(name.c_str())) {
                std::ifstream file(rolesDir() + "/" + name + ".json");
                ofJson j;
                try { file >> j; applyRole(li, layerFromJson(j)); } catch(...) {}
            }
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Loads a set of rules into this lane (its mute, genome, liked pool and Keep stay).");
    ImGui::SameLine();
    if(ImGui::Button("Save role")) {
        std::snprintf(roleNameBuf, sizeof(roleNameBuf), "%s", L.name.c_str());
        ImGui::OpenPopup("##saveRole");
    }
    if(ImGui::BeginPopup("##saveRole")) {
        ImGui::TextDisabled("Save this lane's rules as a role");
        ImGui::SetNextItemWidth(200);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rolename", roleNameBuf, sizeof(roleNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        if((ImGui::Button("Save") || enter) && roleNameBuf[0]) { saveRole(li, roleNameBuf); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    drawField(li, "Generator");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    drawField(li, "Cycle", "Cycle (beats)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    drawField(li, "Layer seed");

    // ── Evolution ──
    drawField(li, "Mutate");
    ImGui::SameLine(); drawField(li, "Re-roll");
    ImGui::SameLine(); drawField(li, "Undo");
    ImGui::SameLine(); drawField(li, "Redo");
    ImGui::SameLine(); drawField(li, "Keep");
    ImGui::SameLine(); drawField(li, "Like");
    ImGui::SameLine();
    ImGui::TextDisabled("%d liked, %d mutations", (int)L.liked.size(), (int)L.ops.size());
    if(L.liked.size() >= 2) {
        ImGui::SameLine();
        std::vector<std::string> names;
        for(int i = 0; i < (int)L.liked.size(); i++) names.push_back("Liked " + ofToString(i + 1));
        crossA = std::clamp(crossA, 0, (int)names.size() - 1);
        crossB = std::clamp(crossB, 0, (int)names.size() - 1);
        ImGui::SetNextItemWidth(90); stringCombo("##ca", crossA, names);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90); stringCombo("##cb", crossB, names);
        ImGui::SameLine();
        ImGui::BeginDisabled(L.keep);
        if(ImGui::Button("Crossover")) { engine.crossover(li, crossA, crossB, nowCycle(li)); refreshPublished(li); }
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("First liked version, with the second half of each cycle from the other.");
    }
    ImGui::SetNextItemWidth(100); drawField(li, "Mutations");

    constexpr float colW = 330.0f, itemW = 170.0f;
    if(ImGui::BeginTable("##editor", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthFixed, colW);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthFixed, colW);
        ImGui::TableSetupColumn("##c3", ImGuiTableColumnFlags_WidthFixed, colW);
        ImGui::TableNextRow();

        // ── Column 1: Placement, Ornaments, Groove ──
        ImGui::TableSetColumnIndex(0);
        ImGui::PushItemWidth(itemW);
        heading("Placement");
        drawField(li, "Density");
        drawField(li, "Shift");
        if(L.generator == GEN_GRID || L.generator == GEN_EUCLIDEAN)
            if(divisionCombo("Step", L.division, divisionOptions, IM_ARRAYSIZE(divisionOptions))) engine.changed(li);
        if(L.generator == GEN_EUCLIDEAN) {
            drawField(li, "Steps");
            drawField(li, "Hits");
            drawField(li, "Rotation");
        }
        if(L.generator == GEN_SUBDIVISION) {
            if(divisionCombo("Finest", L.division, divisionOptions, IM_ARRAYSIZE(divisionOptions))) engine.changed(li);
            drawField(li, "Complexity");
            drawField(li, "Long on strong");
            drawField(li, "Triplets");
        }
        if(L.generator == GEN_MARKOV) {
            if(divisionCombo("Step", L.division, divisionOptions, IM_ARRAYSIZE(divisionOptions))) engine.changed(li);
            ImGui::TextDisabled("Examples (x = hit, one per word, 16ths)");
            static char examples[2048];
            std::snprintf(examples, sizeof(examples), "%s", L.examples.c_str());
            if(ImGui::InputTextMultiline("##examples", examples, sizeof(examples), ImVec2(colW - 20, 70))) {
                L.examples = examples;
                engine.changed(li);
            }
            if(ImGui::BeginCombo("Capture from", "a lane...")) {
                for(int j = 0; j < numLayers; j++) {
                    if(j == li) continue;
                    if(ImGui::Selectable((ofToString(j + 1) + " " + layers[j].name).c_str())) {
                        L.examples = engine.captureExamples(j, nowCycle(j), 4);
                        engine.changed(li);
                    }
                }
                ImGui::EndCombo();
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Learn from what that lane plays in its next four cycles.");
        }
        if(L.generator == GEN_WEIGHTED) {
            ImGui::TextDisabled("Interval (beats) / weight");
            bool changed = false;
            for(int j = 0; j < (int)L.values.size(); j++) {
                ImGui::PushID(j);
                ImGui::SetNextItemWidth(80);
                if(ImGui::DragFloat("##v", &L.values[j], 0.25f / TPB, 1.0f / TPB, (float)MAX_CYCLE_BEATS, "%.3f")) {
                    L.values[j] = std::max(1.0f / TPB, std::round(L.values[j] * TPB) / TPB);
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                changed |= ImGui::SliderFloat("##w", &L.weights[j], 0.0f, 1.0f);
                ImGui::SameLine();
                if(ImGui::SmallButton("x") && L.values.size() > 1) {
                    L.values.erase(L.values.begin() + j);
                    L.weights.erase(L.weights.begin() + j);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if((int)L.values.size() < MAX_VALUES && ImGui::SmallButton("+ interval")) {
                L.values.push_back(0.5f);
                L.weights.push_back(1.0f);
                changed = true;
            }
            if(changed) engine.changed(li);
        }
        heading("Ornaments");
        drawField(li, "Ghost");
        if(L.ghost > 0.0f) { drawField(li, "Ghost side"); drawField(li, "Ghost level"); }
        drawField(li, "Flam");
        if(L.flam > 0.0f) drawField(li, "Flam offset");
        drawField(li, "Ratchet");
        if(L.ratchet > 0.0f) {
            drawField(li, "Ratchet hits");
            if(divisionCombo("Ratchet speed", L.ratchetUnit, ratchetUnitOptions, IM_ARRAYSIZE(ratchetUnitOptions))) engine.changed(li);
            drawField(li, "Ratchet shape");
        }
        heading("Groove");
        drawField(li, "Groove");
        if(L.groove == GROOVE_SWING16 || L.groove == GROOVE_SWING8) drawField(li, "Swing");
        drawField(li, "Humanize");
        ImGui::PopItemWidth();

        // ── Column 2: Metric, Relations ──
        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(itemW);
        heading("Metric");
        char groupBuf[64];
        std::snprintf(groupBuf, sizeof(groupBuf), "%s", L.grouping.c_str());
        if(ImGui::InputTextWithHint("Grouping", "auto, e.g. 3+3+2", groupBuf, sizeof(groupBuf))) {
            L.grouping = groupBuf;
            engine.changed(li);
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Beats per group, adding up to the cycle. Group starts are the secondary strong beats.");
        drawField(li, "Feel main bar");
        drawField(li, "Weighting");
        drawField(li, "Anchor");
        if(L.anchorMode != ANCHOR_NONE) drawField(li, "Anchor amount");
        drawField(li, "Push");
        drawField(li, "Drag");
        if(L.push > 0.0f || L.drag > 0.0f)
            if(divisionCombo("Push / drag by", L.dispUnit, unitOptions, IM_ARRAYSIZE(unitOptions))) engine.changed(li);
        drawField(li, "Cross barline");

        heading("Relations (to lanes above)");
        if(li == 0) {
            ImGui::TextDisabled("Lane 1 has no lanes above it.");
        } else {
            drawField(li, "Relation");
            if(L.relMode != REL_NONE) {
                drawField(li, "Relation lane", "Lane");
                drawField(li, "Relation amount", "Amount");
                if(L.relMode == REL_RESPONSE) drawField(li, "Response delay", "Delay");
            }
            drawField(li, "Choke by");
            drawField(li, "Dynamics");
            if(L.dynMode != DYN_NONE) { drawField(li, "Dynamics lane", "Lane##dyn"); drawField(li, "Dynamics amount", "Amount##dyn"); }
        }
        ImGui::PopItemWidth();

        // ── Column 3: Phrase, Duration, Amplitude ──
        ImGui::TableSetColumnIndex(2);
        ImGui::PushItemWidth(itemW);
        heading("Phrase and repeat");
        drawField(li, "Phrase length", "Length (cycles)");
        drawField(li, "Repeat");
        if(L.repeat == REPEAT_OFF) drawField(li, "Renew");
        if(L.repeat == REPEAT_EVOLVE) { drawField(li, "Evolve every"); drawField(li, "Evolve chance"); }
        char formBuf[32];
        std::snprintf(formBuf, sizeof(formBuf), "%s", L.form.c_str());
        if(ImGui::InputTextWithHint("Form", "none, e.g. AAAB", formBuf, sizeof(formBuf), ImGuiInputTextFlags_CharsUppercase)) {
            L.form = formBuf;
            engine.changed(li);
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("One letter per cycle of the phrase. Every A plays the same; B, C... are mutations of A.\nEmpty: every cycle its own.");
        if(!L.form.empty()) drawField(li, "B strength");
        drawField(li, "Fill length");
        if(L.fillTicks > 0) drawField(li, "Fill intensity");
        drawField(li, "Build-up");
        drawField(li, "Phrase accent");
        drawField(li, "Swell");

        heading("Duration");
        drawField(li, "Duration", "Mode");
        if(L.durMode == DUR_FIXED) drawField(li, "Length");
        else if(L.durMode == DUR_LEGATO) drawField(li, "Legato");
        else { drawField(li, "Weak length"); drawField(li, "Strong length"); }
        drawField(li, "Long hits");
        if(L.longHits > 0 || L.accentsLong) drawField(li, "Long length");

        heading("Amplitude");
        drawField(li, "Max");
        drawField(li, "Min");
        drawField(li, "Metric");
        drawField(li, "Variation");
        drawField(li, "Curve");
        drawField(li, "Tiers");
        drawField(li, "Accent hits");
        if(L.accentHits > 0) { drawField(li, "Accent amount"); drawField(li, "Accents are long"); }
        ImGui::PopItemWidth();

        ImGui::EndTable();
    }
    ImGui::PopID();
}

#endif // OFXOCEANODESC_HAS_TRANSPORT
