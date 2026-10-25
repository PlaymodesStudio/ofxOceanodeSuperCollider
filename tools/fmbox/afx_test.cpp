#include "../../src/nodes/boxSynths/fmBox/scDX7Afx.h"
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
    assert(named==16274&&blank==110);
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
    assert(scDX7::presetLabel(353,names,true)=="DX7 354 / CRASH CY / BANGERS 02");
    assert(scDX7::presetLabel(1496,names)=="DX7 1497 / Unnamed");
    // Verified ROM1A resides at 12544, not the historical PDF's 12800.
    const std::array<const char*,32> rom1a = {{
        "BRASS 1","BRASS 2","BRASS 3","STRINGS 1","STRINGS 2","STRINGS 3",
        "ORCHESTRA","PIANO 1","PIANO 2","PIANO 3","E.PIANO 1","GUITAR 1",
        "GUITAR 2","SYN-LEAD 1","BASS 1","BASS 2","E.ORGAN 1","PIPES 1",
        "HARPSICH 1","CLAV 1","VIBE 1","MARIMBA","KOTO","FLUTE 1",
        "ORCH-CHIME","TUB BELLS","STEEL DRUM","TIMPANI","REFS WHISL",
        "VOICE 1","TRAIN","TAKE OFF"
    }};
    for(int slot=0;slot<32;++slot) {
        assert(names[12544+slot].bank=="ROM1A"&&names[12544+slot].name==rom1a[slot]);
        assert(names[12800+slot].bank!="ROM1A");
    }
    assert(patches[12544][0]==21&&patches[12544][2]==24);
    assert(patches[12554][0]==4&&patches[12554][2]==24);
    assert(patches[12572]!=patches[12573]&&patches[12573]!=patches[12574]&&patches[12574]!=patches[12575]);
    int unverified=0;
    for(size_t i=0;i<names.size();++i)if(names[i].name.rfind("Unverified ",0)==0) {
        assert(i==2272||i==2304||i==2305);++unverified;
    }
    assert(unverified==3);
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
