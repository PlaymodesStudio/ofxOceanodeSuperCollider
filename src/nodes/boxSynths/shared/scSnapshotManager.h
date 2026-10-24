//
//  scSnapshotManager.h
//  ofxOceanodeSuperCollider
//
//  The list that manages a node's snapshots, shared by the synths: click a
//  row to recall it, type in its name to rename it, drag it (or its arrows)
//  to another place, x to delete it (asks first). Drawn inline (a tab of the
//  node's window) or as its own window ("Manage..." next to a snapshot bar).
//
//  The node gives its storage through callbacks over ENTRIES (the snapshots
//  in list order); bindSlots() does it for the nodes that keep fixed slots
//  (std::array<ofJson, N> + used flags, the name in json["name"]).
//

#pragma once

#include "ofMain.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

struct scSnapshotManager {
    std::function<int()> count;
    std::function<int(int)> number;                      // shown before the name (1-based)
    std::function<std::string(int)> name;
    std::function<void(int, const std::string&)> rename;
    std::function<void(int)> remove;
    std::function<void(int, int)> move;                  // entry from -> place of entry to
    std::function<void(int)> recall;
    std::function<int()> active;                         // entry, -1: none
    bool windowOpen = false;
    std::string emptyText = "No snapshots yet: store one.";

    // The list, in the space left (height 0: all of it)
    void drawList(float height = 0.0f) {
        if(!count) return;
        ImGui::BeginChild("##scSnapList", ImVec2(0, height), ImGuiChildFlags_Borders);
        const int n = count();
        if(n == 0) ImGui::TextDisabled("%s", emptyText.c_str());
        const float rowH = ImGui::GetFrameHeight();
        const float numW = ImGui::CalcTextSize("000").x + 8.0f;
        const float btnW = rowH;
        int moveFrom = -1, moveTo = -1, removeAt = -1;
        for(int i = 0; i < n; i++) {
            ImGui::PushID(i);
            // number: click recalls; drag to move
            const bool isActive = active && active() == i;
            const std::string num = ofToString(number ? number(i) : i + 1);
            if(ImGui::Selectable(num.c_str(), isActive, 0, ImVec2(numW, rowH))) if(recall) recall(i);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip(move ? "Click: recall. Drag: move it to another place." : "Click: recall.");
            if(move && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SC_SNAPSHOT_ROW", &i, sizeof(int));
                ImGui::Text("%s  %s", num.c_str(), name ? name(i).c_str() : "");
                ImGui::EndDragDropSource();
            }
            if(move && ImGui::BeginDragDropTarget()) {
                if(const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SC_SNAPSHOT_ROW")) {
                    moveFrom = *static_cast<const int*>(p->Data);
                    moveTo = i;
                }
                ImGui::EndDragDropTarget();
            }
            // name: edited in place, saved when the field is left (or Enter)
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - (move ? 3 : 1) * (btnW + 4.0f)));
            if(editing != i) {
                const std::string nm = name ? name(i) : std::string();
                std::snprintf(editBuf, sizeof(editBuf), "%s", nm.c_str());
            }
            ImGui::InputText("##scSnapName", editBuf, sizeof(editBuf));
            if(ImGui::IsItemActivated()) editing = i;
            if(ImGui::IsItemDeactivated()) {
                if(ImGui::IsItemDeactivatedAfterEdit() && rename) rename(i, ofTrim(std::string(editBuf)));
                editing = -1;
            }
            // move up / down (a list in its own order), delete
            if(move) {
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if(ImGui::ArrowButton("##up", ImGuiDir_Up)) { moveFrom = i; moveTo = i - 1; }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i == n - 1);
                if(ImGui::ArrowButton("##down", ImGuiDir_Down)) { moveFrom = i; moveTo = i + 1; }
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if(ImGui::Button("x", ImVec2(btnW, 0))) { confirmDelete = i; ImGui::OpenPopup("##scSnapDelete"); }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this snapshot");
            if(confirmDelete == i && ImGui::BeginPopup("##scSnapDelete")) {
                ImGui::Text("Delete \"%s\"?", name ? name(i).c_str() : "");
                if(ImGui::Button("Delete")) { removeAt = i; confirmDelete = -1; ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if(ImGui::Button("Cancel")) { confirmDelete = -1; ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        // after the loop: the entries change places
        if(moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveTo < n && move) { move(moveFrom, moveTo); editing = -1; }
        if(removeAt >= 0 && removeAt < n && remove) { remove(removeAt); editing = -1; }
    }

    // The same list in its own window, while windowOpen
    void drawWindow(const std::string& title) {
        if(!windowOpen) return;
        ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
        if(ImGui::Begin((title + "##scSnapManager").c_str(), &windowOpen)) {
            ImGui::TextDisabled("Click a number to recall, drag it to move. Type to rename.");
            drawList();
        }
        ImGui::End();
    }

    // Fixed slots: the entries are the used slots in slot order; moving one
    // shifts the slots between (empty ones too). remapped(old -> new slot,
    // -1 deleted) lets the node follow (its A / B, its active slot is done
    // here); save() writes the library.
    template<size_t N>
    void bindSlots(std::array<ofJson, N>& slots, std::array<bool, N>& used, int& activeSlot,
                   std::function<void(int)> recallSlot, std::function<void()> save,
                   std::type_identity_t<std::function<void(const std::array<int, N>&)>> remapped = nullptr) {
        auto list = [&slots, &used]() {
            std::vector<int> v;
            for(int k = 0; k < (int)N; k++) if(used[k]) v.push_back(k);
            return v;
        };
        count = [list]() { return (int)list().size(); };
        number = [list](int e) { const auto v = list(); return e < (int)v.size() ? v[e] + 1 : 0; };
        name = [list, &slots](int e) -> std::string {
            const auto v = list();
            if(e >= (int)v.size() || !slots[v[e]].is_object() || !slots[v[e]].contains("name")) return {};
            try { return slots[v[e]]["name"].template get<std::string>(); } catch(...) { return {}; }
        };
        rename = [list, &slots, save](int e, const std::string& nm) {
            const auto v = list();
            if(e >= (int)v.size() || nm.empty()) return;
            slots[v[e]]["name"] = nm;
            if(save) save();
        };
        remove = [list, &used, &activeSlot, save, remapped](int e) {
            const auto v = list();
            if(e >= (int)v.size()) return;
            const int s = v[e];
            used[s] = false;
            if(activeSlot == s) activeSlot = -1;
            if(remapped) {
                std::array<int, N> map;
                for(int k = 0; k < (int)N; k++) map[k] = k == s ? -1 : k;
                remapped(map);
            }
            if(save) save();
        };
        move = [list, &slots, &used, &activeSlot, save, remapped](int from, int to) {
            const auto v = list();
            if(from >= (int)v.size() || to >= (int)v.size()) return;
            const int a = v[from], b = v[to];
            std::array<int, N> idx;
            for(int k = 0; k < (int)N; k++) idx[k] = k;
            auto shift = [a, b](auto& arr) {
                if(a < b) std::rotate(arr.begin() + a, arr.begin() + a + 1, arr.begin() + b + 1);
                else std::rotate(arr.begin() + b, arr.begin() + a, arr.begin() + a + 1);
            };
            shift(slots); shift(used); shift(idx);
            std::array<int, N> map;   // old slot -> new slot
            for(int k = 0; k < (int)N; k++) map[idx[k]] = k;
            if(activeSlot >= 0 && activeSlot < (int)N) activeSlot = map[activeSlot];
            if(remapped) remapped(map);
            if(save) save();
        };
        recall = [list, recallSlot](int e) { const auto v = list(); if(e < (int)v.size() && recallSlot) recallSlot(v[e]); };
        active = [list, &activeSlot]() {
            const auto v = list();
            const auto it = std::find(v.begin(), v.end(), activeSlot);
            return it == v.end() ? -1 : (int)(it - v.begin());
        };
    }

    // A vector of snapshots with a name member (ArpeggiatorBox, ChordBox)
    template<typename Snapshot>
    void bindVector(std::vector<Snapshot>& snaps, int& activeIndex, std::function<void(int)> recallIndex,
                    std::function<void()> save) {
        count = [&snaps]() { return (int)snaps.size(); };
        number = [](int e) { return e + 1; };
        name = [&snaps](int e) { return e < (int)snaps.size() ? snaps[e].name : std::string(); };
        rename = [&snaps, save](int e, const std::string& nm) {
            if(e >= (int)snaps.size() || nm.empty()) return;
            snaps[e].name = nm;
            if(save) save();
        };
        remove = [&snaps, &activeIndex, save](int e) {
            if(e >= (int)snaps.size()) return;
            snaps.erase(snaps.begin() + e);
            if(activeIndex == e) activeIndex = -1;
            else if(activeIndex > e) activeIndex--;
            if(save) save();
        };
        move = [&snaps, &activeIndex, save](int from, int to) {
            if(from >= (int)snaps.size() || to >= (int)snaps.size()) return;
            if(from < to) std::rotate(snaps.begin() + from, snaps.begin() + from + 1, snaps.begin() + to + 1);
            else std::rotate(snaps.begin() + to, snaps.begin() + from, snaps.begin() + from + 1);
            if(activeIndex == from) activeIndex = to;
            else if(from < to && activeIndex > from && activeIndex <= to) activeIndex--;
            else if(from > to && activeIndex >= to && activeIndex < from) activeIndex++;
            if(save) save();
        };
        recall = [recallIndex](int e) { if(recallIndex) recallIndex(e); };
        active = [&activeIndex]() { return activeIndex; };
    }

private:
    int editing = -1;
    int confirmDelete = -1;
    char editBuf[64] = "";
};
