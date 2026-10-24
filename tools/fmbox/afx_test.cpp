#include "scDX7Afx.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <filesystem>

int main(int argc,char**argv) {
    assert(argc==2);
    const auto patches=scDX7::load(argv[1]);assert(patches.size()==16384);
    auto catalogue=std::filesystem::path(argv[1]);catalogue.replace_extension(".names.tsv");
    const auto names=scDX7::loadNames(catalogue.string(),patches);
    int named=0,blank=0;
    for(const auto& name:names){assert(!name.bank.empty());if(name.name.empty())++blank;else ++named;}
    assert(named==16251&&blank==133);
    assert(names[0].name=="ARP 2600"&&names[0].bank=="1ST_DEMO"&&names[0].slot=="01");
    assert(names[16383].name=="ORCHESTRA");
    const auto banks=scDX7::bankGroups(patches.size(),names);
    assert(banks.size()==512&&banks.front().name=="1ST_DEMO"&&banks.back().name=="ZONE3");
    for(const auto& bank:banks)assert(bank.presets.size()==32);
    assert(banks[0].presets.front()==0&&banks[0].presets.back()==31&&banks[1].presets.front()==32);
    assert(scDX7::bankIndex(31,banks)==0&&scDX7::bankIndex(32,banks)==1&&scDX7::bankIndex(16383,banks)==511);
    const auto unknown=scDX7::bankGroups(33,{});
    assert(unknown.size()==2&&unknown[0].presets.size()==32&&unknown[1].presets.size()==1);
    assert(scDX7::presetLabel(0,names)=="DX7 1 / ARP 2600");
    assert(scDX7::presetLabel(353,names,true)=="DX7 354 / Unnamed / BANGERS 02");
    assert(scDX7::searchText(scDX7::presetLabel(0,names,true)).find("arp 2600")!=std::string::npos);
    assert(scDX7::searchText(scDX7::presetLabel(0,names,true)).find("1st_demo")!=std::string::npos);
    auto changed=patches;std::swap(changed[0],changed[1]);
    const auto reordered=scDX7::loadNames(catalogue.string(),changed);
    assert(reordered[0].bank.empty()&&reordered[1].bank.empty()&&!reordered[2].bank.empty());
    assert(scDX7::presetLabel(0,reordered)=="DX7 1");
    assert(scDX7::presetLabel(0,scDX7::loadNames("/missing/catalogue",patches))=="DX7 1");
    std::array<bool,32> algorithms{};
    for(const auto& p:patches)algorithms[p[0]]=true;
    for(bool used:algorithms)assert(used);
    auto p=scDX7::initialPatch();std::string line;
    for(int v:p){line+=char('0'+v/10);line+=char('0'+v%10);}
    assert(scDX7::parseLine(line)==p);assert(scDX7::parseLine(line+"\r")==p);
    for(const auto& invalid: {std::string(),line.substr(1),line+"0",std::string("x")+line.substr(1),std::string("99")+line.substr(2)}) {
        bool failed=false;try{scDX7::parseLine(invalid);}catch(const std::runtime_error&){failed=true;}assert(failed);
    }
    bool failed=false;try{scDX7::load("/this/bank/does/not/exist.afx");}catch(const std::runtime_error&){failed=true;}assert(failed);
    // Every algorithm's feedback cell must be an actual connection. Includes
    // off-diagonal feedback loops in algorithms 4 and 6.
    for(int a=0;a<32;++a)assert(scDX7::algorithms[a][scDX7::feedbackCell[a]]==1);
    assert(scDX7::feedbackCell[3]==33&&scDX7::feedbackCell[5]==34);
    std::cout<<"PASS: all 16384 .afx presets and catalogue entries, blank names, search, fingerprint mismatch/missing catalogue fallbacks, strict parsing and 32 algorithms\n";
}
