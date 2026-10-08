//
//  scSynthdefCompiler.cpp
//  ofxOceanodeSuperCollider
//

#include "scSynthdefCompiler.h"
#include "serverManager.h"
#include "imgui.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef __APPLE__
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace {
// Long enough for the biggest synths at high channel counts; a stuck sclang
// is killed after this.
constexpr int buildTimeoutSeconds = 120;
// /d_load is asynchronous; if the /sync never comes back, go on anyway.
constexpr float loadTimeoutSeconds = 5.0f;
// A request must have been left alone this long before the user is asked, so
// dragging N Chan across missing counts does not raise a popup for each.
constexpr float settleSeconds = 0.4f;

std::string describe(const scSynthdefCompiler::Target& target){
    std::string text = target.synthName + "  " + ofToString(target.numChannels) + " ch";
    for(size_t i = 0; i < target.variables.size(); i++){
        text += (i == 0 ? "  (" : ", ") + ofToString(target.variables[i]);
    }
    if(!target.variables.empty()) text += ")";
    return text;
}

// The ERROR line sclang posted and the line after it, else the log's tail.
std::string summarizeLog(const std::string& logPath){
    std::ifstream log(logPath);
    std::vector<std::string> lines;
    for(std::string line; std::getline(log, line);) lines.push_back(line);
    for(size_t i = 0; i < lines.size(); i++){
        if(lines[i].rfind("ERROR", 0) == 0){
            std::string text = lines[i];
            if(i + 1 < lines.size()) text += " " + ofTrim(lines[i + 1]);
            return text;
        }
    }
    std::string tail;
    for(size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); i++) tail += lines[i] + " ";
    return ofTrim(tail);
}
}

scSynthdefCompiler& scSynthdefCompiler::get(){
    static scSynthdefCompiler instance;
    return instance;
}

scSynthdefCompiler::scSynthdefCompiler(){
    updateListener = ofEvents().update.newListener(this, &scSynthdefCompiler::update);
}

// ════════════════════════════════════════════════════════════════════════════
// Requests
// ════════════════════════════════════════════════════════════════════════════

void scSynthdefCompiler::request(void* owner, const Target& target, Callback callback){
    cancel(owner);
    auto inserted = jobs.emplace(target.defName, Job());
    Job& job = inserted.first->second;
    if(inserted.second){
        job.target = target;
        job.message = unavailableReason(target);
        job.state = job.message.empty() ? State::Asking : State::Unavailable;
        ofLogNotice("scSynthdefCompiler") << target.defName << " is missing"
            << (job.message.empty() ? "" : " and cannot be compiled: " + job.message);
    }
    job.waiters.push_back({owner, std::move(callback)});
    job.lastRequestTime = ofGetElapsedTimef();
    job.lastRequestFrame = ofGetFrameNum();
}

void scSynthdefCompiler::cancel(void* owner){
    for(auto it = jobs.begin(); it != jobs.end();){
        auto& waiters = it->second.waiters;
        waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                     [owner](const Waiter& w){ return w.owner == owner; }),
                      waiters.end());
        // A build already running finishes and installs its file regardless.
        const bool running = it->second.state == State::Compiling || it->second.state == State::Loading;
        if(waiters.empty() && !running) it = jobs.erase(it);
        else ++it;
    }
}

void scSynthdefCompiler::finish(const std::string& defName, Result result){
    auto it = jobs.find(defName);
    if(it == jobs.end()) return;
    // Callbacks change node parameters, which can call request()/cancel():
    // nothing may still point into the job table when they run.
    std::vector<Waiter> waiters = std::move(it->second.waiters);
    jobs.erase(it);
    for(auto& waiter : waiters) waiter.callback(result);
}

bool scSynthdefCompiler::isSettled(const Job& job) const {
    return ofGetElapsedTimef() - job.lastRequestTime >= settleSeconds
        && ofGetFrameNum() > job.lastRequestFrame
        && !ImGui::IsMouseDown(0);
}

// ════════════════════════════════════════════════════════════════════════════
// Build queue
// ════════════════════════════════════════════════════════════════════════════

void scSynthdefCompiler::update(ofEventArgs&){
    if(compileTask.valid() &&
       compileTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        finishBuild(compileTask.get());
    }
    if(!compileTask.valid()) startNextBuild();

    std::vector<std::string> loaded;
    for(auto& [defName, job] : jobs){
        if(job.state != State::Loading) continue;
        bool loading = false;
        for(auto* manager : servers){
            if(manager && manager->areRequiredSynthdefsLoading()) loading = true;
        }
        if(!loading || ofGetElapsedTimef() - job.loadStartTime > loadTimeoutSeconds){
            loaded.push_back(defName);
        }
    }
    for(auto& defName : loaded) finish(defName, Result::Ready);
}

void scSynthdefCompiler::startNextBuild(){
    for(auto& [defName, job] : jobs){
        if(job.state != State::Queued) continue;
        job.state = State::Compiling;
        building = defName;
        ofLogNotice("scSynthdefCompiler") << "Compiling " << defName
            << " from " << sourceBySynth[job.target.synthName];
        compileTask = std::async(std::launch::async, &scSynthdefCompiler::runBuild,
                                 getPaths(), job.target, sourceBySynth[job.target.synthName]);
        return;
    }
}

void scSynthdefCompiler::finishBuild(BuildResult result){
    const std::string defName = building;
    building.clear();
    auto it = jobs.find(defName);
    if(it == jobs.end()) return;
    Job& job = it->second;

    if(result.ok){
        const fs::path destination = fs::path(job.target.synthFolder) / (defName + ".scsyndef");
        try {
            fs::copy_file(result.file, destination, fs::copy_options::overwrite_existing);
            fs::remove_all(fs::path(result.file).parent_path().parent_path());
        } catch(const std::exception& error) {
            result.ok = false;
            result.error = std::string("could not install it: ") + error.what();
        }
        if(result.ok){
            ofLogNotice("scSynthdefCompiler") << "Installed " << destination.string();
            for(auto* manager : servers){
                if(manager) manager->loadSynthdefFile(destination.string());
            }
            job.state = State::Loading;
            job.loadStartTime = ofGetElapsedTimef();
            return;
        }
    }
    ofLogError("scSynthdefCompiler") << "Could not compile " << defName << ": " << result.error;
    job.state = State::Failed;
    job.message = result.error;
}

scSynthdefCompiler::BuildResult scSynthdefCompiler::runBuild(Paths paths, Target target, std::string source){
    BuildResult result;
#ifdef __APPLE__
    const fs::path outDir = fs::path(paths.work) / "build" / target.defName;
    const std::string logPath = (fs::path(paths.work) / (target.defName + ".log")).string();
    try {
        fs::remove_all(outDir);
        fs::create_directories(outDir);
    } catch(const std::exception& error) {
        result.error = error.what();
        return result;
    }

    std::vector<std::string> args = {paths.sclang};
    if(!paths.sclangBundle.empty()){
        args.push_back("-l");
        args.push_back((fs::path(paths.work) / "sclang_conf.yaml").string());
    }
    args.push_back((fs::path(paths.sources) / "Oceanode" / "compileExact.scd").string());
    args.push_back(outDir.string());
    args.push_back(source);
    args.push_back(ofToString(target.numChannels));
    for(int value : target.variables) args.push_back(ofToString(value));
    std::vector<char*> argv;
    for(auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    // The bundled compiler gets a private HOME, so the user's startup.scd and
    // SuperCollider settings never run inside it.
    std::vector<std::string> envStrings;
    for(char** e = environ; *e; e++){
        if(!paths.sclangBundle.empty() && std::strncmp(*e, "HOME=", 5) == 0) continue;
        envStrings.push_back(*e);
    }
    if(!paths.sclangBundle.empty()) envStrings.push_back("HOME=" + (fs::path(paths.work) / "home").string());
    std::vector<char*> envp;
    for(auto& entry : envStrings) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Every other descriptor of the app (OSC sockets, files) stays out of sclang.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

    pid_t pid = 0;
    const int spawnError = posix_spawn(&pid, paths.sclang.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if(spawnError != 0){
        result.error = std::string("could not start sclang: ") + std::strerror(spawnError);
        return result;
    }

    int status = 0;
    const auto start = std::chrono::steady_clock::now();
    while(waitpid(pid, &status, WNOHANG) == 0){
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(buildTimeoutSeconds)){
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.error = "sclang did not finish within " + ofToString(buildTimeoutSeconds) + " s";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const fs::path file = outDir / target.synthName / (target.defName + ".scsyndef");
    if(WIFEXITED(status) && WEXITSTATUS(status) == 0 && fs::exists(file)){
        result.ok = true;
        result.file = file.string();
    }else{
        result.error = summarizeLog(logPath);
        if(result.error.empty()) result.error = "sclang wrote no " + file.filename().string();
    }
#else
    result.error = "compiling SynthDefs is only supported on macOS";
#endif
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
// Sources and compiler
// ════════════════════════════════════════════════════════════════════════════

const scSynthdefCompiler::Paths& scSynthdefCompiler::getPaths(){
    if(pathsResolved) return paths;
    pathsResolved = true;

    // Development fallback: this file lives in <addon>/src.
    const fs::path addon = fs::path(__FILE__).parent_path().parent_path();
    const fs::path data = ofToDataPath("Supercollider", true);
    const auto hasRunner = [](const fs::path& root){
        return fs::exists(root / "Oceanode" / "compileExact.scd");
    };
    if(hasRunner(data / "SynthdefSources")) paths.sources = (data / "SynthdefSources").string();
    else if(hasRunner(addon / "synthdefs")) paths.sources = (addon / "synthdefs").string();

    for(const fs::path& bundle : {data / "Sclang", addon / "sclang"}){
        if(fs::exists(bundle / "osx" / "sclang") && fs::exists(bundle / "SCClassLibrary")){
            paths.sclang = (bundle / "osx" / "sclang").string();
            paths.sclangBundle = bundle.string();
            break;
        }
    }
    const std::string systemSclang = "/Applications/SuperCollider.app/Contents/MacOS/sclang";
    if(paths.sclang.empty() && fs::exists(systemSclang)) paths.sclang = systemSclang;

    paths.work = (data / "tmp" / "synthdefCompiler").string();
    try {
        fs::create_directories(fs::path(paths.work) / "home");
    } catch(const std::exception& error) {
        ofLogError("scSynthdefCompiler") << "Cannot create " << paths.work << ": " << error.what();
    }

    // The bundled compiler sees only its own class library, the bundled
    // extensions and the Oceanode classes next to the sources.
    if(!paths.sclangBundle.empty() && !paths.sources.empty()){
        std::ofstream conf(fs::path(paths.work) / "sclang_conf.yaml");
        conf << "includePaths:\n"
             << "    -   \"" << (fs::path(paths.sclangBundle) / "SCClassLibrary").string() << "\"\n"
             << "    -   \"" << (fs::path(paths.sclangBundle) / "Extensions").string() << "\"\n"
             << "    -   \"" << (fs::path(paths.sources) / "Oceanode").string() << "\"\n"
             << "excludePaths:\n    []\n"
             << "postInlineWarnings: false\n"
             << "excludeDefaultPaths: true\n";
    }

    ofLogNotice("scSynthdefCompiler") << "Sources: " << (paths.sources.empty() ? "none" : paths.sources)
        << " | sclang: " << (paths.sclang.empty() ? "none" : paths.sclang);
    return paths;
}

void scSynthdefCompiler::buildSourceIndex(){
    if(indexBuilt) return;
    indexBuilt = true;
    const std::string root = getPaths().sources;
    if(root.empty()) return;

    // ~synthCreator.value("Name", ...) or ~synthCreator.("Name", ...)
    const std::regex creator("~synthCreator\\s*\\.\\s*(?:value\\s*)?\\(\\s*\"([^\"]+)\"");
    try {
        for(auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it){
            const fs::path relative = fs::relative(it->path(), root);
            const std::string top = relative.begin()->string();
            // Defaults are built by hand with their own scripts.
            if(it->is_directory() && (top == "Defaults" || top == "Oceanode")){
                it.disable_recursion_pending();
                continue;
            }
            if(!it->is_regular_file() || it->path().extension() != ".scd") continue;

            std::ifstream file(it->path());
            std::stringstream contents;
            contents << file.rdbuf();
            const std::string text = contents.str();
            const bool excluded = text.find("@oceanode-no-autocompile") != std::string::npos;
            for(std::sregex_iterator match(text.begin(), text.end(), creator), end; match != end; ++match){
                const std::string name = (*match)[1].str();
                if(excluded) noAutocompile.insert(name);
                sourceBySynth.emplace(name, it->path().string());
            }
        }
    } catch(const std::exception& error) {
        ofLogError("scSynthdefCompiler") << "Reading sources failed: " << error.what();
    }
    ofLogNotice("scSynthdefCompiler") << sourceBySynth.size() << " compilable synths in " << root;
}

std::string scSynthdefCompiler::unavailableReason(const Target& target){
    const Paths& p = getPaths();
    if(p.sources.empty()) return "the SynthDef sources were not found (run tools/deploy_synthdef_compiler.sh)";
    if(p.sclang.empty()) return "no SuperCollider compiler (sclang) was found";
    if(target.synthFolder.empty()) return "the folder of its other builds is unknown";
    buildSourceIndex();
    if(noAutocompile.count(target.synthName)) return "its source is marked as not to be recompiled";
    if(!sourceBySynth.count(target.synthName)) return "no source file defines it";
    return "";
}

// ════════════════════════════════════════════════════════════════════════════
// Popup
// ════════════════════════════════════════════════════════════════════════════

void scSynthdefCompiler::drawPopups(){
    std::vector<Job*> asking, failed;
    const Job* busy = nullptr;
    int pending = 0;
    for(auto& [defName, job] : jobs){
        switch(job.state){
            case State::Asking: if(isSettled(job)) asking.push_back(&job); break;
            case State::Unavailable: if(isSettled(job)) failed.push_back(&job); break;
            case State::Failed: failed.push_back(&job); break;
            case State::Queued: case State::Compiling: case State::Loading:
                pending++;
                if(!busy || job.state != State::Queued) busy = &job;
                break;
        }
    }

    constexpr const char* popupId = "SuperCollider SynthDefs##scSynthdefCompiler";
    const bool wanted = !asking.empty() || busy || !failed.empty();
    if(wanted && !ImGui::IsPopupOpen(popupId)) ImGui::OpenPopup(popupId);
    if(!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(!wanted){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    if(!asking.empty()){
        ImGui::TextUnformatted(asking.size() == 1
            ? "This channel count has not been compiled yet:"
            : "These channel counts have not been compiled yet:");
        ImGui::Spacing();
        for(auto* job : asking) ImGui::BulletText("%s", describe(job->target).c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Compile now? It takes about a second each. "
                            "The node stays silent until it is ready.");
        ImGui::Spacing();
        if(ImGui::Button("Compile", ImVec2(120, 0))){
            for(auto* job : asking) job->state = State::Queued;
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel", ImVec2(120, 0))){
            std::vector<std::string> declined;
            for(auto* job : asking) declined.push_back(job->target.defName);
            for(auto& defName : declined) finish(defName, Result::Declined);
        }
    }else if(busy){
        ImGui::Text("Compiling %s ...", describe(busy->target).c_str());
        if(pending > 1) ImGui::TextDisabled("%d more waiting", pending - 1);
    }else{
        ImGui::TextUnformatted("Could not provide these SynthDefs:");
        ImGui::Spacing();
        for(auto* job : failed){
            ImGui::BulletText("%s", describe(job->target).c_str());
            ImGui::Indent();
            ImGui::TextDisabled("%s", job->message.c_str());
            ImGui::Unindent();
        }
        ImGui::Spacing();
        if(ImGui::Button("OK", ImVec2(120, 0))){
            std::vector<std::string> done;
            for(auto* job : failed) done.push_back(job->target.defName);
            for(auto& defName : done) finish(defName, Result::Failed);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}
