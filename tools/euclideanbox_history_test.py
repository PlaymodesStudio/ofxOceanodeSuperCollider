#!/usr/bin/env python3
"""Compare production Note-output caching with the original sliding window.

Standalone ASan/UBSan test: no window or SuperCollider server required.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
header = Path(os.environ.get('EUCLIDEANBOX_HEADER', ROOT / 'src/nodes/boxSynths/euclideanBox/scEuclideanBox.h')).read_text()

def method(signature):
    start = header.index(signature)
    opening = header.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (header[end] == '{') - (header[end] == '}')
        end += 1
    return header[start:end]

cache_start = header.index('    struct NoteHistoryChunk')
cache_end = header.index('noteHistory;', cache_start) + len('noteHistory;')
source = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <climits>
#include <iostream>
#include <map>
#include <vector>
namespace metrobox {
int64_t floorDiv(int64_t a,int64_t b) { auto q=a/b; return q-(a%b<0); }
int64_t positiveMod64(int64_t a,int64_t b) { return (a%b+b)%b; }
int64_t tickAt(double beat) { return int64_t(beat); }
}
template<class T> struct Param {
 T value{}; const T& get() const { return value; }
 Param& operator=(const T& v) { value=v; return *this; }
};
struct Fixture {
 static constexpr int MAX_TRACKS=8, CHUNK=32;
 static constexpr int64_t NO_CHUNK=INT64_MIN;
 struct Onset { int64_t tick; float amp; int dur; bool accent,isLong; float pitch; int voice; };
''' + header[cache_start:cache_end] + r'''
 struct State { struct { double beatPosition=0; } current; } transport;
 const State& getFrameTransportState() const { return transport; }
 int voiceCount=3, revision=0;
 bool silent=false,broadcast=false;
 Param<int> numTracks{3};
 std::array<Param<std::vector<float>>,MAX_TRACKS> notes;
 struct Server { std::array<std::array<int64_t,4>,MAX_TRACKS> slotChunk; };
 std::map<int,Server> servers;
 mutable std::array<int,MAX_TRACKS> calls{};
 int voices() const { return voiceCount; }
 std::vector<Onset> onsets(int ti,int64_t from,int64_t to) const {
   ++calls[ti]; std::vector<Onset> out;
   if(silent) return out;
   for(int64_t tick=from; tick<to; ++tick) {
     if(metrobox::positiveMod64(tick+ti,7+ti)) continue;
     out.push_back({tick,1,1,false,false,float(48+revision+metrobox::positiveMod64(tick,24)),
                    broadcast ? -1 : int(metrobox::positiveMod64(tick,voiceCount))});
   }
   return out;
 }
''' + method('    void updateNotes()') + '\n' + method('    void invalidate(int ti)') + r'''
 void baseline() {
   const int64_t now=metrobox::tickAt(getFrameTransportState().current.beatPosition);
   const int V=voices();
   for(int ti=0;ti<numTracks.get();++ti) {
     std::vector<float> v(V,notes[ti].get().empty()?48.0f:notes[ti].get()[0]);
     const auto& cur=notes[ti].get();
     for(int k=0;k<V && k<int(cur.size());++k) v[k]=cur[k];
     bool any=false;
     for(const auto& o:onsets(ti,now-2*CHUNK,now+1)) {
       if(o.voice<0) std::fill(v.begin(),v.end(),o.pitch); else v[o.voice]=o.pitch;
       any=true;
     }
     if(any && v!=cur) notes[ti]=v;
   }
 }
};
int main() {
 Fixture cached,original;
 auto compare=[&](int64_t tick) {
   cached.transport.current.beatPosition=original.transport.current.beatPosition=double(tick);
   cached.updateNotes(); original.baseline();
   for(int ti=0;ti<cached.numTracks.get();++ti) assert(cached.notes[ti].get()==original.notes[ti].get());
 };
 for(int64_t t=-100;t<1000;++t) { compare(t); compare(t); }
 auto calls=cached.calls;
 for(int i=0;i<1000;++i) compare(999);
 assert(cached.calls==calls); // idle/stopped frames reuse generated events
 for(int64_t t:{-500LL,0LL,32LL,31LL,100000LL,64LL,-1LL}) compare(t);
 for(int v:{1,16,2,8}) {
   cached.voiceCount=original.voiceCount=v;
   cached.revision=original.revision=v;
   cached.invalidate(-1); compare(64); compare(65);
 }
 // A track edit refreshes its cached events without refreshing other tracks.
 calls=cached.calls; ++cached.revision; ++original.revision;
 cached.invalidate(0);
 // Apply the revision to other tracks too, just as a global edit would.
 for(int i=1;i<cached.numTracks.get();++i) cached.invalidate(i);
 compare(65);
 for(int i=0;i<cached.numTracks.get();++i) assert(cached.calls[i]==calls[i]+3);
 calls=cached.calls; cached.invalidate(0); compare(65);
 assert(cached.calls[0]==calls[0]+3 && cached.calls[1]==calls[1]);
 for(bool broadcast:{true,false}) {
   cached.broadcast=original.broadcast=broadcast; cached.invalidate(-1);
   for(int t=0;t<200;++t) compare(t);
 }
 cached.silent=original.silent=true; cached.invalidate(-1);
 compare(200); compare(500); // no onsets retains the existing pitch vectors
 cached.silent=original.silent=false; cached.numTracks=original.numTracks=8;
 cached.invalidate(-1); compare(501);
 std::cout << "PASS: cached Note outputs match original sliding windows across seeks, edits, voice changes and silence; idle event generation eliminated\n";
}
'''
with tempfile.TemporaryDirectory(prefix='euclidean-history-') as directory:
    path=Path(directory,'test.cpp');path.write_text(source)
    exe=Path(directory,'test')
    subprocess.run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
