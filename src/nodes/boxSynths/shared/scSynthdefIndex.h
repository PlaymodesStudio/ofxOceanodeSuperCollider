#pragma once
//
//  scSynthdefIndex.h
//  ofxOceanodeSuperCollider
//
//  The Boxes find their compiled SynthDefs by file name anywhere under
//  data/<SYNTHDEF_DIRECTORY>. Walking that whole tree once per expected file,
//  per node constructed, was the cost: one walk per process now fills a
//  name -> folder index (the first match in walk order, as the per-file walks
//  took). A hit whose file has gone (a sync retired or moved it) and a miss
//  (just compiled) walk the tree again, so a refreshed tree is picked up
//  without a hook. invalidate() forces the next lookup to walk.
//  Header-only, main thread (node construction).
//

#include "ofMain.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"

#include <filesystem>
#include <map>
#include <string>

namespace scSynthdefIndex {
struct State {
    std::map<std::string, std::filesystem::path> folders;   // file name -> its folder
    bool built = false;
};
inline State& state() { static State s; return s; }
inline void invalidate() { state() = State(); }
inline void rebuild() {
    auto& s = state();
    s.folders.clear();
    s.built = true;
    try {
        const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(!std::filesystem::exists(root)) return;
        for(const auto& e : std::filesystem::recursive_directory_iterator(root,
                std::filesystem::directory_options::skip_permission_denied))
            if(e.is_regular_file()) s.folders.emplace(e.path().filename().string(), e.path().parent_path());
    } catch(const std::exception& e) { ofLogWarning("scSynthdefIndex") << e.what(); }
}
// The folder holding `file` under the data SynthDefs, empty when none does
inline std::filesystem::path find(const std::string& file) {
    auto lookup = [&file]() -> std::filesystem::path {
        auto& s = state();
        const auto it = s.folders.find(file);
        if(it == s.folders.end()) return {};
        std::error_code ec;
        return std::filesystem::exists(it->second / file, ec) ? it->second : std::filesystem::path();
    };
    const bool fresh = !state().built;
    if(fresh) rebuild();
    auto folder = lookup();
    if(folder.empty() && !fresh) { rebuild(); folder = lookup(); }
    return folder;
}
}
