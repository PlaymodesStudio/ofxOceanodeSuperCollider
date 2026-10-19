#!/usr/bin/env python3
"""Exercise production bank loading/backup code without touching app data."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
code=(ROOT/'src/nodes/scSnapshots.h').read_text()
methods=code[code.index('    bool saveBank('):code.index('    // ── Named presets:')]
prelude=r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
using ofJson=nlohmann::json;
bool failWrites=false;
bool ofSavePrettyJson(const std::string& p,const ofJson& j){
    if(failWrites){std::ofstream f(p);f<<"{";return false;}std::ofstream f(p);f<<j.dump(2);return f.good();
}
ofJson ofLoadJson(const std::string& p){std::ifstream f(p);return ofJson::parse(f);}
std::string ofToString(int n){return std::to_string(n);}
std::string ofGetTimestampString(){return "fixed-time";}
class scSnapshots {public:
    static constexpr int SLOTS=128;
    std::array<ofJson,SLOTS> slots;
    std::array<bool,SLOTS> used{};
    int active=-1,saves=0;bool running=true;
    std::string message;std::filesystem::path root;
    std::filesystem::path dir()const{return root;}
    void saveLibrary(){saves++;}
'''
main=r'''
};
int main(int argc,char** argv){
    scSnapshots s;s.root=argv[1];std::filesystem::create_directories(s.root/"banks");
    auto moving=ofLoadJson(argv[2]);s.slots[16]=moving;s.used[16]=true;s.active=16;
    auto next=moving;next["name"]="New collection patch";
    auto source=s.root/"source.json";
    ofSavePrettyJson(source.string(),ofJson{{"snapshotBank",1},{"slots",ofJson::array({next})}});
    assert(s.loadBundledBank(source.string()) && s.saves==1 && !s.running);
    auto first=s.root/"banks/Before collection - fixed-time.json";
    auto backup=ofLoadJson(first.string());assert(backup["slots"][16]==moving && backup["active"]==16);
    assert(s.used[0] && s.slots[0]==next && !s.used[16]);
    failWrites=true;
    assert(!s.loadBundledBank(source.string()) && s.saves==1 && s.slots[0]==next);
    assert(ofLoadJson(first.string())==backup);
    failWrites=false;
    assert(s.loadBundledBank(source.string()) && s.saves==2);
    assert(ofLoadJson((s.root/"banks/Before collection - fixed-time-1.json").string())["slots"][0]==next);
    assert(ofLoadJson(first.string())==backup);
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb-bank-loading-')as temp:
    d=pathlib.Path(temp);src=d/'test.cpp';src.write_text(prelude+methods+main)
    exe=d/'test'
    subprocess.run(['clang++','-std=c++17','-O2','-I'+str(ROOT.parent.parent/'libs/json/include'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(d/'data'),str(ROOT/'tools/polycomb/fixtures/MOVING_SAWS.json')],check=True)
print('PASS: bundled bank selection backs up MOVING_SAWS and empty slots; failed backup leaves library unchanged; timestamp collisions preserve earlier backups.')
