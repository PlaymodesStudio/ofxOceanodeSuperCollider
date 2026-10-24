#!/usr/bin/env python3
"""Check the real sync destination mapper against every bundled wavetable."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/synthdefs/scSynthdefSync.cpp').read_text()
start = source.index('    fs::path destinationOf(')
brace = source.index('{', start)
depth = 1
end = brace+1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
mapper = source[start:end]
files = list((ROOT/'data/Supercollider/Wavetables/VITAL_WT').rglob('*.wav'))
assert len(files) == 244
with tempfile.TemporaryDirectory(prefix='wavetable-sync-') as directory:
    tmp = Path(directory)
    cpp = tmp/'test.cpp'
    cpp.write_text(r'''
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
namespace fs=std::filesystem;
std::vector<std::string> ofSplitString(const std::string& text,const std::string&,bool){
 std::vector<std::string> v;size_t begin=0,end;
 while((end=text.find('/',begin))!=std::string::npos){v.push_back(text.substr(begin,end-begin));begin=end+1;}
 v.push_back(text.substr(begin));return v;
}
struct Mapper {
 fs::path data="/application/data/Supercollider",sources=data/"SynthdefSources",sclangDir=data/"Sclang",extensions=data/"Extensions";
 static bool isPresetPath(const std::string&){return false;}
''' + mapper + r'''
};
int main(int argc,char** argv){
 Mapper m;const fs::path addon=argv[1];int count=0;
 for(auto& item:fs::recursive_directory_iterator(addon/"data/Supercollider/Wavetables")){
  if(item.path().extension()!=".wav")continue;
  const auto relative=fs::relative(item.path(),addon).generic_string();
  assert(m.destinationOf(relative)==m.data/"Wavetables"/fs::relative(item.path(),addon/"data/Supercollider/Wavetables"));++count;
 }
 assert(count==244);
 assert(m.destinationOf("data/Supercollider/Wavetables/README.md").empty());
 assert(m.destinationOf("data/other.wav").empty());
 assert(m.destinationOf("synthdefs/Defaults/WavetableBox/wavetableBox.scd")==m.sources/"Defaults/WavetableBox/wavetableBox.scd");
}
''')
    subprocess.run(['clang++','-std=c++17',str(cpp),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'),str(ROOT)],check=True)
print('PASS: all 244 bundled tables map into the synthdef sync asset destination')
