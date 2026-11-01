#!/usr/bin/env python3
"""Exercise production source retirement on bootstrap and repeated syncs."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/synthdefs/scSynthdefSync.cpp').read_text()
a=s.index('int retireReplacedSources(');b=s.index('\n}',a)+2
cpp=r'''
#include <filesystem>
#include <fstream>
#include <cassert>
namespace fs=std::filesystem;
FUNCTION
void put(const fs::path& p,const char* text="original"){fs::create_directories(p.parent_path());std::ofstream(p)<<text;}
int main(int argc,char** argv){
 fs::path root=argv[1],src=root/"sources",defs=root/"defs",backup=root/"retired";
 auto old=src/"Effect/Time/Looping/BeatRepeatPro.scd";
 auto legacy=defs/"Effect/Time/Looping/BeatRepeatPro";
 auto replacement=src/"Defaults/BeatRepeatPro/BeatRepeatPro.scd";
 auto current=defs/"Defaults/BeatRepeatPro";
 put(old);put(legacy/"BeatRepeatPro1.scsyndef");put(legacy/"BeatRepeatPro.txarcmeta");
 // A user source stays if the replacement is not installed.
 assert(retireReplacedSources(src,defs,backup)==0 && fs::exists(old));
 put(replacement);put(current/"BeatRepeatPro1.scsyndef","new");put(current/"BeatRepeatPro2.scsyndef","new");
 put(src/"Effect/Time/Looping/MyRepeat.scd");
 assert(retireReplacedSources(src,defs,backup)==2);
 assert(!fs::exists(old) && !fs::exists(legacy));
 assert(fs::exists(backup/"SynthdefSources/Effect/Time/Looping/BeatRepeatPro.scd"));
 assert(fs::exists(backup/"Effect/Time/Looping/BeatRepeatPro/BeatRepeatPro.txarcmeta"));
 assert(fs::exists(src/"Effect/Time/Looping/MyRepeat.scd"));
 // No source ledger is needed; subsequent syncs leave Defaults builds intact.
 assert(retireReplacedSources(src,defs,backup)==0);
 assert(fs::file_size(current/"BeatRepeatPro1.scsyndef")==3 && fs::exists(current/"BeatRepeatPro2.scsyndef"));
 // Also clean stranded legacy builds when the old source was already removed.
 put(legacy/"BeatRepeatPro2.scsyndef");assert(retireReplacedSources(src,defs,backup)==1);
 assert(retireReplacedSources(src,defs,backup)==0 && fs::exists(replacement));
 assert(fs::exists(backup/"Effect/Time/Looping/BeatRepeatPro/BeatRepeatPro.txarcmeta"));
}
'''.replace('FUNCTION',s[a:b])
with tempfile.TemporaryDirectory(prefix='source-replacement-') as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'fixture')],check=True)
print('PASS: bootstrap source/build retirement, backups, repeat-sync stability and unrelated source preservation')
