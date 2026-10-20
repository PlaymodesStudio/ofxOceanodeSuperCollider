//
//  scTimelineMenu.h
//  ofxOceanodeSuperCollider
//
//  "Add to Timeline" straight from a node window's right-click menu, for the
//  nodes that publish their window controls into the node (SampleBox,
//  GrainBox, PolyComb...). SC Mixer Pro has its own version of the same idea.
//
//  The timeline binds parameters by their path in the node, so a control has
//  to be a node parameter to be automated. Adding it to the timeline from the
//  window publishes it through the node's own publishing (so presets restore
//  it before the timeline resolves its bindings) and hides it from the node
//  GUI (ofxOceanodeParameterFlags_NoGuiWidget): a control published only for
//  the timeline does not clutter the node. "Publish to Node GUI" shows it;
//  unpublishing a control still on the timeline hides it instead. A hidden
//  control no timeline track binds any more is unpublished again (update()).
//
//  A node:
//    timeline.setup(this, publish, unpublish, publishedKeys)
//    in a control's right-click popup:   timeline.menuItems(key)
//      and for its publish entry:        timeline.isHidden(key) -> offer
//      "Publish to Node GUI" (timeline.reveal(key)); unpublish via
//      timeline.unpublish(key) (hides it while the timeline needs it)
//    after the control (its mark):       timeline.mark(key)
//    update():                           timeline.update()
//    presetSave / after restoring its published controls:
//                                        timeline.save(json, prefix) / load(json, prefix)
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeParameter.h"
#include "imgui.h"
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

class scTimelineMenu {
public:
    using Publish = std::function<void(const std::string& key)>;
    using Keys = std::function<std::vector<std::string>()>;

    void setup(ofxOceanodeNodeModel* node, Publish publish, Publish unpublish, Keys publishedKeys) {
        this->node = node;
        this->publishFn = std::move(publish);
        this->unpublishFn = std::move(unpublish);
        this->publishedKeysFn = std::move(publishedKeys);
    }

    // The control's node parameter (published, hidden or not), or nullptr
    ofxOceanodeAbstractParameter* parameter(const std::string& key) const {
        if(!node) return nullptr;
        auto& group = node->getParameterGroup();
        if(!group.contains(key)) return nullptr;
        return dynamic_cast<ofxOceanodeAbstractParameter*>(&group.get(key));
    }
    bool isHidden(const std::string& key) const { return hidden.count(key) > 0; }

    bool isOnTimeline(const std::string& key) const;   // scTimelineMenu.cpp

    // "Publish to Node GUI" on a control that is only on the timeline
    void reveal(const std::string& key) {
        hidden.erase(key);
        setHiddenFlag(key, false);
        refreshSeparator();
    }
    // Unpublish from the node: hidden instead while the timeline drives it
    void unpublish(const std::string& key) {
        if(isOnTimeline(key)) {
            hidden.insert(key);
            setHiddenFlag(key, true);
            refreshSeparator();
            return;
        }
        hidden.erase(key);
        if(unpublishFn) unpublishFn(key);
        refreshSeparator();
    }

    // The timeline entries of a control's right-click menu. title: a menu
    // that holds several controls (an EQ band's): its entries in a submenu
    void menuItems(const std::string& key, const char* title = nullptr) {
#if OFXOCEANODESC_HAS_TIMELINE
        if(title) {
            if(!node || !node->getHostContainer()) return;
            ImGui::Separator();
            if(ImGui::BeginMenu((std::string("Timeline: ") + title + "##tlsub" + key).c_str())) {
                items(key, false);
                ImGui::EndMenu();
            }
            return;
        }
        items(key, true);
#else
        (void)key; (void)title;
#endif
    }

private:
    void items(const std::string& key, bool separator);   // scTimelineMenu.cpp

public:
    // A window control that edits a node parameter already (no publishing):
    // its mark, and a right-click menu with a typed value and the timeline
    // entries. Call right after the control.
    void nodeParameterMenu(const std::string& key, const char* title = nullptr) {
        mark(key);
        auto* p = parameter(key);
        if(!p || !ImGui::BeginPopupContextItem(("##tlnode" + key).c_str())) return;
        ImGui::TextDisabled("%s", title ? title : p->getName().c_str());
        ImGui::Separator();
        static std::string editing;
        static char buffer[512];
        if(ImGui::IsWindowAppearing() || editing != key) {
            editing = key;
            std::snprintf(buffer, sizeof(buffer), "%s", p->toString().c_str());
        }
        ImGui::TextDisabled("Value");
        ImGui::SetNextItemWidth(180.0f);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##tlvalue", buffer, sizeof(buffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
            try { p->fromString(std::string(buffer)); } catch(...) {}
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("On the node");
        menuItems(key);
        ImGui::EndPopup();
    }

    // On the timeline: a line in its track's colour along the top of the last item
    void mark(const std::string& key) const;   // scTimelineMenu.cpp

    // Hidden controls no track binds any more go (once a second)
    void update() {
        if(hidden.empty() || ofGetElapsedTimef() < nextPrune) return;
        nextPrune = ofGetElapsedTimef() + 1.0f;
        prune();
    }

    void save(ofJson& json, const std::string& prefix) {
        prune();
        json[prefix] = std::vector<std::string>(hidden.begin(), hidden.end());
    }
    // After the node restored its published controls (the hidden ones among them)
    void load(const ofJson& json, const std::string& prefix) {
        hidden.clear();
        if(json.contains(prefix) && json[prefix].is_array())
            for(const auto& k : json[prefix]) if(k.is_string()) hidden.insert(k.get<std::string>());
        for(const auto& key : hidden) setHiddenFlag(key, true);
        refreshSeparator();
        nextPrune = ofGetElapsedTimef() + 5.0f;   // the timeline loads its bindings after the nodes
    }

private:
    ofxOceanodeNodeModel* node = nullptr;
    Publish publishFn, unpublishFn;
    Keys publishedKeysFn;
    std::set<std::string> hidden;
    float nextPrune = 0.0f;

    // The control as a node parameter: published (hidden) if it was not
    ofxOceanodeAbstractParameter* ensure(const std::string& key) {
        if(!parameter(key)) {
            if(!publishFn) return nullptr;
            publishFn(key);
            if(!parameter(key)) return nullptr;
            hidden.insert(key);
            setHiddenFlag(key, true);
            refreshSeparator();
        }
        return parameter(key);
    }
    // Added to nothing (an incompatible type): give it back
    void settle(const std::string& key) {
        if(isHidden(key) && !isOnTimeline(key)) unpublish(key);
    }
    void prune() {
        const auto keys = hidden;
        for(const auto& key : keys) {
            if(!parameter(key)) { hidden.erase(key); continue; }
            if(isOnTimeline(key)) continue;
            hidden.erase(key);
            if(unpublishFn) unpublishFn(key);
        }
        refreshSeparator();
    }
    void setHiddenFlag(const std::string& key, bool hide) {
        auto* p = parameter(key);
        if(!p) return;
        const auto flags = p->getFlags();
        const auto wanted = hide ? (flags | ofxOceanodeParameterFlags_NoGuiWidget) : (flags & ~ofxOceanodeParameterFlags_NoGuiWidget);
        if(wanted != flags) p->setFlags(wanted);
    }
    // The node's "Published" separator: hidden while it heads hidden controls only
    void refreshSeparator() {
        if(!node || !publishedKeysFn) return;
        bool anyVisible = false;
        for(const auto& key : publishedKeysFn()) if(!isHidden(key)) { anyVisible = true; break; }
        auto& group = node->getParameterGroup();
        for(int i = 0; i < (int)group.size(); i++) {
            if(group.get(i).getName().rfind("SEPARATOR:|Published|", 0) != 0) continue;
            if(auto* p = dynamic_cast<ofxOceanodeAbstractParameter*>(&group.get(i))) {
                const auto flags = p->getFlags();
                const auto wanted = anyVisible ? (flags & ~ofxOceanodeParameterFlags_NoGuiWidget) : (flags | ofxOceanodeParameterFlags_NoGuiWidget);
                if(wanted != flags) p->setFlags(wanted);
            }
        }
    }
};
