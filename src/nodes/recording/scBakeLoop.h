//
//  scBakeLoop.h
//  ofxOceanodeSuperCollider
//
//  The texture half of a bake (scBake): turns an offline render of a chain
//  into a loop that plays back without a seam, with the crossfade written into
//  the file so the player is a plain looping read.
//
//  Of the render, `skip` seconds are dropped first (the chain starting from
//  silence is not what it sounds like while it runs). The loop is the next
//  `length` seconds, s[0..L); its first X samples are blended with the X that
//  follow its end, s[L..L+X), fading those out while s[0..X) fades in. The
//  loop's last sample is then followed, on the wrap, by what followed it in
//  the render: no seam to hear, only the blend, which is spread over X.
//
//  The blend keeps the level steady whatever the material: equal power for
//  noisy, unrelated halves (correlation 0), equal amplitude for identical ones
//  (correlation 1), and the law in between for the measured correlation.
//  Match Loop shortens L by up to a tenth (and at most half a second) to
//  where the two halves agree best, so tonal drones crossfade in phase.
//
//  No openFrameworks: tools/bake_test compiles this alone.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace scBakeLoop {

struct Audio {
    int channels = 0;
    int sampleRate = 0;
    std::vector<float> samples;   // interleaved
    std::size_t frames() const { return channels > 0 ? samples.size() / (std::size_t)channels : 0; }
};

struct Settings {
    double skip = 4.0;        // seconds dropped at the start of the render
    double length = 20.0;     // the loop, seconds
    double crossfade = 2.0;   // seconds; at most half the loop
    bool matchLoop = true;
};

struct Result {
    bool ok = false;
    std::string message;
    double loopSeconds = 0.0;
    double correlation = 0.0;
    int channels = 0;
};

inline uint32_t readLE32(const unsigned char* raw){
    return (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
}

// 32-bit float RIFF/WAVE only (what scsynth -N writes here).
inline bool readFloatWav(const std::string& path, Audio& audio, std::string& error){
    std::ifstream file(path, std::ios::binary);
    if(!file){ error = "cannot open " + path; return false; }
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    unsigned char head[12];
    if(!file.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0){
        error = "not a WAVE file"; return false;
    }
    uint16_t format = 0, bits = 0;
    bool haveFormat = false;
    for(;;){
        unsigned char chunk[8];
        if(!file.read(reinterpret_cast<char*>(chunk), 8)){ error = "no data chunk"; return false; }
        const uint32_t size = readLE32(chunk + 4);
        const std::streamoff body = file.tellg();
        if(std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16){
            unsigned char fmt[40] = {};
            const std::streamsize want = (std::streamsize)std::min<uint32_t>(size, 40);
            if(!file.read(reinterpret_cast<char*>(fmt), want)){ error = "bad fmt chunk"; return false; }
            format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            audio.channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            audio.sampleRate = (int)readLE32(fmt + 4);
            bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if(format == 0xFFFE && want >= 26) format = (uint16_t)(fmt[24] | (fmt[25] << 8));
            haveFormat = true;
        }else if(std::memcmp(chunk, "data", 4) == 0){
            if(!haveFormat || format != 3 || bits != 32 || audio.channels <= 0){ error = "not 32-bit float"; return false; }
            const uint64_t bytes = std::min<uint64_t>(size, (uint64_t)std::max<std::streamoff>(0, fileSize - body));
            const uint64_t frames = bytes / ((uint64_t)audio.channels * 4ull);
            audio.samples.resize((std::size_t)(frames * (uint64_t)audio.channels));
            file.read(reinterpret_cast<char*>(audio.samples.data()), (std::streamsize)(audio.samples.size() * 4));
            return true;
        }
        file.clear();
        file.seekg(body + (std::streamoff)size + (std::streamoff)(size & 1), std::ios::beg);
    }
}

inline bool writeFloatWav(const std::string& path, const Audio& audio){
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out) return false;
    auto le16 = [&](uint16_t v){ const unsigned char r[2] = {(unsigned char)(v & 0xFF), (unsigned char)(v >> 8)}; out.write((const char*)r, 2); };
    auto le32 = [&](uint32_t v){ const unsigned char r[4] = {(unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                                                             (unsigned char)((v >> 16) & 0xFF), (unsigned char)(v >> 24)}; out.write((const char*)r, 4); };
    const uint32_t dataBytes = (uint32_t)(audio.samples.size() * 4);
    out.write("RIFF", 4); le32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); le32(16); le16(3); le16((uint16_t)audio.channels);
    le32((uint32_t)audio.sampleRate); le32((uint32_t)(audio.sampleRate * audio.channels * 4));
    le16((uint16_t)(audio.channels * 4)); le16(32);
    out.write("data", 4); le32(dataBytes);
    out.write(reinterpret_cast<const char*>(audio.samples.data()), (std::streamsize)dataBytes);
    return (bool)out;
}

// A render that came back as several files of `filesChannels` channels (the
// server's output width), interleaved into one of `channels`.
inline bool readParts(const std::vector<std::string>& files, int filesChannels, int channels,
                      Audio& audio, std::string& error){
    audio = Audio();
    audio.channels = channels;
    std::vector<Audio> parts(files.size());
    std::size_t frames = 0;
    for(std::size_t i = 0; i < files.size(); i++){
        if(!readFloatWav(files[i], parts[i], error)){ error = files[i] + ": " + error; return false; }
        if(i > 0 && parts[i].sampleRate != parts[0].sampleRate){ error = "the parts' rates differ"; return false; }
        frames = i == 0 ? parts[i].frames() : std::min(frames, parts[i].frames());
    }
    if(parts.empty()){ error = "no render"; return false; }
    audio.sampleRate = parts[0].sampleRate;
    audio.samples.assign(frames * (std::size_t)channels, 0.0f);
    for(std::size_t i = 0; i < parts.size(); i++){
        const int first = (int)i * filesChannels;
        const int count = std::min(parts[i].channels, channels - first);
        for(std::size_t f = 0; f < frames; f++)
            for(int c = 0; c < count; c++)
                audio.samples[f * channels + first + c] = parts[i].samples[f * parts[i].channels + c];
    }
    return true;
}

// Normalised correlation of mono[a..a+n) and mono[b..b+n).
inline double correlation(const std::vector<float>& mono, std::size_t a, std::size_t b, std::size_t n){
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for(std::size_t i = 0; i < n; i++){
        const double x = mono[a + i], y = mono[b + i];
        ab += x * y; aa += x * x; bb += y * y;
    }
    if(aa <= 1e-20 || bb <= 1e-20) return 0.0;
    return ab / std::sqrt(aa * bb);
}

inline Result makeTextureLoop(const Audio& in, const Settings& settings, Audio& loop){
    Result result;
    const int ch = in.channels;
    const double sr = in.sampleRate;
    const std::size_t total = in.frames();
    if(ch <= 0 || sr <= 0 || total == 0){ result.message = "The render is empty"; return result; }

    // Channels past the last one that sounds were never written by the node:
    // the player leaves them silent too, so they are dropped.
    int used = 0;
    for(int c = 0; c < ch; c++){
        float peak = 0.0f;
        for(std::size_t f = 0; f < total; f++) peak = std::max(peak, std::fabs(in.samples[f * ch + c]));
        if(peak > 1e-6f) used = c + 1;
    }
    if(used == 0){ result.message = "The node made no sound during the render"; return result; }

    std::size_t L = (std::size_t)std::llround(std::max(0.05, settings.length) * sr);
    std::size_t X = (std::size_t)std::llround(std::max(0.0, settings.crossfade) * sr);
    X = std::min(X, L / 2);
    std::size_t W = (std::size_t)std::llround(std::max(0.0, settings.skip) * sr);
    if(L + X > total){ result.message = "The render is shorter than the loop and its crossfade"; return result; }
    if(W + L + X > total) W = total - L - X;   // skip less rather than fail

    std::vector<float> mono(total - W);
    for(std::size_t f = 0; f < mono.size(); f++){
        double sum = 0.0;
        for(int c = 0; c < used; c++) sum += in.samples[(W + f) * ch + c];
        mono[f] = (float)sum;
    }

    if(settings.matchLoop && X > 0){
        // Compare the first C samples of the loop with what follows a shorter
        // end; the window is short so the search stays well under a second.
        const std::size_t C = std::max<std::size_t>(64, std::min<std::size_t>(X, (std::size_t)(0.25 * sr)));
        const std::size_t D = std::min<std::size_t>(L / 10, (std::size_t)(0.5 * sr));
        std::size_t bestD = 0;
        double best = -2.0;
        for(std::size_t d = 0; d <= D; d++){
            const std::size_t end = L - d;
            if(end + C > mono.size()) continue;
            const double r = correlation(mono, 0, end, C);
            if(r > best){ best = r; bestD = d; }
        }
        L -= bestD;
        X = std::min(X, L / 2);
    }

    const double rho = X > 0 ? std::max(0.0, std::min(1.0, correlation(mono, 0, L, X))) : 0.0;

    loop.channels = used;
    loop.sampleRate = in.sampleRate;
    loop.samples.assign(L * (std::size_t)used, 0.0f);
    const double halfPi = 1.57079632679489661923;
    for(std::size_t i = 0; i < L; i++){
        if(i < X){
            const double t = (i + 0.5) / (double)X;
            const double fadeIn = std::sin(halfPi * t), fadeOut = std::cos(halfPi * t);
            const double gain = 1.0 / std::sqrt(1.0 + 2.0 * rho * fadeIn * fadeOut);
            for(int c = 0; c < used; c++){
                const double head = in.samples[(W + i) * ch + c];
                const double tail = in.samples[(W + L + i) * ch + c];
                loop.samples[i * used + c] = (float)((head * fadeIn + tail * fadeOut) * gain);
            }
        }else{
            for(int c = 0; c < used; c++) loop.samples[i * used + c] = in.samples[(W + i) * ch + c];
        }
    }

    result.ok = true;
    result.loopSeconds = L / sr;
    result.correlation = rho;
    result.channels = used;
    char text[160];
    std::snprintf(text, sizeof(text), "%.2f s loop, %d ch, crossfade %.2f s (correlation %.2f)",
                  result.loopSeconds, used, X / sr, rho);
    result.message = text;
    return result;
}

// Synced Loop: the render holds three cycles of `cycleSeconds` from the
// transport's start. The first is the intro (the sequence starting from
// silence), the second the loop (it starts with the first one's tails, so
// its wrap is seamless as it is), the third only a check: the more it
// differs from the second, the less the source repeats every cycle.
struct SyncedResult {
    bool ok = false;
    std::string message;
    int channels = 0;
    double repeat = 0.0;     // 1: cycle 3 is cycle 2; 0: nothing alike
    int frames = 0;          // per cycle
};

inline SyncedResult makeSyncedLoop(const Audio& in, double cycleSeconds, Audio& intro, Audio& loop){
    SyncedResult result;
    const int ch = in.channels;
    const std::size_t total = in.frames();
    if(ch <= 0 || in.sampleRate <= 0 || total == 0){ result.message = "The render is empty"; return result; }
    const std::size_t L = (std::size_t)std::llround(cycleSeconds * in.sampleRate);
    if(L < 64){ result.message = "The loop is too short"; return result; }
    if(3 * L > total){ result.message = "The render is shorter than three cycles"; return result; }

    int used = 0;
    for(int c = 0; c < ch; c++){
        float peak = 0.0f;
        for(std::size_t f = 0; f < 3 * L; f++) peak = std::max(peak, std::fabs(in.samples[f * ch + c]));
        if(peak > 1e-6f) used = c + 1;
    }
    if(used == 0){ result.message = "The node made no sound during the render"; return result; }

    // How much of cycle 3 is cycle 2: 1 - residual energy / energy
    double residual = 0.0, energy = 0.0;
    for(std::size_t f = 0; f < L; f++)
        for(int c = 0; c < used; c++){
            const double a = in.samples[(L + f) * ch + c], b = in.samples[(2 * L + f) * ch + c];
            residual += (a - b) * (a - b);
            energy += 0.5 * (a * a + b * b);
        }
    result.repeat = energy > 1e-20 ? std::max(0.0, 1.0 - residual / energy) : 0.0;

    auto copyCycle = [&](std::size_t cycle, Audio& out){
        out.channels = used;
        out.sampleRate = in.sampleRate;
        out.samples.assign(L * (std::size_t)used, 0.0f);
        for(std::size_t f = 0; f < L; f++)
            for(int c = 0; c < used; c++) out.samples[f * used + c] = in.samples[(cycle * L + f) * ch + c];
    };
    copyCycle(0, intro);
    copyCycle(1, loop);

    result.ok = true;
    result.channels = used;
    result.frames = (int)L;
    char text[160];
    std::snprintf(text, sizeof(text), "%.2f s synced loop, %d ch, repeats %.0f %%",
                  L / (double)in.sampleRate, used, result.repeat * 100.0);
    result.message = text;
    return result;
}

} // namespace scBakeLoop
