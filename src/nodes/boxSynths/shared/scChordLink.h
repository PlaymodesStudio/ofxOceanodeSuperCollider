//
//  scChordLink.h
//  ofxOceanodeSuperCollider
//
//  ChordBox -> ArpeggiatorBox (or any node): a ChordBox registers each of its
//  OutputN vectors; a node whose input (NotePool, Scale, Root) is connected
//  to one of them finds it here and reads the chord at any beat directly
//  (the progression is deterministic), instead of waiting for the frame the
//  vector changes in. Its notes then follow the chords exactly on the beat,
//  including the ones it writes ahead.
//
//    ChordBox:  chordlink::registry()[&outputParameter] = provider;  (erase on removal)
//    reader:    if(auto* p = chordlink::providerFor(getOceanodeParameter(notePool))) ... p->notesAt(beat)
//
//  Through other nodes (ChordBox -> Vector Item Operations -> NotePool) the
//  reader finds the ChordBox with upstreamKey() and learns what the nodes in
//  between make of each chord (arpbox::ChainMap).
//

#pragma once

#include "ofxOceanodeParameter.h"
#include "ofxOceanodeConnection.h"
#include "ofxOceanodeNodeModel.h"
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace chordlink {

struct Provider {
    std::function<bool()> timed;                          // the chords follow the transport (else: Index)
    std::function<std::vector<float>(double)> notesAt;    // this output's notes at a beat
    std::function<float(double)> rootAt;                  // the root pitch class at a beat
    std::function<double(double)> chordStartAt;           // the beat the chord playing at a beat began
    std::function<uint64_t()> revision;                   // changes whenever the progression does
};

inline std::map<const ofAbstractParameter*, Provider>& registry(){
    static std::map<const ofAbstractParameter*, Provider> r;
    return r;
}

// The provider feeding an input (its connection's source), or null
inline const Provider* providerFor(ofxOceanodeAbstractParameter& input){
    if(!input.hasInConnection() || !input.getInConnection()) return nullptr;
    ofxOceanodeAbstractParameter& source = input.getInConnection()->getSourceParameter();
    for(auto& [param, provider] : registry())
        if(param && source.isReferenceTo(*param)) return &provider;
    return nullptr;
}

// The registered output an input is connected to, or null
inline const ofAbstractParameter* directKey(ofxOceanodeAbstractParameter& input){
    if(!input.hasInConnection() || !input.getInConnection()) return nullptr;
    ofxOceanodeAbstractParameter& source = input.getInConnection()->getSourceParameter();
    for(auto& [param, provider] : registry()) if(param && source.isReferenceTo(*param)) return param;
    return nullptr;
}

// One further up: ChordBox -> other nodes (vector operations...) -> input,
// crossing at most `depth` nodes. Null when none, or two different ones.
inline const ofAbstractParameter* upstreamKey(ofxOceanodeAbstractParameter& input, int depth = 4){
    if(const auto* k = directKey(input)) return k;
    if(depth <= 0 || !input.hasInConnection() || !input.getInConnection()) return nullptr;
    ofxOceanodeAbstractParameter& source = input.getInConnection()->getSourceParameter();
    ofxOceanodeNodeModel* node = source.getNodeModel();
    if(!node) return nullptr;
    const ofAbstractParameter* found = nullptr;
    for(auto& p : node->getParameterGroup()){
        auto* in = dynamic_cast<ofxOceanodeAbstractParameter*>(p.get());
        if(!in || in == &source || !in->hasInConnection()) continue;
        const auto* k = upstreamKey(*in, depth - 1);
        if(!k) continue;
        if(found && found != k) return nullptr;
        found = k;
    }
    return found;
}

} // namespace chordlink
