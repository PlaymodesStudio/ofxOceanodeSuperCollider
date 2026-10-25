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

void scTimelineMenu::items(const std::string& key, bool separator) {
#if OFXOCEANODESC_HAS_TIMELINE
    auto* container = node ? node->getHostContainer() : nullptr;
    if(!container || !container->getAutomationExtension()) return;
    auto& timeline = container->getTimelineManager();
    auto* existing = parameter(key);
    if(existing && !timeline.isStepLaneCompatible(*existing)) return;
    const std::string path = existing ? container->getTimelineParameterPath(*existing) : "";
    struct Bound { std::string trackId, trackName, bindingId; };
    std::vector<Bound> bound;
    if(!path.empty())
        for(const auto& track : timeline.getTracks())
            for(const auto& binding : track.bindings)
                if(binding.parameterPath == path) bound.push_back({track.id, track.name, binding.id});
    if(separator) ImGui::Separator();
    if(ImGui::BeginMenu(("Add to Timeline Track##tl" + key).c_str())) {
        if(ImGui::Selectable("New Timeline Track")) {
            if(auto* p = ensure(key)) {
                const std::string trackId = timeline.createTrack();
                if(!timeline.addBinding(trackId, *p).empty()) timeline.requestTrackRename(trackId, true);
                else timeline.removeTrack(trackId);
                settle(key);
            }
        }
        if(!timeline.getTracks().empty()) {
            ImGui::Separator();
            for(const auto& track : timeline.getTracks()) {
                if(track.isWaveTrack) continue;
                const bool already = std::any_of(bound.begin(), bound.end(), [&](const Bound& b) { return b.trackId == track.id; });
                if(ImGui::Selectable((track.name + "##tl" + track.id).c_str(), already)) {
                    if(auto* p = ensure(key)) timeline.requestParameterLane(track.id, *p);
                    settle(key);
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a lane for this parameter and open its clip.");
            }
        }
        ImGui::EndMenu();
    }
    if(ImGui::Selectable(("Add to Timeline as Piano Roll...##tl" + key).c_str()))
        if(auto* p = ensure(key)) timeline.requestNoteGroupSetup(p);
    if(bound.size() == 1) {
        if(ImGui::Selectable(("Remove from Timeline##tl" + key).c_str())) {
            timeline.removeBinding(bound.front().trackId, bound.front().bindingId);
            prune();
        }
    } else if(bound.size() > 1 && ImGui::BeginMenu(("Remove from Timeline##tl" + key).c_str())) {
        for(const auto& b : bound)
            if(ImGui::MenuItem((b.trackName + "##tl" + b.bindingId).c_str())) {
                timeline.removeBinding(b.trackId, b.bindingId);
                prune();
            }
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
