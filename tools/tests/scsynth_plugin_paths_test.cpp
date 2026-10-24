#include "synthdefs/scSCSynthPluginPaths.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace oceanodeScPlugins;
static void put(const fs::path& p, const std::string& text){ fs::create_directories(p.parent_path()); std::ofstream(p) << text; }
int main(int argc, char** argv){
    const auto root = fs::temp_directory_path() / ("sc-plugin-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto source = root / "source", plain = root / "plain";
    put(source/"VSTPlugin/plugins/VSTPlugin.scx", "scsynth");
    put(source/"VSTPlugin/plugins/VSTPlugin_supernova.scx", "supernova");
    put(source/"VSTPlugin/plugins/libsupport.dylib", "dependency");
    put(source/"Classes/Plugin.sc", "class");
    put(plain/"Ordinary.scx", "plain");
    const auto view = forScsynth(source);
    assert(view != source);
    assert(fs::is_symlink(view/"VSTPlugin/plugins/VSTPlugin.scx"));
    assert(fs::exists(view/"VSTPlugin/plugins/libsupport.dylib"));
    assert(fs::exists(view/"Classes/Plugin.sc"));
    assert(!fs::exists(view/"VSTPlugin/plugins/VSTPlugin_supernova.scx"));
    assert(fs::exists(source/"VSTPlugin/plugins/VSTPlugin_supernova.scx"));
    assert(forScsynth(plain) == plain);
    // A restart sees new plugins added by Sync, without altering existing views.
    put(source/"New.scx", "new");
    const auto next = forScsynth(source);
    assert(fs::exists(next/"New.scx"));
    assert(!fs::exists(view/"New.scx"));
    fs::create_directory_symlink(source, root/"linked");
    const auto linked = forScsynth(root);
    assert(fs::exists(linked/"linked/VSTPlugin/plugins/VSTPlugin.scx"));
    assert(!fs::exists(linked/"linked/VSTPlugin/plugins/VSTPlugin_supernova.scx"));
    fs::remove_all(root);
    if(argc==3){
        std::cout << forScsynth(std::string(argv[1])+":"+argv[2]) << std::endl;
        std::cin.get();
    }else std::cout << "PLUGIN_PATHS_TEST_OK\n";
}
