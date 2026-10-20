#!/usr/bin/env python3
"""New bank + Store workflow, persistence, backup and failed-write safety."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scSnapshots.h').read_text()
store=s[s.index('    std::string name(int k) const'):s.index('    void recall(int slot)')]
methods=s[s.index('    void saveLibrary()'):s.index('    // ── Named presets:')]
pre=r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <functional>
using ofJson=nlohmann::json;
std::string failPrefix;
bool ofSavePrettyJson(const std::string&p,const ofJson&j){
 std::ofstream f(p);if(!failPrefix.empty() && std::filesystem::path(p).filename().string().rfind(failPrefix,0)==0){f<<"{";return false;}
 f<<j.dump(2);return f.good();
}
bool ofSaveJson(const std::string&p,const ofJson&j){return ofSavePrettyJson(p,j);}
ofJson ofLoadJson(const std::string&p){std::ifstream f(p);return ofJson::parse(f);}
std::string ofToString(int n){return std::to_string(n);}
std::string ofGetTimestampString(){return "fixed-time";}
std::string ofTrim(std::string s){auto a=s.find_first_not_of(" \t\r\n"),b=s.find_last_not_of(" \t\r\n");return a==s.npos?"":s.substr(a,b-a+1);}
std::string ofToLower(std::string s){for(char&c:s)c=std::tolower((unsigned char)c);return s;}
template<class T>struct Param{T value{};void setWithoutEventNotifications(T v){value=v;}};
struct scSnapshots{
 static constexpr int SLOTS=128;std::array<ofJson,SLOTS>slots;std::array<bool,SLOTS>used{};
 int active=-1,target=-1;bool running=false;std::string message,currentBank,storeBank,storeError;ofJson storeDestination;
 std::filesystem::path root;std::filesystem::file_time_type libTime{};
 Param<int>morphAP,morphBP;Param<float>morphP;
 struct Host{std::function<ofJson()>capture;std::function<std::vector<ofJson>()>factory;std::function<bool()>importLegacy;}host;
 std::filesystem::path dir()const{return root;}std::filesystem::file_time_type setTime(){return {};}
 void setActive(int n){active=n;}
'''
main=r'''
};
int main(int argc,char**argv){
 scSnapshots s;s.root=argv[1];std::filesystem::create_directories(s.root/"banks");
 auto old=ofLoadJson(argv[2]);s.slots[16]=old;s.used[16]=true;s.active=16;
 ofJson current={{"controls",{{"Pitch",67}}}};s.host.capture=[&](){return current;};s.running=true;
 assert(s.newBank("  My sounds  "));
 assert(s.currentBank=="My sounds.json" && s.active==0 && !s.running && s.used[0] && !s.used[16]);
 auto backup=s.root/"banks/Before new bank - fixed-time.json";
 auto preserved=ofLoadJson(backup.string());assert(preserved["slots"][16]==old && preserved["active"]==16);
 auto bank=s.root/"banks/My sounds.json";
 assert(ofLoadJson(bank.string())["slots"][0]["controls"]==current["controls"]);
 assert(ofLoadJson((s.root/"library.json").string())["bank"]=="My sounds.json");
 // Store into a named bank updates that file, not just the transient library.
 current["controls"]["Pitch"]=72;assert(s.storeNew("Second sound")==1);
 auto saved=ofLoadJson(bank.string());assert(saved["slots"].size()==2 && saved["slots"][1]["name"]=="Second sound");
 assert(ofLoadJson(backup.string())==preserved);
 // A second node / relaunch restores the named save target from the shared marker.
 scSnapshots t;t.root=s.root;t.host.capture=s.host.capture;t.loadLibrary();
 assert(t.currentBank=="My sounds.json" && t.used[1]);
 current["controls"]["Pitch"]=74;assert(t.storeNew("Third sound")==2);
 assert(ofLoadJson(bank.string())["slots"].size()==3);
 // No existing bank is overwritten by New bank; invalid names leave the working set intact.
 const auto before=t.slots;const auto used=t.used;
 for(const std::string n:{"My sounds","Factory","Factory.json","../outside","a/b","a\\b",".",""})assert(!t.newBank(n));
 assert(t.slots==before && t.used==used && ofLoadJson(bank.string())["slots"].size()==3);
 failPrefix="Blocked";assert(!t.newBank("Blocked"));assert(!std::filesystem::exists(t.root/"banks/Blocked.json"));
 failPrefix="Before new bank -";assert(!t.newBank("No backup"));assert(!std::filesystem::exists(t.root/"banks/No backup.json"));
 assert(t.slots==before && t.used==used && t.currentBank=="My sounds.json" && ofLoadJson(backup.string())==preserved);
 failPrefix.clear();assert(t.newBank("Fresh"));assert(ofLoadJson(t.root/"banks/Before new bank - fixed-time-1.json")["slots"].size()==3);
 // Direct Store into another bank keeps the current sound, collection and source file intact.
 const auto sourceSlots=t.slots;const auto sourceUsed=t.used;const int sourceActive=t.active;
 const auto sourceFile=ofLoadJson((t.root/"banks/Fresh.json").string());
 auto dest=ofLoadJson(bank.string());dest["customMetadata"]="keep me";ofSavePrettyJson(bank.string(),dest);
 current["controls"]["Pitch"]=81;
 assert(t.storeInBank("My sounds.json","Cross bank")==3);
 assert(t.currentBank=="Fresh.json" && t.active==sourceActive && t.slots==sourceSlots && t.used==sourceUsed);
 assert(ofLoadJson((t.root/"banks/Fresh.json").string())==sourceFile);
 auto cross=ofLoadJson(bank.string());assert(cross["customMetadata"]=="keep me");
 assert(cross["slots"][3]["controls"]["Pitch"]==81 && cross["slots"][3]["name"]=="Cross bank");
 for(int k=0;k<3;++k)assert(cross["slots"][k]==dest["slots"][k]);
 current["controls"]["Pitch"]=82;
 assert(t.storeInBank("My sounds.json","Cross bank",-1,true)==3); // explicit save-by-name
 assert(ofLoadJson(bank.string())["slots"].size()==4);
 assert(t.storeInBank("My sounds.json","Replacement",0)==0);
 cross=ofLoadJson(bank.string());assert(cross["slots"][0]["name"]=="Replacement");
 cross["slots"][1]=nullptr;ofSavePrettyJson(bank.string(),cross);
 assert(t.storeInBank("My sounds.json","Fill gap")==1);
 const auto beforeFailure=ofLoadJson(bank.string());failPrefix="My sounds.json.store";
 assert(t.storeInBank("My sounds.json","Failed")<0 && ofLoadJson(bank.string())==beforeFailure);
 failPrefix.clear();assert(t.storeInBank("Factory.json","No")<0);assert(t.storeInBank("../outside.json","No")<0);
 assert(t.slots==sourceSlots && t.used==sourceUsed && t.currentBank=="Fresh.json");
 ofJson full{{"slots",ofJson::array()}};for(int k=0;k<128;++k)full["slots"].push_back(ofJson{{"name","Full "+ofToString(k)}});
 ofSavePrettyJson((t.root/"banks/Full.json").string(),full);
 assert(t.storeInBank("Full.json","Too many")<0 && ofLoadJson((t.root/"banks/Full.json").string())==full);
 assert(t.storeInBank("Full.json","Explicit replace",17)==17);
 assert(t.storeInBank("Fresh.json","Current collection")==1 && t.used[1]);
 // Factory collection cannot become an autosave target.
 auto factory=t.root/"banks/Factory.json";ofSavePrettyJson(factory.string(),ofJson{{"slots",ofJson::array({old})}});
 assert(t.loadBank(factory.string()) && t.currentBank.empty());t.storeNew("Independent");
 assert(ofLoadJson(factory.string())["slots"].size()==1);
}
'''
with tempfile.TemporaryDirectory(prefix='snapshot-new-bank-')as d:
 out=pathlib.Path(d);(out/'test.cpp').write_text(pre+store+methods+main)
 subprocess.run(['clang++','-std=c++17','-I'+str(ROOT.parent.parent/'libs/json/include'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
 subprocess.run([str(out/'test'),str(out/'data'),str(ROOT/'tools/polycomb/fixtures/MOVING_SAWS.json')],check=True)
print('PASS: new banks and direct cross-bank Store, source/playback preservation, saved metadata, gap reuse, explicit replacement, full-bank rejection, atomic write failure safety, relaunch persistence and Factory protection.')
