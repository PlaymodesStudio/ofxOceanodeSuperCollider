#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <mutex>
#include <tuple>

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

// An embedded name: 16 lowercase hex digits (the content hash) + extension.
inline bool isHashName(const std::filesystem::path& file) {
    const std::string stem = file.stem().string();
    if(stem.size() != 16) return false;
    for(char c : stem) if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// FNV-1a of the whole file. Saves hash every sample again: the result is kept
// per (path, size, mtime), so an unchanged file is read once per session.
inline bool contentHash(const std::filesystem::path& source, uint64_t& hash) {
    namespace fs = std::filesystem;
    static std::mutex mutex;
    static std::map<std::string, std::tuple<uintmax_t, fs::file_time_type, uint64_t>> cache;
    std::error_code ec;
    const uintmax_t size = fs::file_size(source, ec);
    const bool statOk = !ec;
    const fs::file_time_type mtime = statOk ? fs::last_write_time(source, ec) : fs::file_time_type{};
    const bool cacheable = statOk && !ec;
    const std::string key = source.string();
    if(cacheable) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if(it != cache.end() && std::get<0>(it->second) == size && std::get<1>(it->second) == mtime) {
            hash = std::get<2>(it->second);
            return true;
        }
    }
    std::ifstream input(source, std::ios::binary);
    if(!input) return false;
    hash = 14695981039346656037ULL;
    char buffer[65536];
    while(input) {
        input.read(buffer, sizeof(buffer));
        for(std::streamsize i = 0; i < input.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    if(!input.eof()) return false;
    if(cacheable) {
        std::lock_guard<std::mutex> lock(mutex);
        cache[key] = std::make_tuple(size, mtime, hash);
    }
    return true;
}

inline std::string embed(const std::string& stored, const std::string& presetRoot = root()) {
    namespace fs = std::filesystem;
    if(stored.empty() || presetRoot.empty()) return stored;
    try {
        const fs::path source(resolve(stored));
        std::string fileName;
        // Already embedded in this root (samples/<hash>.ext): it is its own
        // destination, nothing to hash or copy.
        std::error_code ec;
        if(isHashName(source.filename()) && fs::is_regular_file(source, ec) &&
           fs::equivalent(source.parent_path(), fs::path(presetRoot) / "samples", ec)) {
            fileName = source.filename().string();
        } else {
            uint64_t hash = 0;
            if(!contentHash(source, hash)) return stored;
            std::ostringstream name;
            // Use only the content hash and extension, so resaving an embedded file
            // never grows another hash prefix and identical assets share one copy.
            name << std::hex << std::setw(16) << std::setfill('0') << hash << source.extension().string();
            fileName = name.str();
        }
        const fs::path destination = fs::path(presetRoot) / "samples" / fileName;
        fs::create_directories(destination.parent_path());
        if(!fs::exists(destination)) fs::copy_file(source, destination);
        // Keep macros beneath data/Macros compatible with the existing load
        // path, which may run while a different preset is current. Preset
        // paths still rebase onto the loaded preset in resolve().
        const auto relative = fs::relative(destination, fs::path(ofToDataPath("", true)));
        if(!relative.empty() && *relative.begin() != "..") return relative.generic_string();
        return "samples/" + fileName;
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
