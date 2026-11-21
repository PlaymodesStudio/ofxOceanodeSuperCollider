// Offline test of scBakeLoop::makeTextureLoop (src/nodes/recording/scBakeLoop.h).
// Builds synthetic renders, makes loops, and checks the seam, the level through
// the crossfade, and the channel trimming. Run: tools/bake_test/run.sh
#include "../../src/nodes/recording/scBakeLoop.h"
#include <cassert>
#include <cstdio>
#include <random>

using namespace scBakeLoop;
static int failures = 0;
#define CHECK(c, ...) do{ if(!(c)){ std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); failures++; } }while(0)

static double rmsDb(const Audio& a, int ch, std::size_t from, std::size_t n){
    double s = 0; const std::size_t F = a.frames();
    for(std::size_t i = 0; i < n; i++){ double v = a.samples[((from + i) % F) * a.channels + ch]; s += v * v; }
    return 10 * std::log10(s / n + 1e-30);
}

// Largest jump between neighbours inside the loop vs the jump across the wrap
static void seam(const Audio& a, int ch, double& wrapJump, double& p999){
    const std::size_t F = a.frames();
    std::vector<double> d;
    for(std::size_t i = 1; i < F; i++) d.push_back(std::fabs(a.samples[i * a.channels + ch] - a.samples[(i - 1) * a.channels + ch]));
    std::sort(d.begin(), d.end());
    p999 = d[(std::size_t)(d.size() * 0.999)];
    wrapJump = std::fabs(a.samples[ch] - a.samples[(F - 1) * a.channels + ch]);
}

static Audio make(int channels, double seconds, int sr, int sounding, bool tonal, unsigned seed){
    Audio a; a.channels = channels; a.sampleRate = sr;
    const std::size_t F = (std::size_t)(seconds * sr);
    a.samples.assign(F * channels, 0.0f);
    std::mt19937 rng(seed); std::normal_distribution<double> n(0, 1);
    for(int c = 0; c < sounding; c++){
        double lp = 0;
        for(std::size_t i = 0; i < F; i++){
            const double t = (double)i / sr;
            lp += 0.05 * (n(rng) - lp);                       // dark noise
            double v = tonal ? 0.3 * std::sin(2 * M_PI * 110.0 * t + c) + 0.1 * std::sin(2 * M_PI * 220.37 * t)
                             : 0.4 * lp + 0.08 * std::sin(2 * M_PI * 55.0 * t) * (0.6 + 0.4 * std::sin(2 * M_PI * 0.13 * t));
            const double attack = std::min(1.0, t / 2.0);   // the chain starts from silence
            a.samples[i * channels + c] = (float)(v * attack);
        }
    }
    return a;
}

int main(){
    const int sr = 48000;
    {   // Noise texture: 4 rendered channels, 2 sounding
        Audio render = make(4, 27.0, sr, 2, false, 7), loop;
        Settings s; s.skip = 4; s.length = 20; s.crossfade = 2; s.matchLoop = true;
        Result r = makeTextureLoop(render, s, loop);
        std::printf("noise: %s\n", r.message.c_str());
        CHECK(r.ok, "noise loop failed: %s", r.message.c_str());
        CHECK(loop.channels == 2, "silent channels kept: %d", loop.channels);
        CHECK(r.loopSeconds >= 18.0 && r.loopSeconds <= 20.0, "loop length %.3f", r.loopSeconds);
        for(int c = 0; c < 2; c++){
            double wrap, p; seam(loop, c, wrap, p);
            std::printf("  ch%d wrap jump %.5f, 99.9%% neighbour jump %.5f\n", c, wrap, p);
            CHECK(wrap <= p, "audible seam on ch%d", c);
        }
        // Level through the crossfade (it spans the wrap) vs the steady middle
        const std::size_t F = loop.frames(), w = sr / 4;
        double lo = 1e9, hi = -1e9;
        for(std::size_t at = F - sr; at < F + 2 * (std::size_t)sr; at += w){
            const double db = rmsDb(loop, 0, at % F, w); lo = std::min(lo, db); hi = std::max(hi, db);
        }
        double mlo = 1e9, mhi = -1e9;
        for(std::size_t at = 5 * sr; at < 8 * (std::size_t)sr; at += w){
            const double db = rmsDb(loop, 0, at, w); mlo = std::min(mlo, db); mhi = std::max(mhi, db);
        }
        std::printf("  level around the wrap %.2f..%.2f dB, steady part %.2f..%.2f dB\n", lo, hi, mlo, mhi);
        CHECK(lo > mlo - 1.5 && hi < mhi + 1.5, "level dips or bumps at the crossfade");
    }
    {   // Tonal drone: correlated halves, Match Loop finds the phase
        Audio render = make(2, 27.0, sr, 2, true, 3), loop;
        Settings s; s.skip = 4; s.length = 20; s.crossfade = 1; s.matchLoop = true;
        Result r = makeTextureLoop(render, s, loop);
        std::printf("drone: %s\n", r.message.c_str());
        CHECK(r.ok && r.correlation > 0.9, "drone not matched: %.3f", r.correlation);
        const std::size_t F = loop.frames(), w = sr / 20;
        double lo = 1e9, hi = -1e9;
        for(std::size_t at = F - sr / 2; at < F + (std::size_t)sr * 3 / 2; at += w){
            const double db = rmsDb(loop, 0, at % F, w); lo = std::min(lo, db); hi = std::max(hi, db);
        }
        std::printf("  level around the wrap %.2f..%.2f dB\n", lo, hi);
        CHECK(hi - lo < 0.5, "drone level moves %.2f dB through the crossfade", hi - lo);
        double wrap, p; seam(loop, 0, wrap, p);
        CHECK(wrap <= p, "drone seam");
        // Without Match Loop the same drone partly cancels in the blend
        Settings plain = s; plain.matchLoop = false;
        Audio loop2; Result r2 = makeTextureLoop(render, plain, loop2);
        std::printf("drone, no match: %s\n", r2.message.c_str());
    }
    {   // Errors
        Audio silent = make(2, 10.0, sr, 0, false, 1), loop;
        Settings s; s.length = 5;
        CHECK(!makeTextureLoop(silent, s, loop).ok, "silence accepted");
        Audio shortRender = make(2, 3.0, sr, 2, false, 1);
        CHECK(!makeTextureLoop(shortRender, s, loop).ok, "too short accepted");
    }
    {   // WAV round trip
        Audio a = make(3, 1.0, sr, 3, false, 5), b; std::string err;
        const char* path = "/tmp/scbake_roundtrip.wav";
        CHECK(writeFloatWav(path, a), "write");
        CHECK(readFloatWav(path, b, err) && b.channels == 3 && b.sampleRate == sr && b.samples == a.samples, "round trip %s", err.c_str());
        std::remove(path);
    }
    {   // A 3-channel node rendered on a 2-output server: two files, interleaved back
        Audio a = make(3, 0.5, sr, 3, false, 9), p0, p1, merged; std::string err;
        p0.channels = p1.channels = 2; p0.sampleRate = p1.sampleRate = sr;
        for(std::size_t f = 0; f < a.frames(); f++){
            p0.samples.push_back(a.samples[f * 3]); p0.samples.push_back(a.samples[f * 3 + 1]);
            p1.samples.push_back(a.samples[f * 3 + 2]); p1.samples.push_back(0.0f);
        }
        writeFloatWav("/tmp/scbake_p0.wav", p0); writeFloatWav("/tmp/scbake_p1.wav", p1);
        CHECK(readParts({"/tmp/scbake_p0.wav", "/tmp/scbake_p1.wav"}, 2, 3, merged, err) && merged.samples == a.samples,
              "parts not interleaved back: %s", err.c_str());
        std::remove("/tmp/scbake_p0.wav"); std::remove("/tmp/scbake_p1.wav");
    }
    {   // Synced loop: a periodic sequence with a tail that rings into the next cycle
        const double cycle = 2.0; const std::size_t L = (std::size_t)(cycle * sr);
        Audio render; render.channels = 2; render.sampleRate = sr;
        render.samples.assign(3 * L * 2 + sr, 0.0f);
        for(std::size_t f = 0; f < render.frames(); f++){
            const double t = (double)f / sr, inCycle = std::fmod(t, 0.5);   // a hit every half second
            const double hit = std::exp(-inCycle * 3.0) * std::sin(2 * M_PI * 220 * t);  // rings past the next hit
            render.samples[f * 2] = (float)(0.3 * hit);
        }
        Audio intro, loop;
        SyncedResult r = makeSyncedLoop(render, cycle, intro, loop);
        std::printf("synced: %s\n", r.message.c_str());
        CHECK(r.ok && r.channels == 1 && loop.frames() == L && intro.frames() == L, "synced shapes");
        CHECK(r.repeat > 0.999, "a periodic source repeats: %.4f", r.repeat);
        CHECK(loop.samples[0] == render.samples[L * 2] && intro.samples[5] == render.samples[10], "cycles taken from the right place");
        // Not periodic: a fresh random draw every cycle
        std::mt19937 rng(4); std::uniform_real_distribution<double> u(-0.3, 0.3);
        for(auto& v : render.samples) v = (float)u(rng);
        SyncedResult r2 = makeSyncedLoop(render, cycle, intro, loop);
        std::printf("random: %s\n", r2.message.c_str());
        CHECK(r2.ok && r2.repeat < 0.1, "random source flagged: %.3f", r2.repeat);
    }
    std::printf(failures ? "FAILED (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
