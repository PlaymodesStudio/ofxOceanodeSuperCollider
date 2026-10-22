#include "scClassLibraryConfig.h"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace oceanodeClassLibrary;
static void put(const fs::path& path, const std::string& text){
    fs::create_directories(path.parent_path()); std::ofstream(path) << text;
}
static std::string read(const fs::path& path){
    std::ifstream stream(path); return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char** argv){
    assert(declaredClasses(R"SC(
        /* Outer { Fake : UGen { /* nested */ } } */
        Disperser : UGen { *ar { "FakeClass : UGen { }"; Routine { } } }
        MorphSVF : UGen { }
        + Disperser { extraMethod { } }
        PlainObject { var character = $}; }
    )SC") == (std::set<std::string>{"Disperser", "MorphSVF", "PlainObject"}));
    const auto dir = fs::temp_directory_path() / ("oceanode-class-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto installed = dir / "installed", bundled = dir / "bundle";
    put(installed / "GrainUtils/Filters.sc", "Disperser : UGen {} MorphSVF : UGen {}\n");
    put(installed / "GenerateAndTest/Disperser/Disperser.sc", "Disperser : UGen {}\n");
    put(bundled / "Extensions/user/GrainUtils/Filters.sc", "Disperser : UGen {} MorphSVF : UGen {}\n");
    put(bundled / "Extensions/user/Standalone/Unique.sc", "Unique : UGen {}\n");
    fs::create_directories(bundled / "SCClassLibrary");
    write(dir / "config.yaml", bundled, {}, installed,
        {(installed / "GenerateAndTest/Disperser").string()});
    const auto config = read(dir / "config.yaml");
    const auto split = config.find("excludePaths:");
    const auto includes = config.substr(0, split), excludes = config.substr(split);
    assert(includes.find((installed / "GenerateAndTest/Disperser").string()) == std::string::npos);
    assert(excludes.find((installed / "GenerateAndTest/Disperser/Disperser.sc").string()) != std::string::npos);
    assert(excludes.find((bundled / "Extensions/user/GrainUtils/Filters.sc").string()) != std::string::npos);
    assert(excludes.find((installed / "GrainUtils/Filters.sc").string()) == std::string::npos);
    assert(excludes.find("Unique.sc") == std::string::npos);
    fs::remove_all(dir);
    if(argc == 3){
        const fs::path data = argv[1];
        write(argv[2], data / "Sclang", data / "SynthdefSources", data / "Extensions");
    }
    std::cout << "CLASS_CONFIG_TEST_OK\n";
}
