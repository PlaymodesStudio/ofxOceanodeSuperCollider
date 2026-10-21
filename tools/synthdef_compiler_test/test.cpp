#include "ofMain.h"
#include <future>
#include <fstream>
#include <cassert>
// Inspect the actual compiler's queue without driving the confirmation UI.
#define private public
#include "scSynthdefCompiler.h"
#undef private
#include "serverManager.h"
namespace fs = std::filesystem;
static void write(const fs::path& path, const std::string& text){
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
int main(int argc, char** argv){
    assert(argc == 3);
    testData = argv[1];
    const fs::path addon = argv[2], data = fs::path(testData)/"Supercollider";
    auto& compiler = scSynthdefCompiler::get();
    scSynthdefCompiler::Target target;
    target.synthName = "SyncChannelTest";
    target.defName = "SyncChannelTest7_3";
    target.numChannels = 7;
    target.variables = {3};
    target.synthFolder = (data/"Synthdefs"/"MovedAfterSync").string();
    bool ready = false;
    compiler.request(&ready, target, [&](auto result){ ready = result == scSynthdefCompiler::Result::Ready; });
    assert(compiler.jobs.at(target.defName).state == scSynthdefCompiler::State::Unavailable);

    // Simulate files installed by Git sync after the request was made.
    fs::create_directories(data/"SynthdefSources");
    fs::copy(addon/"synthdefs"/"Oceanode", data/"SynthdefSources"/"Oceanode", fs::copy_options::recursive);
    fs::create_directories(data/"Sclang");
    fs::copy(addon/"sclang"/"Extensions", data/"Sclang"/"Extensions", fs::copy_options::recursive);
    fs::create_directory_symlink(addon/"sclang"/"SCClassLibrary", data/"Sclang"/"SCClassLibrary");
    fs::create_directories(data/"Sclang"/"osx");
    fs::create_symlink(addon/"sclang"/"osx"/"sclang", data/"Sclang"/"osx"/"sclang");
    // A bundled class copy deliberately fails. Synced classes must shadow it.
    write(data/"Sclang"/"Extensions"/"user"/"SyncTestPlugin"/"SyncTestHelper.sc",
          "SyncTestHelper { *value { ^MissingBundledClass.value } }\n");
    write(data/"Extensions"/"SyncTestPlugin"/"SyncTestHelper.sc", "SyncTestHelper { *value { ^0.1 } }\n");
    const fs::path source = data/"SynthdefSources"/"Defaults"/"Test"/"channels.scd";
    write(source, "~synthCreator.value(\"SyncChannelTest\", {|n, vars| Out.ar(0, DC.ar(SyncTestHelper.value * vars[0]) ! n) }, \"\", \"\", [\"Voices\"], [4]);\n");
    compiler.refreshPaths();
    assert(compiler.jobs.at(target.defName).state == scSynthdefCompiler::State::Asking);
    assert(compiler.sourceBySynth.at(target.synthName) == source.string());
    compiler.jobs.at(target.defName).state = scSynthdefCompiler::State::Queued;
    compiler.refreshPaths();
    assert(compiler.jobs.at(target.defName).state == scSynthdefCompiler::State::Queued);
    // Also cover invalidation after consent but before the queued build starts.
    compiler.invalidateSourceIndex();
    compiler.startNextBuild();
    auto result = compiler.compileTask.get();
    if(!result.ok){ std::cerr << result.error << '\n'; return 1; }
    const fs::path built = result.file;
    assert(fs::exists(built));
    assert(!fs::exists(built.parent_path()/"SyncChannelTest.scsyndef"));
    assert(!fs::exists(built.parent_path()/"SyncChannelTest2_3.scsyndef"));
    // Verify seven output channels in the generated SynthDef using SC itself.
    auto inspect = compiler.prepareScript("inspect", {});
    const fs::path script = data/"inspect.scd";
    write(script, "var desc = SynthDesc.read(\""+built.string()+"\")['SyncChannelTest7_3']; (if(desc.name.asSymbol == 'SyncChannelTest7_3' and: { desc.outputs[0].numberOfChannels == 7 }) { 0 } { 1 }).exit;\n");
    inspect.args = {script.string()};
    inspect.timeoutSeconds = 15;
    inspect.logPath = (data/"inspect.log").string();
    auto error = compiler.runScript(inspect);
    if(!error.empty()){ std::cerr << std::ifstream(inspect.logPath).rdbuf() << error << '\n'; return 1; }
    serverManager server;
    compiler.setServers({&server});
    compiler.finishBuild(result);
    const fs::path installed = fs::path(target.synthFolder)/(target.defName+".scsyndef");
    assert(fs::exists(installed));
    assert(server.loaded == installed.string());
    ofEventArgs event;
    compiler.update(event);
    assert(ready);
    assert(compiler.jobs.empty());
    std::cout << "\nPASS: refreshed synced sources, UGen class precedence, Defaults discovery, exact 7-channel variable build, install, server load and ready callback\n";
}
