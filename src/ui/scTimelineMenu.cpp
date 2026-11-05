//
//  scTimelineMenu.cpp
//  ofxOceanodeSuperCollider
//
//  The members that reach the timeline, out of the header so the nodes that
//  include scTimelineMenu.h do not depend on ofxOceanodeContainer.h and
//  ofxOceanodeTimeline.h.
//

#include "scTimelineMenu.h"
#if OFXOCEANODESC_HAS_TIMELINE || defined(OFXOCEANODE_USE_MIDI)
#include "ofxOceanodeContainer.h"
#endif
#if OFXOCEANODESC_HAS_TIMELINE
#include "ofxOceanodeTimeline.h"
#elif defined(__has_include)
// Not a probe (the flag stays the only switch): a timeline addon on the
// include path without its define means the project lost the define (it was
// once an Xcode C-only flag) and every window "Add to Timeline" is gone.
#if __has_include("ofxOceanodeTimeline.h")
#warning "ofxOceanodeTimeline is in the project but OFX_OCEANODE_HAS_TIMELINE is not defined for C++: timeline menus compiled out. Regenerate the project."
#endif
#endif

namespace {
#ifdef OFXOCEANODE_USE_MIDI
bool midiCompatible(ofxOceanodeAbstractParameter* parameter) {
    if(!parameter || (parameter->getFlags() & ofxOceanodeParameterFlags_ReadOnly)) return false;
    const auto type = parameter->valueType();
    return type == typeid(float).name() || type == typeid(int).name()
        || type == typeid(bool).name() || type == typeid(void).name()
        || type == typeid(std::vector<float>).name() || type == typeid(std::vector<int>).name();
}

std::string midiKey(const ofxOceanodeAbstractParameter& parameter) {
    const auto hierarchy = parameter.getGroupHierarchyNames();
    return hierarchy.empty() ? std::string() : hierarchy.front() + "-|-" + parameter.getEscapedName();
}
#endif
}

bool scTimelineMenu::hasMidiBinding(const std::string& key) const {
#ifdef OFXOCEANODE_USE_MIDI
    auto* p = parameter(key);
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!p || !container) return false;
    const auto id = midiKey(*p);
    auto contains = [&](const auto& bindings) {
        const auto it = bindings.find(id);
        return it != bindings.end() && !it->second.empty();
    };
    return contains(container->getMidiBindings()) || contains(container->getPersistentMidiBindings());
#else
    (void)key;
    return false;
#endif
}

bool scTimelineMenu::bindMidi(const std::string& key) {
#ifdef OFXOCEANODE_USE_MIDI
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!container) return false;
    auto* p = ensure(key);
    const bool bound = midiCompatible(p) && bool(container->createMidiBinding(*p));
    if(!bound) settle(key); // failed/unsupported publication must not leave a hidden pin
    return bound;
#else
    (void)key;
    return false;
#endif
}

bool scTimelineMenu::unbindLastMidi(const std::string& key) {
#ifdef OFXOCEANODE_USE_MIDI
    auto* p = parameter(key);
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!p || !container) return false;
    const auto& bindings = container->getMidiBindings();
    const auto it = bindings.find(midiKey(*p));
    // The core removal API expects a nonempty ordinary binding list.
    // Persistent bindings belong to the host's persistent MIDI configuration.
    if(it == bindings.end() || it->second.empty()) return false;
    const bool removed = container->removeLastMidiBinding(*p);
    settle(key);
    return removed;
#else
    (void)key;
    return false;
#endif
}

void scTimelineMenu::midiItems(const std::string& key, const char* title) {
#ifdef OFXOCEANODE_USE_MIDI
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!container) return;
    auto* existing = parameter(key);
    if(existing && !midiCompatible(existing)) return;
    ImGui::Separator();
    if(title && !ImGui::BeginMenu((std::string("MIDI: ") + title + "##midi" + key).c_str())) return;
    if(ImGui::Selectable(("Bind MIDI##midiBind" + key).c_str())) bindMidi(key);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Learn the next MIDI control or note for this parameter.");
    // Binding may have published the parameter; resolve it again after the action.
    auto* p = parameter(key);
    bool removable = false;
    if(p) {
        const auto& bindings = container->getMidiBindings();
        const auto it = bindings.find(midiKey(*p));
        removable = it != bindings.end() && !it->second.empty();
    }
    if(ImGui::Selectable(("Unbind last MIDI##midiUnbind" + key).c_str(), false,
                         removable ? 0 : ImGuiSelectableFlags_Disabled)) unbindLastMidi(key);
    if(title) ImGui::EndMenu();
#else
    (void)key; (void)title;
#endif
}


#if OFXOCEANODESC_HAS_TIMELINE
namespace {
using TimelineRef = ofxOceanodeTimelineManager::TimelineRef;
// The timelines a node's parameters can go on: those of its canvas and of
// every canvas around it, from the root down (a timeline drives what is
// inside its canvas). `canvases` gets the canvases walked, the node's last.
std::vector<TimelineRef> eligibleTimelines(ofxOceanodeTimelineManager& timeline, const std::string& parents,
                                           std::vector<std::string>* canvases = nullptr) {
    std::vector<std::string> walked{std::string()};
    for(size_t at = parents.find(" / "); ; at = parents.find(" / ", at + 3)) {
        const std::string prefix = at == std::string::npos ? parents : parents.substr(0, at);
        if(!prefix.empty() && prefix != "Canvas" && prefix != "0" && std::find(walked.begin(), walked.end(), prefix) == walked.end())
            walked.push_back(prefix);
        if(at == std::string::npos) break;
    }
    std::vector<TimelineRef> refs;
    for(const auto& canvas : walked)
        for(const auto& ref : timeline.timelinesOfCanvas(canvas)) refs.push_back(ref);
    if(canvases != nullptr) *canvases = walked;
    return refs;
}
std::string timelineLabel(const ofxOceanodeTimelineManager& timeline, const TimelineRef& ref) {
    const bool root = ref.canvas.empty() || ref.canvas == "Canvas" || ref.canvas == "0";
    return (root ? std::string() : ref.canvas + " > ") + timeline.timelineName(ref);
}
} // namespace
#endif

bool scTimelineMenu::isOnTimeline(const std::string& key) const {
#if OFXOCEANODESC_HAS_TIMELINE
    auto* p = parameter(key);
    auto* container = node ? node->getHostContainer() : nullptr;
    return p && container && container->getAutomationExtension() && container->getTimelineManager().isParameterBound(*p);
#else
    (void)key;
    return false;
#endif
}

bool scTimelineMenu::hasTimeline() const {
#if OFXOCEANODESC_HAS_TIMELINE
    auto* container = node ? node->getHostContainer() : nullptr;
    return container && container->getAutomationExtension();
#else
    return false;
#endif
}

bool scTimelineMenu::addChordTrack(ofxOceanodeAbstractParameter& gates, const std::string& name) {
#if OFXOCEANODESC_HAS_TIMELINE
    if(!hasTimeline()) return false;
    auto& timeline = node->getHostContainer()->getTimelineManager();
    // On the first timeline of the nearest canvas around the node that has one.
    const auto refs = eligibleTimelines(timeline, node->getParents());
    if(refs.empty()) return false;
    const auto& ref = refs.back();
    auto* sequence = timeline.findTimeline(ref);
    if(sequence == nullptr) return false;
    const std::string trackId = timeline.createTrack(*sequence, name);
    if(timeline.requestParameterLane(ref, trackId, gates)) return true;
    timeline.removeTrack(*sequence, trackId);
    return false;
#else
    (void)gates; (void)name;
    return false;
#endif
}

void scTimelineMenu::items(const std::string& key, bool separator) {
#if OFXOCEANODESC_HAS_TIMELINE
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!container || !container->getAutomationExtension()) return;
    auto& timeline = container->getTimelineManager();
    auto* existing = parameter(key);
    if(existing && !timeline.isStepLaneCompatible(*existing)) return;
    // Every timeline it can go on (its canvas's and those around it), each
    // with its tracks; where it is bound already, in any of them.
    std::vector<std::string> canvases;
    const auto refs = eligibleTimelines(timeline, node->getParents(), &canvases);
    struct Bound { TimelineRef ref; std::string trackId, trackName, bindingId; };
    std::vector<Bound> bound;
    if(existing)
        for(const auto& ref : refs) {
            const auto* sequence = timeline.findTimeline(ref);
            if(sequence == nullptr) continue;
            const std::string path = timeline.resolverFor(*sequence).pathOf(*existing);
            if(path.empty()) continue;
            for(const auto& track : sequence->tracks)
                for(const auto& binding : track.bindings)
                    if(binding.parameterPath == path) bound.push_back({ref, track.id, track.name, binding.id});
        }
    if(separator) ImGui::Separator();
    if(ImGui::BeginMenu(("Add to Timeline Track##tl" + key).c_str())) {
        if(refs.empty()) {
            if(ImGui::Selectable("New Timeline...")) {
                if(auto* p = ensure(key)) {
                    ofxOceanodeTimelineManager::NewTimelineRequest request;
                    request.canvas = canvases.back();
                    request.parameter = p;
                    timeline.requestNewTimeline(request);
                    settle(key);
                }
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("No timeline on this canvas or around it: make one, with this parameter on its first track.");
        }
        for(const auto& ref : refs) {
            auto* sequence = timeline.findTimeline(ref);
            if(sequence == nullptr) continue;
            ImGui::PushID(ofxOceanodeTimelineManager::timeSourceId(ref).c_str());
            if(refs.size() > 1) ImGui::TextDisabled("%s", timelineLabel(timeline, ref).c_str());
            if(ImGui::Selectable("New Timeline Track")) {
                if(auto* p = ensure(key)) {
                    const std::string trackId = timeline.createTrack(*sequence, "Timeline Track");
                    if(!timeline.addBinding(*sequence, trackId, *p).empty()) timeline.requestTrackRename(ref, trackId, true);
                    else timeline.removeTrack(*sequence, trackId);
                    settle(key);
                }
            }
            for(const auto& track : sequence->tracks) {
                if(track.isWaveTrack || track.isFolder || track.isMacroTrack()) continue;
                const bool already = std::any_of(bound.begin(), bound.end(),
                                                 [&](const Bound& b) { return b.ref == ref && b.trackId == track.id; });
                if(ImGui::Selectable((track.name + "##tl" + track.id).c_str(), already)) {
                    if(auto* p = ensure(key)) timeline.requestParameterLane(ref, track.id, *p);
                    settle(key);
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a lane for this parameter and open its clip.");
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    // A piano-roll row: on the one timeline, or the one picked.
    if(refs.size() <= 1) {
        if(ImGui::Selectable(("Add to Timeline as Piano Roll...##tl" + key).c_str()) && !refs.empty())
            if(auto* p = ensure(key)) timeline.requestNoteGroupSetup(refs.front(), p);
    } else if(ImGui::BeginMenu(("Add to Timeline as Piano Roll...##tl" + key).c_str())) {
        for(const auto& ref : refs)
            if(ImGui::MenuItem((timelineLabel(timeline, ref) + "##pr" + ofxOceanodeTimelineManager::timeSourceId(ref)).c_str()))
                if(auto* p = ensure(key)) timeline.requestNoteGroupSetup(ref, p);
        ImGui::EndMenu();
    }
    auto remove = [&](const Bound& b) {
        if(auto* sequence = timeline.findTimeline(b.ref)) timeline.removeBinding(*sequence, b.trackId, b.bindingId);
        prune();
    };
    if(bound.size() == 1) {
        if(ImGui::Selectable(("Remove from Timeline##tl" + key).c_str())) remove(bound.front());
    } else if(bound.size() > 1 && ImGui::BeginMenu(("Remove from Timeline##tl" + key).c_str())) {
        for(const auto& b : bound)
            if(ImGui::MenuItem((timelineLabel(timeline, b.ref) + " / " + b.trackName + "##tl" + b.bindingId).c_str())) remove(b);
        ImGui::EndMenu();
    }
#else
    (void)key; (void)separator;
#endif
}

void scTimelineMenu::mark(const std::string& key) const {
#if OFXOCEANODESC_HAS_TIMELINE
    auto* p = parameter(key);
    auto* container = p && node ? node->getHostContainer() : nullptr;
    ofColor colour;
    if(!container || !container->getAutomationExtension() || !container->getTimelineManager().getParameterTrackColor(*p, colour)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, a.y + 1.0f), ImVec2(b.x, a.y + 1.0f),
                                        IM_COL32(colour.r, colour.g, colour.b, 255), 2.0f);
#else
    (void)key;
#endif
}
