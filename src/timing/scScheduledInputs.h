//
//  scScheduledInputs.h
//  ofxOceanodeSuperCollider
//
//  Sample-accurate note inputs (Gate, Pitch, Velocity...) for an instrument
//  node that sends them from its own parameter listeners.
//
//  The timeline hands every scheduled value to the handler a little before it
//  is due (ofxOceanodeScheduling). The handler sets the parameter for real,
//  inside an ofxSCServer::ScopedTimetag, so the node's listener sends it as it
//  would on a frame, but stamped with the exact instant. Meanwhile every other
//  input holds the value it has at that instant (a Gate send also reads
//  Velocity and Pitch); afterwards the real values are put back without
//  notifications. Cables from these inputs do not pass the early value on, so
//  the GUI and the graph keep seeing the frame's values.
//
//  When the playhead reaches the event, the timeline sets the parameter again
//  with the backend send suppressed: the listeners must not send then, nor
//  touch any state the scheduled set already updated:
//
//      listener: if(scScheduling::isBackendSendSuppressed()) return;
//
//  A send that happens for another reason (a control edited, a deferred flush
//  in update()) reads the inputs as they stand at that moment:
//
//      { auto now = sched.inEffectNow(); ...send... }
//
//  Usage:
//      sched.add(handle, gate); sched.add(handle, pitch); ...   (after addParameter)
//      sched.enable(this, [this]{ return any synth exists; });
//      destructor: sched.disable() (also done by ~scScheduledInputs)
//

#pragma once

#include "scSchedulingCompat.h"
#include "ofxSCServer.h"
#include "ofxOceanodeParameter.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class scScheduledInputs {
public:
    using Values = std::vector<float>;

private:
    struct Input {
        ofxOceanodeAbstractParameter* handle = nullptr;
        ofParameter<Values>* parameter = nullptr;
        ofEventListener listener;
        bool scheduled = false;                       // the values below are live
        std::deque<std::pair<uint64_t, Values>> queue; // (due, value) not yet in effect, by due
        bool hasCurrent = false;
        Values current;                               // the last one whose instant has passed
    };

public:

    ~scScheduledInputs() { disable(); }

    void add(const std::shared_ptr<ofxOceanodeAbstractParameter>& handle, ofParameter<Values>& parameter) {
        if(!handle) return;
        auto input = std::make_unique<Input>();
        input->handle = handle.get();
        input->parameter = &parameter;
        Input* raw = input.get();
        // A real set (by hand, a cable, the timeline's first frame): the
        // scheduled values no longer say what the input holds.
        input->listener = parameter.newListener([this, raw](Values&) {
            if(sending || scScheduling::isBackendSendSuppressed()) return;
            raw->scheduled = false;
            raw->queue.clear();
        });
        inputs.push_back(std::move(input));
        if(owner != nullptr) registerInput(*inputs.back());
    }

    // The parameter left the node (unpublished): no longer scheduled.
    void remove(ofParameter<Values>& parameter) {
        for(auto it = inputs.begin(); it != inputs.end(); ++it) {
            if((*it)->parameter != &parameter) continue;
            scScheduling::unregisterParameterTarget((*it)->handle);
            inputs.erase(it);
            return;
        }
    }

    void enable(const void* nodeOwner, std::function<bool()> canSend) {
        owner = nodeOwner;
        hasSynths = std::move(canSend);
        for(auto& input : inputs) registerInput(*input);
    }

    void disable() {
        if(owner != nullptr) scScheduling::unregisterOwner(owner);
        owner = nullptr;
    }

    // True inside a scheduled send (the listeners run from the handler).
    bool isSending() const { return sending; }

    // The inputs as they stand right now (the scheduled values whose instant
    // has passed), for sends that do not come from a scheduled event. A no-op
    // inside a scheduled send, which has its own values in place.
    class InEffect {
    public:
        explicit InEffect(scScheduledInputs& owner) : self(owner) {
            if(self.sending) return;
            const uint64_t nowUs = steadyNowUs();
            for(auto& input : self.inputs) {
                if(!input->scheduled) continue;
                while(!input->queue.empty() && input->queue.front().first <= nowUs) {
                    input->current = input->queue.front().second;
                    input->hasCurrent = true;
                    input->queue.pop_front();
                }
                if(!input->hasCurrent || input->current == input->parameter->get()) continue;
                saved.push_back({input.get(), input->parameter->get()});
                input->parameter->setWithoutEventNotifications(input->current);
            }
        }
        ~InEffect() {
            for(auto& [input, value] : saved) input->parameter->setWithoutEventNotifications(value);
        }
        InEffect(const InEffect&) = delete;
        InEffect& operator=(const InEffect&) = delete;
    private:
        scScheduledInputs& self;
        std::vector<std::pair<Input*, Values>> saved;
    };
    InEffect inEffectNow() { return InEffect(*this); }

private:
    static uint64_t steadyNowUs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // The value an input holds at an instant: the last queued value due by
    // then (a correction can be handed over before events due earlier).
    static const Values& valueAt(const Input& input, uint64_t atUs) {
        for(auto it = input.queue.rbegin(); it != input.queue.rend(); ++it)
            if(it->first <= atUs) return it->second;
        return input.hasCurrent ? input.current : input.parameter->get();
    }

    static Values parse(const std::string& text) {
        Values values;
        size_t start = 0;
        while(start <= text.size()) {
            const size_t comma = text.find(',', start);
            const std::string token = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if(token.find_first_not_of(" \t") != std::string::npos) values.push_back(ofToFloat(token));
            if(comma == std::string::npos) break;
            start = comma + 1;
        }
        return values;
    }

    void registerInput(Input& input) {
        Input* target = &input;
        scScheduling::registerParameterTarget(input.handle, owner,
            [this, target](uint64_t dueUs, const std::string& text) -> bool {
                if(!hasSynths || !hasSynths()) return false;
                const Values value = parse(text);
                if(value.empty()) return false;
                if(!target->scheduled) {
                    target->scheduled = true;
                    target->queue.clear();
                    target->current = target->parameter->get();
                    target->hasCurrent = true;
                }
                // Kept in due order: InEffect promotes from the front, and a
                // correction may arrive before events due ahead of it.
                auto& queue = target->queue;
                queue.insert(std::upper_bound(queue.begin(), queue.end(), dueUs,
                    [](uint64_t due, const auto& entry) { return due < entry.first; }),
                    std::make_pair(dueUs, value));
                if(queue.size() > 256) queue.pop_front();

                // Every input as it stands at this instant.
                std::vector<std::pair<Input*, Values>> real;
                real.reserve(inputs.size());
                std::vector<std::pair<Input*, Values>> atDue;
                atDue.reserve(inputs.size());
                for(auto& other : inputs) {
                    real.push_back({other.get(), other->parameter->get()});
                    if(other.get() != target && other->scheduled) atDue.push_back({other.get(), valueAt(*other, dueUs)});
                }
                for(auto& [other, held] : atDue) other->parameter->setWithoutEventNotifications(held);
                {
                    ofxSCServer::ScopedTimetag tag(ofxSCServer::timetagForSteadyTimeUs(dueUs));
                    ofxOceanodeScheduling::ScopedScheduledSend early; // cables wait for the frame
                    sending = true;
                    target->parameter->set(value);   // the node's listener sends it, stamped
                    sending = false;
                }
                for(auto& [input, original] : real) input->parameter->setWithoutEventNotifications(original);
                return true;
            });
    }

    std::vector<std::unique_ptr<Input>> inputs;
    const void* owner = nullptr;
    std::function<bool()> hasSynths;
    bool sending = false;
};
