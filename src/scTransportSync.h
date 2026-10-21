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
//    - periodically (drift between the SC audio clock and the steady clock),
//      sent ahead at the next beat/time deadline rather than after it passed
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"


#include "ofxOceanodeTransport.h"
#include "ofxSCServer.h"

#include <algorithm>
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
	bool loopWrap = false;        // exact loop restart (pre-sent in realtime)
	// Keep an enqueued deadline exact when a transport change replaces it.
	uint64_t oscTimetag = 0;
	// Transport seconds from the frame that sends it to the anchor's instant
	// (a loop restart ahead: > 0; a start between two frames: < 0). An NRT
	// capture stamps the anchor there (ofxSCServer::ScopedNRTOffset).
	double offsetSeconds = 0.0;
	// Beats elapsed at `beat` since the transport position `referenceBeat`.
	double beatsSince(double referenceBeat) const { return beat - referenceBeat; }
};

// Timetag scope for sending an anchor: every set() on any server inside it
// carries the anchor's instant (and, captured for an NRT render, is stamped
// at that instant of the score).
class AnchorScope {
public:
	explicit AnchorScope(const Anchor& anchor)
	: tag(anchor.oscTimetag != 0 ? anchor.oscTimetag :
	      anchor.steadyTimeUs != 0 ? ofxSCServer::timetagForSteadyTimeUs(anchor.steadyTimeUs) : 0),
	  nrt(anchor.offsetSeconds) {}
private:
	ofxSCServer::ScopedTimetag tag;
	ofxSCServer::ScopedNRTOffset nrt;
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
	// none; a change also replaces any outstanding future deadlines).
	std::vector<Anchor> poll(const ofxOceanodeFrameTransportState& frame) {
		std::vector<Anchor> out;
		const auto& cur = frame.current;
		const double bps = std::max(0.0f, cur.bpm) / 60.0;
		// One frame, in transport seconds. A frame-stepped render (NRT) moves
		// the transport a fixed step per frame whatever the wall clock does:
		// that step; realtime: the wall clock between the two frames.
		double frameSeconds = 1.0 / 60.0;
		if(cur.driverMode == TransportDriverMode::FrameStep) {
			const auto& prev = frame.previous;
			if(bps > 0.0 && cur.isPlaying && prev.isPlaying && cur.generation == prev.generation &&
			   cur.loopCount == prev.loopCount && cur.beatPosition > prev.beatPosition)
				frameStepSeconds = std::min(0.5, (cur.beatPosition - prev.beatPosition) / bps);
			frameSeconds = frameStepSeconds;
		} else if(cur.steadyTimeUs > frame.previous.steadyTimeUs) {
			frameSeconds = std::min(0.1, static_cast<double>(cur.steadyTimeUs - frame.previous.steadyTimeUs) / 1e6);
		}
		const bool realtime = cur.driverMode != TransportDriverMode::FrameStep;
		const double lookaheadSeconds = std::max(3.0 * frameSeconds, 0.05);
		// Only unexecuted realtime anchors need replacement. Offline periodic
		// corrections keep their original frame/score positions.
		if(!realtime) pending.clear();
		pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Anchor& a) {
			return a.steadyTimeUs <= cur.steadyTimeUs;
		}), pending.end());
		const bool loopChanged = initialized && (cur.loopEnabled != lastLoopEnabled ||
			cur.loopStartBeat != lastLoopStart || cur.loopEndBeat != lastLoopEnd);
		const bool changed = !initialized || forceNext || cur.generation != lastGeneration ||
			cur.isPlaying != lastPlaying || std::abs(cur.bpm - lastBpm) > 1e-3f || loopChanged ||
			cur.driverMode != lastDriver;
		if(!initialized || cur.generation != lastGeneration || loopChanged || cur.driverMode != lastDriver ||
		   cur.isPlaying != lastPlaying || std::abs(cur.bpm - lastBpm) > 1e-3f)
			wrapScheduledFor = 0;
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
			// A jump. One back to the start (of the transport, or of the loop)
			// while playing happened between two frames: anchor at the start,
			// back-dated, so its first step still fires (an NRT render starts
			// this way: from beat 0, already a frame in).
			const double start = cur.loopActive() && cur.beatPosition >= cur.loopStartBeat ? cur.loopStartBeat : 0.0;
			const double since = cur.beatPosition - start;
			if(cur.isPlaying && bps > 0.0 && since > 0.0 && since <= 2.5 * frameSeconds * bps) {
				Anchor a;
				a.beat = start;
				a.offsetSeconds = -since / bps;
				a.steadyTimeUs = cur.steadyTimeUs - static_cast<uint64_t>(since / bps * 1e6);
				a.bpm = cur.bpm;
				a.playing = true;
				a.discontinuity = true;
				out.push_back(a);
				lastAnchorBeat = a.beat;
				lastAnchorUs = a.steadyTimeUs;
			} else {
				anchorNow(true);
			}
		} else if(cur.isPlaying && !lastPlaying) {
			// Started. The transport began moving somewhere between the two
			// frames; anchor at that instant, from where it stood, so the
			// step it started on still fires (a jump, like a seek).
			Anchor a;
			const double moved = std::max(0.0, cur.beatPosition - frame.previous.beatPosition);
			a.beat = frame.previous.beatPosition;
			a.offsetSeconds = bps > 0.0 ? -std::min(1.0, moved / bps) : 0.0;
			a.steadyTimeUs = cur.steadyTimeUs - static_cast<uint64_t>(bps > 0.0 ? std::min(1.0, moved / bps) * 1e6 : 0.0);
			a.bpm = cur.bpm;
			a.playing = true;
			a.discontinuity = true;
			out.push_back(a);
			lastAnchorBeat = a.beat;
			lastAnchorUs = a.steadyTimeUs;
		} else if(!cur.isPlaying && lastPlaying) {
			anchorNow(false); // stopped: hold here
		} else if(!realtime && cur.loopCount != lastLoopCount && cur.loopActive() && bps > 0.0) {
			// An offline score has no delivery deadline. Record the exact
			// wrap retrospectively, as starts already are, instead of
			// leaving speculative anchors across future tempo/seek edits.
			Anchor a;
			a.beat = cur.loopStartBeat;
			a.offsetSeconds = -(cur.beatPosition - cur.loopStartBeat) / bps;
			a.steadyTimeUs = static_cast<uint64_t>(std::max<int64_t>(1,
				static_cast<int64_t>(cur.steadyTimeUs) + static_cast<int64_t>(std::llround(a.offsetSeconds * 1e6))));
			a.bpm = cur.bpm;
			a.playing = cur.isPlaying;
			a.loopWrap = a.discontinuity = true;
			out.push_back(a);
		} else if(forceNext) {
			anchorNow(false);
		} else if(std::abs(cur.bpm - lastBpm) > 1e-3f || loopChanged || cur.driverMode != lastDriver) {
			anchorNow(false);
		} else if(cur.loopCount != lastLoopCount && !wrapSentFor(cur.loopCount)) {
			// A realtime wrap missed by a stalled frame remains a timing error.
			anchorNow(false);
		}

		// OSC packets already in scsynth's queue cannot be withdrawn. Replace
		// every outstanding deadline in place after a seek, stop, tempo/loop
		// edit or reset. Both writes share the EXACT OSC tag; the newer complete
		// anchor wins at that instant without restoring the old transport state.
		if(changed && realtime) {
			for(const auto& queued : pending) {
				Anchor replacement;
				replacement.steadyTimeUs = queued.steadyTimeUs;
				replacement.oscTimetag = queued.oscTimetag;
				replacement.offsetSeconds = static_cast<double>(queued.steadyTimeUs - cur.steadyTimeUs) / 1e6;
				replacement.beat = cur.beatPosition + (cur.isPlaying ? replacement.offsetSeconds * bps : 0.0);
				if(cur.isPlaying && cur.loopActive() && replacement.beat >= cur.loopEndBeat)
					replacement.beat = cur.loopStartBeat + positiveMod(replacement.beat - cur.loopEndBeat,
						cur.loopEndBeat - cur.loopStartBeat);
				// An unchanged upcoming wrap still needs its hard reset and first
				// step. A tempo/seek/loop edit moves that reset to the new wrap.
				if(queued.loopWrap && cur.isPlaying && cur.loopActive() && bps > 0.0 &&
				   std::abs(replacement.offsetSeconds - (cur.loopEndBeat - cur.beatPosition) / bps) < 1.5e-6) {
					replacement.beat = cur.loopStartBeat;
					replacement.loopWrap = true;
					replacement.discontinuity = true;
				}
				replacement.bpm = cur.bpm;
				replacement.playing = cur.isPlaying;
				out.push_back(replacement);
			}
		}

		// A queued wrap belongs to the next loop iteration. Do not compare
		// this iteration's position with its future post-wrap beat (the old
		// absolute-difference check produced a second hard reset at each wrap).
		const bool awaitingWrap = wrapScheduledFor > cur.loopCount;
		if(out.empty() && cur.isPlaying && bps > 0.0 && !awaitingWrap) {
			const double beatsUntil = periodicBeats > 0.0 ?
				(lastAnchorBeat + periodicBeats - cur.beatPosition) / bps : INFINITY;
			const double secondsUntil = periodicSeconds > 0.0 ?
				static_cast<double>(lastAnchorUs) / 1e6 + periodicSeconds - static_cast<double>(cur.steadyTimeUs) / 1e6 : INFINITY;
			const double until = std::min(beatsUntil, secondsUntil);
			if(realtime && until <= lookaheadSeconds) {
				Anchor a;
				a.offsetSeconds = until;
				a.beat = cur.beatPosition + until * bps;
				a.steadyTimeUs = static_cast<uint64_t>(std::max<int64_t>(1,
					static_cast<int64_t>(cur.steadyTimeUs) + static_cast<int64_t>(std::llround(until * 1e6))));
				a.bpm = cur.bpm;
				a.playing = true;
				// The exact loop anchor replaces a correction at/beyond the wrap.
				if(!cur.loopActive() || a.beat < cur.loopEndBeat - 1e-9) {
					out.push_back(a);
					lastAnchorBeat = a.beat;
					lastAnchorUs = a.steadyTimeUs;
				}
			} else if(!realtime && until <= 0.0) {
				anchorNow(false);
			}
		}

		forceNext = false;

		// Loop restart coming within the next ~3 frames: send it ahead, at the
		// exact instant of the wrap, so it is not heard a frame late.
		if(realtime && cur.isPlaying && cur.loopActive() && bps > 0.0) {
			const double remaining = cur.loopEndBeat - cur.beatPosition;
			// Keep the window short: an anchor already sent cannot be taken
			// back if the user seeks before it plays.
			const double lookahead = lookaheadSeconds * bps;
			const uint64_t nextLoop = cur.loopCount + 1;
			if(remaining > 0.0 && remaining <= lookahead && wrapScheduledFor != nextLoop) {
				Anchor a;
				a.beat = cur.loopStartBeat;
				a.steadyTimeUs = cur.steadyTimeUs + static_cast<uint64_t>(remaining / bps * 1e6);
				a.offsetSeconds = remaining / bps;
				a.bpm = cur.bpm;
				a.playing = true;
				a.loopWrap = true;
				a.discontinuity = true;
				// A reset may already have repaired this same queued wrap.
				// Reuse its original tag instead of enqueueing a second reset.
				const bool repaired = std::any_of(out.begin(), out.end(), [&](const Anchor& q) {
					return q.loopWrap && std::abs(static_cast<int64_t>(q.steadyTimeUs) -
						static_cast<int64_t>(a.steadyTimeUs)) <= 1;
				});
				if(!repaired) out.push_back(a);
				wrapScheduledFor = nextLoop;
			}
		}

		initialized = true;
		lastGeneration = cur.generation;
		lastPlaying = cur.isPlaying;
		lastBpm = cur.bpm;
		lastLoopCount = cur.loopCount;
		lastLoopEnabled = cur.loopEnabled;
		lastLoopStart = cur.loopStartBeat;
		lastLoopEnd = cur.loopEndBeat;
		lastDriver = cur.driverMode;
		if(!out.empty() && out.back().loopWrap) {
			// The periodic timer restarts from the wrap.
			lastAnchorBeat = out.back().beat;
			lastAnchorUs = out.back().steadyTimeUs;
		}
		std::stable_sort(out.begin(), out.end(), [](const Anchor& a, const Anchor& b) {
			return a.steadyTimeUs < b.steadyTimeUs;
		});
		for(auto& a : out) {
			if(a.oscTimetag == 0 && a.steadyTimeUs != 0)
				a.oscTimetag = ofxSCServer::timetagForSteadyTimeUs(a.steadyTimeUs);
			if(!realtime || a.steadyTimeUs <= cur.steadyTimeUs) continue;
			auto it = std::find_if(pending.begin(), pending.end(), [&](const Anchor& q) {
				return q.oscTimetag == a.oscTimetag;
			});
			if(it == pending.end()) pending.push_back(a);
			else *it = a;
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
	bool lastLoopEnabled = false;
	double lastLoopStart = 0.0, lastLoopEnd = 0.0;
	TransportDriverMode lastDriver = TransportDriverMode::RealTime;
	std::vector<Anchor> pending;
	double frameStepSeconds = 1.0 / 60.0;
	double lastAnchorBeat = 0.0;
	uint64_t lastAnchorUs = 0;
};

} // namespace scTransportSync
