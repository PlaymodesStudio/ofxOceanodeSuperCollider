//
//  scDrumKitBoxWindow.cpp
//  ofxOceanodeSuperCollider
//
//  DrumKitBox's window: the pads (left, the sample browser below), the
//  selected pad's header and envelope view, and the tabs: Layers, Voice,
//  Modulation, Routing, Mixer. GrainBox's look (label column | slider,
//  filled section headers), SampleBox's pad list and mixer.
//

#include "scDrumKitBox.h"
#include "../../../ui/scTypedValue.h"
#include "ofxOceanodeShared.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <filesystem>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  34);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 handle     = IM_COL32(235, 235, 240, 230);
const unsigned char accent[3] = {226, 120, 82};     // DrumKitBox: the node's colour
const unsigned char padRGB[8][3] = {
    {236, 150,  80}, {108, 180, 232}, {230, 200,  90}, {228, 122, 146},
    {120, 200, 140}, {186, 146, 232}, { 96, 200, 176}, {232, 146, 104},
};
// Body, Click, Noise, Metal, Sample
const unsigned char layerRGB[5][3] = {{236, 150, 80}, {220, 220, 230}, {108, 180, 232}, {230, 200, 90}, {120, 200, 140}};
ImU32 accentCol(int alpha) { return IM_COL32(accent[0], accent[1], accent[2], alpha); }
ImU32 padCol(int k, int alpha) { const auto* c = padRGB[((k % 8) + 8) % 8]; return IM_COL32(c[0], c[1], c[2], alpha); }
ImU32 layerCol(int l, int alpha) { const auto* c = layerRGB[std::clamp(l, 0, 4)]; return IM_COL32(c[0], c[1], c[2], alpha); }
}
const char* layerNames[] = {"Body", "Click", "Noise", "Metal", "Sample"};
const char* fxNames[] = {"EQ", "Comp", "Sat", "Echo", "Grains", "Reverb", "Width"};

const scDrumKitBox::Spec* specOf(const char* label) {
    for(const auto& s : scDrumKitBox::specs()) if(std::strcmp(s.label, label) == 0) return &s;
    return nullptr;
}
// "Body Decay" in the Body column reads "Decay"
std::string shortLabel(const scDrumKitBox::Spec& s) {
    static const std::map<std::string, std::string> prefix = {
        {"body", "Body "}, {"click", "Click "}, {"noise", "Noise "}, {"metal", "Metal "}, {"sample", "Sample "},
        {"comp", "Comp "}, {"sat", "Sat "}, {"echo", "Echo "}, {"grain", "Grain "}, {"reverb", "Rev "}, {"duck", "Duck "}};
    std::string l = s.label;
    auto it = prefix.find(s.section);
    if(it != prefix.end() && l.rfind(it->second, 0) == 0 && l.size() > it->second.size()) l = l.substr(it->second.size());
    return l;
}
// SC's Env curve: progress 0..1 of a segment at x (0..1)
float envShape(float x, float c) {
    if(std::abs(c) < 0.001f) return x;
    return (1.0f - std::exp(c * x)) / (1.0f - std::exp(c));
}
// attack (linear), hold, decay (curve) at time t
float ahd(float t, float a, float h, float d, float c) {
    if(t < 0.0f) return 0.0f;
    if(t < a) return t / std::max(1e-6f, a);
    if(t < a + h) return 1.0f;
    if(t < a + h + d) return 1.0f - envShape((t - a - h) / std::max(1e-6f, d), c);
    return 0.0f;
}
}

// ════════════════════════════════════════════════════════════════════════════
// Widgets
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::drawPublishMenu(const std::string& key) {
    if(!ImGui::BeginPopupContextItem(("##drumKitBoxPublish" + key).c_str())) return;
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::Separator();
    if(Control* c = findControl(key); c && c->save && c->load)
        scTyped::field(key, c->save, [&](const std::string& t){ c->load(ofTrim(t)); }, false);
    if(getParameterGroup().contains(key) && !published.count(key)) ImGui::TextDisabled("Always on the node");
    else if(timeline.isHidden(key)) { if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key); }
    else if(published.count(key)) {
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
    } else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
    timeline.menuItems(key);
    if(Control* c = findControl(key); c && c->pad >= 0 && c->pad < (int)pads.size()) pads[c->pad]->mod.modulateMenu(key);
    else if(!pads.empty()) pads[std::clamp(selected, 0, (int)pads.size() - 1)]->mod.modulateMenu(key);
    ImGui::EndPopup();
}

void scDrumKitBox::drawPublishedUnderline(const std::string& key) const {
    timeline.mark(key);
    if(!published.count(key) || timeline.isHidden(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

void scDrumKitBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool logarithmic, const char* tip) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    if(tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt,
                          logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None)) p.set(v);
    if(tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scDrumKitBox::sliderInt(ofParameter<int>& p, const char* label, float w, const char* fmt, const char* tip) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    if(tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax(), fmt)) p.set(v);
    if(tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scDrumKitBox::combo(ofParameter<int>& p, const char* label, float w, const std::vector<std::string>& items, const char* tip) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    if(tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    {
        const auto it = published.find(key);
        dropdowns.set(key, items, it == published.end() ? nullptr : it->second);
    }
    if(!items.empty()) {
        const int v = std::max(0, std::min((int)items.size() - 1, p.get()));
        if(ImGui::BeginCombo("##v", items[v].c_str())) {
            for(int i = 0; i < (int)items.size(); i++) if(ImGui::Selectable(items[i].c_str(), i == v)) p.set(i);
            ImGui::EndCombo();
        }
        if(tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
        drawPublishMenu(key);
    }
    ImGui::PopID();
}

void scDrumKitBox::toggle(ofParameter<bool>& p, const char* label, const char* tip) {
    bool v = p.get();
    ImGui::PushID(p.getEscapedName().c_str());
    if(ImGui::Checkbox(label, &v)) p.set(v);
    drawPublishedUnderline(p.getEscapedName());
    if(tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    drawPublishMenu(p.getEscapedName());
    ImGui::PopID();
}

void scDrumKitBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float padV = 3.0f * z, padH = 6.0f * z;
    const float bh = ImGui::GetFrameHeight() + 2.0f * padV;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent ? accent : pal::accentCol(120), 1.5f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + padH, cur.y + padV));
    if(on) {
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) on->set(v);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off (off: no CPU)", title);
        drawPublishedUnderline(on->getEscapedName());
        drawPublishMenu(on->getEscapedName());
        ImGui::PopID();
        ImGui::SameLine(0, 4.0f * z);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::headerText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f * z));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void scDrumKitBox::beginColumn(const char* id) { ImGui::PushID(id); ImGui::BeginGroup(); }
void scDrumKitBox::endColumn(bool last) {
    ImGui::EndGroup();
    ImGui::PopID();
    if(!last) ImGui::SameLine(0, 12.0f * ofxOceanodeShared::getZoomLevel());
}

// One control as its spec says (combos get their names here)
void scDrumKitBox::drawSpec(Pad& p, const Spec& s, float w) {
    const std::string label = shortLabel(s);
    if(s.type == 'f') { slider(p.F(s.label), label.c_str(), w, s.fmt, s.log, s.tip); return; }
    if(s.type == 'b') { toggle(p.B(s.label), label.c_str(), s.tip); return; }
    std::vector<std::string> items;
    const std::string l = s.label;
    if(l == "Channel") {
        items = {"Auto: row " + ofToString(p.index + 1)};
        for(int c = 1; c <= MaxChannels; c++) { const std::string src = channelSource(c - 1); items.push_back(ofToString(c) + (src.empty() ? "" : "  " + src)); }
    } else if(l == "Choke") {
        items = {"None"};
        for(int g = 1; g <= ChokeGroups; g++) items.push_back("Group " + ofToString(g));
    } else if(l == "Duck From") {
        items = {"Off"};
        for(int k = 0; k < MaxPads; k++) items.push_back(k < (int)pads.size() ? padLabel(*pads[k]) : "Pad " + ofToString(k + 1));
    } else if(l == "Model") items = modelNames();
    else if(l == "Style") items = styleNames(p.I("Model").get());
    else if(l == "Click Type") items = {"Pulse", "Noise", "Blip", "Ping"};
    else if(l == "Noise Filter") items = {"Low pass", "Band pass", "High pass", "Notch"};
    else if(l == "Metal Mode") items = {"Square", "Ring", "FM"};
    else if(l == "Filter Type") items = {"LP 12", "LP 24", "High pass", "Band pass"};
    else if(l == "Drive Type") items = {"Soft", "Hard", "Fold", "Tube"};
    else if(l == "Echo Filter") items = {"Low pass", "High pass", "Band pass", "Peak"};
    if(items.empty()) sliderInt(p.I(s.label), label.c_str(), w, s.fmt, s.tip);
    else combo(p.I(s.label), label.c_str(), w, items, s.tip);
}

// Every control of a section but its on/off (drawn in the header)
void scDrumKitBox::drawSection(Pad& p, const char* section, float w) {
    for(const auto& s : specs()) {
        if(std::strcmp(s.section, section) != 0) continue;
        if(s.type == 'b' && std::string(s.label).size() > 3 && std::string(s.label).compare(std::string(s.label).size() - 3, 3, " On") == 0) continue;
        drawSpec(p, s, w);
    }
}

void scDrumKitBox::playButton(Pad& p, const char* id, ImVec2 size) {
    ImGui::Button(id, size);
    if(ImGui::IsItemActivated()) {
        // the gate stays open while the button is held (a Gated pad sounds,
        // a stuttering one buzzes, until it is released)
        p.auditionHeld = true;
        for(auto& [srv, sc] : p.sc) if(sc.trig) sc.trig->set("gate", 1.0f);
        fire(p);
    }
    if(ImGui::IsItemDeactivated() && p.auditionHeld) { p.auditionHeld = false; sendChannelInputs(p); }
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
    else { browser.stopPreview(); windowHovered = false; }
    if(pendingMoveFrom >= 0) { movePad(pendingMoveFrom, pendingMoveTo); pendingMoveFrom = pendingMoveTo = -1; }
    if(pendingDuplicate >= 0) { duplicatePad(pendingDuplicate); pendingDuplicate = -1; }
    if(pendingRemove >= 0) { removePad(pendingRemove); pendingRemove = -1; }
    if(pendingAdd >= 0) { if(Pad* np = addPad(pendingAdd)) selected = np->index; pendingAdd = -1; }
    if(!pendingDropFile.empty() && !pads.empty()) {
        const int k = hoveredPad >= 0 && hoveredPad < (int)pads.size() ? hoveredPad : selected;
        Pad& p = *pads[std::clamp(k, 0, (int)pads.size() - 1)];
        loadSample(p, pendingDropFile);
        if(!p.B("Sample On").get()) p.B("Sample On") = true;
        pendingDropFile.clear();
    }
    keyboardPiano.finishFrame();
}

void scDrumKitBox::drawWindow() {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1180 * z, 800 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = getWindowTitle();
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        windowHovered = false;
        browser.stopPreview();
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open || pads.empty()) { if(!open) showWindow = false; windowHovered = false; browser.stopPreview(); ImGui::End(); return; }
    selected = std::clamp(selected, 0, (int)pads.size() - 1);
    keyboardPiano.beginContent(pitch, gate, 1, padChannel(*pads[selected]));
    windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    hoveredPad = -1;
    const float totalW = ImGui::GetContentRegionAvail().x;
    const float totalH = ImGui::GetContentRegionAvail().y;
    if(totalW < 200.0f * z || totalH < 80.0f * z) { browser.stopPreview(); scKeyboardPiano::endContent(); ImGui::End(); return; }
    Pad& p = *pads[selected];

    // ── Left: the pads, the sample browser below; the splitter ──────────────
    const float dragW = 5.0f * z;
    const float bw = std::min(browserWidth * z, totalW * 0.4f);
    ImGui::BeginChild("##dkLeft", ImVec2(bw, totalH), false);
    const float rowH = ImGui::GetFrameHeight() + 8.0f * z;
    const float listH = std::clamp(pads.size() * rowH + ImGui::GetFrameHeightWithSpacing() * 2.0f + 14.0f * z,
                                   140.0f * z, totalH * 0.62f);
    drawPadList(bw, listH);
    const float browserH = std::max(60.0f * z, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##dkBrowser", ImVec2(bw, browserH), false);
    ImGui::TextColored(ImColor(pal::textDim), "Samples (double-click: the selected pad's Sample layer)");
    browser.draw(bw, browserH);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    scSampleBrowser::drawSplitter("##dkSplit", browserWidth, dragW, totalH, 150.0f, std::max(150.0f, totalW * 0.4f / z), 1.0f / z);
    ImGui::SameLine(0, 0);

    // ── Right: the selected pad ─────────────────────────────────────────────
    ImGui::BeginChild("##dkRight", ImVec2(0, totalH), false);
    const float rightW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 4.0f * z;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * z);
    ImGui::BeginGroup();
    const float w = rightW - 6.0f * z;
    snaps.drawBar(w);
    drawHeader(p, w);
    drawEnvelopeView(p, w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##dkTabs")) {
        if(ImGui::BeginTabItem("Layers")) { ImGui::Spacing(); drawLayersTab(p, w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Voice")) { ImGui::Spacing(); drawVoiceTab(p, w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Modulation")) {
            ImGui::Spacing();
            p.mod.drawEnvelopes(w);
            ImGui::Spacing();
            p.mod.drawLfos(w);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Routing")) { ImGui::Spacing(); p.mod.drawRouting(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Mixer")) { ImGui::Spacing(); drawMixerTab(w); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    scKeyboardPiano::endContent();
    ImGui::End();
}

// ── The pad list ────────────────────────────────────────────────────────────

namespace { struct PadDrag { const void* node; int pad; }; }

void scDrumKitBox::drawPadList(float w, float h) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImGui::BeginChild("##dkPads", ImVec2(w, h), true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetFrameHeight() + 8.0f * z;
    const float innerW = ImGui::GetContentRegionAvail().x;
    for(int k = 0; k < (int)pads.size(); k++) {
        Pad& p = *pads[k];
        ImGui::PushID(k);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const bool isSel = k == selected;
        const bool muted = p.B("Mute").get();
        if(ImGui::Selectable("##row", isSel, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(innerW, rowH))) {
            selected = k;
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { renaming = k; std::snprintf(renameBuf, sizeof(renameBuf), "%s", p.name.c_str()); }
        }
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) hoveredPad = k;
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip("Click: edit   Double-click: rename   Drag: move   Right-click: more\nDrop a sample here to layer it into this pad");
        if(ImGui::BeginDragDropSource()) {
            const PadDrag d{this, k};
            ImGui::SetDragDropPayload("DrumKitBoxPad", &d, sizeof(d));
            ImGui::Text("Move %s", padLabel(p).c_str());
            ImGui::EndDragDropSource();
        }
        if(ImGui::BeginDragDropTarget()) {
            if(const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("DrumKitBoxPad")) {
                const PadDrag d = *(const PadDrag*)pl->Data;
                if(d.node == this && d.pad != k) { pendingMoveFrom = d.pad; pendingMoveTo = k; }
            }
            ImGui::EndDragDropTarget();
        }
        std::string dropped;
        if(browser.acceptDropOnLastItem(dropped)) { loadSample(p, dropped); if(!p.B("Sample On").get()) p.B("Sample On") = true; selected = k; }
        if(ImGui::BeginPopupContextItem("##padMenu")) {
            ImGui::TextDisabled("%s", padLabel(p).c_str());
            ImGui::Separator();
            if(ImGui::MenuItem("Rename")) { renaming = k; std::snprintf(renameBuf, sizeof(renameBuf), "%s", p.name.c_str()); }
            if(ImGui::BeginMenu("Model")) {
                for(int m = 0; m < M_COUNT; m++) if(ImGui::MenuItem(modelNames()[m].c_str(), nullptr, p.I("Model").get() == m)) p.I("Model") = m;
                ImGui::EndMenu();
            }
            const std::string source = channelSource(padChannel(p));
            if(!source.empty() && ImGui::MenuItem(("Name it \"" + source + "\"").c_str())) p.name = source;
            if(ImGui::MenuItem("Duplicate", nullptr, false, (int)pads.size() < MaxPads)) pendingDuplicate = k;
            if(ImGui::MenuItem("Delete", nullptr, false, pads.size() > 1)) pendingRemove = k;
            ImGui::EndPopup();
        }

        // the row: colour, hit light, number and name, model, channel; Play, M, S
        const float cy = a.y + rowH * 0.5f;
        dl->AddRectFilled(ImVec2(a.x + 2 * z, a.y + 4 * z), ImVec2(a.x + 6 * z, a.y + rowH - 4 * z), pal::padCol(k, muted ? 80 : 255));
        const float lightR = 4.0f * z;
        const ImVec2 lc(a.x + 14 * z, cy);
        dl->AddCircleFilled(lc, lightR, pal::padCol(k, 40 + (int)(215 * p.flash)));
        dl->AddCircle(lc, lightR, pal::padCol(k, 120), 0, 1.0f * z);
        const float btn = ImGui::GetFrameHeight();
        const float buttonsW = 3 * btn + 6 * z;
        const float textX = a.x + 24 * z, textR = a.x + innerW - buttonsW - 6 * z;
        const int ch = padChannel(p);
        const std::string chText = "ch " + ofToString(ch + 1);
        if(renaming == k) {
            ImGui::SetCursorScreenPos(ImVec2(textX, cy - ImGui::GetFrameHeight() * 0.5f));
            ImGui::SetNextItemWidth(std::max(40.0f * z, textR - textX));
            if(ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##rename", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if(enter || ImGui::IsItemDeactivated()) { if(renameBuf[0]) p.name = renameBuf; renaming = -1; }
        } else {
            dl->PushClipRect(ImVec2(textX, a.y), ImVec2(textR, a.y + rowH), true);
            const std::string label = padLabel(p);
            const float ty = cy - ImGui::GetTextLineHeight() * 0.5f;
            dl->AddText(ImVec2(textX, ty), muted ? pal::textDim : pal::headerText, label.c_str());
            const float lw = ImGui::CalcTextSize(label.c_str()).x;
            const float cw = ImGui::CalcTextSize(chText.c_str()).x;
            dl->AddText(ImVec2(std::max(textX + lw + 8 * z, textR - cw), ty), pal::textDim, chText.c_str());
            dl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x + innerW - buttonsW, cy - btn * 0.5f));
        playButton(p, "##play", ImVec2(btn, btn));
        {
            const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
            const float s = (b1.y - b0.y) * 0.28f;
            const ImVec2 c((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
            dl->AddTriangleFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x - s * 0.7f, c.y + s), ImVec2(c.x + s, c.y), pal::headerText);
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Play this pad");
        ImGui::SameLine(0, 3 * z);
        auto flag = [&](ofParameter<bool>& f, const char* text, ImU32 on) {
            const bool lit = f.get();   // the colour pushed must be the one popped, whatever the click does
            if(lit) ImGui::PushStyleColor(ImGuiCol_Button, on);
            if(ImGui::Button(text, ImVec2(btn, btn))) f = !f.get();
            if(lit) ImGui::PopStyleColor();
            drawPublishMenu(f.getEscapedName());
        };
        flag(p.B("Mute"), "M", IM_COL32(190, 90, 70, 255));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Mute");
        ImGui::SameLine(0, 3 * z);
        flag(p.B("Solo"), "S", IM_COL32(200, 170, 60, 255));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Solo");
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rowH));
        ImGui::Dummy(ImVec2(innerW, 0));
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled((int)pads.size() >= MaxPads);
    if(ImGui::Button("+ Add pad")) ImGui::OpenPopup("##dkAdd");
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new pad: pick its model. It plays the next channel (the timeline's next row).");
    if(ImGui::BeginPopup("##dkAdd")) {
        for(int m = 0; m < M_COUNT; m++) if(ImGui::MenuItem(modelNames()[m].c_str())) pendingAdd = m;
        ImGui::EndPopup();
    }
    bool anySource = false;
    for(auto& q : pads) if(!channelSource(padChannel(*q)).empty()) anySource = true;
    if(anySource) {
        ImGui::SameLine();
        if(ImGui::Button("Name from input"))
            for(auto& q : pads) { const std::string s = channelSource(padChannel(*q)); if(!s.empty()) q->name = s; }
    }
    ImGui::EndChild();
}

// Number, name, Model, Style, Play; the node's Gain on the right
void scDrumKitBox::drawHeader(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::padCol(p.index, 255));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d", p.index + 1);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", p.name.c_str());
    ImGui::SetNextItemWidth(120.0f * z);
    if(ImGui::InputText("##padName", nameBuf, sizeof(nameBuf))) p.name = nameBuf;
    ImGui::SameLine(0, 10.0f * z);
    const float keep = labelW;
    labelW = 42.0f;
    combo(p.I("Model"), "Model", 170.0f * z, modelNames(), specOf("Model")->tip);
    ImGui::SameLine(0, 10.0f * z);
    combo(p.I("Style"), "Style", 170.0f * z, styleNames(p.I("Model").get()), specOf("Style")->tip);
    ImGui::SameLine(0, 6.0f * z);
    drawSoundBrowser(p);
    ImGui::SameLine(0, 10.0f * z);
    playButton(p, "Play", ImVec2(60.0f * z, 0));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Play this pad (the keyboard plays it too: its pitch is the key)");
    ImGui::SameLine(0, 8.0f * z);
    toggle(p.B("Poly"), "Poly", specOf("Poly")->tip);
    const float gainW = 200.0f * z;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - gainW));
    labelW = 38.0f;
    slider(gain, "Gain", gainW, "%.2f", false, "Every pad's level (the Out mix and the Pad outputs)");
    labelW = keep;
}

// The model's library: < > step through its styles and then its saved
// sounds; the combo lists the saved ones, saves, overwrites, deletes
void scDrumKitBox::drawSoundBrowser(Pad& p) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const int model = p.I("Model").get();
    const std::string modelName = modelNames()[std::clamp(model, 0, (int)modelNames().size() - 1)];
    ImGui::PushID("sounds");
    if(ImGui::ArrowButton("##prev", ImGuiDir_Left)) stepSound(p, -1);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Previous %s sound (its styles, then your saved ones)", modelName.c_str());
    ImGui::SameLine(0, 2.0f * z);
    if(ImGui::ArrowButton("##next", ImGuiDir_Right)) stepSound(p, 1);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Next %s sound (its styles, then your saved ones)", modelName.c_str());
    ImGui::SameLine(0, 4.0f * z);
    ImGui::SetNextItemWidth(170.0f * z);
    const std::string preview = p.sound.empty() ? "Saved " + modelName + " sounds" : p.sound;
    if(ImGui::BeginCombo("##list", preview.c_str())) {
        const auto list = sounds(model);   // a copy: storing / deleting reloads it
        ImGui::TextDisabled("%s sounds (every DrumKitBox)", modelName.c_str());
        if(list.empty()) ImGui::TextDisabled("  none yet");
        for(const auto& snd : list) {
            if(ImGui::Selectable(snd.name.c_str(), snd.name == p.sound)) recallSound(p, snd);
            if(ImGui::BeginPopupContextItem()) {
                if(ImGui::MenuItem(("Overwrite with this pad (\"" + snd.name + "\")").c_str())) storeSound(p, snd.name);
                if(ImGui::MenuItem("Delete")) deleteSound(model, snd.name);
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();
        if(ImGui::Selectable("Save this pad as...")) {
            std::snprintf(soundNameBuf, sizeof(soundNameBuf), "%s", p.sound.empty() ? p.name.c_str() : p.sound.c_str());
            openStorePopup = true;
        }
        if(!p.sound.empty()) {
            if(ImGui::Selectable(("Overwrite \"" + p.sound + "\"").c_str())) storeSound(p, p.sound);
            if(ImGui::Selectable(("Delete \"" + p.sound + "\"").c_str())) deleteSound(model, p.sound);
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Your saved %s sounds: layers, voice, effects, modulation and sample.\n"
                                                 "Recalling one keeps the pad's channel, level and ducking.\n"
                                                 "Right-click a sound in the list to overwrite or delete it.", modelName.c_str());
    if(openStorePopup) { ImGui::OpenPopup("##storeSound"); openStorePopup = false; }
    if(ImGui::BeginPopup("##storeSound")) {
        ImGui::Text("Save this pad as a %s sound", modelName.c_str());
        ImGui::SetNextItemWidth(220.0f * z);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", soundNameBuf, sizeof(soundNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = ofTrim(std::string(soundNameBuf));
        bool exists = false;
        for(const auto& snd : sounds(model)) if(snd.name == name) exists = true;
        if(exists) ImGui::TextColored(ImColor(pal::accentCol(255)), "Replaces the saved one");
        ImGui::BeginDisabled(name.empty());
        if(ImGui::Button(exists ? "Replace" : "Save") || (enter && !name.empty())) { storeSound(p, name); ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// The pad's hit at a glance: each layer's envelope in its colour, the body's
// pitch (dashed) and the filter's sweep; drag a layer's end to change its decay
void scDrumKitBox::drawEnvelopeView(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float h = 118.0f * z;
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##dkEnv", ImVec2(std::max(1.0f, w), h));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, pal::canvas, 3.0f * z);

    const float k = p.F("Decay").get();
    struct Env { int layer; float a, h, d, c; const char* decayLabel; };
    std::vector<Env> envs;
    auto on = [&](int l) { return p.layerOn[l]->get(); };
    if(on(L_BODY)) envs.push_back({L_BODY, p.F("Body Attack").get(), p.F("Body Hold").get(), p.F("Body Decay").get() * k, p.F("Body Curve").get(), "Body Decay"});
    if(on(L_CLICK)) envs.push_back({L_CLICK, 0.0f, 0.0f, p.F("Click Decay").get() * k, -6.0f, "Click Decay"});
    if(on(L_NOISE)) {
        const float bursts = (p.I("Bursts").get() - 1) * p.F("Burst Spacing").get();
        envs.push_back({L_NOISE, bursts + p.F("Noise Attack").get(), p.F("Noise Hold").get(), p.F("Noise Decay").get() * k, p.F("Noise Curve").get(), "Noise Decay"});
    }
    if(on(L_METAL)) envs.push_back({L_METAL, p.F("Metal Attack").get(), p.F("Metal Hold").get(), p.F("Metal Decay").get() * k, p.F("Metal Curve").get(), "Metal Decay"});
    if(on(L_SAMPLE)) {
        float d = p.F("Sample Decay").get() * k;
        if(p.F("Sample Decay").get() >= 9.99f) {
            const float len = p.sample->seconds * p.F("Sample Length").get() / std::pow(2.0f, p.F("Sample Tune").get() / 12.0f);
            d = len > 0.0f ? len : 0.5f;
        }
        envs.push_back({L_SAMPLE, p.F("Sample Attack").get(), p.F("Sample Hold").get(), d, p.F("Sample Curve").get(), "Sample Decay"});
    }
    float total = 0.05f;
    for(const auto& e : envs) total = std::max(total, e.a + e.h + e.d);
    total = std::min(12.0f, total * 1.12f);
    const float padL = 6.0f * z, padT = 16.0f * z, padB = 14.0f * z;
    const float iw = w - 2 * padL, ih = h - padT - padB;
    auto X = [&](float t) { return a.x + padL + iw * t / total; };
    auto Y = [&](float v) { return a.y + padT + ih * (1.0f - v); };

    // time grid
    static const float steps[] = {0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5};
    float step = steps[0];
    for(float s : steps) { step = s; if(iw * s / total >= 60.0f * z) break; }
    for(float t = step; t < total; t += step) {
        dl->AddLine(ImVec2(X(t), a.y + padT), ImVec2(X(t), b.y - padB), pal::grid);
        char lab[16];
        if(step < 0.1f) std::snprintf(lab, sizeof(lab), "%.0f ms", t * 1000.0f); else std::snprintf(lab, sizeof(lab), "%.2g s", t);
        dl->AddText(ImVec2(X(t) + 2 * z, b.y - padB), pal::textDim, lab);
    }
    dl->AddLine(ImVec2(a.x + padL, Y(0)), ImVec2(b.x - padL, Y(0)), pal::gridStrong);

    // the envelopes, filled
    const int n = std::max(32, (int)(iw / (2.0f * z)));
    for(const auto& e : envs) {
        std::vector<ImVec2> pts;
        for(int i = 0; i <= n; i++) {
            const float t = total * i / n;
            pts.push_back(ImVec2(X(t), Y(ahd(t, e.a, e.h, e.d, e.c))));
        }
        for(int i = 0; i < n; i++)
            dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, Y(0)), ImVec2(pts[i].x, Y(0)), pal::layerCol(e.layer, 34));
        dl->AddPolyline(pts.data(), (int)pts.size(), pal::layerCol(e.layer, 230), 0, 1.6f * z);
    }
    // the body's pitch: semitones above Freq, scaled to the larger of sweep / bend
    if(on(L_BODY)) {
        const float sweep = p.F("Sweep").get(), st = p.F("Sweep Time").get() * k, sc = p.F("Sweep Curve").get();
        const float bend = p.F("Bend").get(), bc = p.F("Bend Curve").get();
        const float bodyLen = p.F("Body Hold").get() + p.F("Body Decay").get() * k;
        const float range = std::max(1.0f, std::max(std::abs(sweep), std::abs(bend)) * 1.05f);
        ImVec2 prev;
        for(int i = 0; i <= n; i++) {
            const float t = total * i / n;
            const float sw = t < st ? 1.0f - envShape(t / std::max(1e-5f, st), sc) : 0.0f;
            const float bn = envShape(std::min(1.0f, t / std::max(1e-5f, bodyLen)), bc);
            const float v = 0.5f + 0.5f * (sweep * sw + bend * bn) / range;
            const ImVec2 q(X(t), Y(v));
            if(i > 0 && (i / 3) % 2 == 0) dl->AddLine(prev, q, IM_COL32(255, 255, 255, 120), 1.2f * z);
            prev = q;
        }
    }
    // the filter's sweep
    if(p.B("Filter On").get() && std::abs(p.F("Env Amount").get()) > 0.01f) {
        const float fd = p.F("Env Decay").get() * k, amt = p.F("Env Amount").get();
        ImVec2 prev;
        for(int i = 0; i <= n; i++) {
            const float t = total * i / n;
            const float v = 0.5f + 0.5f * (amt / 8.0f) * (1.0f - envShape(std::min(1.0f, t / std::max(1e-5f, fd)), -4.0f));
            const ImVec2 q(X(t), Y(v));
            if(i > 0 && (i / 2) % 2 == 0) dl->AddLine(prev, q, IM_COL32(200, 140, 255, 140), 1.0f * z);
            prev = q;
        }
    }
    // legend
    float lx = a.x + padL;
    for(const auto& e : envs) {
        dl->AddRectFilled(ImVec2(lx, a.y + 4 * z), ImVec2(lx + 8 * z, a.y + 12 * z), pal::layerCol(e.layer, 230));
        dl->AddText(ImVec2(lx + 11 * z, a.y + 1 * z), pal::textDim, layerNames[e.layer]);
        lx += ImGui::CalcTextSize(layerNames[e.layer]).x + 22 * z;
    }
    if(on(L_BODY)) { dl->AddText(ImVec2(lx, a.y + 1 * z), IM_COL32(255, 255, 255, 120), "- - pitch"); lx += 64 * z; }
    if(envs.empty()) dl->AddText(ImVec2(a.x + w * 0.5f - 80 * z, a.y + h * 0.5f), pal::textDim, "Every layer is off: switch one on");

    // handles: each layer's end (its decay), dragged in time
    int hot = -1;
    const ImVec2 m = ImGui::GetMousePos();
    for(int i = 0; i < (int)envs.size(); i++) {
        const auto& e = envs[i];
        const ImVec2 hp(X(e.a + e.h + e.d), Y(0.12f));
        const bool near = std::abs(m.x - hp.x) < 7 * z && std::abs(m.y - hp.y) < 12 * z;
        if(near && hovered && envDrag < 0) hot = i;
        const bool lit = envDrag == e.layer || hot == i;
        dl->AddCircleFilled(hp, (lit ? 5.0f : 3.5f) * z, pal::layerCol(e.layer, lit ? 255 : 200));
    }
    if(hot >= 0 && !active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetTooltip("%s: drag to change its decay", envs[hot].decayLabel);
    }
    if(ImGui::IsItemActivated() && hot >= 0) envDrag = envs[hot].layer;
    if(active && envDrag >= 0) {
        for(const auto& e : envs) if(e.layer == envDrag) {
            ofParameter<float>& d = p.F(e.decayLabel);
            const float t = ofClamp((m.x - a.x - padL) / iw, 0.0f, 1.0f) * total;
            d = ofClamp((t - e.a - e.h) / std::max(0.01f, k), d.getMin(), d.getMax());
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if(!active) envDrag = -1;
    if(hovered && hot < 0 && envDrag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) fire(p);
    if(hovered && hot < 0 && envDrag < 0) ImGui::SetTooltip("Click: play the pad. Drag a dot: that layer's decay.");
    dl->AddRect(a, b, p.flash > 0.01f ? pal::padCol(p.index, (int)(80 + 175 * p.flash)) : pal::border, 3.0f * z, 0, 1.0f * z);
    dl->PopClipRect();
}

// ── Layers: Body | Body+ and Click | Noise | Metal | Sample ─────────────────

void scDrumKitBox::drawSampleWave(Pad& p, float w, float h) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##dkWave", ImVec2(std::max(1.0f, w), h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, pal::canvas);
    const auto& peaks = p.sample->peaks;
    if(peaks.empty()) {
        const char* msg = "Drop a sample here";
        dl->AddText(ImVec2(a.x + 6 * z, a.y + h * 0.5f - ImGui::GetTextLineHeight() * 0.5f), pal::textDim, msg);
    } else {
        const float st = p.F("Sample Start").get(), en = std::min(1.0f, st + p.F("Sample Length").get());
        const int pixels = std::max(1, (int)w);
        for(int i = 0; i < pixels; i++) {
            const int b0 = (int)((int64_t)i * peaks.size() / pixels);
            const int b1 = std::max(b0 + 1, (int)((int64_t)(i + 1) * peaks.size() / pixels));
            float pk = 0;
            for(int q = b0; q < b1 && q < (int)peaks.size(); q++) pk = std::max(pk, peaks[q]);
            const float x = a.x + i + 0.5f, f = (float)i / pixels, dy = std::max(0.5f, pk * h * 0.46f);
            dl->AddLine(ImVec2(x, a.y + h * 0.5f - dy), ImVec2(x, a.y + h * 0.5f + dy), pal::layerCol(L_SAMPLE, f >= st && f <= en ? 200 : 50));
        }
        dl->AddLine(ImVec2(a.x + w * st, a.y), ImVec2(a.x + w * st, b.y), pal::handle, 1.5f * z);
        dl->AddLine(ImVec2(a.x + w * en, a.y), ImVec2(a.x + w * en, b.y), pal::handle, 1.0f * z);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Click / drag: Start");
        if(ImGui::IsItemActive()) p.F("Sample Start") = ofClamp((ImGui::GetMousePos().x - a.x) / w, 0.0f, 0.999f);
    }
    dl->AddRect(a, b, browser.isDraggingFile() && ImGui::IsItemHovered() ? pal::accentCol(255) : pal::border);
    std::string dropped;
    if(browser.acceptDropOnLastItem(dropped)) { loadSample(p, dropped); if(!p.B("Sample On").get()) p.B("Sample On") = true; }
    if(ImGui::IsItemHovered()) hoveredPad = p.index;
}

void scDrumKitBox::drawLayersTab(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    const float colW = (w - 4 * gap) / 5.0f;
    labelW = 70.0f;
    auto layerColumn = [&](int l, const char* title, const char* section) {
        sectionHeader(title, colW, p.layerOn[l], pal::layerCol(l, 200));
        ImGui::BeginDisabled(!p.layerOn[l]->get());
        drawSection(p, section, colW);
        ImGui::EndDisabled();
    };
    beginColumn("body");
    layerColumn(L_BODY, "Body", "body");
    endColumn();

    beginColumn("bodyx");
    sectionHeader("Body: FM, Mode 2, Drive", colW, nullptr, pal::layerCol(L_BODY, 120));
    ImGui::BeginDisabled(!p.layerOn[L_BODY]->get());
    drawSection(p, "bodyx", colW);
    ImGui::EndDisabled();
    ImGui::Spacing();
    layerColumn(L_CLICK, "Click", "click");
    endColumn();

    beginColumn("noise");
    layerColumn(L_NOISE, "Noise", "noise");
    endColumn();

    beginColumn("metal");
    layerColumn(L_METAL, "Metal", "metal");
    endColumn();

    beginColumn("sample");
    sectionHeader("Sample", colW, p.layerOn[L_SAMPLE], pal::layerCol(L_SAMPLE, 200));
    {
        const Sample& s = *p.sample;
        if(ImGui::Button("Load...")) pendingLoadDialog = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("A file to layer with the synthesis (or drop one from the browser / Finder)");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if(!s.decodingPath.empty() || !s.pendingFile.empty()) ImGui::TextColored(ImColor(pal::textDim), "Loading...");
        else if(s.path.empty()) ImGui::TextColored(ImColor(pal::textDim), "No sample");
        else {
            ImGui::TextUnformatted(std::filesystem::path(s.path).filename().string().c_str());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%.3f s, %s", s.path.c_str(), s.seconds, s.channels == 1 ? "mono" : "stereo");
        }
        if(!s.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.error.c_str());
        drawSampleWave(p, colW, 46.0f * z);
    }
    ImGui::BeginDisabled(!p.layerOn[L_SAMPLE]->get());
    drawSection(p, "sample", colW);
    ImGui::EndDisabled();
    endColumn(true);
}

// ── Voice: Filter | Character | Transient + Gate | Pad | Played by ─────────

void scDrumKitBox::drawVoiceTab(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    const float colW = (w - 4 * gap) / 5.0f;
    labelW = 70.0f;
    auto stageColumn = [&](const char* title, const char* onLabel, const char* section) {
        ofParameter<bool>& on = p.B(onLabel);
        sectionHeader(title, colW, &on);
        ImGui::BeginDisabled(!on.get());
        drawSection(p, section, colW);
        ImGui::EndDisabled();
    };
    beginColumn("filter");
    stageColumn("Filter", "Filter On", "filter");
    endColumn();
    beginColumn("char");
    stageColumn("Character", "Character On", "char");
    endColumn();
    beginColumn("trans");
    stageColumn("Transient", "Transient On", "trans");
    ImGui::Spacing();
    sectionHeader("Gate", colW);
    drawSpec(p, *specOf("Gated"), colW);
    ImGui::BeginDisabled(!p.B("Gated").get());
    drawSpec(p, *specOf("Release"), colW);
    ImGui::EndDisabled();
    endColumn();
    beginColumn("pad");
    sectionHeader("Pad", colW);
    drawSection(p, "piece", colW);
    drawSpec(p, *specOf("Pan"), colW);
    drawSpec(p, *specOf("Width"), colW);
    endColumn();
    beginColumn("play");
    sectionHeader("Played by", colW);
    drawSpec(p, *specOf("Channel"), colW);
    const std::string source = channelSource(padChannel(p));
    ImGui::TextColored(ImColor(pal::textDim), "%s", source.empty() ? ("Gate / Pitch channel " + ofToString(padChannel(p) + 1)).c_str()
                                                                     : ("Played by: " + source).c_str());
    drawSpec(p, *specOf("Choke"), colW);
    drawSpec(p, *specOf("Poly"), colW);
    ImGui::TextColored(ImColor(pal::textDim), "Pitch 60 = this pad's tuning");
    endColumn(true);
}

// ── The mixer ───────────────────────────────────────────────────────────────

void scDrumKitBox::drawMixerTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float stripW = 104.0f * z, stripH = 316.0f * z;
    ImGui::BeginChild("##dkStrips", ImVec2(w, stripH + ImGui::GetStyle().ScrollbarSize + 8.0f * z), false, ImGuiWindowFlags_HorizontalScrollbar);
    for(auto& p : pads) { drawStrip(*p, stripW, stripH); ImGui::SameLine(0, 6.0f * z); }
    ImGui::EndChild();
    ImGui::Spacing();
    drawEffects(*pads[std::clamp(selected, 0, (int)pads.size() - 1)], w);
}

void scDrumKitBox::drawStrip(Pad& p, float w, float h) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(p.index);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool isSel = p.index == selected;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), isSel ? IM_COL32(40, 44, 54, 255) : pal::header, 4.0f * z);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + 3.0f * z), pal::padCol(p.index, 255), 4.0f * z);
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, a.y + 6 * z));
    const float iw = w - 8 * z;
    if(ImGui::Selectable(padLabel(p).c_str(), isSel, 0, ImVec2(iw, 0))) selected = p.index;
    for(int fx = 0; fx < FX_COUNT; fx++) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y));
        const bool on = p.fxOn[fx]->get();
        const bool usable = fx != FX_WIDTH || channels.get() > 1;
        ImGui::BeginDisabled(!usable);
        if(on) ImGui::PushStyleColor(ImGuiCol_Button, pal::padCol(p.index, 150));
        if(ImGui::Button(fxNames[fx], ImVec2(iw, 0))) { *p.fxOn[fx] = !on; selected = p.index; }
        if(on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(usable ? "%s: click to switch it %s (its settings below)" : "%s needs N Chan > 1", fxNames[fx], on ? "off" : "on");
        drawPublishMenu(p.fxOn[fx]->getEscapedName());
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y + 4 * z));
    ImGui::SetNextItemWidth(iw);
    float pan = p.F("Pan").get();
    if(ImGui::SliderFloat("##pan", &pan, -1.0f, 1.0f, pan == 0.0f ? "C" : pan < 0 ? "L %.2f" : "R %.2f")) p.F("Pan") = pan;
    drawPublishMenu(p.F("Pan").getEscapedName());
    const float btnH = ImGui::GetFrameHeight();
    const float faderTop = ImGui::GetCursorScreenPos().y + 4 * z;
    const float faderH = std::max(40.0f * z, a.y + h - faderTop - 2 * btnH - 14 * z);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, faderTop));
    float lv = p.F("Level").get();
    const float db = lv > 0.0001f ? 20.0f * std::log10(lv) : -INFINITY;
    char fmt[16];
    if(std::isinf(db)) std::snprintf(fmt, sizeof(fmt), "-inf"); else std::snprintf(fmt, sizeof(fmt), "%+.1f", db);
    if(ImGui::VSliderFloat("##level", ImVec2(iw * 0.55f, faderH), &lv, 0.0f, 2.0f, fmt)) p.F("Level") = lv;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Level %.2f (%s dB)", p.F("Level").get(), fmt);
    drawPublishMenu(p.F("Level").getEscapedName());
    {
        const float mx = a.x + 4 * z + iw * 0.62f, mw = iw * 0.3f;
        dl->AddRectFilled(ImVec2(mx, faderTop), ImVec2(mx + mw, faderTop + faderH), pal::canvas, 2.0f * z);
        const float mdb = p.meter > 0.00001f ? 20.0f * std::log10(p.meter) : -80.0f;
        const float kk = ofClamp((mdb + 60.0f) / 66.0f, 0.0f, 1.0f);
        const ImU32 mc = mdb > 0.0f ? IM_COL32(230, 80, 60, 255) : mdb > -6.0f ? IM_COL32(230, 200, 80, 255) : pal::padCol(p.index, 230);
        if(kk > 0.0f) dl->AddRectFilled(ImVec2(mx, faderTop + faderH * (1 - kk)), ImVec2(mx + mw, faderTop + faderH), mc, 2.0f * z);
        const float zero = faderTop + faderH * (1 - 60.0f / 66.0f);
        dl->AddLine(ImVec2(mx - 2 * z, zero), ImVec2(mx + mw + 2 * z, zero), pal::gridStrong);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, faderTop + faderH + 6 * z));
    auto flag = [&](ofParameter<bool>& f, const char* text, ImU32 col, float bw) {
        const bool lit = f.get();   // the colour pushed must be the one popped, whatever the click does
        if(lit) ImGui::PushStyleColor(ImGuiCol_Button, col);
        if(ImGui::Button(text, ImVec2(bw, 0))) f = !f.get();
        if(lit) ImGui::PopStyleColor();
        drawPublishMenu(f.getEscapedName());
    };
    const float half = (iw - 4 * z) * 0.5f;
    flag(p.B("Mute"), "M", IM_COL32(190, 90, 70, 255), half);
    ImGui::SameLine(0, 4 * z);
    flag(p.B("Solo"), "S", IM_COL32(200, 170, 60, 255), half);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y));
    const int from = p.I("Duck From").get();
    if(from > 0 && p.F("Duck Amount").get() > 0.0f) ImGui::TextColored(ImColor(pal::accentCol(255)), "ducks from %d", from);
    else ImGui::TextColored(ImColor(pal::textDim), "Pad %d out", p.index + 1);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(a.x + w, a.y));
    ImGui::Dummy(ImVec2(0, h));
    ImGui::PopID();
}

void scDrumKitBox::drawEffects(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    labelW = 70.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, pal::padCol(p.index, 255));
    ImGui::TextUnformatted(("Effects of " + padLabel(p)).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(ImColor(pal::textDim), "  voice -> EQ -> Comp -> Sat -> Echo -> Grains -> Reverb -> Width -> Level (ducked)");
    const float colW = (w - 4 * gap) / 5.0f;
    auto fxColumn = [&](const char* title, int fx, const char* section, float cw) {
        sectionHeader(title, cw, p.fxOn[fx]);
        ImGui::BeginDisabled(!p.fxOn[fx]->get() || (fx == FX_WIDTH && channels.get() < 2));
        drawSection(p, section, cw);
        ImGui::EndDisabled();
    };
    const float eqW = colW * 2 + gap;
    beginColumn("eq");
    sectionHeader("EQ", eqW, p.fxOn[FX_EQ]);
    ImGui::TextColored(ImColor(pal::textDim), "Drag the bands; wheel = Q / slope; right-click a band for values");
    drawEqEditor(p, eqW, 170.0f * z);
    endColumn();
    beginColumn("comp"); fxColumn("Compressor", FX_COMP, "comp", colW); endColumn();
    beginColumn("sat"); fxColumn("Saturator", FX_SAT, "sat", colW); endColumn();
    beginColumn("duck");
    sectionHeader("Ducking (side-chain)", colW);
    drawSection(p, "duck", colW);
    endColumn(true);
    ImGui::Spacing();
    beginColumn("echo"); fxColumn("Echo", FX_ECHO, "echo", colW); endColumn();
    beginColumn("grain"); fxColumn("Grains", FX_GRAIN, "grain", colW); endColumn();
    beginColumn("reverb"); fxColumn("Reverb", FX_REVERB, "reverb", colW); endColumn();
    beginColumn("stereo");
    fxColumn("Width", FX_WIDTH, "stereo", colW);
    if(channels.get() < 2) ImGui::TextColored(ImColor(pal::textDim), "Needs N Chan > 1");
    endColumn(true);
}

void scDrumKitBox::drawEqEditor(Pad& p, float w, float h) {
    static const char* gainL[5] = {"EQ Low", "EQ Lo Mid", "EQ Mid", "EQ Hi Mid", "EQ High"};
    static const char* shapeL[5] = {"EQ Low Slope", "EQ Lo Mid Q", "EQ Mid Q", "EQ Hi Mid Q", "EQ High Slope"};
    auto freqOf = [&](int b) -> ofParameter<float>& { return p.F((std::string(gainL[b]) + " Freq").c_str()); };
    for(int b = 0; b < 5; ++b) p.eqEditor.bands[b] = {freqOf(b).get(), p.F(gainL[b]).get(), p.F(shapeL[b]).get()};
    p.eqEditor.dbRange = 24.0f;
    p.eqEditor.gainLimit = 24.0f;
    p.eqEditor.qMin = 0.1f; p.eqEditor.qMax = 20.0f;
    p.eqEditor.slopeMin = 0.1f; p.eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()) { sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    p.eqEditor.recompute(sr > 0 ? sr : 48000.0f);
    static scEQEditor::Colors col = [] {
        auto c = scEQEditor::defaultColors();
        c.bg = pal::canvas;
        c.border = pal::border;
        c.gridLine = c.dbGridLine = c.fft = pal::grid;
        c.gridText = c.dbGridText = pal::textDim;
        c.zeroGrid = c.zeroLine = pal::gridStrong;
        c.curve = c.handle = IM_COL32(214, 216, 222, 215);
        c.handleActive = pal::handle;
        for(auto& marker : c.bandMarker) marker = pal::grid;
        return c;
    }();
    Pad* pp = &p;
    p.eqEditor.bandMenu = [this, pp, freqOf](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        auto value = [this](ofParameter<float>& q, const char* label) {
            float v = q.get();
            ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
            if(ImGui::InputFloat(label, &v, 0, 0, "%.3f")) q.set(ofClamp(v, q.getMin(), q.getMax()));
            const auto key = q.getEscapedName();
            const bool isPublished = published.count(key) != 0;
            const std::string action = std::string(isPublished ? "Unpublish " : "Publish ") + label;
            if(ImGui::MenuItem(action.c_str(), nullptr, false, !isConnected(key))) { if(isPublished) unpublish(key); else publish(key); }
        };
        value(freqOf(b), "Freq (Hz)");
        value(pp->F(gainL[b]), "Gain (dB)");
        value(pp->F(shapeL[b]), scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    const bool off = !p.fxOn[FX_EQ]->get();
    ImGui::BeginDisabled(off);
    if(p.eqEditor.draw(std::max(120.0f, w), h, col, !off, nullptr, 0, ("##drumKitBoxEq" + ofToString(p.index)).c_str())) {
        for(int b = 0; b < 5; ++b) {
            freqOf(b) = p.eqEditor.bands[b].freqHz;
            p.F(gainL[b]) = p.eqEditor.bands[b].gainDb;
            p.F(shapeL[b]) = p.eqEditor.bands[b].shape;
        }
    }
    ImGui::EndDisabled();
}
