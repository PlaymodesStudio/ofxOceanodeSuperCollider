//
//  scTimelineMenu.cpp
//  ofxOceanodeSuperCollider
//
//  The members that reach the timeline, out of the header so the nodes that
//  include scTimelineMenu.h do not depend on ofxOceanodeContainer.h and
//  ofxOceanodeTimeline.h.
//

#include "scTimelineMenu.h"
#if OFXOCEANODESC_HAS_TIMELINE
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeTimeline.h"
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
                if(ImGui::Selectable((track.name + "##tl" + track.id).c_str(), already, already ? ImGuiSelectableFlags_Disabled : 0)) {
                    if(auto* p = ensure(key)) timeline.addBinding(track.id, *p);
                    settle(key);
                }
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
