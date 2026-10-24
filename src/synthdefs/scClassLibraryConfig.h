#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace oceanodeClassLibrary {
namespace fs = std::filesystem;

// Only top-level declarations count. In particular, ignore method bodies,
// class extensions (+ Class), strings, character literals and nested comments.
inline std::set<std::string> declaredClasses(const std::string& source){
    std::vector<std::string> tokens;
    for(size_t i = 0; i < source.size();){
        const auto pair = source.substr(i, 2);
        if(pair == "/*"){
            int depth = 1; i += 2;
            while(i < source.size() && depth){
                if(source.compare(i, 2, "/*") == 0){ depth++; i += 2; }
                else if(source.compare(i, 2, "*/") == 0){ depth--; i += 2; }
                else i++;
            }
        } else if(pair == "//"){
            while(i < source.size() && source[i] != '\n' && source[i] != '\r') i++;
        } else if(source[i] == '"' || source[i] == '\''){
            const char quote = source[i++];
            while(i < source.size()){
                if(source[i] == '\\') i = std::min(i + 2, source.size());
                else if(source[i++] == quote) break;
            }
        } else if(source[i] == '$'){
            i = std::min(i + ((i + 1 < source.size() && source[i + 1] == '\\') ? 3 : 2), source.size());
        } else if(std::isalpha(static_cast<unsigned char>(source[i])) || source[i] == '_'){
            const size_t start = i++;
            while(i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) i++;
            tokens.push_back(source.substr(start, i - start));
        } else if(std::isspace(static_cast<unsigned char>(source[i]))) i++;
        else tokens.emplace_back(1, source[i++]);
    }
    std::set<std::string> result;
    int depth = 0;
    for(size_t i = 0; i < tokens.size(); i++){
        const auto& token = tokens[i];
        const bool declaration = depth == 0 && std::isupper(static_cast<unsigned char>(token[0])) &&
            (i == 0 || (tokens[i - 1] != "+" && tokens[i - 1] != ":" && tokens[i - 1] != "\\"));
        if(declaration && ((i + 1 < tokens.size() && tokens[i + 1] == "{") ||
            (i + 3 < tokens.size() && tokens[i + 1] == ":" && tokens[i + 3] == "{"))) result.insert(token);
        if(token == "{") depth++;
        else if(token == "}") depth--;
    }
    return result;
}

inline fs::path canonicalPath(const fs::path& path){
    std::error_code error;
    const auto resolved = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}

inline bool contains(const fs::path& parent, const fs::path& child){
    auto a = parent.begin(), b = child.begin();
    for(; a != parent.end() && b != child.end(); ++a, ++b) if(*a != *b) return false;
    return a == parent.end();
}

inline bool hasClasses(const fs::path& root){
    std::error_code error;
    for(auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error);
        !error && it != fs::recursive_directory_iterator(); it.increment(error)){
        if(it->is_regular_file() && it->path().extension() == ".sc") return true;
    }
    return false;
}

struct Root { fs::path path; int priority; };
struct ClassFile { fs::path path; int priority; std::set<std::string> classes; };

inline void write(const fs::path& file, const fs::path& bundle, const fs::path& sources,
                  const fs::path& installed, const std::vector<std::string>& extra = {}){
    std::vector<Root> roots;
    auto add = [&](const fs::path& path, int priority){
        if(path.empty() || !fs::exists(path)) return;
        const auto resolved = canonicalPath(path);
        for(const auto& root : roots) if(contains(root.path, resolved)) return;
        roots.erase(std::remove_if(roots.begin(), roots.end(), [&](const Root& root){
            return contains(resolved, root.path);
        }), roots.end());
        roots.push_back({resolved, priority});
    };
    if(!bundle.empty()) add(bundle / "SCClassLibrary", 0);
    if(!sources.empty()) add(sources / "Oceanode", 1);
    if(fs::exists(installed)){
        for(const auto& entry : fs::directory_iterator(installed)){
            const auto name = entry.path().filename().string();
            if(entry.is_directory() && name != "Oceanode" && name != "synthdefs" && hasClasses(entry.path())) add(entry.path(), 2);
        }
    }
    for(const auto& path : extra) add(path, 2);
    if(!bundle.empty()) add(bundle / "Extensions", 3);

    std::vector<ClassFile> files;
    std::set<fs::path> visited;
    for(const auto& root : roots){
        std::error_code error;
        for(auto it = fs::recursive_directory_iterator(root.path, fs::directory_options::skip_permission_denied, error);
            !error && it != fs::recursive_directory_iterator(); it.increment(error)){
            if(!it->is_regular_file() || it->path().extension() != ".sc") continue;
            const auto path = canonicalPath(it->path());
            if(!visited.insert(path).second) continue;
            std::ifstream stream(path);
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            const auto classes = declaredClasses(text);
            if(!classes.empty()) files.push_back({path, root.priority, classes});
        }
    }
    std::sort(files.begin(), files.end(), [](const ClassFile& a, const ClassFile& b){
        if(a.priority != b.priority) return a.priority < b.priority;
        // Keep a combined class file before its standalone duplicate: excluding
        // the combined file could also remove classes that have no other copy.
        if(a.classes.size() != b.classes.size()) return a.classes.size() > b.classes.size();
        return a.path < b.path;
    });
    std::set<std::string> provided;
    std::vector<fs::path> excluded;
    for(const auto& entry : files){
        size_t duplicates = 0;
        for(const auto& name : entry.classes) if(provided.count(name)) duplicates++;
        if(duplicates == entry.classes.size()) excluded.push_back(entry.path);
        else {
            // A partial overlap is left visible for sclang to report. Never
            // silently discard a file that also supplies a unique class.
            provided.insert(entry.classes.begin(), entry.classes.end());
        }
    }
    std::ofstream conf(file);
    conf << "includePaths:\n";
    for(const auto& root : roots) conf << "    -   \"" << root.path.generic_string() << "\"\n";
    conf << "excludePaths:\n";
    if(excluded.empty()) conf << "    []\n";
    for(const auto& path : excluded) conf << "    -   \"" << path.generic_string() << "\"\n";
    conf << "postInlineWarnings: false\nexcludeDefaultPaths: " << (bundle.empty() ? "false" : "true") << "\n";
}
}
