//
//  scGlobalPitch.h
//  ofxOceanodeSuperCollider
//
//  Global Pitch for SuperCollider: ofxOceanode's registry
//  (ofxOceanodeGlobalPitch.h, the Global Pitch node's scale, transpose and
//  detune and its destinations) plus the control buses the pitched Boxes read.
//
//  Boxes read them from a block of 31 control buses per server (allocated by
//  serverManager): [transpose, scale on, the offset of each pitch class 0-11 to
//  its nearest scale degree, detune amount in cents, transpose wraps (modulo
//  12), the wrap's base pitch class, the split note (-1000 when off), the low
//  scale's 12 offsets, low scale on]. Scale on and low scale on are the two
//  bypasses: the scale at and above the split note, the low scale below it.
//  Negative harmony is folded into both offset tables (harmonyOffsets), and
//  turns its side on even when that side's scale is bypassed.
//  There is one block for each combination of the features a destination
//  follows, so a destination sets one control and the synth that makes its
//  sounding pitch quantizes and transposes the input note (gq), then adds its
//  voices' detune (gt):
//
//    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch
//    var gt = gg[14] * 0.01 * ({ Rand(-1, 1) } ! voices);               // one random per voice, drawn once
//    var gq = { |p| var r = p.round, on = ((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1]);   // the note's side of the split
//        var q = Select.kr(on > 0.5, [p, r + Select.kr(r.mod(12) + ((r < gg[17]) * 12), gg[2..13] ++ gg[18..29])]);
//        var w = gg[16] + (((q - gg[16]) / 12).floor * 12);              // the octave from the base holding the note
//        q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };    // transposed, wrapped into it when modulo 12
//    ... note = gq.(input pitch) ... + gt
//
//  so Oceanode values, Pitch cables and piano-roll notes all follow, and held
//  notes move with them.
//
//  Glide: when the server runs GlobalPitchGlide (Defaults/Core/globalPitchGlide.scd,
//  started by serverManager at the head of the root group), the values are
//  written into a second, hidden block instead, and that synth copies them into
//  the block the synths read, gliding the transpose and the scale offsets over
//  Glide seconds; a held note slides to its new pitch. Without it they are
//  written straight into the block the synths read (no glide).
//
//    setup():      scGlobalPitch::follow(this, true, [this]{ resend gtrbus to the live synths });
//    destructor:   scGlobalPitch::unfollow(this);
//    each synth:   s->set("gtrbus", scGlobalPitch::bus(this, server));
//
//  A generic SynthDef (scSynthdef) and FM7 cannot read the buses without being
//  compiled again, so they send applyInRange(value, channel, min, max) for their
//  note controls (isPitchControl; one value per channel while spread() is on)
//  and resend them when a feature changes (valueChanged): Oceanode values and
//  the timeline follow, an audio cable into the pitch does not.
//

#pragma once

#include "ofxOceanodeGlobalPitch.h"
#include "ofxSCServer.h"
#include <map>
#include <string>

class scGlobalPitch : public ofxOceanodeGlobalPitch {
public:
    static constexpr int BlockSize = 31;   // [transpose, scale on, 12 pitch-class offsets, detune cents, wrap on, wrap base,
                                           //  split note, 12 low-scale offsets, low scale on]
    static constexpr int NumBlocks = 8;    // by what a destination follows: bit 0 Transpose, bit 1 Scale, bit 2 Detune

    // serverManager: the first of the server's NumBlocks * BlockSize buses the
    // synths read (-1 when it has none); with a glide, the first of the hidden
    // block the values are written to and the GlobalPitchGlide node copying it
    static void setServerBus(ofxSCServer* server, int index, int glideSource = -1, int glideNode = -1) {
        static const bool listening = [](){ onValuesChanged([](){ for(auto& [s, b] : buses()) sendValues(s, b); }); return true; }();
        (void)listening;
        if(index < 0) { buses().erase(server); return; }
        Buses b{index, glideSource >= 0 ? glideSource : index, glideNode};
        buses()[server] = b;
        sendValues(server, b);
    }

    // the block a destination's synths read, -1 when it follows no feature
    static int bus(ofxOceanodeNodeModel* node, ofxSCServer* server) {
        const int block = (follows(node, Transpose) ? 1 : 0) | (follows(node, Scale) ? 2 : 0) | (follows(node, Detune) ? 4 : 0);
        auto it = buses().find(server);
        if(block == 0 || it == buses().end()) return -1;
        return it->second.read + block * BlockSize;
    }

    // the block a part of a destination reads (a drum kit's pad: follow()'s parts)
    static int bus(ofxOceanodeNodeModel* node, const std::string& part, ofxSCServer* server) {
        const int block = (follows(node, part, Transpose) ? 1 : 0) | (follows(node, part, Scale) ? 2 : 0) | (follows(node, part, Detune) ? 4 : 0);
        auto it = buses().find(server);
        if(block == 0 || it == buses().end()) return -1;
        return it->second.read + block * BlockSize;
    }
    // the controls of a generic / old-format SynthDef that carry its note (MIDI):
    // pitch, pitcharray, and carp (PMod's carrier; its modulator modp is timbre).
    // A filter's cutoff is cutp, so Global Pitch never reaches it.
    static bool isPitchControl(const std::string& name) {
        return name == "pitch" || name == "pitcharray" || name == "carp";
    }

private:
    struct Buses { int read = -1, write = -1, glideNode = -1; };
    static std::map<ofxSCServer*, Buses>& buses() { static std::map<ofxSCServer*, Buses> b; return b; }

    static void sendValues(ofxSCServer* server, const Buses& b) {
        if(!server) return;
        if(b.glideNode >= 0) {
            ofxOscMessage g;
            g.setAddress("/n_set");
            g.addIntArg(b.glideNode);
            g.addStringArg("glide");
            g.addFloatArg(glide());
            server->sendMsg(g);
        }
        ofxOscMessage m;
        m.setAddress("/c_setn");
        m.addIntArg(b.write);
        m.addIntArg(NumBlocks * BlockSize);
        // the scale with negative harmony folded in: the synths read one table per side
        const auto offsets = harmonyOffsets(false), lowOffsets = harmonyOffsets(true);
        for(int block = 0; block < NumBlocks; block++) {
            const bool t = block & 1, s = block & 2, q = s && harmonyOn(false), lq = s && splitOn() && harmonyOn(true), d = block & 4;
            m.addFloatArg(t ? transpose() : 0.0f);
            m.addFloatArg(q ? 1.0f : 0.0f);
            for(float o : offsets) m.addFloatArg(q ? o : 0.0f);
            m.addFloatArg(d ? detune() : 0.0f);
            m.addFloatArg(t && transposeWraps() ? 1.0f : 0.0f);
            m.addFloatArg(t ? float(transposeBase()) : 0.0f);
            m.addFloatArg(s && splitOn() ? split() : -1000.0f);   // where the low side starts, bypassed or not
            for(float o : lowOffsets) m.addFloatArg(lq ? o : 0.0f);
            m.addFloatArg(lq ? 1.0f : 0.0f);
        }
        server->sendMsg(m);
    }
};
