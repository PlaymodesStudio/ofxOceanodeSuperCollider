#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "scSynthControlBatch.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// Gate Mode "Pulse" for the Box synths (PhysicalBox, SteamPipeBox): automatic
// gates from a clock to a Geiger counter, together or each voice its own.
//
// The PulseGate synth (synthdefs/Defaults/Modulation/pulseGate.scd) goes at
// the head of the node's gate group and writes 16 gate levels to its own audio
// bus; the node's gate synth (ModGate, SteamPipeBoxGate) maps that bus as its
// \gate_ar with \gate_sel 1 and gets zeros as Oceanode gate values. With
// Follow Gate the node's Gate (values or cable) holds and scales each voice.
//
// The host registers the parameters (its reg: snapshots, publishing), calls
// start / stop / release per server, send() on changes, enable() from its
// sendNotes, read() with its vis values while the window shows, and
// drawPanel() in its window. Random draws are a hash of (seed, stream, event
// number) on the server: Seed > 0 replays the same pattern from every start,
// 0 draws a fresh seed each start.
class scPulseGate {
public:
    static constexpr int VIS_SIZE = 48;   // per voice: event count, last length, last velocity

    struct Host {
        std::function<void(ofParameter<float>&, std::function<void()>)> regF;
        std::function<void(ofParameter<int>&, std::function<void()>)> regI;
        std::function<void(ofParameter<bool>&, std::function<void()>)> regB;
        std::function<void()> sendAll;          // Follow Gate: the notes again
        std::function<float()> bpm;
        std::function<int()> voices;
        // the host's own widgets (label widths, publishing, menus)
        std::function<void(const char*, float, const char*)> section;   // title, width, help (the "?")
        std::function<void(ofParameter<float>&, const char*, float, const char*, bool)> slider;
        std::function<void(ofParameter<bool>&, const char*)> toggle;
        std::function<void(const std::string&)> mark, menu;
        float* labelW = nullptr;
    };

    ofParameter<bool> sync, fixed, follow;
    ofParameter<float> rate, beats, chaos, cluster, chance, link, phasing, strum, length, time, jitter, velocity, velJitter;
    ofParameter<int> seed;
    bool restart = false;    // a new Seed, Reset: the host starts it again (start())

    void setup(Host h) {
        host = std::move(h);
        auto toAll = [this]() { for(auto& [s, x] : synths) send(s); };
        auto f = [&](ofParameter<float>& p, const char* name, float def, float lo, float hi) { p.set(name, def, lo, hi); host.regF(p, toAll); };
        sync.set("Pulse Sync", false);           host.regB(sync, toAll);
        f(rate, "Pulse Rate", 4.0f, 0.05f, 200.0f);
        f(beats, "Pulse Beats", 0.5f, 0.0625f, 16.0f);
        f(chaos, "Pulse Chaos", 0.0f, 0.0f, 1.0f);
        f(cluster, "Pulse Cluster", 0.0f, 0.0f, 1.0f);
        f(chance, "Pulse Chance", 1.0f, 0.0f, 1.0f);
        f(link, "Pulse Link", 1.0f, 0.0f, 1.0f);
        f(phasing, "Pulse Phasing", 0.0f, 0.0f, 1.0f);
        f(strum, "Pulse Strum", 0.0f, -80.0f, 80.0f);
        f(length, "Pulse Length", 0.5f, 0.01f, 2.0f);
        fixed.set("Pulse Fixed Length", false);  host.regB(fixed, toAll);
        f(time, "Pulse Length Time", 0.1f, 0.001f, 8.0f);
        f(jitter, "Pulse Length Jitter", 0.0f, 0.0f, 1.0f);
        f(velocity, "Pulse Velocity", 0.8f, 0.0f, 1.0f);
        f(velJitter, "Pulse Velocity Jitter", 0.0f, 0.0f, 1.0f);
        follow.set("Pulse Follow Gate", false);  host.regB(follow, [this]() { host.sendAll(); });
        seed.set("Pulse Seed", 0, 0, 9999);       host.regI(seed, [this]() { restart = true; });
    }

    float hz() const {
        if(sync.get()) return host.bpm() / 60.0f / std::max(0.0625f, beats.get());
        return std::max(0.01f, rate.get());
    }
    bool running(ofxSCServer* s) const { auto it = synths.find(s); return it != synths.end() && it->second.synth && it->second.bus; }
    int bus(ofxSCServer* s) const { return running(s) ? synths.at(s).bus->index : -1; }

    // a new generator at the head of `group` (the old one freed); its vis at `vis`
    bool start(ofxSCServer* server, int group, int vis, int voices) {
        auto& x = synths[server];
        if(!live(server, x.bus)) {
            drop(server, x.bus);
            x.bus = new ofxSCBus(RATE_AUDIO, 16, server);
            if(!live(server, x.bus)) { x.bus->free(); delete x.bus; x.bus = nullptr; return false; }
        }
        stop(server);
        x.voices = voices;
        x.synth = new ofxSCSynth("PulseGate", server);
        x.synth->set("seed", seed.get() > 0 ? seed.get() : int(ofRandom(1, 99999)));
        x.synth->set("out", x.bus->index);
        x.synth->set("vis", vis);
        send(server);
        x.synth->createAndRun(0, group, true);
        counts.fill(0);
        for(auto& e : events) e.clear();
        return true;
    }
    void stop(ofxSCServer* server) {
        auto it = synths.find(server);
        if(it == synths.end() || !it->second.synth) return;
        it->second.synth->free(); delete it->second.synth; it->second.synth = nullptr;
    }
    // the host's group is gone (its synth with it): forget both
    void release(ofxSCServer* server) {
        auto it = synths.find(server);
        if(it == synths.end()) return;
        delete it->second.synth; it->second.synth = nullptr;
        drop(server, it->second.bus);
        synths.erase(it);
    }
    void send(ofxSCServer* server) {
        auto it = synths.find(server);
        if(it == synths.end() || !it->second.synth) return;
        scSynthControlBatch c(it->second.synth, server);
        c.set("voices", float(it->second.voices));
        c.set("rate", hz());
        c.set("chaos", chaos.get()); c.set("cluster", cluster.get());
        c.set("chance", chance.get()); c.set("link", link.get());
        c.set("phasing", phasing.get()); c.set("strum", strum.get() * 0.001f);
        c.set("length", length.get()); c.set("lengthsec", time.get());
        c.set("fixed", fixed.get() ? 1.0f : 0.0f); c.set("jitter", jitter.get());
        c.set("velocity", velocity.get()); c.set("veljitter", velJitter.get());
        c.send();
    }
    void sendAll() { for(auto& [s, x] : synths) send(s); }
    // Follow Gate: the node's Gate values (and its cable, gateBus >= 0) hold the voices
    void enable(ofxSCServer* server, const std::vector<float>& gate, int voices, int gateBus) {
        auto it = synths.find(server);
        if(it == synths.end() || !it->second.synth) return;
        auto* x = it->second.synth;
        std::vector<float> held(16, 0.0f);
        for(int i = 0; i < voices && i < 16; ++i) held[i] = gate.empty() ? 0.0f : gate[i % gate.size()];
        x->set("enable", held);
        x->set("free", follow.get() ? 0.0f : 1.0f);
        x->set("enable_sel", gateBus >= 0 ? 1.0f : 0.0f);
        if(gateBus >= 0) x->mapan("enable_ar", gateBus, voices);
    }

    // the generator's event counts since the last read: new events into the raster
    void read(const std::vector<float>& vis, size_t offset) {
        const float now = ofGetElapsedTimef(), window = 6.0f;
        if(vis.size() >= offset + VIS_SIZE) {
            const float since = std::clamp(now - readAt, 0.0f, 0.25f);
            for(int v = 0; v < 16; ++v) {
                const float count = vis[offset + v];
                if(count < counts[v]) counts[v] = count;      // a new generator
                const int fresh = std::min(16, int(count - counts[v]));
                counts[v] = count;
                for(int k = 0; k < fresh; ++k)
                    events[v].push_back({now - since * (1 - float(k + 1) / fresh), vis[offset + 16 + v], vis[offset + 32 + v]});
            }
        }
        readAt = now;
        for(auto& e : events)
            e.erase(e.begin(), std::find_if(e.begin(), e.end(), [&](const Event& x){ return x.t + x.dur > now - window; }));
    }

    // a character to start from (Rate stays the user's)
    void shape(int which) {
        struct Shape { float chaos, cluster, chance, link, phasing, strum, length, jitter, veljitter; };
        static const Shape shapes[] = {
            {0, 0, 1, 1, 0, 0, 0.5f, 0, 0},                          // Clock
            {0, 0, 1, 1, 0, 30, 0.8f, 0.1f, 0.25f},                  // Strum
            {0, 0, 1, 0, 0.03f, 0, 0.3f, 0, 0.1f},                   // Phasing
            {1, 0, 1, 0, 0, 0, 0.15f, 0.5f, 0.7f},                   // Rain
            {1, 0.6f, 1, 0, 0, 0, 0.05f, 0.3f, 0.3f},                // Geiger
            {0.6f, 0.2f, 0.85f, 0.35f, 0.2f, 0, 0.4f, 0.6f, 0.5f},   // Swarm
            {0.15f, 1, 1, 1, 0, 0, 0.3f, 0.2f, 0.35f},               // Bursts
        };
        const auto& x = shapes[std::clamp(which, 0, int(std::size(shapes)) - 1)];
        chaos = x.chaos; cluster = x.cluster; chance = x.chance; link = x.link; phasing = x.phasing;
        strum = x.strum; length = x.length; fixed = false; jitter = x.jitter; velJitter = x.veljitter;
    }

    // the panel: the events as they play and the characters, then Time / Voices / Notes
    void drawPanel(float w) {
        const float z = ofxOceanodeShared::getZoomLevel(), gap = 12.0f * z;
        const float cw = std::max(1.0f, (w - gap * 4) / 5), rasterW = cw * 2 + gap;
        auto nextCol = [&]() { ImGui::EndGroup(); ImGui::SameLine(0, gap); ImGui::BeginGroup(); };
        auto tip = [](const char* text) { if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text); };
        const float labelBefore = host.labelW ? *host.labelW : 70;
        if(host.labelW) *host.labelW = 66;
        const float labelW = (host.labelW ? *host.labelW : 66) * z;
        ImGui::Spacing();
        ImGui::BeginGroup();
        host.section("Pulse", rasterW,
            "The voices play by themselves (Gate Mode Pulse), from a steady clock to a Geiger counter.\n\n"
            "The view shows the events as they play: a row per voice, newest on the right; each block is a gate "
            "(its length), as tall and bright as its velocity.\n"
            "The buttons are characters to start from (they leave Rate alone): Clock, Strum, Phasing, Rain, Geiger, "
            "Swarm, Bursts. Hover one for what it does.\n\n"
            "Gate's per-note sources (Velocity, Random) and the envelopes follow the pulses, so each event can sound different.");
        {
            static const char* names[] = {"Clock", "Strum", "Phasing", "Rain", "Geiger", "Swarm", "Bursts"};
            static const char* tips[] = {
                "Every voice on the same beat",
                "Together, rolled across the voices like a strummed chord",
                "Each voice its own clock, slightly apart: they drift in and out of phase",
                "Each voice its own random drops (Poisson), soft and loud",
                "Random clicks with bursts and silences, each voice its own counter",
                "Loose, partly shared, partly free: a flock",
                "Together, in clusters of quick repeats"};
            const int count = int(std::size(names));
            const float bgap = 4 * z, bw = (rasterW - bgap * (count - 1)) / count;
            for(int i = 0; i < count; ++i) {
                if(i) ImGui::SameLine(0, bgap);
                if(ImGui::Button(names[i], ImVec2(bw, 0))) shape(i);
                tip(tips[i]);
            }
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float rows = 6 * ImGui::GetFrameHeightWithSpacing() - ImGui::GetFrameHeight() - 4 * z;
        drawRaster(at, ImVec2(rasterW, rows));
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rows));
        ImGui::Dummy(ImVec2(rasterW, 0));
        nextCol();
        host.section("Time", cw,
            "When the events happen.\n\n"
            "Rate: events per second per voice on average (Sync to tempo: a note value instead).\n"
            "Chaos: 0 a clock; 1 random like a Geiger counter (a Poisson process); the average rate stays the same.\n"
            "Cluster: the events gather into bursts of quick repeats and long silences (same average).\n"
            "Chance: each event plays or not (thins the pattern).");
        host.toggle(sync, "Sync to tempo");
        tip("Rate from the tempo (a note value) instead of Hz");
        if(sync.get()) {
            static const std::vector<std::pair<const char*, float>> notes = {
                {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/8", 0.5f},
                {"1/4T", 2.0f / 3.0f}, {"1/4", 1.0f}, {"1/2", 2.0f}, {"1 bar", 4.0f}, {"2 bars", 8.0f}, {"4 bars", 16.0f}};
            int k = 0;
            for(int i = 0; i < (int)notes.size(); i++)
                if(std::abs(notes[i].second - beats.get()) < std::abs(notes[k].second - beats.get())) k = i;
            const float x0 = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Every");
            host.mark(beats.getEscapedName());
            ImGui::SameLine();
            ImGui::SetCursorPosX(x0 + labelW);
            ImGui::SetNextItemWidth(cw - labelW);
            if(ImGui::SliderInt("##pulseNote", &k, 0, (int)notes.size() - 1, notes[k].first)) beats = notes[k].second;
            tip("The average time between events, in beats");
            host.menu(beats.getEscapedName());
        } else {
            host.slider(rate, "Rate", cw, "%.2f Hz", true);
            tip("Events per second per voice, on average (whatever Chaos and Cluster are)");
        }
        host.slider(chaos, "Chaos", cw, "%.2f", false);
        tip("0: periodic, a clock\n1: random like a Geiger counter (Poisson): same average rate, no pattern");
        host.slider(cluster, "Cluster", cw, "%.2f", false);
        tip("Gather the events into bursts of quick repeats and long silences (same average rate)");
        host.slider(chance, "Chance", cw, "%.2f", false);
        tip("The chance each event plays (thins the pattern)");
        nextCol();
        host.section("Voices", cw,
            "How the voices share the events.\n\n"
            "Link: 1 every voice plays the same events (one seed for all, simultaneous); 0 each voice its own (its own seed); "
            "between, each event is shared or a voice's own by chance.\n"
            "Phasing: each voice's own clock slightly faster or slower: periodic voices drift in and out of phase (Steve Reich). Acts where Link < 1.\n"
            "Strum: shared events rolled across the voices, ms per voice (negative: downwards).\n"
            "Seed: 0 a new pattern every start; above 0 the same pattern every time (Reset plays it from the start).\n"
            "Follow Gate: a voice pulses only while its Gate is held, as loud as the Gate (hold notes to play rolls).");
        host.slider(link, "Link", cw, link.get() >= 0.995f ? "together" : link.get() <= 0.005f ? "independent" : "%.2f", false);
        tip("1: every voice plays the same events (one seed for all)\n0: each voice its own events (its own seed)\n"
            "between: each event is shared or a voice's own by chance");
        host.slider(phasing, "Phasing", cw, "%.2f", false);
        tip("Each voice's own clock a little faster or slower (up to an octave):\nperiodic voices drift in and out of phase. Acts where Link < 1");
        host.slider(strum, "Strum", cw, "%+.0f ms", false);
        tip("Roll the shared events across the voices, ms per voice (negative: downwards)");
        {
            const std::string key = seed.getEscapedName();
            ImGui::PushID(key.c_str());
            const float x0 = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Seed");
            host.mark(key);
            ImGui::SameLine();
            ImGui::SetCursorPosX(x0 + labelW);
            ImGui::SetNextItemWidth(std::max(30.0f * z, cw - labelW));
            int v = seed.get();
            if(ImGui::DragInt("##v", &v, 0.2f, 0, 9999, v == 0 ? "fresh" : "%d")) seed = std::clamp(v, 0, 9999);
            tip("0: a new pattern every start\nabove 0: the same pattern every time (Reset plays it from the start)");
            host.menu(key);
            ImGui::PopID();
        }
        host.toggle(follow, "Follow Gate");
        tip("Off: the pulses run by themselves\nOn: a voice pulses only while its Gate is held, as loud as the Gate\n"
            "(hold notes from a keyboard or the timeline to play rolls)");
        nextCol();
        host.section("Notes", cw,
            "Each event's length and strength.\n\n"
            "Length: how long the gate stays open, a fraction of the average period (or in seconds). Struck sounds only need "
            "the start; sustained ones (bow, blow, steam) sound while it is open. Past 1 notes run into the next (legato), "
            "each event still a new note.\n"
            "Jitter: a random length per event (up to 4x shorter or longer).\n"
            "Velocity: how hard each event plays. Vel Jitter: random velocity per event, accents and ghost notes.");
        if(fixed.get()) {
            host.slider(time, "Length", cw, "%.3f s", true);
            tip("How long each gate stays open, in seconds");
        } else {
            char fmt[48];
            std::snprintf(fmt, sizeof(fmt), "%%.2f x  (%.0f ms)", length.get() / hz() * 1000);
            host.slider(length, "Length", cw, fmt, false);
            tip("How long each gate stays open, as a fraction of the average period.\n"
                "Past 1 notes run into the next: legato, each event a new note");
        }
        host.toggle(fixed, "Length in seconds");
        host.slider(jitter, "Jitter", cw, "%.2f", false);
        tip("Random length per event: up to 4x shorter or longer");
        host.slider(velocity, "Velocity", cw, "%.2f", false);
        tip("How hard each event plays (the gate level)");
        host.slider(velJitter, "Vel Jitter", cw, "%.2f", false);
        tip("Random velocity per event: accents and ghost notes");
        ImGui::EndGroup();
        if(host.labelW) *host.labelW = labelBefore;
    }

private:
    struct Event { float t, dur, vel; };
    struct Server { ofxSCSynth* synth = nullptr; ofxSCBus* bus = nullptr; int voices = 1; };
    Host host;
    std::map<ofxSCServer*, Server> synths;
    std::array<std::vector<Event>, 16> events;   // the last seconds, read back from the server
    std::array<float, 16> counts{};
    float readAt = 0;

    static bool live(ofxSCServer* s, ofxSCBus* b) {
        if(!s || !b || b->index < 0) return false;
        return b->index < int(s->audioBusses.size()) && s->audioBusses[b->index] == b;
    }
    static void drop(ofxSCServer* s, ofxSCBus*& b) {
        if(live(s, b)) b->free();
        delete b; b = nullptr;
    }
    static ImU32 mix(ImU32 a, ImU32 b, float t) {
        t = ofClamp(t, 0, 1);
        auto ch = [&](int s) { return int(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
        return IM_COL32(ch(0), ch(8), ch(16), ch(24));
    }

    // a row per voice: a block per gate (its length), as bright and tall as its velocity
    void drawRaster(ImVec2 a, ImVec2 size) {
        const float z = ofxOceanodeShared::getZoomLevel();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 b(a.x + size.x, a.y + size.y);
        dl->AddRectFilled(a, b, IM_COL32(11, 12, 14, 255), 3 * z);
        dl->AddRect(a, b, IM_COL32(46, 50, 58, 255), 3 * z);
        const int n = std::clamp(host.voices(), 1, 16);
        const float f = hz();
        const float window = ofClamp(24.0f / f, 1.5f, 6.0f), now = ofGetElapsedTimef();
        const float x0 = a.x + 4 * z, x1 = b.x - 4 * z, top = a.y + 4 * z, bottom = b.y - 18 * z;
        const float rh = (bottom - top) / n;
        auto xOf = [&](float t) { return ofLerp(x0, x1, ofClamp(1 - (now - t) / window, 0.0f, 1.0f)); };
        const float period = 1.0f / f;
        if(window / period < 64)
            for(float t = now - std::fmod(now, period); t > now - window; t -= period)
                dl->AddLine(ImVec2(xOf(t), top), ImVec2(xOf(t), bottom), IM_COL32(255, 255, 255, 14));
        dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
        for(int v = 0; v < n; ++v) {
            const float y0 = top + v * rh, y1 = y0 + rh, mid = (y0 + y1) * 0.5f;
            if(v % 2) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 6));
            for(const auto& e : events[v]) {
                const float vel = ofClamp(e.vel, 0.0f, 1.0f);
                const float h = (rh - 2 * z) * (0.3f + 0.7f * vel) * 0.5f;
                const float ex0 = xOf(e.t), ex1 = std::max(ex0 + 2 * z, xOf(e.t + e.dur));
                dl->AddRectFilled(ImVec2(ex0, mid - h), ImVec2(ex1, mid + h), mix(IM_COL32(106, 212, 196, 30), IM_COL32(106, 212, 196, 120), vel));
                dl->AddLine(ImVec2(ex0, mid - h), ImVec2(ex0, mid + h), mix(IM_COL32(106, 212, 196, 70), IM_COL32(220, 255, 245, 230), vel), 1.5f * z);
            }
        }
        dl->PopClipRect();
        const float c = chaos.get(), l = link.get();
        const std::string text = ofToString(f, f < 10 ? 2 : 1) + " /s per voice   " +
            (c < 0.05f ? "periodic" : c > 0.95f ? "random (Poisson)" : "loose " + ofToString(int(c * 100)) + "%") +
            (cluster.get() > 0.05f ? ", clustered" : "") + "   " +
            (l > 0.995f ? "together" : l < 0.005f ? "independent" : ofToString(int(l * 100)) + "% shared") +
            (follow.get() ? "   held by Gate" : "");
        dl->AddText(ImVec2(a.x + 6 * z, b.y - 16 * z), IM_COL32(150, 150, 160, 255), text.c_str());
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##pulseRaster", size);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("The pulses as they play: a row per voice, newest on the right (%.1f s).\n"
                              "Each block is a gate, its length and its velocity.", window);
    }
};
