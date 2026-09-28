//
//  scTransportSync.h
//  ofxOceanodeSuperCollider
//
//  Shared "Sync To Transport" plumbing for SC nodes with a step or phase
//  clock (RhythmBox, Polyphonic Arpeggiator, GrainBox).
//
//  The clock keeps running inside SuperCollider (sample accurate), but in
//  sync mode it is re-positioned from ofxOceanode's global transport with
//  ANCHORS: "at steady time T the transport is at beat B, tempo bpm,
//  playing/stopped". Each anchor goes out as timetagged OSC
//  (ofxSCServer::ScopedTimetag), so scsynth applies it at exactly that
//  instant (plus the server's usual latency, the same offset every other
//  parameter send gets).
//
//  SynthDef contract (per clock, positions in that clock's units, e.g. steps):
//    \sync        0 = the node's original free-running clock (default), 1 = this
//    \anchorId    counter: every change starts a new anchor. Detect changes
//                 (Changed.kr), never send 1-then-0 pulses: two sets landing in
//                 the same block cancel out.
//    \anchorHard  counter: changes on discontinuities (seek, stop, start, loop
//                 restart, rebase). Resets the "highest step fired" memory.
//    \anchorPos   position at the anchor instant (C++ keeps it below a large
//                 multiple of the pattern length, so float32 stays precise)
//    \anchorFire  1 = the hard anchor lands exactly on a step: fire it
//    \run         1 = moving, 0 = holding the anchored position
//    \bpm         tempo in effect from the anchor on
//
//    pos     = anchorPos + Sweep.ar(K2A.ar(Changed.kr(anchorId)), stepsPerSecond * run)
//    idx     = pos.floor
//    hardTrig = K2A.ar(Changed.kr(anchorHard))
//    top     = RunningMax.ar(idx, hardTrig)                 // reset on jumps
//    stepTrig = ((idx > Delay1.ar(top)) * (1 - hardTrig)) + (hardTrig * anchorFire)
//    step    = idx mod numSteps
//
//  Firing only when the step index goes ABOVE the highest one already played
//  makes soft anchors (drift corrections) safe: a correction that moves the
//  position back across a boundary it just crossed does not fire that step
//  twice, and one that moves it forward fires it once.
//
//  Anchors are sent when:
//    - sync starts (first poll), or the node forces one (new synth, params)
//    - the transport jumps (seek, stop, external song position: generation)
//    - play state or tempo changes
//    - a loop restart is coming: sent AHEAD, timetagged at the exact wrap
//      instant, so the loop point is sample accurate
//    - periodically (drift between the SC audio clock and the steady clock)
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TRANSPORT

#include "ofxOceanodeTransport.h"
#include "ofxSCServer.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace scTransportSync {

inline double positiveMod(double value, double length) {
	if(!(length > 0.0)) return 0.0;
	double wrapped = std::fmod(value, length);
	if(wrapped < 0.0) wrapped += length;
	// fmod can return `length` for values a hair below a multiple of it.
	if(wrapped >= length) wrapped = 0.0;
	return wrapped;
}

// Anchor values for one step clock, computed from an Anchor.
struct StepAnchor {
	double pos = 0.0;      // \anchorPos
	bool hard = false;     // bump \anchorHard
	bool fire = false;     // \anchorFire
};

// Keeps one clock's positions small (below `period`, a multiple of the
// pattern length) and flags the rebase as a hard anchor.
class StepClock {
public:
	// patternLength: steps before the pattern repeats (numSteps, seqSize...).
	void setPatternLength(double length) {
		const double safeLength = std::max(1.0, length);
		period = safeLength * std::ceil(2048.0 / safeLength);
	}
	// stepPos: absolute position in steps (double, may be huge).
	// discontinuity: the anchor is a jump (seek, stop/start, loop restart).
	StepAnchor make(double stepPos, bool discontinuity, bool playing = true) {
		StepAnchor a;
		a.pos = positiveMod(stepPos, period);
		const bool rebased = hasLast && a.pos + 0.5 < lastPos && !discontinuity;
		a.hard = discontinuity || rebased || !hasLast;
		const double frac = stepPos - std::floor(stepPos);
		a.fire = discontinuity && playing && (frac < 1e-4 || frac > 1.0 - 1e-4);
		if(a.fire && frac > 0.5) a.pos = positiveMod(std::round(stepPos), period); // exactly on it
		lastPos = a.pos;
		hasLast = true;
		return a;
	}
	void reset() { hasLast = false; }
	double getPeriod() const { return period; }
private:
	double period = 2048.0;
	double lastPos = 0.0;
	bool hasLast = false;
};

struct Anchor {
	double beat = 0.0;            // transport beat at the anchor instant
	uint64_t steadyTimeUs = 0;    // steady-clock instant of the anchor
	float bpm = 120.0f;
	bool playing = false;
	bool discontinuity = false;   // a jump (seek/stop/start): not a correction
	bool loopWrap = false;        // pre-sent loop restart
	// Beats elapsed at `beat` since the transport position `referenceBeat`.
	double beatsSince(double referenceBeat) const { return beat - referenceBeat; }
};

// Timetag scope for sending an anchor: every set() on any server inside it
// carries the anchor's instant.
class AnchorScope {
public:
	explicit AnchorScope(const Anchor& anchor)
	: tag(anchor.steadyTimeUs != 0 ? ofxSCServer::timetagForSteadyTimeUs(anchor.steadyTimeUs) : 0) {}
private:
	ofxSCServer::ScopedTimetag tag;
};

class Follower {
public:
	// Re-anchor at least every this many beats while playing (drift).
	double periodicBeats = 4.0;
	// And at least this often in seconds (slow tempos).
	double periodicSeconds = 2.0;

	// Forget everything: the next poll anchors at the current position.
	void reset() { initialized = false; }
	// Ask for an anchor on the next poll (a new synth, a changed length...).
	void requestAnchor() { forceNext = true; }

	// Once per frame. Returns the anchors to send now, in time order (usually
	// none, one when something changed, a second one for a coming loop wrap).
	std::vector<Anchor> poll(const ofxOceanodeFrameTransportState& frame) {
		std::vector<Anchor> out;
		const auto& cur = frame.current;
		const double bps = std::max(0.0f, cur.bpm) / 60.0;
		auto anchorNow = [&](bool discontinuity) {
			Anchor a;
			a.beat = cur.beatPosition;
			a.steadyTimeUs = cur.steadyTimeUs;
			a.bpm = cur.bpm;
			a.playing = cur.isPlaying;
			a.discontinuity = discontinuity;
			out.push_back(a);
			lastAnchorBeat = a.beat;
			lastAnchorUs = a.steadyTimeUs;
		};

		if(!initialized) {
			anchorNow(true);
		} else if(cur.generation != lastGeneration) {
			anchorNow(true);
		} else if(cur.isPlaying && !lastPlaying) {
			// Started. The transport began moving somewhere between the two
			// frames; anchor at that instant, from where it stood, so the
			// step it started on still fires (a jump, like a seek).
			Anchor a;
			const double moved = std::max(0.0, cur.beatPosition - frame.previous.beatPosition);
			a.beat = frame.previous.beatPosition;
			a.steadyTimeUs = cur.steadyTimeUs - static_cast<uint64_t>(bps > 0.0 ? std::min(1.0, moved / bps) * 1e6 : 0.0);
			a.bpm = cur.bpm;
			a.playing = true;
			a.discontinuity = true;
			out.push_back(a);
			lastAnchorBeat = a.beat;
			lastAnchorUs = a.steadyTimeUs;
		} else if(!cur.isPlaying && lastPlaying) {
			anchorNow(false); // stopped: hold here
		} else if(forceNext) {
			anchorNow(false);
		} else if(std::abs(cur.bpm - lastBpm) > 1e-3f) {
			anchorNow(false);
		} else if(cur.loopCount != lastLoopCount && !wrapSentFor(cur.loopCount)) {
			// A wrap we did not see coming (tempo jump, long frame): fix it now.
			anchorNow(false);
		} else if(cur.isPlaying &&
				  (std::abs(cur.beatPosition - lastAnchorBeat) >= periodicBeats ||
				   (cur.steadyTimeUs > lastAnchorUs &&
					static_cast<double>(cur.steadyTimeUs - lastAnchorUs) / 1e6 >= periodicSeconds))) {
			anchorNow(false);
		}
		forceNext = false;

		// Loop restart coming within the next ~3 frames: send it ahead, at the
		// exact instant of the wrap, so it is not heard a frame late.
		if(cur.isPlaying && cur.loopActive() && bps > 0.0) {
			const double remaining = cur.loopEndBeat - cur.beatPosition;
			// Keep the window short: an anchor already sent cannot be taken
			// back if the user seeks before it plays.
			const double frameSeconds = cur.steadyTimeUs > frame.previous.steadyTimeUs
				? std::min(0.1, static_cast<double>(cur.steadyTimeUs - frame.previous.steadyTimeUs) / 1e6) : 1.0 / 60.0;
			const double lookahead = std::max(3.0 * frameSeconds, 0.05) * bps;
			const uint64_t nextLoop = cur.loopCount + 1;
			if(remaining > 0.0 && remaining <= lookahead && wrapScheduledFor != nextLoop) {
				Anchor a;
				a.beat = cur.loopStartBeat;
				a.steadyTimeUs = cur.steadyTimeUs + static_cast<uint64_t>(remaining / bps * 1e6);
				a.bpm = cur.bpm;
				a.playing = true;
				a.loopWrap = true;
				a.discontinuity = true;
				out.push_back(a);
				wrapScheduledFor = nextLoop;
			}
		}

		initialized = true;
		lastGeneration = cur.generation;
		lastPlaying = cur.isPlaying;
		lastBpm = cur.bpm;
		lastLoopCount = cur.loopCount;
		if(!out.empty() && out.back().loopWrap) {
			// The periodic timer restarts from the wrap.
			lastAnchorBeat = out.back().beat;
			lastAnchorUs = out.back().steadyTimeUs;
		}
		return out;
	}

private:
	bool wrapSentFor(uint64_t loopCount) const { return wrapScheduledFor == loopCount; }
	bool initialized = false;
	bool forceNext = false;
	uint64_t lastGeneration = 0;
	bool lastPlaying = false;
	float lastBpm = 0.0f;
	uint64_t lastLoopCount = 0;
	uint64_t wrapScheduledFor = 0;
	double lastAnchorBeat = 0.0;
	uint64_t lastAnchorUs = 0;
};

} // namespace scTransportSync

#endif // OFXOCEANODESC_HAS_TRANSPORT
