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

// Note values, in ticks (48 per beat). T = triplet, . = dotted.
const scMetroBox::NoteList everyNotes = {
    {6, "1/32"}, {8, "1/16T"}, {12, "1/16"}, {16, "1/8T"}, {18, "1/16."}, {24, "1/8"}, {32, "1/4T"},
    {36, "1/8."}, {48, "1/4"}, {72, "1/4."}, {96, "1/2"}, {144, "1/2."}, {192, "1/1"},
};
const scMetroBox::NoteList lengthNotes = {
    {3, "1/64"}, {6, "1/32"}, {8, "1/16T"}, {12, "1/16"}, {16, "1/8T"}, {18, "1/16."}, {24, "1/8"},
    {36, "1/8."}, {48, "1/4"}, {72, "1/4."}, {96, "1/2"}, {144, "1/2."}, {192, "1/1"},
};
const scMetroBox::NoteList offsetNotes = {
    {0, "none"}, {6, "1/32"}, {8, "1/16T"}, {12, "1/16"}, {16, "1/8T"}, {24, "1/8"}, {36, "1/8."},
    {48, "1/4"}, {72, "1/4."}, {96, "1/2"}, {144, "3/4"},
};
const scMetroBox::NoteList ratchetNotes = {{12, "1/16"}, {8, "1/16T"}, {6, "1/32"}, {4, "1/32T"}, {3, "1/64"}};
const scMetroBox::NoteList delayNotes = {{12, "1/16"}, {24, "1/8"}, {48, "1/4"}, {72, "1/4."}, {96, "1/2"}, {192, "1/1"}};

const std::vector<std::string> patternNames = {"Steps", "Euclidean", "Free"};
const std::vector<std::string> accentNames = {"Strong beats", "Every Nth hit", "Random"};
const std::vector<std::string> relationNames = {"None", "Follow", "Avoid", "Answer"};
const std::vector<std::string> dynamicsNames = {"None", "Duck", "Boost"};
const std::vector<std::string> swingNames = {"16ths", "8ths"};

ImU32 layerColor(int i, float alpha = 1.0f) {
    static const float hues[] = {0.58f, 0.02f, 0.13f, 0.33f, 0.78f, 0.48f, 0.90f, 0.20f};
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hues[i % 8], 0.55f, 0.95f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
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

std::string percent(float v) { return ofToString((int)std::lround(v * 100.0f)) + "%%"; }

// What the Change knob does right now, in words
std::string changeWord(float c) {
    switch(changeZone(c)) {
        case CHANGE_FIXED:    return "Fixed";
        case CHANGE_LOOP:     return "Loop";
        case CHANGE_EVOLVING: { const int n = evolveEvery(c); return n == 1 ? "Evolving (every phrase)" : "Evolving (every " + ofToString(n) + ")"; }
        case CHANGE_VARYING:  return "Varying (" + percent(renewChance(c)) + " new)";
        default:              return "New";
    }
}

ofJson opsToJson(const std::vector<Op>& ops) {
    ofJson arr = ofJson::array();
    for(const auto& op : ops) arr.push_back({op.type, op.seed});
    return arr;
}

std::vector<Op> opsFromJson(const ofJson& arr) {
    std::vector<Op> ops;
    if(!arr.is_array()) return ops;
    for(const auto& j : arr) {
        if(!j.is_array() || j.size() < 2) continue;
        Op op;
        try {
            op.type = std::clamp(j[0].get<int>(), 0, (int)OP_COUNT - 1);
            op.seed = j[1].get<uint32_t>();
        } catch(...) { continue; }
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
        auto F = [&](const char* n, float Layer::* m, float lo, float hi, const char* tip) {
            Field f; f.name = n; f.kind = F_FLOAT; f.f = m; f.min = lo; f.max = hi; f.tooltip = tip; v.push_back(f);
        };
        auto I = [&](const char* n, int Layer::* m, float lo, float hi, const char* tip) {
            Field f; f.name = n; f.kind = F_INT; f.i = m; f.min = lo; f.max = hi; f.tooltip = tip; v.push_back(f);
        };
        auto B = [&](const char* n, bool Layer::* m, const char* tip) {
            Field f; f.name = n; f.kind = F_BOOL; f.b = m; f.min = 0; f.max = 1; f.tooltip = tip; v.push_back(f);
        };
        auto E = [&](const char* n, int Layer::* m, const std::vector<std::string>& opts, const char* tip) {
            Field f; f.name = n; f.kind = F_ENUM; f.i = m; f.min = 0; f.max = (float)opts.size() - 1; f.options = opts; f.tooltip = tip; v.push_back(f);
        };
        auto N = [&](const char* n, int Layer::* m, const NoteList& notes, const char* tip) {
            Field f; f.name = n; f.kind = F_NOTE; f.i = m; f.notes = &notes;
            f.min = (float)notes.front().first / TPB; f.max = (float)MAX_CYCLE_BEATS; f.tooltip = tip; v.push_back(f);
        };
        auto L = [&](const char* n, int Layer::* m, const char* tip) {
            Field f; f.name = n; f.kind = F_LANE; f.i = m; f.min = 0; f.max = MAX_LAYERS; f.tooltip = tip; v.push_back(f);
        };
        auto A = [&](const char* n, int action, const char* tip) {
            Field f; f.name = n; f.kind = F_ACTION; f.action = action; f.tooltip = tip; v.push_back(f);
        };

        // ── Main ──
        N("Every", &Layer::division, everyNotes, "How often the lane can play: one hit per this note value.");
        N("Offset", &Layer::shift, offsetNotes, "Moves the whole lane later by this much (an 1/8 offset on 1/4s = offbeats).");
        F("Chance", &Layer::density, 0, 1, "Chance that each hit plays. 100% = every step; lower = sparser and different each time.");
        N("Length", &Layer::durTicks, lengthNotes, "How long each gate stays open.");
        F("Accent", &Layer::accent, 0, 1, "How much louder the accented hits are than the rest. 0 = all the same.\nWhich hits are accented: \"Accent on\" (More), strong beats by default.");
        F("Change", &Layer::change, 0, 1,
          "How the lane changes over time, from left to right:\n"
          "  Fixed: the same phrase forever, locked.\n"
          "  Loop: the same phrase forever; Mutate still changes it.\n"
          "  Evolving: the loop mutates itself every few phrases (more often to the right).\n"
          "  Varying: sometimes a new phrase, otherwise the last one again.\n"
          "  New: a new phrase every time.");
        F("Smart", &Layer::smart, 0, 1,
          "When Chance drops hits, drop the weak ones (offbeat 16ths) first and keep the downbeats.\n"
          "0 = every hit has the same chance. Only matters when Chance is below 100% (or the pattern is Free).");
        B("Mute", &Layer::mute, "Silences the outputs only: lanes that follow, avoid or answer this one still hear it.");

        // ── Pattern ──
        E("Pattern", &Layer::pattern, patternNames,
          "Steps: a hit every step (Every).\n"
          "Euclidean: some of the steps, spread as evenly as possible.\n"
          "Free: the cycle splits into long and short notes, down to Every; strong beats get the long ones.");
        I("Cycle", &Layer::cycleBeats, 1, MAX_CYCLE_BEATS, "Beats before the lane's pattern starts again. Different from the bar = polymeter.");
        I("Steps", &Layer::eucSteps, 1, 64, "Euclidean: steps in the pattern (each one Every long).");
        I("Hits", &Layer::eucHits, 0, 64, "Euclidean: how many of the steps play.");
        I("Rotation", &Layer::eucRotation, 0, 63, "Euclidean: turns the pattern by this many steps.");
        F("Complexity", &Layer::complexity, 0, 1, "Free: how much the cycle splits into shorter notes.");
        F("Triplets", &Layer::triplets, 0, 1, "Free: chance a split is in three instead of two.");

        // ── Feel ──
        F("Swing", &Layer::swing, 0, 1, "Delays every other 16th (or 8th). Half way is a triplet feel.");
        E("Swing on", &Layer::swingUnit, swingNames, "Which notes swing.");
        F("Humanize", &Layer::humanize, 0, 1, "Every hit slightly early or late, up to a 32nd.");
        F("Push", &Layer::push, 0, 1, "Chance a hit on a beat comes a 16th early (anticipation).");
        F("Drag", &Layer::drag, 0, 1, "Chance a hit on a beat comes a 16th late.");
        F("Ghosts", &Layer::ghost, 0, 1, "Chance of a quiet extra hit a 16th before a hit.");
        F("Flams", &Layer::flam, 0, 1, "Chance of a grace hit just before a hit.");
        F("Ratchets", &Layer::ratchet, 0, 1, "Chance a hit becomes a quick burst of three.");
        N("Ratchet speed", &Layer::ratchetUnit, ratchetNotes, "Time between the burst's hits.");

        // ── Shape ──
        F("Longer on strong", &Layer::longOnStrong, 0, 1, "Gates on strong beats last longer (up to 4x Length on the downbeat).");
        E("Accent on", &Layer::accentSource, accentNames,
          "Strong beats: the downbeat loudest, then the beats, then the offbeats.\n"
          "Every Nth hit: a number of the cycle's hits, evenly spread.\n"
          "Random: each hit its own level.");
        I("Accented hits", &Layer::accentHits, 1, 16, "Every Nth hit: how many hits per cycle are accented.");
        F("Soft", &Layer::ampMin, 0, 1, "The level of the softest hits.");
        F("Loud", &Layer::level, 0, 1, "The level of the loudest hits.");

        // ── Over time ──
        I("Phrase", &Layer::phraseLen, 1, 16, "Cycles per phrase: the length of the loop (Change Fixed / Loop / Evolving), and where fills go.");
        F("Fill", &Layer::fill, 0, 1, "A busier last beat (two beats above half) at the end of each phrase, with bursts and a crescendo.");
        F("Build-up", &Layer::buildUp, 0, 1, "Starts each phrase sparse and fills up towards its end.");

        // ── Lanes ──
        E("Relation", &Layer::relMode, relationNames,
          "Follow: plays only where the other lane plays.\n"
          "Avoid: never plays where the other lane plays.\n"
          "Answer: plays what the other lane played, a bit later (call and response).");
        L("Relation lane", &Layer::relLane, "The lane to follow, avoid or answer (any other lane, as long as it does not loop back).");
        F("Relation amount", &Layer::relAmount, 0, 1, "How strictly: 100% always, lower now and then.");
        N("Answer delay", &Layer::respDelay, delayNotes, "How long after the other lane the answer comes.");
        L("Choked by", &Layer::chokeLane, "That lane's hits cut this lane's gates (closed hat chokes open hat).");
        E("Dynamics", &Layer::dynMode, dynamicsNames, "Duck: quieter when the other lane hits at the same time. Boost: louder.");
        L("Dynamics lane", &Layer::dynLane, "The lane to duck under or be boosted by.");
        F("Dynamics amount", &Layer::dynAmount, 0, 1, "How much quieter or louder.");

        // ── Evolution ──
        A("Mutate", ACT_MUTATE, "A small change from the next cycle on: a hit added, removed, moved, tied or accented.");
        A("Re-roll", ACT_REROLL, "A completely new rhythm with the same settings.");
        A("Undo", ACT_UNDO, "Back to the rhythm before the last Mutate / Re-roll.");
        A("Redo", ACT_REDO, "");
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

namespace {
int noteIndex(const scMetroBox::NoteList& notes, int ticks) {
    int best = 0;
    for(int i = 0; i < (int)notes.size(); i++)
        if(std::abs(notes[i].first - ticks) < std::abs(notes[best].first - ticks)) best = i;
    return best;
}
}

float scMetroBox::fieldValue(int li, const Field& f) const {
    const Layer& L = layers[li];
    switch(f.kind) {
        case F_FLOAT:  return L.*(f.f);
        case F_BOOL:   return L.*(f.b) ? 1.0f : 0.0f;
        case F_NOTE:   return (float)(L.*(f.i)) / TPB;
        case F_ACTION: return 0.0f;
        default:       return (float)(L.*(f.i));
    }
}

void scMetroBox::setFieldValue(int li, const Field& f, float value) {
    Layer& L = layers[li];
    if(f.kind == F_ACTION) { doAction(li, f.action); return; }
    if(f.f == &Layer::change) {
        engine.setChange(li, value, nowCycle(li));
        refreshPublished(li);
        return;
    }
    switch(f.kind) {
        case F_FLOAT: L.*(f.f) = ofClamp(value, f.min, f.max); break;
        case F_BOOL:  L.*(f.b) = value >= 0.5f; break;
        case F_NOTE: {
            const int minTicks = f.notes->front().first;
            L.*(f.i) = std::max(minTicks, (int)std::lround(ofClamp(value, 0.0f, f.max) * TPB));
            break;
        }
        case F_LANE:  L.*(f.i) = std::clamp((int)std::lround(value), 0, MAX_LAYERS); break;
        default:      L.*(f.i) = std::clamp((int)std::lround(value), (int)f.min, (int)f.max); break;
    }
    // Settings that bound others
    L.eucHits = std::clamp(L.eucHits, 0, L.eucSteps);
    L.eucRotation = std::clamp(L.eucRotation, 0, std::max(0, L.eucSteps - 1));
    L.shift = std::clamp(L.shift, 0, std::max(0, L.cycleTicks() - 1));
    L.ampMin = std::min(L.ampMin, L.level);

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
        default: break;
    }
    refreshPublished(li);
}

// The note values a field offers on a lane: offsets stay inside the cycle
std::vector<const std::pair<int, std::string>*> scMetroBox::notesFor(int li, const Field& f) const {
    std::vector<const std::pair<int, std::string>*> out;
    for(const auto& n : *f.notes)
        if(f.i != &Layer::shift || n.first < layers[li].cycleTicks()) out.push_back(&n);
    return out;
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
            std::string format = "%.2f";
            if(f.f == &Layer::change) format = changeWord(v);
            else if(f.max == 1.0f && f.min == 0.0f) {
                // Amounts read "off" at 0; levels and chances stay a percentage
                const bool level = f.f == &Layer::density || f.f == &Layer::level || f.f == &Layer::ampMin
                                || f.f == &Layer::relAmount || f.f == &Layer::dynAmount;
                format = v <= 0.0f && !level ? "off" : percent(v);
            }
            changed = ImGui::SliderFloat(text.c_str(), &v, f.min, f.max, format.c_str());
            break;
        }
        case F_INT: {
            int iv = (int)v;
            if(f.i == &Layer::cycleBeats) changed = ImGui::SliderInt(text.c_str(), &iv, (int)f.min, (int)f.max, iv == 1 ? "1 beat" : "%d beats");
            else if(f.i == &Layer::phraseLen) changed = ImGui::SliderInt(text.c_str(), &iv, (int)f.min, (int)f.max, iv == 1 ? "1 cycle" : "%d cycles");
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
        case F_NOTE: {
            const int ticks = layers[li].*(f.i);
            const auto notes = notesFor(li, f);
            std::string preview = ofToString(v, 3) + " beats";
            for(const auto* n : notes) if(n->first == ticks) preview = n->second;
            if(ImGui::BeginCombo(text.c_str(), preview.c_str())) {
                for(const auto* n : notes)
                    if(ImGui::Selectable(n->second.c_str(), n->first == ticks)) { v = (float)n->first / TPB; changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case F_LANE: {
            // any other lane; one that would loop back to this one is greyed
            const int iv = (int)v;
            const bool ignored = iv > 0 && engine.link(li, f.i == &Layer::chokeLane ? LINK_CHOKE
                                                          : f.i == &Layer::dynLane ? LINK_DYN : LINK_REL) < 0;
            std::string preview = iv <= 0 || iv > numLayers ? "None" : ofToString(iv) + " " + layers[iv - 1].name;
            if(ignored && iv > 0 && iv <= numLayers) preview += " (loop: ignored)";
            if(ImGui::BeginCombo(text.c_str(), preview.c_str())) {
                if(ImGui::Selectable("None", iv <= 0)) { v = 0.0f; changed = true; }
                for(int j = 0; j < numLayers; j++) {
                    if(j == li) continue;
                    const bool loop = engine.wouldLoop(li, j);
                    const std::string label = ofToString(j + 1) + " " + layers[j].name + (loop ? "  (would loop)" : "");
                    if(ImGui::Selectable(label.c_str(), iv == j + 1, loop ? ImGuiSelectableFlags_Disabled : 0)) { v = (float)(j + 1); changed = true; }
                    if(loop && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("That lane already depends on this one (directly or through others).");
                }
                ImGui::EndCombo();
            }
            break;
        }
        case F_ACTION: {
            const bool disabled = (f.action == ACT_UNDO && !engine.canUndo(li)) || (f.action == ACT_REDO && !engine.canRedo(li))
                || ((f.action == ACT_MUTATE || f.action == ACT_REROLL) && !engine.canMutate(li));
            ImGui::BeginDisabled(disabled);
            changed = ImGui::Button(text.c_str());
            ImGui::EndDisabled();
            if(disabled && (f.action == ACT_MUTATE || f.action == ACT_REROLL) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Change is Fixed: the lane is locked. Turn Change up to Loop to edit it.");
                fieldMenu(li, f);
                return false;
            }
            break;
        }
    }
    if(!f.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", f.tooltip.c_str());
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
            if(f.kind == F_FLOAT || f.kind == F_NOTE) std::snprintf(buffer, sizeof(buffer), "%g", v);
            else std::snprintf(buffer, sizeof(buffer), "%d", (int)v);
        }
        const char* unitText = f.kind == F_NOTE ? "Value (beats: 0.25 = a 16th)"
                             : f.kind == F_ENUM ? "Value (index from 0)"
                             : f.kind == F_LANE ? "Value (lane number, 0 = none)"
                             : f.kind == F_BOOL ? "Value (0 / 1)"
                             : f.kind == F_FLOAT && f.max == 1.0f ? "Value (0 .. 1)" : "Value";
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
    } else if(f.kind == F_FLOAT) {
        p->handle = addParameter(p->pf.set(key, v, f.min, f.max));
        p->listener = p->pf.newListener([apply](float& x) { apply(x); });
    } else if(f.kind == F_BOOL) {
        p->handle = addParameter(p->pb.set(key, v >= 0.5f));
        p->listener = p->pb.newListener([apply](bool& x) { apply(x ? 1.0f : 0.0f); });
    } else if(f.kind == F_NOTE) {
        // A dropdown of the note values
        std::vector<std::string> labels;
        for(const auto& n : *f.notes) labels.push_back(n.second);
        const NoteList* notes = f.notes;
        p->handle = addParameterDropdown(p->pi, key, noteIndex(*notes, layers[li].*(f.i)), labels);
        p->listener = p->pi.newListener([apply, notes](int& x) {
            apply((float)(*notes)[std::clamp(x, 0, (int)notes->size() - 1)].first / TPB);
        });
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
        if(f->kind == F_FLOAT) { if(p->pf.get() != v) p->pf.setWithoutEventNotifications(v); }
        else if(f->kind == F_BOOL) { if(p->pb.get() != (v >= 0.5f)) p->pb.setWithoutEventNotifications(v >= 0.5f); }
        else if(f->kind == F_NOTE) {
            const int idx = noteIndex(*f->notes, layers[li].*(f->i));
            if(p->pi.get() != idx) p->pi.setWithoutEventNotifications(idx);
        }
        else if(p->pi.get() != (int)v) p->pi.setWithoutEventNotifications((int)v);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Roles
// ════════════════════════════════════════════════════════════════════════════

const std::vector<std::string>& scMetroBox::builtInRoleNames() {
    static const std::vector<std::string> names = {"Kick", "Snare", "Hat", "Perc", "Plain"};
    return names;
}

scMetroBox::Layer scMetroBox::builtInRole(int index) {
    Layer L;
    switch(index) {
        case 0: // Kick: free long / short notes, always on 1, evolving slowly
            L.name = "Kick";
            L.pattern = PAT_FREE;
            L.division = 24;
            L.complexity = 0.45f;
            L.smart = 0.7f;
            L.durTicks = 12;
            L.longOnStrong = 0.5f;
            L.accent = 0.4f;
            L.phraseLen = 2;
            L.change = 0.45f;
            break;
        case 1: // Snare: backbeats with ghosts before them, a few flams, a fill
            L.name = "Snare";
            L.division = 96;
            L.shift = 48;
            L.durTicks = 12;
            L.accent = 0.0f;
            L.ghost = 0.3f;
            L.flam = 0.1f;
            L.fill = 0.4f;
            L.change = 0.25f;
            break;
        case 2: // Hat: 16ths thinned on the weak positions, accented, swung
            L.name = "Hat";
            L.division = 12;
            L.density = 0.85f;
            L.smart = 0.5f;
            L.durTicks = 6;
            L.accent = 0.6f;
            L.ampMin = 0.15f;
            L.swing = 0.25f;
            L.ratchet = 0.05f;
            L.change = 0.75f;
            break;
        case 3: // Perc: free 16ths with every-Nth accents, pushed, avoiding lane 1
            L.name = "Perc";
            L.pattern = PAT_FREE;
            L.division = 12;
            L.complexity = 0.6f;
            L.density = 0.6f;
            L.smart = 0.3f;
            L.durTicks = 12;
            L.accent = 0.5f;
            L.accentSource = ACC_EUCLID;
            L.accentHits = 3;
            L.push = 0.2f;
            L.relMode = REL_AVOID;
            L.relLane = 1;
            L.relAmount = 0.6f;
            L.change = 0.55f;
            break;
        default: // Plain: a steady 8th metro
            L.name = "Plain";
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

// A role brings settings, not a lane's state: its mute, genome (seed and
// mutations) and open panel stay. Its Change goes through setChange, so a
// frozen role keeps the phrase playing now.
void scMetroBox::applyRole(int li, const Layer& role) {
    Layer& L = layers[li];
    const float change = role.change;
    Layer next = role;
    next.mute = L.mute;
    next.id = L.id;
    next.advanced = L.advanced;
    next.seed = L.seed;
    next.ops = L.ops;
    next.change = L.change;
    next.keepPhrase = L.keepPhrase;
    L = next;
    engine.setChange(li, change, nowCycle(li));
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
        {"name", L.name},
        {"division", L.division}, {"shift", L.shift}, {"density", L.density}, {"durTicks", L.durTicks},
        {"accent", L.accent}, {"change", L.change}, {"smart", L.smart},
        {"pattern", L.pattern}, {"cycleBeats", L.cycleBeats},
        {"eucSteps", L.eucSteps}, {"eucHits", L.eucHits}, {"eucRotation", L.eucRotation},
        {"complexity", L.complexity}, {"triplets", L.triplets}, {"grouping", L.grouping},
        {"swing", L.swing}, {"swingUnit", L.swingUnit}, {"humanize", L.humanize},
        {"push", L.push}, {"drag", L.drag},
        {"ghost", L.ghost}, {"flam", L.flam}, {"ratchet", L.ratchet}, {"ratchetUnit", L.ratchetUnit},
        {"longOnStrong", L.longOnStrong}, {"accentSource", L.accentSource}, {"accentHits", L.accentHits},
        {"ampMin", L.ampMin}, {"level", L.level},
        {"phraseLen", L.phraseLen}, {"fill", L.fill}, {"buildUp", L.buildUp},
        {"relMode", L.relMode}, {"relLane", L.relLane}, {"relAmount", L.relAmount}, {"respDelay", L.respDelay},
        {"chokeLane", L.chokeLane}, {"dynMode", L.dynMode}, {"dynLane", L.dynLane}, {"dynAmount", L.dynAmount},
    };
    if(withState) {
        j["mute"] = L.mute;
        j["id"] = L.id;
        j["advanced"] = L.advanced;
        j["seed"] = L.seed;
        j["ops"] = opsToJson(L.ops);
        j["keepPhrase"] = L.keepPhrase;
    }
    return j;
}

// Settings missing from the JSON take the defaults
scMetroBox::Layer scMetroBox::layerFromJson(const ofJson& j) {
    Layer L;
    auto get = [&](const char* key, auto& field) {
        try { if(j.contains(key)) field = j[key].get<std::decay_t<decltype(field)>>(); } catch(...) {}
    };
    get("name", L.name); get("id", L.id); get("mute", L.mute); get("advanced", L.advanced);
    get("division", L.division); get("shift", L.shift); get("density", L.density); get("durTicks", L.durTicks);
    get("accent", L.accent); get("change", L.change); get("smart", L.smart);
    get("pattern", L.pattern); get("cycleBeats", L.cycleBeats); get("seed", L.seed);
    get("eucSteps", L.eucSteps); get("eucHits", L.eucHits); get("eucRotation", L.eucRotation);
    get("complexity", L.complexity); get("triplets", L.triplets); get("grouping", L.grouping);
    get("swing", L.swing); get("swingUnit", L.swingUnit); get("humanize", L.humanize);
    get("push", L.push); get("drag", L.drag);
    get("ghost", L.ghost); get("flam", L.flam); get("ratchet", L.ratchet); get("ratchetUnit", L.ratchetUnit);
    get("longOnStrong", L.longOnStrong); get("accentSource", L.accentSource); get("accentHits", L.accentHits);
    get("ampMin", L.ampMin); get("level", L.level);
    get("phraseLen", L.phraseLen); get("fill", L.fill); get("buildUp", L.buildUp);
    get("relMode", L.relMode); get("relLane", L.relLane); get("relAmount", L.relAmount); get("respDelay", L.respDelay);
    get("chokeLane", L.chokeLane); get("dynMode", L.dynMode); get("dynLane", L.dynLane); get("dynAmount", L.dynAmount);
    get("keepPhrase", L.keepPhrase);
    if(j.contains("ops")) L.ops = opsFromJson(j["ops"]);

    // Ranges
    L.pattern      = std::clamp(L.pattern, 0, (int)patternNames.size() - 1);
    L.cycleBeats   = std::clamp(L.cycleBeats, 1, MAX_CYCLE_BEATS);
    L.division     = std::max(1, L.division);
    L.shift        = std::clamp(L.shift, 0, L.cycleTicks() - 1);
    L.durTicks     = std::max(1, L.durTicks);
    L.eucSteps     = std::clamp(L.eucSteps, 1, 64);
    L.eucHits      = std::clamp(L.eucHits, 0, L.eucSteps);
    L.swingUnit    = std::clamp(L.swingUnit, 0, 1);
    L.ratchetUnit  = std::max(1, L.ratchetUnit);
    L.accentSource = std::clamp(L.accentSource, 0, (int)accentNames.size() - 1);
    L.accentHits   = std::clamp(L.accentHits, 1, 16);
    L.phraseLen    = std::clamp(L.phraseLen, 1, 16);
    L.relMode      = std::clamp(L.relMode, 0, (int)relationNames.size() - 1);
    L.relLane      = std::clamp(L.relLane, 0, MAX_LAYERS);
    L.respDelay    = std::max(1, L.respDelay);
    L.chokeLane    = std::clamp(L.chokeLane, 0, MAX_LAYERS);
    L.dynMode      = std::clamp(L.dynMode, 0, (int)dynamicsNames.size() - 1);
    L.dynLane      = std::clamp(L.dynLane, 0, MAX_LAYERS);
    return L;
}

// ════════════════════════════════════════════════════════════════════════════
// Construction
// ════════════════════════════════════════════════════════════════════════════

scMetroBox::scMetroBox() : scNode("MetroBox"), layers(engine.layers) {
    anchoredCycleTicks.fill(-1);
    lastPhrase.fill(NO_CYCLE);
    lastPassPhrase.fill(NO_CYCLE);
    phrasesSinceAuto.fill(0);
    // A starting groove from the roles: kick, snare with ghosts and fills,
    // swung hats that duck under the kick, perc avoiding the kick.
    for(int i = 0; i < 4; i++) layers[i] = builtInRole(i);
    layers[2].dynMode = DYN_DUCK;
    layers[2].dynLane = 1;
    layers[2].dynAmount = 0.4f;
    for(int i = 4; i < MAX_LAYERS; i++) layers[i].name = "Lane " + ofToString(i + 1);
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
    description = "Generative rhythm locked to the transport: up to 8 lanes, each a "
                  "metro you shape with a few controls (Every, Offset, Chance, Length, "
                  "Accent, Change, Smart) and a role (Kick, Snare, Hat, Perc). More "
                  "opens a lane's advanced settings: patterns, swing, ornaments, accents, "
                  "fills and relations to other lanes. Gates carries every lane (lane n = "
                  "channel n: wire it to a SampleBox kit), and each lane has its own Gate; "
                  "a gate's level is the hit's. Drag a lane by its name to reorder. Seeded "
                  "per cycle, so the window previews exactly what will play. Open it with "
                  "Show; right-click a control there to type a value or publish it here.";

    addParameter(showWindow.set("Show", false));
    addParameter(numLayers.set("Layers", 4, 1, MAX_LAYERS));
    addParameter(globalSeed.set("Seed", 1, 0, 99999));
    addParameter(densityIn.set("Density", {1.0f}, {0.0f}, {1.0f}));

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
    listeners.push(resendParams.newListener([this]() {
        for(auto& [server, st] : servers) sendStaticControls(st);
        follower.requestAnchor();
    }));
}

// ════════════════════════════════════════════════════════════════════════════
// Ports
// ════════════════════════════════════════════════════════════════════════════

std::string scMetroBox::busName(int index) {
    return index == 0 ? std::string("gatesout") : "gateout" + ofToString(index);
}

void scMetroBox::syncOutputPorts() {
    const int n = std::clamp(numLayers.get(), 1, MAX_LAYERS);
    if(getNumOutputs() == 0) scNode::addOutput("Gates");
    while(portLayers < n) {
        scNode::addOutput("Gate " + ofToString(portLayers + 1));
        portLayers++;
    }
    while(portLayers > n) {
        scNode::removeOutput(portLayers);
        // A removed port's bus may be handed to another node: forget it, so it
        // is not reused if the lane comes back (the synth silences the lane).
        for(auto& [server, st] : servers) st.outputBuses.erase(portLayers);
        portLayers--;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Change Evolving: a mutation every few phrases
// ════════════════════════════════════════════════════════════════════════════

void scMetroBox::autoEvolve() {
    const auto t = getFrameTransportState().current;
    for(int li = 0; li < numLayers; li++) {
        const Layer& L = layers[li];
        const int64_t now = engine.cycleAt(li, t.beatPosition);
        const int64_t phrase = engine.phraseOf(li, now);
        if(!t.isPlaying || changeZone(L.change) != CHANGE_EVOLVING) { lastPhrase[li] = phrase; phrasesSinceAuto[li] = 0; continue; }
        if(lastPhrase[li] != NO_CYCLE && phrase == lastPhrase[li] + 1 && ++phrasesSinceAuto[li] >= evolveEvery(L.change)) {
            phrasesSinceAuto[li] = 0;
            engine.mutate(li, now, (uint32_t)hashOf((uint64_t)ofGetElapsedTimeMicros(), ++autoCounter, (uint64_t)li));
        }
        lastPhrase[li] = phrase;
    }
    // Seed 0: a phrase the playhead left plays something new next time round
    for(int li = 0; li < numLayers; li++) {
        const int64_t phrase = engine.phraseOf(li, engine.cycleAt(li, t.beatPosition));
        if(engine.unseeded() && lastPassPhrase[li] != NO_CYCLE && phrase != lastPassPhrase[li]) engine.leavePhrase(li, lastPassPhrase[li]);
        lastPassPhrase[li] = phrase;
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
    st.synth->set("layers", (float)numLayers.get());
    std::vector<float> mutes(MAX_LAYERS);
    for(int i = 0; i < MAX_LAYERS; i++) mutes[i] = layers[i].mute ? 1.0f : 0.0f;
    st.synth->set("mute", mutes);
    for(auto& [index, bus] : st.outputBuses)
        if(index >= 0 && index < NUM_OUTPUTS) st.synth->set(busName(index), bus);
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
    if(!server || index < 0 || index >= NUM_OUTPUTS) return;
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
    const std::string title = getWindowTitle();
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1220.0f, 760.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open)) {
        ImGui::PushID(this);

        int n = numLayers.get();
        ImGui::SetNextItemWidth(120);
        if(ImGui::SliderInt("Lanes", &n, 1, MAX_LAYERS)) numLayers = n;
        ImGui::SameLine();
        int seed = globalSeed.get();
        ImGui::SetNextItemWidth(120);
        if(ImGui::InputInt("Seed", &seed)) globalSeed = std::clamp(seed, 0, 99999);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Changes the random choices of every lane at once (the same seed always plays the same).\n"
                              "0 = no seed: new choices every time a phrase comes round (loops still repeat).");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("Bars shown", &viewBars, 1, 16);

        drawTimeline(std::max(200.0f, ImGui::GetContentRegionAvail().x));

        ImGui::Separator();
        if(ImGui::BeginChild("##lanesChild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) drawLanes();
        ImGui::EndChild();

        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
    // a drop, applied once the frame is drawn
    if(pendingMoveFrom >= 0) {
        moveLane(pendingMoveFrom, pendingMoveTo);
        pendingMoveFrom = pendingMoveTo = -1;
    }
}

// ── Moving lanes ────────────────────────────────────────────────────────────

namespace {
struct LaneDrag { const void* node; int lane; };
}

void scMetroBox::laneDragSource(int li) {
    if(!ImGui::BeginDragDropSource()) return;
    const LaneDrag d{this, li};
    ImGui::SetDragDropPayload("MetroBoxLane", &d, sizeof(d));
    ImGui::Text("Move %d %s", li + 1, layers[li].name.c_str());
    ImGui::EndDragDropSource();
}

void scMetroBox::laneDropTarget(int li) {
    if(!ImGui::BeginDragDropTarget()) return;
    if(const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MetroBoxLane")) {
        const LaneDrag d = *(const LaneDrag*)p->Data;
        if(d.node == this && d.lane != li) { pendingMoveFrom = d.lane; pendingMoveTo = li; }
    }
    ImGui::EndDragDropTarget();
}

// The lane moves with everything (settings, rhythm, history); its Gate port
// and Gates channel are its new position's. Published controls stay with
// their position ("L2 Chance" is lane 2's, whichever lane that is now).
void scMetroBox::moveLane(int from, int to) {
    if(from < 0 || to < 0 || from >= numLayers || to >= numLayers || from == to) return;
    auto move = [&](auto& a) {
        auto v = a[from];
        if(from < to) for(int i = from; i < to; i++) a[i] = a[i + 1];
        else for(int i = from; i > to; i--) a[i] = a[i - 1];
        a[to] = v;
    };
    move(lastPhrase);
    move(lastPassPhrase);
    move(phrasesSinceAuto);
    engine.moveLane(from, to);
    for(auto& [server, st] : servers) sendStaticControls(st);
    for(int i = 0; i < MAX_LAYERS; i++) refreshPublished(i);
    follower.requestAnchor();
}

void scMetroBox::drawTimeline(float width) {
    constexpr float labelW = 150.0f, rowH = 40.0f, rulerH = 18.0f;
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
        const int fillLen = L.fillTicks();
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
            if(fillLen > 0 && positiveMod64(c, n) == n - 1) {
                const double fs = cycleBeat + (double)(cT - fillLen) / TPB;
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

    // Lane labels: a click opens / closes the lane's advanced settings, M mutes
    for(int li = 0; li < rows; li++) {
        Layer& L = layers[li];
        const float y0 = yRows + li * rowH;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
        ImGui::PushID(li);
        if(L.advanced) dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x0 - 4, y0 + rowH - 2), ImGui::GetColorU32(ImGuiCol_Header));
        // a click (not a drag) opens / closes the advanced settings
        if(ImGui::InvisibleButton("##sel", ImVec2(labelW - 34, rowH - 2)) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36.0f)
            L.advanced = !L.advanced;
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip(L.advanced ? "Click: close the advanced settings\nDrag onto another lane: move it there"
                                         : "Click: open the advanced settings\nDrag onto another lane: move it there");
        laneDragSource(li);
        laneDropTarget(li);
        dl->AddRectFilled(ImVec2(origin.x + 4, y0 + 6), ImVec2(origin.x + 8, y0 + rowH - 8), layerColor(li, L.mute ? 0.3f : 1.0f));
        dl->AddText(ImVec2(origin.x + 14, y0 + 4), L.mute ? textDim : text, (ofToString(li + 1) + " " + L.name).c_str());
        std::string sub = changeWord(L.change);
        ofStringReplace(sub, "%%", "%");
        dl->AddText(ImVec2(origin.x + 14, y0 + 20), textDim, sub.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x0 - 32, y0 + 8));
        if(L.mute) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        if(ImGui::SmallButton("M")) setFieldValue(li, *findField("Mute"), L.mute ? 0.0f : 1.0f);
        if(L.mute) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, yEnd + 6));
    ImGui::Dummy(ImVec2(width, 1));
}

// ── Lane rows ───────────────────────────────────────────────────────────────

namespace {
struct LaneColumn { const char* name; float width; const char* field; };
const LaneColumn laneColumns[] = {
    {"Lane", 130, nullptr}, {"Role", 84, nullptr}, {"Every", 74, "Every"}, {"Offset", 74, "Offset"},
    {"Chance", 84, "Chance"}, {"Length", 74, "Length"}, {"Accent", 84, "Accent"}, {"Change", 170, "Change"},
    {"Smart", 84, "Smart"}, {"Evolve", 112, nullptr}, {"", 0, nullptr},
};
constexpr int laneColumnCount = IM_ARRAYSIZE(laneColumns);
}

void scMetroBox::drawLanes() {
    // One table per run of rows: an open advanced panel sits between two
    auto begin = [&](int segment, bool headers) {
        if(!ImGui::BeginTable(("##lanes" + ofToString(segment)).c_str(), laneColumnCount, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
            return false;
        for(int c = 0; c < laneColumnCount; c++)
            ImGui::TableSetupColumn(laneColumns[c].name, c == laneColumnCount - 1 ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed, laneColumns[c].width);
        if(headers) {
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for(int c = 0; c < laneColumnCount; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TableHeader(laneColumns[c].name);
                if(laneColumns[c].field && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    if(const Field* f = findField(laneColumns[c].field)) ImGui::SetTooltip("%s", f->tooltip.c_str());
            }
        }
        return true;
    };
    int segment = 0;
    bool open = begin(segment++, true);
    for(int li = 0; li < numLayers; li++) {
        if(open) { ImGui::TableNextRow(); drawLaneRow(li); }
        if(layers[li].advanced) {
            if(open) ImGui::EndTable();
            drawAdvanced(li);
            open = li + 1 < numLayers && begin(segment++, false);
        }
    }
    if(open) ImGui::EndTable();
}

void scMetroBox::roleCombo(int li, const char* label) {
    if(ImGui::BeginCombo(label, "Role...")) {
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
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Sets this lane up as a kick, snare, hat... (its rhythm's seed and mutations stay).");
}

void scMetroBox::drawLaneRow(int li) {
    Layer& L = layers[li];
    ImGui::PushID(li);

    ImGui::TableSetColumnIndex(0);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3), ImVec2(p.x + 4, p.y + h - 3), layerColor(li, L.mute ? 0.3f : 1.0f));
        ImGui::Dummy(ImVec2(6, h));
        ImGui::SameLine();
        // the name: drag it onto another lane to move this one there
        const std::string label = ofToString(li + 1) + " " + L.name + "##drag";
        if(L.mute) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::Selectable(label.c_str(), false, 0, ImVec2(laneColumns[0].width - 40, h));
        ImGui::PopStyleVar();
        if(L.mute) ImGui::PopStyleColor();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip("Drag onto another lane to move it there (its Gate and its channel in Gates move with it)");
        laneDragSource(li);
        laneDropTarget(li);
        ImGui::SameLine(laneColumns[0].width - 22);
        if(L.mute) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        if(ImGui::SmallButton("M")) setFieldValue(li, *findField("Mute"), L.mute ? 0.0f : 1.0f);
        if(L.mute) ImGui::PopStyleColor();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Mute");
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    roleCombo(li, "##role");

    for(int c = 2; c <= 8; c++) {
        ImGui::TableSetColumnIndex(c);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Smart has nothing to drop at full Chance (Free patterns aside): dimmed
        const bool idle = c == 8 && L.density >= 1.0f && L.pattern != PAT_FREE;
        if(idle) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
        drawField(li, laneColumns[c].field, (std::string("##") + laneColumns[c].field).c_str());
        if(idle) ImGui::PopStyleVar();
    }

    ImGui::TableSetColumnIndex(9);
    drawField(li, "Mutate");
    ImGui::SameLine();
    drawField(li, "Undo");

    ImGui::TableSetColumnIndex(10);
    if(ImGui::SmallButton(L.advanced ? "Less" : "More")) L.advanced = !L.advanced;
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(L.advanced ? "Close the advanced settings (they keep acting)" : "Pattern, feel, accents, phrase and relations to other lanes");
    if(!L.advanced) {
        const auto summary = advancedSummary(li);
        if(!summary.empty()) {
            std::string all;
            for(const auto& s : summary) all += (all.empty() ? "" : ", ") + s;
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const float room = ImGui::GetContentRegionAvail().x;
            std::string shown = "+ " + all;
            if(ImGui::CalcTextSize(shown.c_str()).x > room) {
                while(shown.size() > 4 && ImGui::CalcTextSize((shown + "...").c_str()).x > room) shown.pop_back();
                shown += "...";
            }
            ImGui::TextDisabled("%s", shown.c_str());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Advanced settings in use:\n%s", all.c_str());
        }
    }
    ImGui::PopID();
}

// The advanced settings a lane uses, in a few words each (the row lists them
// while the panel is closed)
std::vector<std::string> scMetroBox::advancedSummary(int li) const {
    const Layer& L = layers[li];
    std::vector<std::string> out;
    // a link's lane as a number, or why it does nothing
    auto lane = [&](int kind) {
        const int set = engine.linkSetting(li, kind), used = engine.link(li, kind);
        return used >= 0 ? ofToString(used + 1) : set > 0 && set <= numLayers && set != li + 1 ? ofToString(set) + " (loop: ignored)" : std::string();
    };
    if(L.pattern == PAT_EUCLIDEAN) out.push_back("Euclidean " + ofToString(L.eucHits) + "/" + ofToString(L.eucSteps));
    if(L.pattern == PAT_FREE) out.push_back("Free pattern");
    if(L.cycleBeats != (int)std::lround(mainBarBeats)) out.push_back(ofToString(L.cycleBeats) + "-beat cycle");
    if(!L.grouping.empty()) out.push_back("groups " + L.grouping);
    if(L.swing > 0.0f) out.push_back("swing");
    if(L.humanize > 0.0f) out.push_back("humanize");
    if(L.push > 0.0f) out.push_back("push");
    if(L.drag > 0.0f) out.push_back("drag");
    if(L.ghost > 0.0f) out.push_back("ghosts");
    if(L.flam > 0.0f) out.push_back("flams");
    if(L.ratchet > 0.0f) out.push_back("ratchets");
    if(L.longOnStrong > 0.0f) out.push_back("longer on strong");
    if(L.accentSource == ACC_EUCLID) out.push_back(ofToString(L.accentHits) + " accents per cycle");
    if(L.accentSource == ACC_RANDOM) out.push_back("random accents");
    if(L.ampMin > 0.0f || L.level < 1.0f) out.push_back("level " + ofToString((int)std::lround(L.ampMin * 100)) + "-" + ofToString((int)std::lround(L.level * 100)) + "%");
    if(L.phraseLen != 4) out.push_back(ofToString(L.phraseLen) + "-cycle phrase");
    if(L.fill > 0.0f) out.push_back("fill");
    if(L.buildUp > 0.0f) out.push_back("build-up");
    if(!lane(LINK_REL).empty())
        out.push_back(std::string(L.relMode == REL_FOLLOW ? "follows " : L.relMode == REL_AVOID ? "avoids " : "answers ") + lane(LINK_REL));
    if(!lane(LINK_CHOKE).empty()) out.push_back("choked by " + lane(LINK_CHOKE));
    if(!lane(LINK_DYN).empty())
        out.push_back(std::string(L.dynMode == DYN_DUCK ? "ducks under " : "boosted by ") + lane(LINK_DYN));
    return out;
}

void scMetroBox::drawAdvanced(int li) {
    Layer& L = layers[li];
    ImGui::PushID(1000 + li);
    ImGui::Indent(12.0f);

    // ── Header: name, save role, the other evolution actions ──
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(layerColor(li)), "Lane %d", li + 1);
    ImGui::SameLine();
    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", L.name.c_str());
    ImGui::SetNextItemWidth(140);
    if(ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) L.name = nameBuf;
    ImGui::SameLine();
    if(ImGui::Button("Save as role")) {
        std::snprintf(roleNameBuf, sizeof(roleNameBuf), "%s", L.name.c_str());
        ImGui::OpenPopup("##saveRole");
    }
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Keeps this lane's settings in the Role menu.");
    if(ImGui::BeginPopup("##saveRole")) {
        ImGui::TextDisabled("Save this lane's settings as a role");
        ImGui::SetNextItemWidth(200);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rolename", roleNameBuf, sizeof(roleNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        if((ImGui::Button("Save") || enter) && roleNameBuf[0]) { saveRole(li, roleNameBuf); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    drawField(li, "Re-roll");
    ImGui::SameLine();
    drawField(li, "Redo");
    ImGui::SameLine();
    ImGui::TextDisabled("%d mutations", (int)L.ops.size());

    constexpr float colW = 232.0f, itemW = 120.0f;
    if(ImGui::BeginTable("##advanced", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        for(int c = 0; c < 5; c++) ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, colW);
        ImGui::TableNextRow();

        // ── Pattern ──
        ImGui::TableSetColumnIndex(0);
        ImGui::PushItemWidth(itemW);
        heading("Pattern");
        drawField(li, "Pattern");
        drawField(li, "Cycle");
        if(L.pattern == PAT_EUCLIDEAN) {
            drawField(li, "Steps");
            drawField(li, "Hits");
            drawField(li, "Rotation");
        }
        if(L.pattern == PAT_FREE) {
            drawField(li, "Complexity");
            drawField(li, "Triplets");
        }
        char groupBuf[64];
        std::snprintf(groupBuf, sizeof(groupBuf), "%s", L.grouping.c_str());
        if(ImGui::InputTextWithHint("Groups", "auto, e.g. 3+3+2", groupBuf, sizeof(groupBuf))) {
            L.grouping = groupBuf;
            engine.changed(li);
        }
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("How the cycle's beats group, adding up to the cycle (3+3+2 for 8 beats).\n"
                              "Group starts count as strong beats for Smart, accents and Free patterns.");
        ImGui::PopItemWidth();

        // ── Feel ──
        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(itemW);
        heading("Feel");
        drawField(li, "Swing");
        if(L.swing > 0.0f) drawField(li, "Swing on");
        drawField(li, "Humanize");
        drawField(li, "Push");
        drawField(li, "Drag");
        drawField(li, "Ghosts");
        drawField(li, "Flams");
        drawField(li, "Ratchets");
        if(L.ratchet > 0.0f) drawField(li, "Ratchet speed");
        ImGui::PopItemWidth();

        // ── Shape ──
        ImGui::TableSetColumnIndex(2);
        ImGui::PushItemWidth(itemW);
        heading("Shape");
        drawField(li, "Accent on");
        if(L.accentSource == ACC_EUCLID) drawField(li, "Accented hits");
        drawField(li, "Soft");
        drawField(li, "Loud");
        drawField(li, "Longer on strong");
        ImGui::PopItemWidth();

        // ── Over time ──
        ImGui::TableSetColumnIndex(3);
        ImGui::PushItemWidth(itemW);
        heading("Over time");
        drawField(li, "Phrase");
        drawField(li, "Fill");
        drawField(li, "Build-up");
        ImGui::PopItemWidth();

        // ── Lanes ──
        ImGui::TableSetColumnIndex(4);
        ImGui::PushItemWidth(itemW);
        heading("Other lanes");
        drawField(li, "Relation");
        if(L.relMode != REL_NONE) {
            drawField(li, "Relation lane", "Lane");
            drawField(li, "Relation amount", "Amount");
            if(L.relMode == REL_ANSWER) drawField(li, "Answer delay", "Delay");
        }
        drawField(li, "Choked by");
        drawField(li, "Dynamics");
        if(L.dynMode != DYN_NONE) { drawField(li, "Dynamics lane", "Lane##dyn"); drawField(li, "Dynamics amount", "Amount##dyn"); }
        ImGui::PopItemWidth();

        ImGui::EndTable();
    }
    ImGui::Unindent(12.0f);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PopID();
}

#endif // OFXOCEANODESC_HAS_TRANSPORT
