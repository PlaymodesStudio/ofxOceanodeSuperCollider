//
//  scSynthdefSync.h
//  ofxOceanodeSuperCollider
//
//  "Sync with SynthDefs Git" (SuperCollider menu): brings an app's
//  SuperCollider data up to date with this addon's repository, without git or
//  a GitHub account (the repository is public).
//
//    1. Downloads what changed of synthdefs/ (-> data/Supercollider/SynthdefSources),
//       sclang/ (-> Sclang, the bundled compiler) and our own UGens in ugens/
//       (-> Extensions/<Plugin>). Files are compared by their git blob hash.
//    2. Installs third-party UGen folders published as assets of the
//       repository's "sc-extensions" release (tools/publish_sc_extensions.py),
//       when the local folder's content hash differs.
//    3. Compiles with the bundled sclang (Oceanode/syncCompile.scd), only what
//       the app does not have: sources this sync downloaded, sources changed
//       since a sync compiled them, and synths whose SynthDefs are missing
//       (or everything, with "Compile everything again"):
//         - ~synthCreator synths: the metadata SynthDef plus the stereo build.
//           When a synth changed, its other installed builds are retired.
//           Other channel counts are compiled on demand (scSynthdefCompiler).
//         - Defaults/ scripts (SynthDefs of the C++ nodes): run whole.
//    4. Installs into data/Supercollider/Synthdefs. Defaults go to
//       Synthdefs/Defaults/<source folder>/, mirroring the sources; any other
//       copy of the same file is moved to data/Supercollider/_retired_synthdefs/
//       (nodes find their SynthDefs by file name, so the move is safe), as is,
//       once every Defaults source has been compiled by a sync, every file
//       under Synthdefs/Defaults that no Defaults source builds.
//    5. Loads the new SynthDefs into the running servers, or offers to restart
//       them when UGens changed (scsynth loads plugins only when it boots).
//
//  What was synced and compiled is kept in data/Supercollider/Config/SynthdefSync.json.
//  macOS only, like the compiler.
//

#ifndef scSynthdefSync_h
#define scSynthdefSync_h

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class serverManager;

class scSynthdefSync {
public:
    scSynthdefSync();
    ~scSynthdefSync();

    void setServers(const std::vector<serverManager*>& servers){ this->servers = servers; }
    // Re-reads the SynthDef descriptions (registered node types)
    void setReloadCallback(std::function<void()> callback){ reloadCallback = std::move(callback); }

    // Inside the SuperCollider menu
    void drawMenuItem();
    // Every frame, at the top level of the ImGui frame
    void drawPopup();

    // git's object id of a file: sha1("blob <size>\0<contents>"), "" if unreadable
    static std::string gitBlobSha(const std::string& path);
    // sha256 over the folder's files (relative path and sha256 of each, sorted);
    // tools/publish_sc_extensions.py computes the same
    static std::string folderHash(const std::string& folder);

    struct Report {
        std::string error;                      // the sync stopped
        int downloaded = 0, removed = 0;
        std::vector<std::string> extensions;    // UGen folders installed
        int compiledSynths = 0, compiledScripts = 0;
        int installed = 0, retired = 0;
        std::vector<std::pair<std::string, std::string>> failures;  // what, why
        std::vector<std::string> notes;
        std::vector<std::string> changedSynths; // ~synthCreator names rebuilt
        bool ugensChanged = false;
        bool synthdefsChanged = false;
        bool cancelled = false;
    };

    struct Progress {
        std::mutex mutex;
        std::string phase, item;
        int done = 0, total = 0;
        bool finished = false;
        Report report;
    };

private:
    enum class Stage { Closed, Confirm, Running, Done };

    void start();
    void finish();
    void restartServers();

    std::vector<serverManager*> servers;
    std::function<void()> reloadCallback;

    Stage stage = Stage::Closed;
    bool openRequested = false;
    std::string addonRoot;          // this checkout, "" when not on this machine
    char branch[128] = "";
    bool fromCheckout = false;
    bool rebuildAll = false;

    std::thread worker;
    std::shared_ptr<Progress> progress;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    Report report;                  // of the finished sync
};

#endif /* scSynthdefSync_h */
