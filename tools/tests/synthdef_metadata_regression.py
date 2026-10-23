#!/usr/bin/env python3
"""Run the production txarcmeta reader with minimal file/string OF adapters."""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT=Path(__file__).resolve().parents[2]
PRELUDE=r'''
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
struct ofBuffer { vector<string> lines; vector<string>& getLines(){return lines;} };
ofBuffer ofBufferFromFile(string file){ ofBuffer b; ifstream in(file); string line; while(getline(in,line)) b.lines.push_back(line); return b; }
void ofStringReplace(string& s,const string& from,const string& to){ size_t p=0; while((p=s.find(from,p))!=string::npos){ s.replace(p,from.size(),to); p+=to.size(); } }
int ofStringTimesInString(const string& s,const string& x){int n=0;size_t p=0;while((p=s.find(x,p))!=string::npos){++n;p+=x.size();}return n;}
vector<string> ofSplitString(const string& s,const string& delimiter){vector<string> out;size_t b=0,p;while((p=s.find(delimiter,b))!=string::npos){out.push_back(s.substr(b,p-b));b=p+delimiter.size();}out.push_back(s.substr(b));return out;}
int ofToInt(const string& s){return atoi(s.c_str());} string ofToString(int n){return to_string(n);}
int errors=0; ostream& ofLog(){++errors;return cerr;}
struct synthdefDesc{ string filepath,name,type,description,category;map<string,map<string,string>> params;vector<pair<string,int>> variables; };
struct scSynthdef { static synthdefDesc readAndCreateSynthdef(string); };
'''
CASES=r'''
int main(int argc,char** argv){
 assert(argc==3 || argc==4);
 auto channel=scSynthdef::readAndCreateSynthdef(argv[1]);
 assert(channel.name=="ChannelMapper" && channel.category=="Routing" && channel.type=="source");
 assert(channel.params.at("mapping").at("default")== (argc==4 ? argv[3] : "1"));
 assert(channel.params.at("mapping").at("units")=="avi");
 assert(channel.params.at("gaincomp").at("default")=="1");
 auto dbap=scSynthdef::readAndCreateSynthdef(argv[2]);
 assert(dbap.name=="DBAP" && dbap.category=="Spatial/Panning");
 assert(dbap.variables.size()==1 && dbap.variables[0]==make_pair(string("NumSpeakers"),32));
 assert(dbap.params.at("speaker_x").at("default")=="0.9");
 assert(dbap.params.at("speaker_y").at("default")=="0.5");
 assert(dbap.params.at("position_x").at("default")=="0.5");
 assert(dbap.params.at("rolloff").at("default")=="6");
 assert(dbap.params.at("speaker_x").at("units")=="vf");
 assert(errors==0);
 cout<<"METADATA_TEST_OK: ChannelMapper/DBAP arrays, defaults, units and variables\n";
}
'''
def main():
 source=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'src/scSynthdef.cpp'
 fixtures=Path(sys.argv[2]) if len(sys.argv)>2 else Path(__file__).with_name('fixtures')
 text=source.read_text(); body=text[text.index('synthdefDesc scSynthdef::readAndCreateSynthdef('):text.index('\nstring scSynthdef::getSynthdefFilename()')]
 with tempfile.TemporaryDirectory(prefix='sc-metadata-') as folder:
  folder=Path(folder); cpp=folder/'test.cpp'; cpp.write_text(PRELUDE+body+CASES)
  exe=folder/'test'
  subprocess.run(['clang++','-std=c++17','-g','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe),str(fixtures/'ChannelMapper.txarcmeta'),str(fixtures/'DBAP.txarcmeta')],check=True)
  multiple=folder/'ChannelMapper.txarcmeta'
  multiple.write_text((fixtures/'ChannelMapper.txarcmeta').read_text().replace('13, [ 1 ]','13, [ -1, 2, 3.5 ]'))
  subprocess.run([str(exe),str(multiple),str(fixtures/'DBAP.txarcmeta'),'-1,2,3.5'],check=True)
if __name__=='__main__':main()
