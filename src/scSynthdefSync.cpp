//
//  scSynthdefSync.cpp
//  ofxOceanodeSuperCollider
//

#include "scSynthdefSync.h"
#include "scSynthdefCompiler.h"
#include "scSynthdef.h"
#include "serverManager.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofMain.h"
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* repository = "PlaymodesStudio/ofxOceanodeSuperCollider";
// Third-party UGen folders, one zip each plus extensions.json
constexpr const char* extensionsRelease = "sc-extensions";
constexpr const char* popupId = "Sync with SynthDefs Git##scSynthdefSync";
// Files per curl run (one connection for all of them)
constexpr size_t downloadBatch = 25;
// ~synthCreator sources per sclang run
constexpr size_t compileChunk = 20;
// Generous: the biggest Defaults script takes about four minutes on an M1
constexpr int scriptTimeoutSeconds = 1800;
constexpr int chunkTimeoutSeconds = 900;
constexpr int downloadTimeoutSeconds = 600;

std::string readFile(const fs::path& path){
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool sameContents(const fs::path& a, const fs::path& b){
    std::error_code error;
    if(!fs::exists(a, error) || !fs::exists(b, error)) return false;
    if(fs::file_size(a, error) != fs::file_size(b, error)) return false;
    return readFile(a) == readFile(b);
}

std::string hex(const unsigned char* bytes, size_t size){
    static const char* digits = "0123456789abcdef";
    std::string text;
    for(size_t i = 0; i < size; i++){
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

std::string sha256(const std::string& data){
#ifdef __APPLE__
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), (CC_LONG)data.size(), digest);
    return hex(digest, sizeof(digest));
#else
    return "";
#endif
}

std::string lower(std::string text){
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return std::tolower(c); });
    return text;
}

bool startsWith(const std::string& text, const std::string& prefix){
    return text.compare(0, prefix.size(), prefix) == 0;
}

// Inside `root` (both absolute)
bool isInside(const fs::path& path, const fs::path& root){
    const std::string p = path.lexically_normal().string(), r = root.lexically_normal().string();
    return p.size() > r.size() && startsWith(p, r) && (p[r.size()] == '/' || r.back() == '/');
}

std::string urlEncodePath(const std::string& path){
    std::string encoded;
    for(unsigned char c : path){
        if(std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') encoded += (char)c;
        else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
            encoded += buffer;
        }
    }
    return encoded;
}

// A process other than sclang, through the compiler's spawner
std::string runProcess(const std::string& executable, const std::vector<std::string>& args,
                       const std::string& logPath, int timeoutSeconds){
    scSynthdefCompiler::ScriptJob job;
    job.sclang = executable;
    job.args = args;
    job.logPath = logPath;
    job.timeoutSeconds = timeoutSeconds;
    return scSynthdefCompiler::runScript(job);
}

// GET into a file; the HTTP status, 0 when there was no answer
int httpGet(const std::string& url, const fs::path& out, const fs::path& log){
    runProcess("/usr/bin/curl", {"-sSL", "--retry", "2", "--connect-timeout", "20",
                                 "-H", "Accept: application/vnd.github+json",
                                 "-o", out.string(), "-w", "\n%{http_code}", url},
               log.string(), downloadTimeoutSeconds);
    const std::string text = ofTrim(readFile(log));
    const size_t newline = text.find_last_of('\n');
    return ofToInt(newline == std::string::npos ? text : text.substr(newline + 1));
}

// Remove and copy, never write over: macOS kills a process that maps a
// binary changed in place (scsynth and its .scx plugins).
void replaceFile(const fs::path& from, const fs::path& to, bool executable){
    fs::create_directories(to.parent_path());
    std::error_code ignored;
    fs::remove(to, ignored);
    fs::copy_file(from, to);
    fs::permissions(to, executable ? fs::perms(0755) : fs::perms(0644));
}

struct RemoteFile {
    std::string path;       // in the repository
    std::string sha;        // git blob id
    bool executable = false;
    bool preset = false;    // the user may have changed it: never overwritten
    fs::path dest;          // in the app's data folder
};

struct Source {
    std::string rel;        // under SynthdefSources
    fs::path path;
    enum Kind { Generic, Full, Script } kind = Generic;
    std::string key;        // blob id: compiled again when it changes
    std::vector<std::string> names;     // ~synthCreator names it declares
};

struct SourceResult {
    bool ok = false;
    std::string reason;
};

// One sclang run of syncCompile.scd
struct CompileRun {
    std::string mode;                   // stereo, full, script
    std::vector<std::string> entries;   // source paths, optionally ::builds
    fs::path outDir, log;
};

class SyncRun {
public:
    SyncRun(std::shared_ptr<scSynthdefSync::Progress> progress, std::shared_ptr<std::atomic<bool>> cancel,
            bool fromCheckout, bool rebuildAll, std::string addonRoot, std::string branch)
    : progress(std::move(progress)), cancel(std::move(cancel)), fromCheckout(fromCheckout), rebuildAll(rebuildAll),
      addonRoot(std::move(addonRoot)), branch(std::move(branch)){
        data = ofToDataPath("Supercollider", true);
        sources = data / "SynthdefSources";
        sclangDir = data / "Sclang";
        extensions = data / "Extensions";
        synthdefs = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        retiredRoot = data / "_retired_synthdefs";
        work = data / "tmp" / "synthdefSync";
        statePath = data / "Config" / "SynthdefSync.json";
        parallel = std::clamp((int)std::thread::hardware_concurrency() / 2, 2, 6);
    }

    void run(){
        try {
#ifndef __APPLE__
            fail("Syncing SynthDefs is only supported on macOS");
            return;
#endif
            std::error_code ignored;
            fs::remove_all(work, ignored);
            fs::create_directories(work / "home");
            fs::create_directories(statePath.parent_path());
            loadState();
            if(!listFiles() || cancelled()) return;
            downloadFiles();
            // What was done is kept even when stopped halfway
            if(!cancelled()) syncExtensions();
            if(!cancelled()) compile();
            saveState();
            if(!cancelled()) fs::remove_all(work, ignored);
            phase("Done", "");
        } catch(const std::exception& error) {
            fail(error.what());
        }
    }

private:
    // ── Progress ──────────────────────────────────────────────────────────────

    void phase(const std::string& name, const std::string& item, int total = 0){
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->phase = name;
        progress->item = item;
        progress->done = 0;
        progress->total = total;
    }
    void step(const std::string& item, int count = 1){
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->done += count;
        if(!item.empty()) progress->item = item;
    }
    void fail(const std::string& error){
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->report.error = error;
        ofLogError("scSynthdefSync") << error;
    }
    void failure(const std::string& what, const std::string& why){
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->report.failures.push_back({what, why});
        ofLogWarning("scSynthdefSync") << what << ": " << why;
    }
    void note(const std::string& text){
        std::lock_guard<std::mutex> lock(progress->mutex);
        progress->report.notes.push_back(text);
        ofLogNotice("scSynthdefSync") << text;
    }
    template<typename F> void edit(F&& change){
        std::lock_guard<std::mutex> lock(progress->mutex);
        change(progress->report);
    }
    bool cancelled(){
        if(!cancel->load()) return false;
        edit([](scSynthdefSync::Report& r){ r.cancelled = true; });
        return true;
    }

    // ── State ─────────────────────────────────────────────────────────────────

    void loadState(){
        if(!fs::exists(statePath)) return;
        try {
            state = ofJson::parse(readFile(statePath));
        } catch(const std::exception&) {
            state = ofJson::object();
        }
        if(!state.is_object()) state = ofJson::object();
    }

    void saveState(){
        std::ofstream(statePath) << state.dump(1);
    }

    // ── 1. Files of the repository ────────────────────────────────────────────

    // Where a repository file goes in the data folder, empty to skip it
    fs::path destinationOf(const std::string& path){
        const auto parts = ofSplitString(path, "/", true);
        if(parts.empty() || parts.back() == ".DS_Store") return {};
        if(parts[0] == "synthdefs" && parts.size() > 1){
            if(parts[1] == "WIP") return {};
            if(std::find(parts.begin(), parts.end(), "CompiledSynthdefs") != parts.end()) return {};
            return sources / path.substr(std::string("synthdefs/").size());
        }
        if(parts[0] == "sclang" && parts.size() > 1){
            if(parts.size() == 2 && (parts[1] == "build_sclang.sh" || parts[1] == "README.md")) return {};
            return sclangDir / path.substr(std::string("sclang/").size());
        }
        // Our own UGens: ugens/<Plugin>/<Plugin>.scx and its class file
        if(parts[0] == "ugens" && parts.size() == 3){
            const std::string extension = fs::path(parts[2]).extension().string();
            if(extension == ".scx" || extension == ".sc") return extensions / parts[1] / parts[2];
        }
        // Shared presets, at their path in the data folder:
        // presets/nodeSnapshots/<Node>/banks|Presets/<x>.json
        if(isPresetPath(path)) return data.parent_path() / path.substr(std::string("presets/").size());
        return {};
    }

    static bool isPresetPath(const std::string& path){
        const auto parts = ofSplitString(path, "/", true);
        if(parts.size() != 5 || parts[0] != "presets" || fs::path(parts[4]).extension() != ".json") return false;
        return parts[1] == "nodeSnapshots" && (parts[3] == "banks" || parts[3] == "Presets");
    }

    // Where the repository's version of a preset goes when the user's copy
    // differs: "<name> (repo).json" next to it
    static fs::path repoCopyOf(const fs::path& dest){
        return dest.parent_path() / (dest.stem().string() + " (repo)" + dest.extension().string());
    }

    bool listFiles(){
        phase("Reading the repository", fromCheckout ? addonRoot : std::string(repository) + " @ " + branch);
        files.clear();
        if(fromCheckout){
            for(const std::string top : {"synthdefs", "sclang", "ugens", "presets"}){
                const fs::path root = fs::path(addonRoot) / top;
                if(!fs::exists(root)) continue;
                for(auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it){
                    if(it->is_directory() && it->path().filename() == "CompiledSynthdefs"){
                        it.disable_recursion_pending();
                        continue;
                    }
                    if(!it->is_regular_file()) continue;
                    RemoteFile file;
                    file.path = fs::relative(it->path(), addonRoot).generic_string();
                    file.dest = destinationOf(file.path);
                    if(file.dest.empty()) continue;
                    file.preset = isPresetPath(file.path);
                    file.sha = scSynthdefSync::gitBlobSha(it->path().string());
                    file.executable = (fs::status(it->path()).permissions() & fs::perms::owner_exec) != fs::perms::none;
                    files.push_back(file);
                }
            }
            sourceId = "checkout:" + addonRoot;
        }else{
            const std::string api = std::string("https://api.github.com/repos/") + repository;
            const int status = httpGet(api + "/commits/" + urlEncodePath(branch), work / "commit.json", work / "commit.log");
            if(status != 200){
                fail(status == 0 ? "GitHub could not be reached (no internet connection?)"
                     : status == 404 ? "The branch \"" + branch + "\" does not exist in " + repository
                     : status == 403 ? "GitHub refused the request (too many requests: try again in an hour)"
                     : "GitHub answered " + ofToString(status) + " for the branch");
                return false;
            }
            commit = ofJson::parse(readFile(work / "commit.json")).value("sha", "");
            if(httpGet(api + "/git/trees/" + commit + "?recursive=1", work / "tree.json", work / "tree.log") != 200){
                fail("Could not read the file list of " + commit.substr(0, 7));
                return false;
            }
            const ofJson tree = ofJson::parse(readFile(work / "tree.json"));
            if(tree.value("truncated", false)){
                fail("GitHub truncated the file list of the repository");
                return false;
            }
            for(const auto& entry : tree["tree"]){
                if(entry.value("type", "") != "blob") continue;
                const std::string mode = entry.value("mode", "");
                if(mode == "120000") continue;  // symbolic link
                RemoteFile file;
                file.path = entry.value("path", "");
                file.dest = destinationOf(file.path);
                if(file.dest.empty()) continue;
                file.preset = isPresetPath(file.path);
                file.sha = entry.value("sha", "");
                file.executable = mode == "100755";
                files.push_back(file);
            }
            sourceId = std::string(repository) + "@" + branch;
            note("Repository " + std::string(repository) + ", branch " + branch + ", commit " + commit.substr(0, 7));
        }
        if(files.empty()){
            fail("The repository has no SynthDef sources");
            return false;
        }
        return true;
    }

    // ── 2. Download ───────────────────────────────────────────────────────────

    void downloadFiles(){
        // What the last sync installed: a preset still like that was not
        // changed by the user and can be updated
        if(state.contains("files") && state["files"].is_object()){
            for(auto it = state["files"].begin(); it != state["files"].end(); ++it) synced[it.key()] = it.value().get<std::string>();
        }
        std::vector<const RemoteFile*> missing;
        phase("Comparing files", "", (int)files.size());
        for(const auto& file : files){
            const std::string local = fs::exists(file.dest) ? scSynthdefSync::gitBlobSha(file.dest.string()) : "";
            bool needed = local != file.sha;
            // A preset the user changed keeps its place; the repository's
            // version goes next to it, unless that copy is already current
            if(needed && file.preset && !local.empty() && (!synced.count(file.path) || synced[file.path] != local)){
                const fs::path copy = repoCopyOf(file.dest);
                needed = !fs::exists(copy) || scSynthdefSync::gitBlobSha(copy.string()) != file.sha;
            }
            if(needed) missing.push_back(&file);
            step("");
        }

        phase(fromCheckout ? "Copying files" : "Downloading files", "", (int)missing.size());
        std::vector<std::vector<const RemoteFile*>> batches;
        for(size_t i = 0; i < missing.size(); i += downloadBatch){
            batches.emplace_back(missing.begin() + i, missing.begin() + std::min(missing.size(), i + downloadBatch));
        }
        runParallel(batches.size(), 6, [&](size_t b){ fetchBatch(b, batches[b]); });

        // Files of an earlier sync that are gone from the repository
        std::set<std::string> current;
        for(const auto& file : files) current.insert(file.path);
        int removed = 0;
        if(state.contains("files") && state["files"].is_object()){
            for(auto it = state["files"].begin(); it != state["files"].end(); ++it){
                if(current.count(it.key())) continue;
                const fs::path dest = destinationOf(it.key());
                if(dest.empty() || !fs::exists(dest)) continue;
                // A preset the user changed stays
                if(isPresetPath(it.key()) && scSynthdefSync::gitBlobSha(dest.string()) != it.value().get<std::string>()) continue;
                std::error_code ignored;
                if(fs::remove(dest, ignored)) removed++;
            }
        }
        // Defaults and the shared prelude are exactly the repository's: a
        // leftover script there would build SynthDefs no node asks for. Other
        // folders may hold the user's own synths.
        std::set<std::string> wanted;
        for(const auto& file : files) wanted.insert(file.dest.lexically_normal().string());
        for(const fs::path root : {sources / "Defaults", sources / "Oceanode"}){
            if(!fs::exists(root)) continue;
            std::vector<fs::path> extra;
            for(auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it){
                if(it->is_directory() && it->path().filename() == "CompiledSynthdefs"){
                    it.disable_recursion_pending();
                    continue;
                }
                if(it->is_regular_file() && it->path().filename() != ".DS_Store" &&
                   !wanted.count(it->path().lexically_normal().string())) extra.push_back(it->path());
            }
            std::error_code ignored;
            for(const auto& path : extra) if(fs::remove(path, ignored)) removed++;
        }
        ofJson record = ofJson::object();
        for(const auto& file : files) record[file.path] = file.sha;
        state["files"] = record;
        state["source"] = sourceId;
        if(!commit.empty()) state["commit"] = commit;
        edit([&](scSynthdefSync::Report& r){ r.removed = removed; });
    }

    void fetchBatch(size_t index, const std::vector<const RemoteFile*>& batch){
        if(cancel->load()) return;
        const fs::path folder = work / "download" / ofToString(index);
        fs::create_directories(folder);
        if(!fromCheckout){
            std::vector<std::string> args = {"-sSfL", "--retry", "2", "--connect-timeout", "20"};
            for(size_t i = 0; i < batch.size(); i++){
                args.push_back("-o");
                args.push_back((folder / ofToString(i)).string());
                args.push_back("https://raw.githubusercontent.com/" + std::string(repository) + "/"
                               + commit + "/" + urlEncodePath(batch[i]->path));
            }
            runProcess("/usr/bin/curl", args, (folder / "curl.log").string(), downloadTimeoutSeconds);
        }
        for(size_t i = 0; i < batch.size(); i++){
            const RemoteFile& file = *batch[i];
            const fs::path temp = fromCheckout ? fs::path(addonRoot) / file.path : folder / ofToString(i);
            if(scSynthdefSync::gitBlobSha(temp.string()) != file.sha){
                failure(file.path, "the download failed");
                continue;
            }
            fs::path dest = file.dest;
            if(file.preset && fs::exists(dest)){
                const std::string local = scSynthdefSync::gitBlobSha(dest.string());
                const auto last = synced.find(file.path);
                if(last == synced.end() || last->second != local){
                    dest = repoCopyOf(dest);
                    edit([&](scSynthdefSync::Report& r){ r.presetCopies.push_back(fs::relative(dest, data.parent_path()).generic_string()); });
                }
            }
            try {
                replaceFile(temp, dest, file.executable);
                if(file.preset) edit([](scSynthdefSync::Report& r){ r.presets++; });
                {
                    std::lock_guard<std::mutex> lock(rewrittenMutex);
                    rewritten.insert(file.dest.lexically_normal().string());
                }
                edit([&](scSynthdefSync::Report& r){
                    r.downloaded++;
                    if(isInside(file.dest, extensions) && file.dest.extension() == ".scx") r.ugensChanged = true;
                });
            } catch(const std::exception& error) {
                failure(file.path, error.what());
            }
        }
        step(batch.back()->path, (int)batch.size());
    }

    // ── 3. Third-party UGens ──────────────────────────────────────────────────

    void syncExtensions(){
        phase("Checking UGens", "");
        const std::string api = std::string("https://api.github.com/repos/") + repository;
        const int status = httpGet(api + "/releases/tags/" + extensionsRelease, work / "release.json", work / "release.log");
        if(status == 404){
            note("No third-party UGens are published yet (release \"" + std::string(extensionsRelease) + "\")");
            return;
        }
        if(status != 200){
            failure("Third-party UGens", "GitHub answered " + ofToString(status));
            return;
        }
        std::map<std::string, std::string> assets;
        // Kept in a variable: a range-for over a member of a temporary dangles
        const ofJson release = ofJson::parse(readFile(work / "release.json"));
        for(const auto& asset : release.value("assets", ofJson::array())){
            assets[asset.value("name", "")] = asset.value("browser_download_url", "");
        }
        if(!assets.count("extensions.json") ||
           httpGet(assets["extensions.json"], work / "extensions.json", work / "extensions.log") != 200){
            failure("Third-party UGens", "the release has no readable extensions.json");
            return;
        }
        const ofJson manifest = ofJson::parse(readFile(work / "extensions.json"));
        if(!manifest.contains("folders")) return;

        std::vector<std::pair<std::string, ofJson>> outdated;
        for(auto it = manifest["folders"].begin(); it != manifest["folders"].end(); ++it){
            const fs::path local = extensions / it.key();
            if(!fs::exists(local) || scSynthdefSync::folderHash(local.string()) != it.value().value("hash", "")){
                outdated.push_back({it.key(), it.value()});
            }
        }
        phase("Installing UGens", "", (int)outdated.size());
        for(const auto& [name, entry] : outdated){
            if(cancelled()) return;
            step(name, 0);
            const std::string asset = entry.value("asset", name + ".zip");
            const fs::path zip = work / "ugens" / asset, unpacked = work / "ugens" / (name + "_unpacked");
            fs::create_directories(unpacked);
            if(!assets.count(asset) || httpGet(assets[asset], zip, work / "ugens" / (name + ".log")) != 200){
                failure(name, "could not download " + asset);
                step("");
                continue;
            }
            const std::string error = runProcess("/usr/bin/ditto", {"-x", "-k", zip.string(), unpacked.string()},
                                                 (work / "ugens" / (name + "_ditto.log")).string(), 300);
            const fs::path folder = unpacked / name;
            if(!error.empty() || !fs::exists(folder)){
                failure(name, "could not unpack " + asset);
                step("");
                continue;
            }
            const fs::path dest = extensions / name;
            fs::remove_all(dest);
            fs::create_directories(dest.parent_path());
            fs::rename(folder, dest);
            edit([&](scSynthdefSync::Report& r){
                r.extensions.push_back(name);
                r.ugensChanged = true;
            });
            step("");
        }
    }

    // ── 4. Compile ────────────────────────────────────────────────────────────

    void compile(){
        const fs::path sclang = sclangDir / "osx" / "sclang";
        if(!fs::exists(sclang) || !fs::exists(sources / "Oceanode" / "syncCompile.scd")){
            fail("The SuperCollider compiler or the sources are missing after the download");
            return;
        }
        writeClassLibraryConfig();

        const std::regex creator("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(");
        const std::regex creatorName("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(\\s*\"([^\"]+)\"\\s*,");
        std::vector<Source> all;
        for(auto it = fs::recursive_directory_iterator(sources); it != fs::recursive_directory_iterator(); ++it){
            const std::string rel = fs::relative(it->path(), sources).generic_string();
            const std::string top = rel.substr(0, rel.find('/'));
            if(it->is_directory()){
                if(top == "Oceanode" || top == "OceanodeCode" || top == "WIP" || it->path().filename() == "CompiledSynthdefs"){
                    it.disable_recursion_pending();
                }
                continue;
            }
            if(it->path().extension() != ".scd") continue;
            const std::string text = readFile(it->path());
            const bool isCreator = std::regex_search(text, creator);
            Source source;
            source.rel = rel;
            source.path = it->path();
            if(top == "Defaults") source.kind = isCreator ? Source::Full : Source::Script;
            else if(!isCreator || text.find("@oceanode-no-autocompile") != std::string::npos) continue;
            source.key = scSynthdefSync::gitBlobSha(it->path().string());
            for(std::sregex_iterator m(text.begin(), text.end(), creatorName), e; m != e; ++m) source.names.push_back((*m)[1].str());
            all.push_back(source);
        }

        if(!state.contains("compiled") || !state["compiled"].is_object()) state["compiled"] = ofJson::object();
        ofJson& compiled = state["compiled"];
        // Sources removed from the repository: their files are pruned below
        std::set<std::string> present;
        for(const auto& source : all) present.insert(source.rel);
        for(auto it = compiled.begin(); it != compiled.end();){
            if(!present.count(it.key())) it = compiled.erase(it);
            else ++it;
        }

        // Only what the app does not have yet: a source this sync downloaded
        // (new or changed), one changed since it was compiled here, or one whose
        // SynthDefs are missing. A source the app already had, unchanged and
        // never compiled by a sync, is taken as built (its binaries came with it).
        indexSynthdefs();
        retireListed();
        std::vector<const Source*> todo;
        for(const auto& source : all){
            bool needed = rebuildAll || rewritten.count(source.path.lexically_normal().string()) > 0;
            if(!needed && compiled.contains(source.rel)){
                needed = compiled[source.rel].value("key", "") != source.key;
                for(const auto& output : compiled[source.rel]["outputs"]){
                    if(needed) break;
                    needed = !fs::exists(synthdefs / output.get<std::string>());
                }
            }else if(!needed && source.kind == Source::Generic){
                for(const auto& name : source.names){
                    if(!hasGenericBuilds(name)){ needed = true; break; }
                }
            }
            if(needed) todo.push_back(&source);
        }
        if(todo.empty()){
            note("Every SynthDef is up to date");
            prune(all, compiled);
            cleanEmptyFolders(synthdefs);
            matchFolderCase();
            return;
        }

        phase("Compiling", "", (int)todo.size());

        // Defaults scripts write next to themselves: the scripts of one folder
        // run one after the other, each harvested before the next.
        std::map<fs::path, std::vector<const Source*>> scriptsByFolder;
        std::vector<const Source*> generic, full;
        for(auto* source : todo){
            if(source->kind == Source::Script) scriptsByFolder[source->path.parent_path()].push_back(source);
            else if(source->kind == Source::Full) full.push_back(source);
            else generic.push_back(source);
        }

        std::vector<std::function<void()>> tasks;
        std::mutex resultsMutex;
        std::map<std::string, fs::path> scriptOutputs;           // rel -> harvested folder
        std::vector<std::pair<CompileRun, std::vector<const Source*>>> runs;
        for(auto& [folder, scripts] : scriptsByFolder){
            tasks.push_back([&, scripts = scripts](){
                for(auto* source : scripts){
                    if(cancel->load()) return;
                    const fs::path compiledDir = source->path.parent_path() / "CompiledSynthdefs";
                    std::error_code ignored;
                    fs::remove_all(compiledDir, ignored);
                    CompileRun run;
                    run.mode = "script";
                    run.entries = {source->path.string()};
                    run.outDir = work / "unused";
                    run.log = work / "logs" / (ofJoinString(ofSplitString(source->rel, "/"), "_") + ".log");
                    const auto results = runSclang(run, scriptTimeoutSeconds);
                    const fs::path harvested = work / "scripts" / ofJoinString(ofSplitString(source->rel, "/"), "_");
                    bool wrote = false;
                    if(fs::exists(compiledDir)){
                        fs::create_directories(harvested.parent_path());
                        fs::rename(compiledDir, harvested);
                        for(auto f = fs::recursive_directory_iterator(harvested); f != fs::recursive_directory_iterator(); ++f){
                            if(f->is_regular_file() && f->path().extension() == ".scsyndef"){ wrote = true; break; }
                        }
                    }
                    auto result = results.count(source->path.string()) ? results.at(source->path.string()) : SourceResult{false, "sclang stopped"};
                    if(result.ok && !wrote) result = {false, "it wrote no SynthDef"};
                    if(result.ok){
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        scriptOutputs[source->rel] = harvested;
                    }else{
                        failure(source->rel, result.reason);
                    }
                    step(source->rel);
                }
            });
        }
        auto addChunks = [&](const std::string& mode, const std::vector<const Source*>& list, size_t size){
            for(size_t i = 0; i < list.size(); i += size){
                std::vector<const Source*> chunk(list.begin() + i, list.begin() + std::min(list.size(), i + size));
                CompileRun run;
                run.mode = mode;
                for(auto* source : chunk) run.entries.push_back(source->path.string());
                run.outDir = work / mode / ofToString(i);
                run.log = work / "logs" / (mode + ofToString(i) + ".log");
                runs.push_back({run, chunk});
            }
        };
        addChunks("full", full, compileChunk);
        addChunks("stereo", generic, compileChunk);
        std::vector<std::map<std::string, SourceResult>> runResults(runs.size());
        for(size_t r = 0; r < runs.size(); r++){
            tasks.push_back([&, r](){
                if(cancel->load()) return;
                runResults[r] = runSclang(runs[r].first, chunkTimeoutSeconds);
                for(auto* source : runs[r].second) step(source->rel);
            });
        }
        runParallel(tasks.size(), parallel, [&](size_t t){ tasks[t](); });
        if(cancelled()) return;

        // ── Install ──
        phase("Installing", "", (int)todo.size());
        for(size_t r = 0; r < runs.size(); r++){
            const auto& [run, chunk] = runs[r];
            for(size_t i = 0; i < chunk.size(); i++){
                const Source* source = chunk[i];
                const auto found = runResults[r].find(source->path.string());
                if(found == runResults[r].end() || !found->second.ok){
                    if(!cancel->load()) failure(source->rel, found == runResults[r].end() ? "sclang stopped" : found->second.reason);
                    step("");
                    continue;
                }
                std::vector<std::string> outputs;
                const fs::path built = run.outDir / ofToString(i);
                for(const auto& synth : fs::directory_iterator(built)){
                    if(!synth.is_directory()) continue;
                    const std::string name = synth.path().filename().string();
                    if(run.mode == "full") installFull(*source, synth.path(), outputs);
                    else installGeneric(*source, name, synth.path(), outputs, sourceChanged(*source, compiled));
                }
                compiled[source->rel] = {{"key", source->key}, {"outputs", outputs}};
                edit([](scSynthdefSync::Report& rep){ rep.compiledSynths++; });
                step(source->rel);
            }
        }
        for(const auto& [rel, folder] : scriptOutputs){
            const Source* source = nullptr;
            for(auto* s : todo) if(s->rel == rel) source = s;
            std::vector<std::string> outputs;
            installScript(*source, folder, outputs);
            compiled[rel] = {{"key", source->key}, {"outputs", outputs}};
            edit([](scSynthdefSync::Report& rep){ rep.compiledScripts++; });
            step(rel);
        }

        prune(all, compiled);
        cleanEmptyFolders(synthdefs);
        matchFolderCase();
    }

    // macOS ignores case: Defaults/MixerPro went into an existing
    // Defaults/mixerpro. Give such folders their source folder's name.
    void matchFolderCase(){
        const fs::path installed = synthdefs / "Defaults", source = sources / "Defaults";
        if(!fs::exists(installed) || !fs::exists(source)) return;
        std::map<std::string, std::string> names;
        for(const auto& entry : fs::directory_iterator(source)){
            if(entry.is_directory()) names[lower(entry.path().filename().string())] = entry.path().filename().string();
        }
        std::vector<std::pair<fs::path, std::string>> renames;
        for(const auto& entry : fs::directory_iterator(installed)){
            const std::string name = entry.path().filename().string();
            const auto found = names.find(lower(name));
            if(entry.is_directory() && found != names.end() && found->second != name) renames.push_back({entry.path(), found->second});
        }
        for(const auto& [path, name] : renames){
            std::error_code error;
            const fs::path temp = path.parent_path() / (name + ".renaming");
            fs::rename(path, temp, error);
            if(!error) fs::rename(temp, path.parent_path() / name, error);
        }
    }

    void writeClassLibraryConfig(){
        classConfig = work / "sclang_conf.yaml";
        std::ofstream conf(classConfig);
        conf << "includePaths:\n"
             << "    -   \"" << (sclangDir / "SCClassLibrary").string() << "\"\n"
             << "    -   \"" << (sclangDir / "Extensions").string() << "\"\n"
             << "    -   \"" << (sources / "Oceanode").string() << "\"\n";
        // Our own UGens' class files: needed to compile the SynthDefs using them
        std::set<fs::path> ownPlugins;
        for(const auto& file : files){
            if(isInside(file.dest, extensions) && file.dest.extension() == ".sc") ownPlugins.insert(file.dest.parent_path());
        }
        for(const auto& folder : ownPlugins) conf << "    -   \"" << folder.string() << "\"\n";
        // The bundled sclang can carry its own copy of these classes
        // (Extensions/user/OceanodePhysical): two copies fail the class
        // library, so the just-synced one wins.
        std::vector<fs::path> shadowed;
        const fs::path bundled = sclangDir / "Extensions";
        if(fs::exists(bundled)){
            std::error_code error;
            for(auto it = fs::recursive_directory_iterator(bundled, fs::directory_options::skip_permission_denied, error);
                it != fs::recursive_directory_iterator(); it.increment(error)){
                if(error) break;
                if(it.depth() >= 1) it.disable_recursion_pending();
                if(!it->is_directory()) continue;
                for(const auto& folder : ownPlugins){
                    if(it->path().filename() == folder.filename()) shadowed.push_back(it->path());
                }
            }
        }
        conf << "excludePaths:\n";
        if(shadowed.empty()) conf << "    []\n";
        for(const auto& folder : shadowed) conf << "    -   \"" << folder.string() << "\"\n";
        conf
             << "postInlineWarnings: false\n"
             << "excludeDefaultPaths: true\n";
    }

    // Results by source path, from the OCEANODE_SYNC_ lines of the log
    std::map<std::string, SourceResult> runSclang(const CompileRun& run, int timeoutSeconds){
        fs::create_directories(run.log.parent_path());
        fs::create_directories(run.outDir);
        scSynthdefCompiler::ScriptJob job;
        job.sclang = (sclangDir / "osx" / "sclang").string();
        job.confPath = classConfig.string();
        job.home = (work / "home").string();
        job.args = {(sources / "Oceanode" / "syncCompile.scd").string(), run.mode, run.outDir.string()};
        job.args.insert(job.args.end(), run.entries.begin(), run.entries.end());
        job.logPath = run.log.string();
        job.timeoutSeconds = timeoutSeconds;
        const std::string error = scSynthdefCompiler::runScript(job);

        std::map<std::string, SourceResult> results;
        std::ifstream log(run.log);
        for(std::string line; std::getline(log, line);){
            if(startsWith(line, "OCEANODE_SYNC_OK ")){
                results[line.substr(17)] = {true, ""};
            }else if(startsWith(line, "OCEANODE_SYNC_FAILED ")){
                const std::string rest = line.substr(21);
                const size_t split = rest.find(" :: ");
                results[rest.substr(0, split)] = {false, split == std::string::npos ? "failed" : rest.substr(split + 4)};
            }
        }
        // A run that died (timeout, crash) leaves the rest without a line
        for(const auto& entry : run.entries){
            const std::string path = entry.substr(0, entry.find("::"));
            if(!results.count(path)) results[path] = {false, error.empty() ? "sclang stopped before it" : error};
        }
        return results;
    }

    // ── 5. Install ────────────────────────────────────────────────────────────

    void indexSynthdefs(){
        index.clear();
        if(!fs::exists(synthdefs)) return;
        for(const auto& entry : fs::recursive_directory_iterator(synthdefs, fs::directory_options::skip_permission_denied)){
            if(!entry.is_regular_file()) continue;
            const std::string extension = entry.path().extension().string();
            if(extension == ".scsyndef" || extension == ".txarcmeta") index[entry.path().filename().string()].push_back(entry.path());
        }
    }

    std::string relative(const fs::path& path){
        return fs::relative(path, synthdefs).generic_string();
    }

    // Copies when the contents differ. Other copies of the file move to the
    // retired folder (the server loads every folder, the last one wins): any
    // copy, or with `synth` only those in a folder of that synth, since some
    // names clash (Pulsar2 is a synth and Pulsar's stereo build).
    void install(const fs::path& from, const fs::path& to, const std::string& synth = ""){
        if(!sameContents(from, to)){
            replaceFile(from, to, false);
            edit([](scSynthdefSync::Report& r){ r.installed++; r.synthdefsChanged = true; });
        }
        auto& copies = index[to.filename().string()];
        std::error_code error;
        for(const auto& other : std::vector<fs::path>(copies)){
            if(!synth.empty() && other.parent_path().filename() != synth) continue;
            if(fs::exists(other) && !fs::equivalent(other, to, error)) retire(other);
        }
        copies.erase(std::remove_if(copies.begin(), copies.end(), [](const fs::path& p){ return !fs::exists(p); }), copies.end());
        if(std::none_of(copies.begin(), copies.end(), [&](const fs::path& p){ return fs::equivalent(p, to, error); })) copies.push_back(to);
    }

    void retire(const fs::path& path){
        const fs::path target = retiredRoot / fs::relative(path, synthdefs);
        std::error_code ignored;
        fs::create_directories(target.parent_path());
        fs::remove(target, ignored);
        fs::rename(path, target, ignored);
        if(ignored){
            fs::copy_file(path, target, ignored);
            fs::remove(path, ignored);
        }
        edit([](scSynthdefSync::Report& r){ r.retired++; r.synthdefsChanged = true; });
    }

    // Folder of a Defaults source's builds: Defaults/<its folder>/, a first
    // subfolder named like that folder dropped (CompiledSynthdefs/PolyComb/).
    fs::path defaultsDestination(const Source& source, const fs::path& relInOutput){
        fs::path folder = fs::path(source.rel).parent_path();     // Defaults/PolyComb
        auto it = relInOutput.begin();
        fs::path rest;
        if(relInOutput.has_parent_path() && lower(it->string()) == lower(folder.filename().string())) ++it;
        for(; it != relInOutput.end(); ++it) rest /= *it;
        return synthdefs / folder / rest;
    }

    void installScript(const Source& source, const fs::path& harvested, std::vector<std::string>& outputs){
        for(const auto& entry : fs::recursive_directory_iterator(harvested)){
            // Only the definitions: a .txarcmeta next to them is not Oceanode's
            // node description (VSTPlugin writes its own metadata)
            if(!entry.is_regular_file() || entry.path().extension() != ".scsyndef") continue;
            const fs::path dest = defaultsDestination(source, fs::relative(entry.path(), harvested));
            install(entry.path(), dest);
            outputs.push_back(relative(dest));
        }
    }

    void installFull(const Source& source, const fs::path& synthFolder, std::vector<std::string>& outputs){
        for(const auto& entry : fs::directory_iterator(synthFolder)){
            if(!entry.is_regular_file()) continue;
            const fs::path dest = defaultsDestination(source, synthFolder.filename() / entry.path().filename());
            install(entry.path(), dest);
            outputs.push_back(relative(dest));
        }
    }

    // Built because its source is new or changed, not only for missing builds
    bool sourceChanged(const Source& source, const ofJson& compiled){
        if(rebuildAll || rewritten.count(source.path.lexically_normal().string())) return true;
        return compiled.contains(source.rel) && compiled[source.rel].value("key", "") != source.key;
    }

    // Synths listed in Oceanode/retiredSynthdefs.txt: their metadata and
    // builds, in the folder holding their .txarcmeta (another synth's build
    // may share a file name elsewhere: Pulsar2 was Pulsar's stereo build)
    void retireListed(){
        std::ifstream list(sources / "Oceanode" / "retiredSynthdefs.txt");
        int count = 0;
        for(std::string line; std::getline(list, line);){
            const std::string name = ofTrim(line);
            if(name.empty() || name[0] == '#') continue;
            const std::regex files(escapeRegex(name) + R"((\d+(?:_\d+)*)?\.(scsyndef|txarcmeta))");
            for(const auto& meta : std::vector<fs::path>(index[name + ".txarcmeta"])){
                if(!fs::exists(meta)) continue;
                std::vector<fs::path> stale;
                for(const auto& entry : fs::directory_iterator(meta.parent_path())){
                    if(std::regex_match(entry.path().filename().string(), files)) stale.push_back(entry.path());
                }
                for(const auto& path : stale){ retire(path); count++; }
            }
        }
        if(count) note(ofToString(count) + " SynthDefs of retired synths moved to _retired_synthdefs");
    }

    static std::string escapeRegex(const std::string& text){
        return std::regex_replace(text, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
    }

    // The metadata SynthDef and the mono and stereo builds (every variable at 1)
    bool hasGenericBuilds(const std::string& name){
        fs::path folder;
        for(const auto& path : index[name + ".txarcmeta"]) if(fs::exists(path)){ folder = path.parent_path(); break; }
        if(folder.empty() || !fs::exists(folder / (name + ".scsyndef"))) return false;
        for(const std::string n : {"1", "2"}){
            bool found = false;
            for(const auto& entry : fs::directory_iterator(folder)){
                const std::string file = entry.path().filename().string();
                if(startsWith(file, name + n) && std::regex_match(file.substr(name.size() + n.size()), std::regex(R"((_1)*\.scsyndef)"))){
                    found = true;
                    break;
                }
            }
            if(!found) return false;
        }
        return true;
    }

    void installGeneric(const Source& source, const std::string& name, const fs::path& synthFolder,
                        std::vector<std::string>& outputs, bool sourceChanged){
        // Where its builds already are, unless under Defaults (a generic synth
        // that used to be filed there): then next to its source's category.
        fs::path dest = synthdefs / fs::path(source.rel).parent_path() / name;
        fs::path existing;
        for(const auto& path : index[name + ".txarcmeta"]){
            if(fs::exists(path)){ existing = path.parent_path(); break; }
        }
        const std::regex build(escapeRegex(name) + R"((\d+(?:_\d+)*)\.scsyndef)");
        if(!existing.empty()){
            if(isInside(existing, synthdefs / "Defaults")){
                // Bring its builds along
                for(const auto& entry : fs::directory_iterator(existing)){
                    const std::string file = entry.path().filename().string();
                    if(std::regex_match(file, build) && !fs::exists(dest / file)){
                        fs::create_directories(dest);
                        fs::rename(entry.path(), dest / file);
                        auto& copies = index[file];
                        std::replace(copies.begin(), copies.end(), entry.path(), dest / file);
                    }
                }
            }else{
                dest = existing;
            }
        }
        // A new source can change only higher channel counts (code under
        // `if(n > 2)`), where the metadata SynthDef (1 channel) stays the same
        const bool changed = sourceChanged || !sameContents(synthFolder / (name + ".scsyndef"), dest / (name + ".scsyndef"));

        std::set<std::string> fresh;
        for(const auto& entry : fs::directory_iterator(synthFolder)){
            if(!entry.is_regular_file()) continue;
            install(entry.path(), dest / entry.path().filename(), name);
            outputs.push_back(relative(dest / entry.path().filename()));
            fresh.insert(entry.path().filename().string());
        }
        if(!changed) return;
        edit([&](scSynthdefSync::Report& r){ r.changedSynths.push_back(name); });

        // The other channel counts installed were built from the old source and
        // may not match the new parameters: they go, and are compiled again on
        // demand when a node asks for one.
        if(!fs::exists(dest)) return;
        std::vector<fs::path> stale;
        for(const auto& entry : fs::directory_iterator(dest)){
            const std::string file = entry.path().filename().string();
            if(!fresh.count(file) && std::regex_match(file, build)) stale.push_back(entry.path());
        }
        for(const auto& path : stale) retire(path);
    }

    // Everything under Synthdefs/Defaults that no Defaults source builds, once
    // every Defaults source has been built at least once.
    void prune(const std::vector<Source>& all, const ofJson& compiled){
        std::set<std::string> kept;
        for(const auto& source : all){
            if(source.kind == Source::Generic) continue;
            if(!compiled.contains(source.rel)) return;
            for(const auto& output : compiled[source.rel]["outputs"]) kept.insert(lower(output.get<std::string>()));
        }
        // ~synthCreator synths (not under Defaults) recorded too: never pruned
        for(auto it = compiled.begin(); it != compiled.end(); ++it){
            for(const auto& output : it.value()["outputs"]) kept.insert(lower(output.get<std::string>()));
        }
        const fs::path defaults = synthdefs / "Defaults";
        if(!fs::exists(defaults)) return;
        std::vector<fs::path> stale;
        for(const auto& entry : fs::recursive_directory_iterator(defaults)){
            if(!entry.is_regular_file()) continue;
            const std::string extension = entry.path().extension().string();
            if(extension != ".scsyndef" && extension != ".txarcmeta") continue;
            if(!kept.count(lower(relative(entry.path())))) stale.push_back(entry.path());
        }
        for(const auto& path : stale) retire(path);
        if(!stale.empty()) note(ofToString(stale.size()) + " SynthDefs no source builds were moved to _retired_synthdefs");
    }

    void cleanEmptyFolders(const fs::path& root){
        if(!fs::exists(root)) return;
        std::vector<fs::path> folders;
        for(const auto& entry : fs::recursive_directory_iterator(root)){
            if(entry.is_directory()) folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b){
            return a.string().size() > b.string().size();
        });
        for(const auto& folder : folders){
            std::error_code ignored;
            // Only .DS_Store left counts as empty
            bool empty = true;
            for(const auto& entry : fs::directory_iterator(folder)){
                if(entry.path().filename() != ".DS_Store"){ empty = false; break; }
            }
            if(empty) fs::remove_all(folder, ignored);
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    template<typename F> void runParallel(size_t count, int threads, F&& task){
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for(int t = 0; t < std::min<int>(threads, (int)count); t++){
            pool.emplace_back([&](){
                for(size_t i = next++; i < count; i = next++){
                    try {
                        task(i);
                    } catch(const std::exception& error) {
                        failure("internal", error.what());
                    }
                }
            });
        }
        for(auto& thread : pool) thread.join();
    }

    std::shared_ptr<scSynthdefSync::Progress> progress;
    std::shared_ptr<std::atomic<bool>> cancel;
    bool fromCheckout, rebuildAll;
    std::string addonRoot, branch, commit, sourceId;
    std::set<std::string> rewritten;    // files this sync wrote
    std::map<std::string, std::string> synced;  // repository path -> blob id the last sync installed
    std::mutex rewrittenMutex;
    fs::path data, sources, sclangDir, extensions, synthdefs, retiredRoot, work, statePath, classConfig;
    int parallel = 2;
    ofJson state = ofJson::object();
    std::vector<RemoteFile> files;
    std::map<std::string, std::vector<fs::path>> index;    // file name -> installed copies
    const fs::file_time_type syncStart = fs::file_time_type::clock::now();
};

// The branch this checkout is on, "" if it is not a git checkout
std::string checkoutBranch(const fs::path& root){
    fs::path git = root / ".git";
    if(fs::is_regular_file(git)){
        // A worktree: "gitdir: <path>"
        const std::string text = ofTrim(readFile(git));
        if(!startsWith(text, "gitdir: ")) return "";
        git = text.substr(8);
        if(git.is_relative()) git = root / git;
    }
    const std::string head = ofTrim(readFile(git / "HEAD"));
    return startsWith(head, "ref: refs/heads/") ? head.substr(16) : "";
}

}

// ════════════════════════════════════════════════════════════════════════════

std::string scSynthdefSync::gitBlobSha(const std::string& path){
#ifdef __APPLE__
    std::ifstream file(path, std::ios::binary);
    if(!file) return "";
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string header = "blob " + std::to_string(contents.size()) + std::string(1, '\0');
    CC_SHA1_CTX context;
    CC_SHA1_Init(&context);
    CC_SHA1_Update(&context, header.data(), (CC_LONG)header.size());
    CC_SHA1_Update(&context, contents.data(), (CC_LONG)contents.size());
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(digest, &context);
    return hex(digest, sizeof(digest));
#else
    return "";
#endif
}

std::string scSynthdefSync::folderHash(const std::string& folder){
    std::vector<std::pair<std::string, fs::path>> entries;
    std::error_code error;
    for(auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator(); it.increment(error)){
        const std::string name = it->path().filename().string();
        if(!it->is_regular_file() || name == ".DS_Store" || startsWith(name, "._")) continue;
        entries.push_back({fs::relative(it->path(), folder).generic_string(), it->path()});
    }
    std::sort(entries.begin(), entries.end());
    std::string listing;
    for(const auto& [rel, path] : entries) listing += rel + '\0' + sha256(readFile(path)) + '\n';
    return sha256(listing);
}

scSynthdefSync::scSynthdefSync(){
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    if(fs::exists(root / "synthdefs" / "Oceanode" / "syncCompile.scd")){
        addonRoot = root.string();
        const std::string current = checkoutBranch(root);
        std::snprintf(branch, sizeof(branch), "%s", current.empty() ? "main" : current.c_str());
    }else{
        std::snprintf(branch, sizeof(branch), "%s", "main");
    }
}

scSynthdefSync::~scSynthdefSync(){
    if(worker.joinable()){
        // A running sclang finishes on its own; nothing waits for it
        if(cancelFlag) cancelFlag->store(true);
        worker.detach();
    }
}

void scSynthdefSync::drawMenuItem(){
    const bool running = stage == Stage::Running;
    if(ImGui::MenuItem("Sync with SynthDefs Git...", nullptr, false, !running)) openRequested = true;
}

void scSynthdefSync::start(){
    progress = std::make_shared<Progress>();
    cancelFlag = std::make_shared<std::atomic<bool>>(false);
    stage = Stage::Running;
    auto run = std::make_shared<SyncRun>(progress, cancelFlag, fromCheckout, rebuildAll, addonRoot, std::string(branch));
    auto shared = progress;
    worker = std::thread([run, shared](){
        run->run();
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->finished = true;
    });
}

void scSynthdefSync::finish(){
    if(worker.joinable()) worker.join();
    {
        std::lock_guard<std::mutex> lock(progress->mutex);
        report = progress->report;
    }
    stage = Stage::Done;
    if(!report.synthdefsChanged && report.downloaded == 0) return;
    // New sources and compiler for the on-demand builds, new node types, and
    // the new definitions in the running servers.
    scSynthdefCompiler::get().refreshPaths();
    if(reloadCallback) reloadCallback();
    // Definitions using new UGens fail to load until the servers restart
    for(auto* manager : servers) if(manager) manager->loadDefs(true);
    for(const auto& name : report.changedSynths) scSynthdef::reloadAll(name);
}

void scSynthdefSync::restartServers(){
    for(auto* manager : servers) if(manager) manager->prepareForRestart();
    for(auto* manager : servers) if(manager) manager->boot();
}

void scSynthdefSync::drawPopup(){
    if(openRequested){
        openRequested = false;
        if(stage != Stage::Running) stage = Stage::Confirm;
        ImGui::OpenPopup(popupId);
    }
    if(stage == Stage::Running){
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(progress->mutex);
            finished = progress->finished;
        }
        if(finished) finish();
    }
    if(stage == Stage::Closed) return;
    if(!ImGui::IsPopupOpen(popupId)) ImGui::OpenPopup(popupId);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 30, 0), ImVec2(ImGui::GetFontSize() * 44, FLT_MAX));
    if(!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42);

    if(stage == Stage::Confirm){
        ImGui::TextUnformatted("Brings the SynthDef sources, the SuperCollider compiler, the UGens and the "
                               "shared presets and snapshot banks up to date, then compiles what changed or is missing.");
        ImGui::TextDisabled("No GitHub account is needed. The first sync can take several minutes; "
                            "the next ones only handle what changed.");
        ImGui::Spacing();
        if(ImGui::RadioButton("GitHub", !fromCheckout)) fromCheckout = false;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", repository);
        if(!fromCheckout){
            ImGui::Indent();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            ImGui::InputText("Branch", branch, sizeof(branch));
            ImGui::Unindent();
        }
        if(!addonRoot.empty()){
            if(ImGui::RadioButton("This addon checkout", fromCheckout)) fromCheckout = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(no download, includes uncommitted changes)");
        }
        ImGui::Spacing();
        ImGui::Checkbox("Compile everything again", &rebuildAll);
        ImGui::SameLine();
        ImGui::TextDisabled("(slow: every synth and Defaults script)");
        ImGui::Spacing();
        ImGui::BeginDisabled(!fromCheckout && ofTrim(branch).empty());
        if(ImGui::Button("Sync", ImVec2(120, 0))) start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Cancel", ImVec2(120, 0))){
            stage = Stage::Closed;
            ImGui::CloseCurrentPopup();
        }
    }else if(stage == Stage::Running){
        std::string phase, item;
        int done = 0, total = 0;
        {
            std::lock_guard<std::mutex> lock(progress->mutex);
            phase = progress->phase;
            item = progress->item;
            done = progress->done;
            total = progress->total;
        }
        ImGui::TextUnformatted(phase.c_str());
        const float fraction = total > 0 ? (float)done / total : 0.0f;
        const std::string overlay = total > 0 ? ofToString(done) + " / " + ofToString(total) : "";
        if(total > 0) ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 30, 0), overlay.c_str());
        else ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(ImGui::GetFontSize() * 30, 0), "");
        ImGui::TextDisabled("%s", item.c_str());
        ImGui::Spacing();
        if(cancelFlag->load()) ImGui::TextDisabled("Stopping after the current step...");
        else if(ImGui::Button("Stop", ImVec2(120, 0))) cancelFlag->store(true);
    }else{
        if(!report.error.empty()){
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "The sync stopped:");
            ImGui::TextUnformatted(report.error.c_str());
        }else if(report.cancelled){
            ImGui::TextUnformatted("The sync was stopped. Run it again to finish.");
        }else{
            ImGui::TextUnformatted(report.failures.empty() ? "Everything is up to date." : "Done, with problems:");
        }
        ImGui::Spacing();
        if(report.downloaded || report.removed) ImGui::BulletText("%d files downloaded, %d removed", report.downloaded, report.removed);
        if(report.presets) ImGui::BulletText("%d presets and snapshot banks updated", report.presets);
        if(!report.presetCopies.empty()){
            ImGui::BulletText("%d presets you changed were kept; the shared version is next to each as \"(repo)\":",
                              (int)report.presetCopies.size());
            ImGui::Indent();
            for(const auto& path : report.presetCopies) ImGui::TextDisabled("%s", path.c_str());
            ImGui::Unindent();
        }
        if(!report.extensions.empty()) ImGui::BulletText("UGens installed: %s", ofJoinString(report.extensions, ", ").c_str());
        if(report.compiledSynths || report.compiledScripts){
            ImGui::BulletText("%d synths and %d Defaults scripts compiled, %d SynthDefs installed",
                              report.compiledSynths, report.compiledScripts, report.installed);
        }
        if(report.retired) ImGui::BulletText("%d old SynthDefs moved to _retired_synthdefs", report.retired);
        for(const auto& text : report.notes) ImGui::TextDisabled("%s", text.c_str());
        if(!report.failures.empty() &&
           ImGui::CollapsingHeader(("Problems (" + ofToString(report.failures.size()) + ")").c_str())){
            ImGui::BeginChild("failures", ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 12), true);
            for(const auto& [what, why] : report.failures){
                ImGui::TextUnformatted(what.c_str());
                ImGui::Indent();
                ImGui::TextDisabled("%s", why.c_str());
                ImGui::Unindent();
            }
            ImGui::EndChild();
        }
        ImGui::Spacing();
        bool close = false;
        if(report.ugensChanged){
            ImGui::TextUnformatted("New UGens are loaded only when the servers start: restart them now? "
                                   "The sound stops for a moment.");
            if(ImGui::Button("Restart servers", ImVec2(140, 0))){
                restartServers();
                close = true;
            }
            ImGui::SameLine();
            if(ImGui::Button("Later", ImVec2(120, 0))) close = true;
        }else if(ImGui::Button("OK", ImVec2(120, 0))){
            close = true;
        }
        if(close){
            stage = Stage::Closed;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

// ════════════════════════════════════════════════════════════════════════════
// Share Presets to Git
// ════════════════════════════════════════════════════════════════════════════

namespace {

constexpr const char* sharePopupId = "Share Presets to Git##scPresetShare";

// git in the checkout; "" when it succeeded, else its last lines
std::string git(const std::string& root, std::vector<std::string> args, const fs::path& log, int timeout = 300){
    args.insert(args.begin(), {"-C", root});
    return runProcess("/usr/bin/git", args, log.string(), timeout);
}

// Snapshot banks and presets as the sync installs them, relative to the data folder
std::vector<std::string> sharablePresets(const fs::path& data){
    std::vector<std::string> found;
    const auto add = [&](const fs::path& folder){
        if(!fs::exists(folder)) return;
        for(const auto& entry : fs::directory_iterator(folder)){
            const std::string name = entry.path().filename().string();
            if(!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            // the sync's copies, automatic backups, generated factory sets
            if(name.find(" (repo)") != std::string::npos || startsWith(name, "Before ") || name == "Factory.json") continue;
            found.push_back(fs::relative(entry.path(), data).generic_string());
        }
    };
    const fs::path root = data / "nodeSnapshots";
    if(fs::exists(root)){
        for(const auto& node : fs::directory_iterator(root)){
            if(!node.is_directory()) continue;
            add(node.path() / "banks");
            add(node.path() / "Presets");
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}

scPresetShare::scPresetShare(){
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    if(fs::exists(root / ".git") && fs::exists(root / "presets")){
        addonRoot = root.string();
        branch = checkoutBranch(root);
    }
}

scPresetShare::~scPresetShare(){
    // A running git finishes on its own
    if(worker.joinable()) worker.detach();
}

void scPresetShare::drawMenuItem(){
    if(ImGui::MenuItem("Share Presets to Git...", nullptr, false, stage != Stage::Running)) openRequested = true;
}

void scPresetShare::scan(){
    candidates.clear();
    problem.clear();
    if(addonRoot.empty()){
        problem = "Sharing needs this addon's git checkout (ofxOceanodeSuperCollider with its presets/ folder).";
        return;
    }
    if(branch.empty()){
        problem = "The addon checkout is not on a branch.";
        return;
    }
    const fs::path data = ofToDataPath("", true);
    // What the last sync installed: unchanged since then means nothing to share
    std::map<std::string, std::string> synced;
    try {
        const ofJson state = ofJson::parse(readFile(data / "Supercollider" / "Config" / "SynthdefSync.json"));
        if(state.contains("files")) for(auto it = state["files"].begin(); it != state["files"].end(); ++it){
            if(startsWith(it.key(), "presets/")) synced[it.key().substr(8)] = it.value().get<std::string>();
        }
    } catch(...) {}
    for(const auto& rel : sharablePresets(data)){
        const fs::path local = data / rel, shared = fs::path(addonRoot) / "presets" / rel;
        const std::string localSha = scSynthdefSync::gitBlobSha(local.string());
        const auto last = synced.find(rel);
        Candidate candidate;
        candidate.rel = rel;
        if(last != synced.end()){
            if(localSha == last->second) continue;
            candidate.isNew = false;
            candidate.sharedChanged = fs::exists(shared) && scSynthdefSync::gitBlobSha(shared.string()) != last->second;
        }else{
            if(fs::exists(shared) && scSynthdefSync::gitBlobSha(shared.string()) == localSha) continue;
            candidate.isNew = !fs::exists(shared);
            candidate.sharedChanged = !candidate.isNew;
        }
        // Someone else's newer version would be replaced: only on purpose
        candidate.selected = !candidate.sharedChanged;
        candidates.push_back(candidate);
    }
    std::snprintf(message, sizeof(message), "%s", "Presets: ");
}

void scPresetShare::start(){
    std::vector<std::string> chosen;
    for(const auto& c : candidates) if(c.selected) chosen.push_back(c.rel);
    task = std::make_shared<Task>();
    stage = Stage::Running;
    std::string text = ofTrim(message);
    if(text.empty() || text == "Presets:"){
        std::vector<std::string> names;
        for(const auto& rel : chosen) names.push_back(fs::path(rel).stem().string());
        text = "Presets: " + ofJoinString(names, ", ");
    }
    worker = std::thread([task = task, root = addonRoot, branch = branch, chosen, text,
                          data = fs::path(ofToDataPath("", true))](){
        const fs::path log = data / "Supercollider" / "tmp" / "presetShare.log";
        fs::create_directories(log.parent_path());
        const auto status = [&](const std::string& s){ std::lock_guard<std::mutex> lock(task->mutex); task->status = s; };
        const auto finish = [&](bool ok, const std::string& result){
            std::lock_guard<std::mutex> lock(task->mutex);
            task->ok = ok;
            task->result = result;
            task->done = true;
        };
        try {
            // Up to date first, so the commit applies on the latest presets
            status("Updating the checkout (git pull)...");
            std::string error = git(root, {"pull", "--rebase", "--autostash", "origin", branch}, log);
            if(!error.empty()){
                git(root, {"rebase", "--abort"}, log.string() + ".abort");
                return finish(false, "git pull failed: " + error);
            }
            status("Copying...");
            std::vector<std::string> paths;
            for(const auto& rel : chosen){
                const fs::path from = data / rel, to = fs::path(root) / "presets" / rel;
                if(fs::exists(to) && sameContents(from, to)) continue;
                fs::create_directories(to.parent_path());
                fs::copy_file(from, to, fs::copy_options::overwrite_existing);
                paths.push_back("presets/" + rel);
            }
            if(paths.empty()) return finish(true, "Those presets are already shared.");
            status("Committing...");
            std::vector<std::string> add = {"add", "--"};
            add.insert(add.end(), paths.begin(), paths.end());
            if(!(error = git(root, add, log)).empty()) return finish(false, "git add failed: " + error);
            // Only these files: other changes in the checkout stay as they are
            std::vector<std::string> commit = {"commit", "-m", text, "--"};
            commit.insert(commit.end(), paths.begin(), paths.end());
            if(!(error = git(root, commit, log)).empty()) return finish(false, "git commit failed: " + error);
            status("Pushing...");
            if(!git(root, {"push", "origin", "HEAD:" + branch}, log).empty()){
                // Someone pushed in between
                if(!git(root, {"pull", "--rebase", "--autostash", "origin", branch}, log).empty()){
                    git(root, {"rebase", "--abort"}, log.string() + ".abort");
                    return finish(false, "Someone changed the same presets meanwhile. Your commit is in the checkout, "
                                         "not pushed: run Sync with SynthDefs Git, then ask for help merging it.");
                }
                if(!(error = git(root, {"push", "origin", "HEAD:" + branch}, log)).empty())
                    return finish(false, "git push failed (" + error + "). The commit is in the checkout, not pushed.");
            }
            finish(true, ofToString(paths.size()) + " presets shared on " + branch + ". Everyone gets them with Sync with SynthDefs Git.");
        } catch(const std::exception& e) {
            finish(false, e.what());
        }
    });
}

void scPresetShare::drawPopup(){
    if(openRequested){
        openRequested = false;
        if(stage != Stage::Running){
            scan();
            stage = Stage::Choose;
        }
        ImGui::OpenPopup(sharePopupId);
    }
    if(stage == Stage::Running){
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            done = task->done;
        }
        if(done){
            if(worker.joinable()) worker.join();
            stage = Stage::Done;
        }
    }
    if(stage == Stage::Closed) return;
    if(!ImGui::IsPopupOpen(sharePopupId)) ImGui::OpenPopup(sharePopupId);
    if(!ImGui::BeginPopupModal(sharePopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
    bool close = false;
    if(stage == Stage::Choose){
        if(!problem.empty()){
            ImGui::TextUnformatted(problem.c_str());
        }else if(candidates.empty()){
            ImGui::TextUnformatted("Nothing to share: every snapshot bank and preset is the same as the shared one.");
        }else{
            ImGui::TextUnformatted("Snapshot banks and presets that are new or changed here:");
            ImGui::TextDisabled("Shared in the addon's presets/ on branch %s, with your git account.", branch.c_str());
            ImGui::Spacing();
            ImGui::BeginChild("candidates", ImVec2(ImGui::GetFontSize() * 38, ImGui::GetFontSize() * std::min(14.0f, 1.6f * candidates.size() + 1)), true);
            for(size_t i = 0; i < candidates.size(); i++){
                auto& c = candidates[i];
                ImGui::PushID((int)i);
                ImGui::Checkbox(c.rel.c_str(), &c.selected);
                ImGui::SameLine();
                if(c.sharedChanged) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "replaces a newer shared version");
                else ImGui::TextDisabled(c.isNew ? "new" : "changed");
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30);
            ImGui::InputText("Message", message, sizeof(message));
        }
        ImGui::Spacing();
        const bool any = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c){ return c.selected; });
        ImGui::BeginDisabled(!any || !problem.empty());
        if(ImGui::Button("Share", ImVec2(120, 0))) start();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Cancel", ImVec2(120, 0))) close = true;
    }else if(stage == Stage::Running){
        std::string status;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            status = task->status;
        }
        ImGui::TextUnformatted(status.c_str());
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(ImGui::GetFontSize() * 30, 0), "");
    }else{
        std::lock_guard<std::mutex> lock(task->mutex);
        if(task->ok) ImGui::TextUnformatted(task->result.c_str());
        else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", task->result.c_str());
        ImGui::Spacing();
        if(ImGui::Button("OK", ImVec2(120, 0))) close = true;
    }
    if(close){
        stage = Stage::Closed;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}
