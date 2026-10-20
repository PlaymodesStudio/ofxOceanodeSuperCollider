//
//  scSynthdefCompiler.h
//  ofxOceanodeSuperCollider
//
//  Compiles a ~synthCreator SynthDef for a channel count (and variable values)
//  that has no .scsyndef yet. A node asks for it, the user confirms in a popup,
//  a bundled headless sclang builds the one definition from the .scd sources,
//  and the file is installed next to the synth's other builds and loaded into
//  every running server before the node is called back.
//
//  Paths, first one found wins:
//    sources   data/Supercollider/SynthdefSources/   addon synthdefs/
//    sclang    data/Supercollider/Sclang/osx/sclang  addon sclang/osx/sclang
//              /Applications/SuperCollider.app (user's own class library)
//

#ifndef scSynthdefCompiler_h
#define scSynthdefCompiler_h

#include "ofMain.h"
#include <future>

class serverManager;

class scSynthdefCompiler {
public:
    struct Target {
        std::string synthName;          // "PanAz"
        std::string defName;            // "PanAz78_6"
        int numChannels = 1;
        std::vector<int> variables;     // values, in declaration order
        std::string synthFolder;        // folder holding <synthName>.txarcmeta
    };
    enum class Result { Ready, Declined, Failed };
    using Callback = std::function<void(Result)>;

    static scSynthdefCompiler& get();

    void setServers(const std::vector<serverManager*>& servers){ this->servers = servers; }

    // Called back on the main thread once the definition is loaded, or when
    // the user declines / it cannot be built. One request per owner: a new one
    // replaces the previous. Requests for the same definition share one build.
    void request(void* owner, const Target& target, Callback callback);
    // Forget the owner's request (it moved on, or is being destroyed).
    void cancel(void* owner);

    // Every frame, at the top level of the ImGui frame.
    void drawPopups();

    // A headless sclang run of some other script (the SC Code node's capture
    // runner), with the same sclang, class library and private HOME as the
    // channel-count builds.
    struct ScriptJob {
        std::string sclang;                 // "" when there is no sclang
        std::string confPath;               // -l class library config, "" for none
        std::string home;                   // private HOME, "" keeps the user's
        std::vector<std::string> args;      // the script and its arguments
        std::string logPath;                // stdout and stderr
        int timeoutSeconds = 60;
    };
    // Main thread. `name` keeps its class library config apart from the
    // builds'; extraClassFolders are compiled into the class library too.
    ScriptJob prepareScript(const std::string& name, const std::vector<std::string>& extraClassFolders);
    // Root of the .scd tree in use, "" if none was found.
    std::string getSourcesFolder(){ return getPaths().sources; }
    // A source was added or changed: read the source tree again on the next request
    void invalidateSourceIndex(){ indexBuilt = false; sourceBySynth.clear(); noAutocompile.clear(); }
    // Sources or compiler installed in the data folder (Sync with SynthDefs
    // Git): look for them again on the next request
    void refreshPaths(){ pathsResolved = false; paths = Paths(); invalidateSourceIndex(); }
    // Any thread, blocking. "" when sclang exited with status 0, else why not.
    static std::string runScript(const ScriptJob& job);

private:
    scSynthdefCompiler();

    enum class State { Asking, Unavailable, Queued, Compiling, Loading, Failed };

    struct Waiter {
        void* owner;
        Callback callback;
    };

    struct Job {
        Target target;
        State state = State::Asking;
        std::string message;            // why it cannot be / was not built
        std::vector<Waiter> waiters;
        uint64_t lastRequestFrame = 0;
        float lastRequestTime = 0;
        float loadStartTime = 0;
        // Listed in the popup: from then on it stays there, whatever the mouse
        // does (pressing a button in the popup must not hide it again).
        bool shown = false;
    };

    struct Paths {
        std::string sources;            // root of the .scd tree
        std::string sclang;             // executable, "" if none
        std::string sclangBundle;       // folder with SCClassLibrary/ Extensions/, "" for a system install
        std::string work;               // scratch folder for builds, conf and HOME
    };

    struct BuildResult {
        bool ok = false;
        std::string file;               // compiled .scsyndef
        std::string error;
    };

    void update(ofEventArgs&);
    void startNextBuild();
    void finishBuild(BuildResult result);
    void finish(const std::string& defName, Result result);
    // Ready to be listed: already shown, or its last request is old enough
    // and the mouse is up (not mid-drag on N Chan).
    bool isSettled(Job& job) const;

    const Paths& getPaths();
    void buildSourceIndex();
    std::string unavailableReason(const Target& target);
    static BuildResult runBuild(Paths paths, Target target, std::string source);

    std::vector<serverManager*> servers;
    std::map<std::string, Job> jobs;    // by defName
    std::string building;               // defName of the job in compileTask, if any
    std::future<BuildResult> compileTask;

    bool pathsResolved = false;
    Paths paths;
    bool indexBuilt = false;
    std::map<std::string, std::string> sourceBySynth;
    std::set<std::string> noAutocompile;

    ofEventListener updateListener;
};

#endif /* scSynthdefCompiler_h */
