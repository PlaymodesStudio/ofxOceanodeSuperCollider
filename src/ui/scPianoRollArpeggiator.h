//
//  scPianoRollArpeggiator.h
//  ofxOceanodeSuperCollider
//
//  The timeline piano roll's Arpeggiator tool: select chords drawn as notes,
//  and it writes their arpeggio in their place, as ordinary notes
//  (nodes/boxSynths/arpeggiatorBox/scArpeggiatorNotes.h, on the
//  ArpeggiatorBox engine). The timeline shows it from the roll's "Arp" tab,
//  in a window of its own (ofxOceanodeTimelinePianoTool).
//
//  While the notes it wrote are still in the roll, every control rewrites
//  them from the chords it keeps, so the roll is the preview; Restore puts
//  the chords back. One undo step per gesture, like any edit in the roll
//  (undone from the timeline's window, which has the shortcut).
//
//  Its settings last the session (they are the tool's, not a lane's).
//

#pragma once

#include "../ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TIMELINE

#include "ofxOceanodeTimeline.h"
#include "imgui.h"
#include "../nodes/boxSynths/arpeggiatorBox/scArpeggiatorNotes.h"
#include <random>

namespace scPianoRollArpeggiator {

struct State {
    arpbox::RollSettings settings;
    int rate = 0;                                           // 0: the lane's grid
    std::string laneId;                                     // where the notes below are
    std::vector<ofxOceanodeTimelinePianoNote> chords;       // what was arpeggiated
    std::vector<ofxOceanodeTimelinePianoNote> written;      // what the tool wrote
    size_t notesAfter = 0;                                  // the lane's note count then
    int outside = 0;                                        // notes left out: outside the roll's range
    bool active = false;
};
inline State& state() { static State s; return s; }

inline bool sameNote(const ofxOceanodeTimelinePianoNote& a, const ofxOceanodeTimelinePianoNote& b) {
    return a.pitch == b.pitch && std::abs(a.startBeat - b.startBeat) < 1e-9 && std::abs(a.durationBeats - b.durationBeats) < 1e-9 &&
           std::abs(a.velocity - b.velocity) < 1e-6f;
}
// Where the written notes are in the lane now. False when any is gone or
// changed (edited, undone, another lane): the tool lets go of them.
inline bool locate(const ofxOceanodeTimelineLane& lane, const std::vector<ofxOceanodeTimelinePianoNote>& notes, std::set<int>& indices) {
    indices.clear();
    for(const auto& note : notes) {
        bool found = false;
        for(int i = 0; i < (int)lane.pianoNotes.size() && !found; i++)
            if(!indices.count(i) && sameNote(lane.pianoNotes[i], note)) { indices.insert(i); found = true; }
        if(!found) return false;
    }
    return true;
}
inline void erase(ofxOceanodeTimelineLane& lane, const std::set<int>& indices) {
    for(auto it = indices.rbegin(); it != indices.rend(); ++it)
        if(*it >= 0 && *it < (int)lane.pianoNotes.size()) lane.pianoNotes.erase(lane.pianoNotes.begin() + *it);
}
inline std::set<int> append(ofxOceanodeTimelineLane& lane, const std::vector<ofxOceanodeTimelinePianoNote>& notes) {
    std::set<int> indices;
    for(const auto& note : notes) { lane.pianoNotes.push_back(note); indices.insert((int)lane.pianoNotes.size() - 1); }
    return indices;
}

struct Rate { const char* label; double beats; };
inline const std::vector<Rate>& rates() {
    static const std::vector<Rate> r = {{"Lane grid", 0.0}, {"Quarter", 1.0}, {"QuarterT", 2.0 / 3.0}, {"8th", 0.5}, {"8thD", 0.75},
        {"8thT", 1.0 / 3.0}, {"16th", 0.25}, {"16thD", 0.375}, {"16thT", 1.0 / 6.0}, {"32nd", 0.125}, {"32ndT", 1.0 / 12.0}, {"64th", 0.0625}};
    return r;
}

// The chords' arpeggio, as the lane's notes
inline std::vector<ofxOceanodeTimelinePianoNote> generate(State& st, const ofxOceanodeTimelineLane& lane, int* outside = nullptr) {
    std::vector<arpbox::RollNote> chords;
    for(const auto& note : st.chords) chords.push_back({note.startBeat, note.durationBeats, note.pitch, note.velocity});
    arpbox::RollSettings s = st.settings;
    const double chosen = rates()[std::clamp(st.rate, 0, (int)rates().size() - 1)].beats;
    s.stepBeats = chosen > 0.0 ? chosen : (lane.beatsPerStep > 0.0 ? lane.beatsPerStep : 0.25);
    s.mono = lane.pianoMonophonic;
    s.low = lane.pianoLowPitch;
    s.high = lane.pianoHighPitch;
    std::vector<ofxOceanodeTimelinePianoNote> out;
    for(const auto& n : arpbox::arpeggiate(chords, s, outside)) {
        ofxOceanodeTimelinePianoNote note;
        note.startBeat = n.start;
        note.durationBeats = n.dur;
        note.pitch = n.pitch;
        note.velocity = n.vel;
        out.push_back(note);
    }
    return out;
}
// Writes the arpeggio over what the tool wrote before (indices: where that is)
inline void rewrite(State& st, ofxOceanodeTimelinePianoToolContext& context, const std::set<int>& previous) {
    erase(context.lane, previous);
    st.written = generate(st, context.lane, &st.outside);
    context.selection = append(context.lane, st.written);
    st.laneId = context.lane.id;
    st.notesAfter = context.lane.pianoNotes.size();
    st.active = true;
}

inline void draw(ofxOceanodeTimelinePianoToolContext& context) {
    State& st = state();
    auto& lane = context.lane;
    auto& s = st.settings;

    // Are the notes written last still there, as written?
    std::set<int> written;
    bool live = st.active && st.laneId == lane.id && locate(lane, st.written, written) &&
                (!st.written.empty() || lane.pianoNotes.size() == st.notesAfter);
    if(!live) st.active = false;
    // A selection that is not those notes: chords to arpeggiate
    const bool fresh = !context.selection.empty() && !(live && context.selection == written);

    if(fresh) {
        const std::string label = "Arpeggiate " + std::to_string(context.selection.size()) +
                                  (context.selection.size() == 1 ? " selected note" : " selected notes");
        if(ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
            st.chords.clear();
            for(int index : context.selection) st.chords.push_back(lane.pianoNotes[index]);
            rewrite(st, context, context.selection);
            return;
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces them with their arpeggio, over the same stretch of time.\nNotes sounding together are a chord; the chords may change along the way.");
    } else if(live) {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if(ImGui::Button("Restore chords", ImVec2(half, 0.0f))) {
            erase(lane, written);
            context.selection = append(lane, st.chords);
            st.active = false;
            return;
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Puts back the %d notes that were arpeggiated", (int)st.chords.size());
        ImGui::SameLine();
        if(ImGui::Button("Done", ImVec2(-FLT_MIN, 0.0f))) { st.active = false; context.selection.clear(); return; }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the notes as they are: the controls stop rewriting them");
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Select the chords to arpeggiate", ImVec2(-FLT_MIN, 0.0f));
        ImGui::EndDisabled();
    }
    if(live) ImGui::TextDisabled("%d notes written from %d: the controls rewrite them", (int)st.written.size(), (int)st.chords.size());
    if(live && st.outside > 0)
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%d left out: outside the roll's Low..High (%d..%d)", st.outside, lane.pianoLowPitch, lane.pianoHighPitch);
    else ImGui::TextDisabled("Draw chords in the roll, select them, arpeggiate");

    bool changed = false;
    const float labelWidth = ImGui::CalcTextSize("Vel random").x + ImGui::GetStyle().ItemSpacing.x;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    auto tip = [](const char* text) { if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text); };
    auto sliderI = [&](const char* label, const char* id, int& v, int lo, int hi, const char* fmt, const char* help) {
        row(label);
        if(ImGui::SliderInt(id, &v, lo, hi, fmt)) { v = std::clamp(v, lo, hi); changed = true; }
        tip(help);
    };
    auto percent = [&](const char* label, const char* id, float& v, const char* help, const char* zero = nullptr) {
        row(label);
        float value = v * 100.0f;
        if(ImGui::SliderFloat(id, &value, 0.0f, 100.0f, zero && value <= 0.0f ? zero : "%.0f%%")) {
            v = std::clamp(std::round(value) / 100.0f, 0.0f, 1.0f);
            changed = true;
        }
        tip(help);
    };
    auto steps = [&](const char* label, const char* id, float& v, float lo, float hi, const char* help, const char* zero = nullptr) {
        row(label);
        if(ImGui::SliderFloat(id, &v, lo, hi, zero && v <= 0.0f ? zero : "%.2f steps")) { v = std::clamp(v, lo, hi); changed = true; }
        tip(help);
    };
    // Length / hits / rotation of a Euclidean pattern, on one row
    auto euclid = [&](const char* label, const char* id, int& len, int& hits, int& rotate, const char* help) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        const float w = (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(w);
        if(ImGui::DragInt("##hits", &hits, 0.1f, 0, len, "%d hits")) changed = true;
        tip(help);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        if(ImGui::DragInt("##len", &len, 0.1f, 1, 32, "of %d")) changed = true;
        tip("The pattern's length in steps");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::DragInt("##rotate", &rotate, 0.1f, 0, 31, "rot %d")) changed = true;
        tip("Rotates the pattern");
        ImGui::PopID();
        len = std::clamp(len, 1, 32); hits = std::clamp(hits, 0, len); rotate = std::clamp(rotate, 0, 31);
    };

    ImGui::SeparatorText("Rhythm");
    row("Rate");
    st.rate = std::clamp(st.rate, 0, (int)rates().size() - 1);
    if(ImGui::BeginCombo("##arpRate", rates()[st.rate].label)) {
        for(int i = 0; i < (int)rates().size(); i++)
            if(ImGui::Selectable(rates()[i].label, i == st.rate)) { st.rate = i; changed = true; }
        ImGui::EndCombo();
    }
    tip("A step's length. Steps count from the first chord's start.");
    euclid("Pulses", "arpPulses", s.pulseLen, s.pulseHits, s.pulseRotate, "Steps that play, spread evenly over the length (all of them: every step)");
    percent("Step chance", "##arpStepChance", s.stepChance, "The chance each step plays");
    steps("Swing", "##arpSwing", s.swing, 0.0f, 1.0f, "Every other step starts this late", "Off");

    ImGui::SeparatorText("Pattern");
    row("Order");
    static const char* const orders[] = {"Up", "Down", "Up and down", "Down and up", "Random", "As played"};
    if(ImGui::BeginCombo("##arpOrder", orders[std::clamp(s.order, 0, 5)])) {
        for(int i = 0; i < 6; i++) if(ImGui::Selectable(orders[i], i == s.order)) { s.order = i; changed = true; }
        ImGui::EndCombo();
    }
    tip("The order the chord's notes play in. As played: the order they start in the roll.");
    sliderI("Octaves", "##arpOctaves", s.octaves, 1, 6, "%d", "The chord, then again this many octaves up");
    sliderI("Cycle", "##arpCycle", s.cycle, 0, 64, s.cycle <= 0 ? "Off" : "%d steps", "The pattern (and its accents) start again after this many steps");
    row("");
    if(ImGui::Checkbox("Restart with each chord", &s.restart)) changed = true;
    tip("Each chord starts the pattern again. Off: it runs on over the chord changes.");

    ImGui::SeparatorText("Voices");
    sliderI("Polyphony", "##arpPoly", s.polyphony, 1, 8, "%d", "Notes per step");
    ImGui::BeginDisabled(s.polyphony <= 1);
    sliderI("Interval", "##arpInterval", s.interval, 1, 8, "%d", "How many chord notes apart a step's notes are");
    steps("Strum", "##arpStrum", s.strum, 0.0f, 1.0f, "A step's notes start this far apart", "Off");
    ImGui::EndDisabled();
    percent("Note chance", "##arpNoteChance", s.noteChance, "The chance each note of a step plays");
    percent("Octave jump", "##arpOctaveChance", s.octaveChance, "The chance a note plays octaves up", "Off");
    ImGui::BeginDisabled(s.octaveChance <= 0.0f);
    sliderI("Jump range", "##arpOctaveRange", s.octaveRange, 1, 4, "up to %d oct", "How many octaves up it may go");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Notes");
    steps("Length", "##arpGate", s.gate, 0.05f, 8.0f, "How long each note is (1: up to the next step)");
    steps("Len random", "##arpGateRandom", s.gateRandom, 0.0f, 4.0f, "Each step's notes are up to this much longer", "Off");
    percent("Velocity", "##arpVelocity", s.velocity, "Every note's velocity");
    percent("Vel random", "##arpVelocityRandom", s.velocityRandom, "Each note is up to this much louder", "Off");
    {
        float value = s.accentStrength * 100.0f;
        row("Accent");
        if(ImGui::SliderFloat("##arpAccent", &value, -100.0f, 100.0f, std::abs(value) < 0.5f ? "Off" : "%+.0f%%")) {
            s.accentStrength = std::clamp(std::round(value) / 100.0f, -1.0f, 1.0f);
            changed = true;
        }
        tip("Velocity added to the accented steps");
    }
    ImGui::BeginDisabled(std::abs(s.accentStrength) < 0.005f);
    euclid("Accents", "arpAccents", s.accentLen, s.accentHits, s.accentRotate, "Accented steps, spread evenly over the length (counted in played steps)");
    ImGui::EndDisabled();
    steps("Humanize", "##arpHumanize", s.humanize, 0.0f, 0.5f, "Each note starts up to this late", "Off");

    ImGui::SeparatorText("Bass");
    row("");
    if(ImGui::Checkbox("Add the chord's lowest note", &s.bass)) changed = true;
    ImGui::BeginDisabled(!s.bass);
    sliderI("Every", "##arpBassEvery", s.bassEvery, 1, 32, "%d steps", "Which steps get a bass note");
    sliderI("Octave", "##arpBassOctave", s.bassOctave, -4, 0, "%d", "Octaves under the chord's lowest note");
    percent("Velocity", "##arpBassVelocity", s.bassVelocity, "The bass notes' velocity");
    ImGui::EndDisabled();

    ImGui::Separator();
    {
        const float button = ImGui::CalcTextSize("New").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Seed");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-button);
        if(ImGui::DragInt("##arpSeed", &s.seed, 1.0f, 0, 99999)) { s.seed = std::clamp(s.seed, 0, 99999); changed = true; }
        tip("Every random choice above (chances, Random order, jumps...). Change it for another take.");
        ImGui::SameLine();
        if(ImGui::Button("New##arpSeed")) {
            static std::mt19937 generator{std::random_device{}()};
            s.seed = std::uniform_int_distribution<int>(1, 99999)(generator);
            changed = true;
        }
    }

    // The lane's own settings count too (its grid as the rate, Mono, its range)
    if(live && !fresh) {
        if(!changed) {
            const auto now = generate(st, lane);
            changed = now.size() != st.written.size();
            for(size_t i = 0; !changed && i < now.size(); i++) changed = !sameNote(now[i], st.written[i]);
        }
        if(changed) rewrite(st, context, written);
    }
}

// Once, when the addon registers its nodes
inline void registerTool() {
    ofxOceanodeTimelinePianoTool tool;
    tool.name = "Arpeggiator";
    tool.tab = "Arp";
    tool.description = "Select chords drawn as notes: the arpeggiator writes their arpeggio in their place.";
    tool.draw = [](ofxOceanodeTimelinePianoToolContext& context) { draw(context); };
    ofxOceanodeTimelineRegisterPianoTool(std::move(tool));
}

} // namespace scPianoRollArpeggiator

#else

namespace scPianoRollArpeggiator { inline void registerTool() {} }

#endif
