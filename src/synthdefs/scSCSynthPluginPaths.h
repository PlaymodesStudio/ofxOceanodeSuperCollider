#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace oceanodeScPlugins {
namespace fs = std::filesystem;

inline bool isSupernovaPlugin(const fs::path& path){
    const auto stem = path.stem().string();
    const auto extension = path.extension().string();
    return (extension == ".scx" || extension == ".so" || extension == ".dll") &&
        stem.size() >= 10 && stem.compare(stem.size() - 10, 10, "_supernova") == 0;
}

// scsynth dlopens both variants before checking their plugin API. On macOS
// that registers VSTPlugin's Objective-C classes twice. A private directory
// view excludes the Supernova binaries without changing installed packages.
inline fs::path forScsynth(const fs::path& source){
    if(!fs::is_directory(source)) return source;
    bool needsFilter = false;
    for(const auto& entry : fs::recursive_directory_iterator(source, fs::directory_options::follow_directory_symlink)){
        if(isSupernovaPlugin(entry.path())) { needsFilter = true; break; }
    }
    if(!needsFilter) return source;

    struct Views {
        std::mutex mutex;
        std::vector<fs::path> roots;
        ~Views(){
            for(const auto& root : roots){
                std::error_code error;
                fs::remove_all(root, error);
            }
        }
    };
    static Views views;
    fs::path destination;
    for(unsigned int attempt = 0; ; ++attempt){
        destination = fs::temp_directory_path() / ("oceanode-scsynth-plugins-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
        if(fs::create_directory(destination)) break;
    }
    { std::lock_guard<std::mutex> lock(views.mutex); views.roots.push_back(destination); }
    const auto absolute = fs::absolute(source);
    for(const auto& entry : fs::recursive_directory_iterator(absolute)){
        const auto relative = entry.path().lexically_relative(absolute);
        const auto target = destination / relative;
        if(isSupernovaPlugin(entry.path())) continue;
        if(entry.is_directory()){
            // Preserve symlinked package folders, filtering their contents too.
            if(entry.is_symlink()) fs::create_directory_symlink(forScsynth(fs::canonical(entry.path())), target);
            else fs::create_directory(target);
        }else if(entry.is_regular_file()){
            fs::create_symlink(entry.path(), target);
        }
    }
    return destination;
}

inline std::string forScsynth(const std::string& paths){
    std::string filtered;
    size_t begin = 0;
    while(begin < paths.size()){
        const auto end = paths.find(':', begin);
        const auto path = paths.substr(begin, end == std::string::npos ? end : end - begin);
        if(!path.empty()){
            if(!filtered.empty()) filtered += ':';
            filtered += forScsynth(fs::path(path)).string();
        }
        if(end == std::string::npos) break;
        begin = end + 1;
    }
    return filtered;
}
}
