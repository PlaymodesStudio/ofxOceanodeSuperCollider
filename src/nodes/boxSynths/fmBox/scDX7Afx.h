#pragma once

#include "scDX7Data.h"
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <map>
#include <vector>

namespace scDX7 {
using Patch = std::array<int,145>;
inline Patch initialPatch() {
    Patch p{};
    for(size_t i=0;i<globalFields.size();++i) p[i]=globalFields[i].initial;
    for(int op=0;op<6;++op) for(size_t k=0;k<operatorFields.size();++k) p[19+op*21+k]=operatorFields[k].initial;
    return p;
}
inline Patch parseLine(std::string line, size_t number=1) {
    if(!line.empty()&&line.back()=='\r') line.pop_back();
    if(line.size()!=290) throw std::runtime_error("Line "+std::to_string(number)+": expected 290 decimal digits (145 fields)");
    Patch p{};
    for(size_t k=0;k<p.size();++k) {
        const unsigned char a=line[k*2],b=line[k*2+1];
        if(a<'0'||a>'9'||b<'0'||b>'9') throw std::runtime_error("Line "+std::to_string(number)+": non-decimal data");
        p[k]=(a-'0')*10+b-'0';
        const auto& field=k<19 ? globalFields[k] : operatorFields[(k-19)%21];
        if(p[k]>field.max) throw std::runtime_error("Line "+std::to_string(number)+": "+field.name+" is out of range");
    }
    return p;
}
inline std::vector<Patch> load(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("Cannot open .afx bank");
    std::vector<Patch> patches;std::string line;size_t number=0;
    while(std::getline(file,line)) {
        ++number;if(line.empty()||line=="\r") continue;
        if(patches.size()>=65536) throw std::runtime_error("Bank exceeds 65536 presets");
        patches.push_back(parseLine(line,number));
    }
    if(file.bad()) throw std::runtime_error("Could not finish reading .afx bank");
    if(patches.empty()) throw std::runtime_error("The .afx bank contains no presets");
    return patches;
}
struct PresetName {
    std::string bank, slot, name;
};
inline uint64_t fingerprint(const Patch& patch) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(int value:patch) hash=(hash^static_cast<uint64_t>(value))*UINT64_C(1099511628211);
    return hash;
}
// The fingerprints prevent a different/reordered bank from inheriting these
// names. The historical correspondence of the upstream PDF remains unverified.
inline std::vector<PresetName> loadNames(const std::string& path,const std::vector<Patch>& patches) {
    std::vector<PresetName> names(patches.size());
    std::ifstream file(path);std::string line;
    while(std::getline(file,line)) {
        if(line.empty()||line[0]=='#')continue;
        std::istringstream row(line);std::array<std::string,5> fields;
        for(int k=0;k<4;++k)std::getline(row,fields[k],'\t');
        std::getline(row,fields[4]);
        if(!fields[4].empty()&&fields[4].back()=='\r')fields[4].pop_back();
        try {
            size_t used=0;const auto index=std::stoull(fields[0],&used);
            if(used!=fields[0].size()||index>=patches.size())continue;
            const auto hash=std::stoull(fields[1],&used,16);
            if(used!=fields[1].size()||hash!=fingerprint(patches[index])||fields[2].empty())continue;
            names[index]={fields[2],fields[3],fields[4]};
        }catch(const std::exception&){} // An absent/bad catalogue never prevents playing a bank.
    }
    return names;
}
inline std::string presetLabel(int index,const std::vector<PresetName>& names,bool bank=false) {
    std::string label="DX7 "+std::to_string(index+1);
    if(index>=0&&size_t(index)<names.size()) {
        const auto& n=names[index];
        if(!n.bank.empty()) {
            label+=" / "+(n.name.empty()?std::string("Unnamed"):n.name);
            if(bank)label+=" / "+n.bank+" "+n.slot;
        }
    }
    return label;
}
inline std::string searchText(std::string text) {
    for(auto& c:text)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
struct PresetBank {
    std::string name;
    std::vector<int> presets;
};
inline std::vector<PresetBank> bankGroups(size_t count,const std::vector<PresetName>& names) {
    std::vector<PresetBank> banks;std::map<std::string,size_t> indices;
    for(size_t i=0;i<count;++i) {
        const auto name=i<names.size()&&!names[i].bank.empty()?names[i].bank:"Bank "+std::to_string(i/32+1)+" (uncatalogued)";
        auto [it,added]=indices.emplace(name,banks.size());
        if(added)banks.push_back({name,{}});
        banks[it->second].presets.push_back(int(i));
    }
    return banks;
}
inline int bankIndex(int preset,const std::vector<PresetBank>& banks) {
    for(size_t b=0;b<banks.size();++b)
        for(int index:banks[b].presets)if(index==preset)return int(b);
    return 0;
}
}
