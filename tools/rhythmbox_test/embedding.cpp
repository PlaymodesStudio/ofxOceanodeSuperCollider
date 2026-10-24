#include "samples/scSampleEmbedding.h"
#include <cassert>
#include <iostream>

std::string presetPath;
static void write(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
static std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    assert(argc == 2);
    const fs::path work(argv[1]);
    presetPath = (work / "Presets/Original").string();
    const auto a = work / "kitA/kick.wav", b = work / "kitB/kick.wav";
    write(a, "first kick"); write(b, "second kick");
    const auto embeddedA = scSampleEmbedding::embed(a.string());
    const auto embeddedB = scSampleEmbedding::embed(b.string());
    assert(embeddedA != embeddedB);
    assert(read(scSampleEmbedding::resolve(embeddedA)) == "first kick");
    assert(read(scSampleEmbedding::resolve(embeddedB)) == "second kick");
    assert(scSampleEmbedding::embed(a.string()) == embeddedA);
    assert(scSampleEmbedding::embed(embeddedA) == embeddedA);
    // A renamed/copied preset must read its own samples even if the original exists.
    const auto copied = work / "Presets/Copied";
    fs::copy(presetPath, copied, fs::copy_options::recursive);
    presetPath = copied.string();
    write(work / "Presets/Original" / embeddedA, "changed original");
    fs::remove_all(work / "kitA"); fs::remove_all(work / "kitB");
    assert(read(scSampleEmbedding::resolve(embeddedA)) == "first kick");
    assert(scSampleEmbedding::embed(embeddedA) == embeddedA);
    assert(scSampleEmbedding::embed("/missing/sample.wav") == "/missing/sample.wav");
    assert(scSampleEmbedding::embed("").empty());
    assert(scSampleEmbedding::embed(a.string(), "") == a.string());
    assert(scSampleEmbedding::resolve(a.string()) == a.string());
    // Legacy RhythmBox paths rebase onto the loaded preset too.
    write(copied / "samples/legacy.wav", "legacy kick");
    assert(read(scSampleEmbedding::resolve("Presets/Old/samples/legacy.wav")) == "legacy kick");
    const auto binary = work / "data/file.bin";
    write(binary, std::string("\0\1\2\xff", 4));
    const auto data = scSampleEmbedding::embed(binary.string());
    assert(fs::path(data).extension() == ".bin");
    assert(read(scSampleEmbedding::resolve(data)) == std::string("\0\1\2\xff", 4));
    std::cout << "PASS: basename collisions, deduplication, repeated saves, moved presets, missing files, legacy paths, binary data\n";
}
