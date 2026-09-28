//
//  scSampleBrowser.h
//  ofxOceanodeSuperCollider
//
//  Sample file browser with audio preview, shared by scRhythmBox and
//  scGrainBox (header-only, so no build-project change is needed).
//
//  Extracted from scRhythmBox's browser, whose behaviour and look it keeps:
//    • "..." picks a folder, "^ .." goes to the parent folder.
//    • Folders first, then audio files, each sorted by name; hidden entries
//      (".name") are skipped.
//    • Click a file (or move to it with Up/Down) to preview it on the preview
//      server (BufferBrowserPreview synth, out 0, gain 0.7); click a folder
//      (or Enter on it) to open it.
//    • Files are drag sources (payload type "FSS_SAMPLE" by default, the path
//      as a zero-terminated string): drop them on whatever accepts samples.
//    • Only the visible rows are drawn (ImGuiListClipper), except while the
//      keyboard selection is off screen (it must be drawn to re-centre).
//    • A folder click is acted on after the row loop (no use-after-free of
//      the entry list while the clipper runs).
//  Optional: onActivate(path) is called on a double-click or Enter on a file
//  (RhythmBox leaves it unset: there, a file is assigned by drag & drop only).
//

#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include "ofxSuperCollider.h"
#include "ofxSCServer.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class scSampleBrowser {
public:
    struct Entry { bool isDir; std::string name, fullPath; };

    explicit scSampleBrowser(std::string logTag_ = "scSampleBrowser",
                             std::vector<std::string> extensions_ = {".wav", ".aif", ".aiff"},
                             std::string payloadType_ = "FSS_SAMPLE")
        : logTag(std::move(logTag_)),
          extensions(std::move(extensions_)),
          payloadType(std::move(payloadType_)) {}

    ~scSampleBrowser() { stopPreview(); }

    scSampleBrowser(const scSampleBrowser&) = delete;
    scSampleBrowser& operator=(const scSampleBrowser&) = delete;

    // Optional: double-click / Enter on a file.
    std::function<void(const std::string&)> onActivate;

    void         setPreviewServer(ofxSCServer* s) { previewServer = s; }
    ofxSCServer* getPreviewServer() const          { return previewServer; }
    const std::string& getDir() const              { return browseDir; }
    const std::string& getPayloadType() const      { return payloadType; }
    int  getSelection() const                      { return browserSel; }
    void setSelection(int i)                       { browserSel = i; }

    // List `dir` (the current folder becomes `dir` even if it does not exist,
    // which then simply lists nothing).
    void refresh(const std::string& dir) {
        browseEntries.clear();
        browseDir = dir;
        if(!std::filesystem::exists(dir)) return;
        try {
            std::vector<Entry> dirs, files;
            for(auto& e : std::filesystem::directory_iterator(dir)) {
                std::string n = e.path().filename().string();
                if(n.empty() || n.front() == '.') continue;
                if(e.is_directory()) {
                    dirs.push_back({true, n, e.path().string()});
                } else {
                    std::string ext = ofToLower(e.path().extension().string());
                    if(std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
                        files.push_back({false, n, e.path().string()});
                }
            }
            std::sort(dirs.begin(),  dirs.end(),  [](auto& a, auto& b){ return a.name < b.name; });
            std::sort(files.begin(), files.end(), [](auto& a, auto& b){ return a.name < b.name; });
            browseEntries.insert(browseEntries.end(), dirs.begin(),  dirs.end());
            browseEntries.insert(browseEntries.end(), files.begin(), files.end());
        } catch(const std::exception& e) {
            ofLogWarning(logTag) << "refreshBrowse: " << e.what();
        }
    }

    void triggerPreview(const std::string& path) {
        stopPreview();
        if(path.empty() || !previewServer) return;
        try {
            previewBuf = new ofxSCBuffer(0, 0, previewServer);
            previewBuf->read(path);
            previewSynth = new ofxSCSynth("BufferBrowserPreview", previewServer);
            previewSynth->set("bufnum", previewBuf->index);
            previewSynth->set("out",    0);
            previewSynth->set("gain",   0.7f);
            previewSynth->addToTail();
        } catch(const std::exception& e) {
            ofLogError(logTag) << "preview: " << e.what();
            stopPreview();
        }
    }

    void stopPreview() {
        if(previewSynth) { previewSynth->free(); delete previewSynth; previewSynth = nullptr; }
        if(previewBuf)   { previewBuf->free();   delete previewBuf;   previewBuf   = nullptr; }
    }

    // Accepts a dropped browser file on the last ImGui item. Returns true and
    // the path when one was dropped this frame.
    bool acceptDropOnLastItem(std::string& pathOut) const {
        bool dropped = false;
        if(ImGui::BeginDragDropTarget()) {
            if(const ImGuiPayload* p = ImGui::AcceptDragDropPayload(payloadType.c_str())) {
                if(p->Data && p->DataSize > 1) {
                    pathOut.assign(static_cast<const char*>(p->Data), p->DataSize - 1);
                    dropped = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        return dropped;
    }
    // A browser file is being dragged right now.
    bool isDraggingFile() const {
        const ImGuiPayload* p = ImGui::GetDragDropPayload();
        return p && p->IsDataType(payloadType.c_str());
    }

    // The browser panel (call inside a child window of the wanted size).
    void draw(float /*w*/, float /*h*/) {
        // Navigation bar
        if(ImGui::Button("...")) {
            auto res = ofSystemLoadDialog("Select Samples Folder", true, browseDir);
            if(res.bSuccess) refresh(res.getPath());
        }
        ImGui::SameLine();
        // Store filename in a local to avoid dangling-pointer UB (filename() returns
        // a temporary path whose .string() temporary is destroyed before TextUnformatted reads it).
        {
            std::string dirName = std::filesystem::path(browseDir).filename().string();
            if(dirName.empty()) dirName = browseDir; // root or drive letter
            ImGui::TextUnformatted(dirName.c_str());
        }

        if(ImGui::Button("^ ..")) {
            auto parent = std::filesystem::path(browseDir).parent_path().string();
            if(!parent.empty() && parent != browseDir)
                refresh(parent);
        }
        ImGui::Separator();

        int n = (int)browseEntries.size();
        browserSel = std::min(browserSel, n - 1); // clamp after any refresh

        ImGui::BeginChild("##blist", ImVec2(0, 0), false);

        std::string activatePath;   // acted on at the end (see onActivate)

        // ── Keyboard navigation (only when this child is focused / hovered) ───
        if(ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
            if(ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n > 0) {
                browserSel = std::min(browserSel + 1, n - 1);
                if(browserSel >= 0 && browserSel < n && !browseEntries[browserSel].isDir)
                    triggerPreview(browseEntries[browserSel].fullPath);
            }
            if(ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n > 0) {
                browserSel = std::max(browserSel - 1, 0);
                if(browserSel >= 0 && browserSel < n && !browseEntries[browserSel].isDir)
                    triggerPreview(browseEntries[browserSel].fullPath);
            }
            if(ImGui::IsKeyPressed(ImGuiKey_Enter) && browserSel >= 0 && browserSel < n) {
                if(browseEntries[browserSel].isDir) {
                    std::string navPath = browseEntries[browserSel].fullPath;
                    browserSel = -1;
                    refresh(navPath);
                    n = 0; // skip the for loop below — browseEntries is rebuilt
                } else if(onActivate) {
                    activatePath = browseEntries[browserSel].fullPath;
                }
                // files without onActivate: already previewing from arrow key; Enter just confirms
            }
        }

        // Draw only the rows in view: a large samples folder is otherwise
        // thousands of widgets per frame to show the thirty or so that fit.
        //
        // The selected row re-centres itself every frame (SetScrollHereY below),
        // which it can only do if it is drawn. The arrow keys can put it off
        // screen -- the list keeps its scroll between folders, so entering one and
        // pressing Down selects row 0 wherever the view happens to be -- so when it
        // is not fully in view the whole list is drawn, exactly as before, and it
        // re-centres. From the next frame on it is in view and clipping resumes.
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float selectedTop = ImGui::GetCursorPosY() + browserSel * rowHeight;
        const bool clipRows = browserSel < 0 ||
            (selectedTop >= ImGui::GetScrollY() &&
             selectedTop + ImGui::GetTextLineHeight() <= ImGui::GetScrollY() + ImGui::GetWindowHeight());

        // A click on a folder is acted on after the loop rather than inside it, so
        // the loop never stops part-way and the clipper always runs to completion.
        std::string navigateTo;
        auto drawRow = [&](int i) {
            auto& e = browseEntries[i];
            ImGui::PushID(i);

            std::string lbl = (e.isDir ? "[D] " : "    ") + e.name;
            // Single unified selection highlight: browserSel is the source of truth.
            // Clicking a file sets browserSel so there is never more than one highlighted row.
            bool selected = (i == browserSel);

            if(ImGui::Selectable(lbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if(e.isDir) {
                    navigateTo = e.fullPath;   // acted on after the loop
                } else {
                    browserSel = i;           // highlight moves to clicked row
                    triggerPreview(e.fullPath);
                    if(onActivate && ImGui::IsMouseDoubleClicked(0))
                        activatePath = e.fullPath;
                }
            }

            // Auto-scroll to keep the keyboard-selected item visible
            if(selected) ImGui::SetScrollHereY(0.5f);

            // Drag source for audio files → drop onto whatever accepts samples
            if(!e.isDir) {
                if(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload(payloadType.c_str(), e.fullPath.c_str(), e.fullPath.size() + 1);
                    ImGui::TextUnformatted(("  " + e.name).c_str());
                    ImGui::EndDragDropSource();
                }
            }

            if(!e.isDir && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e.fullPath.c_str());

            ImGui::PopID();
        };

        if(clipRows) {
            ImGuiListClipper clipper;
            clipper.Begin(n);
            while(clipper.Step())
                for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) drawRow(i);
            clipper.End();
        } else {
            for(int i = 0; i < n; i++) drawRow(i);
        }

        if(!navigateTo.empty()) {
            browserSel = -1;
            refresh(navigateTo);
        }
        ImGui::EndChild();

        if(!activatePath.empty() && onActivate) onActivate(activatePath);
    }

    // Vertical drag splitter with RhythmBox's look (a thin line that brightens
    // on hover). Call right after the browser child, on the same line. Width
    // is changed in place by the mouse delta times `scale` (1 when `width` is
    // in pixels). Returns true while dragging.
    static bool drawSplitter(const char* id, float& width, float splitterW, float h,
                             float minW, float maxW, float scale = 1.0f) {
        ImGui::InvisibleButton(id, ImVec2(splitterW, h));
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();
        if(hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if(act) width = ofClamp(width + ImGui::GetIO().MouseDelta.x * scale, minW, std::max(minW, maxW));
        ImVec2 p = ImGui::GetItemRectMin();
        ImVec2 q = ImGui::GetItemRectMax();
        float cx = (p.x + q.x) * 0.5f;
        ImU32 col = (hov || act) ? IM_COL32(180,180,180,200) : IM_COL32(90,90,90,150);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, p.y), ImVec2(cx, q.y), col,
                                            1.5f * ofxOceanodeShared::getZoomLevel());
        return act;
    }

private:
    std::string              logTag;
    std::vector<std::string> extensions;
    std::string              payloadType;

    std::string        browseDir;
    std::vector<Entry> browseEntries;
    int                browserSel = -1;   // keyboard-selected entry index (-1 = none)

    ofxSCServer* previewServer = nullptr;
    ofxSCBuffer* previewBuf    = nullptr;
    ofxSCSynth*  previewSynth  = nullptr;
};
