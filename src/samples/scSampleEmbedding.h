#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// Preset-local assets. Only the serialized path changes: the running node keeps
// its source file. Content names deduplicate shared files without basename clashes.
namespace scSampleEmbedding {
inline std::string root() {
    const auto path = ofxOceanodeShared::getCurrentPresetPath();
    return path.empty() ? std::string() : ofToDataPath(path, true);
}

// True when a save folder lives inside the data directory (Presets/, Macros/...).
// Copy, cut, encapsulate and canvas undo save through clipboardPreset or the
// system temp dir, which are deleted afterwards: nothing may be embedded there.
inline bool isDurableSaveFolder(const std::string& folder) {
    namespace fs = std::filesystem;
    try {
        const fs::path target = fs::weakly_canonical(fs::path(ofToDataPath(folder, true)));
        for(const auto& part : target) if(part == "clipboardPreset") return false;
        const fs::path relative = target.lexically_relative(fs::weakly_canonical(fs::path(ofToDataPath("", true))));
        return !relative.empty() && *relative.begin() != "..";
    } catch(const std::exception&) {
        return false;
    }
}

inline std::string resolve(const std::string& stored, const std::string& presetRoot = root()) {
    namespace fs = std::filesystem;
    if(stored.empty() || fs::path(stored).is_absolute()) return stored;
    const fs::path path(stored);
    // Current format, plus old RhythmBox Presets/.../samples/file paths.
    if(!presetRoot.empty() && (stored.rfind("samples/", 0) == 0 ||
       (stored.rfind("Presets/", 0) == 0 && path.parent_path().filename() == "samples")))
        return (fs::path(presetRoot) / "samples" / path.filename()).string();
    return ofToDataPath(stored, true);
}

inline std::string embed(const std::string& stored, const std::string& presetRoot = root()) {
    namespace fs = std::filesystem;
    if(stored.empty() || presetRoot.empty()) return stored;
    try {
        const fs::path source(resolve(stored));
        std::ifstream input(source, std::ios::binary);
        if(!input) return stored;
        uint64_t hash = 14695981039346656037ULL;
        char buffer[65536];
        while(input) {
            input.read(buffer, sizeof(buffer));
            for(std::streamsize i = 0; i < input.gcount(); ++i) {
                hash ^= static_cast<unsigned char>(buffer[i]);
                hash *= 1099511628211ULL;
            }
        }
        if(!input.eof()) return stored;
        std::ostringstream name;
        // Use only the content hash and extension, so resaving an embedded file
        // never grows another hash prefix and identical assets share one copy.
        name << std::hex << std::setw(16) << std::setfill('0') << hash << source.extension().string();
        const fs::path destination = fs::path(presetRoot) / "samples" / name.str();
        fs::create_directories(destination.parent_path());
        if(!fs::exists(destination)) fs::copy_file(source, destination);
        // Keep macros beneath data/Macros compatible with the existing load
        // path, which may run while a different preset is current. Preset
        // paths still rebase onto the loaded preset in resolve().
        const auto relative = fs::relative(destination, fs::path(ofToDataPath("", true)));
        if(!relative.empty() && *relative.begin() != "..") return relative.generic_string();
        return "samples/" + name.str();
    } catch(const std::exception& e) {
        ofLogWarning("Sample embedding") << stored << ": " << e.what();
        return stored;
    }
}

// Where a node save into `folder` embeds its samples; empty = keep plain paths.
// presetSave does not know the folder, so nodes embed in macroSave(json, folder)
// with this root. A temporary save (copy, cut, encapsulate, canvas undo) gets
// none: an embedded "Presets/A/samples/x" in the clipboard would be rebased on
// paste onto the then-current preset B, where the file is not.
// Under Presets/ (the patch or a local macro in it): the patch's folder; a
// global macro keeps its samples in its own folder.
inline std::string saveRoot(const std::string& folder) {
    if(folder.empty() || !isDurableSaveFolder(folder)) return {};
    std::string target = ofToDataPath(folder, true);
    const std::string preset = root();
    if(!preset.empty() && target.find("/Presets/") != std::string::npos) target = preset;
    return target;
}

// json[key] (a path string) embedded under saveRoot(folder), when there is one.
inline void embedInto(ofJson& value, const std::string& rootFolder) {
    if(rootFolder.empty() || !value.is_string()) return;
    const std::string stored = value.get<std::string>();
    if(!stored.empty()) value = embed(stored, rootFolder);
}
}
