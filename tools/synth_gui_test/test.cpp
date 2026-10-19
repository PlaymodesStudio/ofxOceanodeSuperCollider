#include "scGuiMeshCache.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

// Compare expanded triangles rather than raw index numbers: ImGui may split
// the same geometry at a different 16-bit vertex-offset boundary on replay.
static std::vector<ImDrawVert> triangles(const ImDrawList& dl, int first) {
    std::vector<ImDrawVert> result;
    for(const auto& cmd : dl.CmdBuffer)
        for(unsigned int i = std::max((unsigned int)first, cmd.IdxOffset);
            i < cmd.IdxOffset + cmd.ElemCount; ++i)
            result.push_back(dl.VtxBuffer[cmd.VtxOffset + dl.IdxBuffer[i]]);
    return result;
}

static void equal(const std::vector<ImDrawVert>& a, const std::vector<ImDrawVert>& b) {
    assert(a.size() == b.size());
    for(size_t i = 0; i < a.size(); ++i) {
        assert(a[i].pos.x == b[i].pos.x && a[i].pos.y == b[i].pos.y);
        assert(a[i].uv.x == b[i].uv.x && a[i].uv.y == b[i].uv.y);
        assert(a[i].col == b[i].col);
    }
}

static void reset(ImDrawList& dl, int prefix = 0) {
    dl._ResetForNewFrame();
    dl.PushClipRect(ImVec2(0, 0), ImVec2(50000, 50000));
    // Existing commands, vertices and index offsets must not enter the cache.
    for(int i = 0; i < prefix; ++i)
        dl.AddLine(ImVec2((float)i, 0), ImVec2((float)i, 4), IM_COL32_WHITE);
}

static void waveform(ImDrawList& dl, int count, float x = 0, ImU32 col = IM_COL32_WHITE) {
    for(int i = 0; i < count; ++i) {
        const float h = 30.0f + std::sin(i * 0.11f) * 25.0f;
        dl.AddLine(ImVec2(x + i, 70 - h), ImVec2(x + i, 70 + h), col);
    }
}

int main() {
    ImGui::CreateContext();
    {
        ImDrawListSharedData shared;
        std::array<ImVec4, IM_DRAWLIST_TEX_LINES_WIDTH_MAX + 1> lineUV{};
        lineUV[1] = ImVec4(0.1f, 0.2f, 0.3f, 0.4f);
        shared.TexUvLines = lineUV.data();
        shared.TexUvWhitePixel = ImVec2(0.5f, 0.5f);
        ImDrawList dl(&shared), reference(&shared);
        for(int flags : {0, (int)ImDrawListFlags_AntiAliasedLines,
                        ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedLinesUseTex}) {
            shared.InitialFlags = flags | ImDrawListFlags_AllowVtxOffset;
            scGuiMeshCache cache;
            int builds = 0;
            std::array<double, 3> key{1000, 0, (double)IM_COL32_WHITE};
            auto build = [&] { ++builds; waveform(dl, (int)key[0], (float)key[1], (ImU32)key[2]); };
            reset(dl, 3); const int first = dl.IdxBuffer.Size;
            cache.draw(&dl, key, 0, build);
            auto original = triangles(dl, first);
            for(int prefix : {0, 17, 16000}) {
                reset(dl, prefix); const int begin = dl.IdxBuffer.Size;
                cache.draw(&dl, key, 0, build);
                equal(original, triangles(dl, begin));
                assert(builds == 1);
            }
            // Reuse current clipping, rather than the clipping of the cold frame.
            reset(dl); dl.PushClipRect(ImVec2(20, 30), ImVec2(100, 120));
            cache.draw(&dl, key, 0, build);
            assert(dl.CmdBuffer.back().ClipRect.x == 20 && dl.CmdBuffer.back().ClipRect.w == 120);
            assert(builds == 1);
            cache.draw(&dl, key, 1, build); assert(builds == 2);
            cache.invalidate(); cache.draw(&dl, key, 1, build); assert(builds == 3);
            // Moving/resizing, changing colours and updating live data all rebuild.
            for(int component = 0; component < 3; ++component) {
                key[component] += component == 2 ? -256 : 1;
                reset(dl); cache.draw(&dl, key, 1, build);
                reset(reference); waveform(reference, (int)key[0], (float)key[1], (ImU32)key[2]);
                equal(triangles(dl, 0), triangles(reference, 0));
            }
            const int before = builds;
            shared.TexUvWhitePixel.x += 0.01f;
            reset(dl); cache.draw(&dl, key, 1, build); assert(builds == before + 1);
            reset(reference); waveform(reference, (int)key[0], (float)key[1], (ImU32)key[2]);
            equal(triangles(dl, 0), triangles(reference, 0));
            lineUV[1].x += 0.02f;
            reset(dl); cache.draw(&dl, key, 1, build); assert(builds == before + 2);
            dl._FringeScale = 1.5f; cache.draw(&dl, key, 1, build); assert(builds == before + 3);
        }

        // Capture/replay a waveform which itself crosses vertex-offset boundaries.
        shared.InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AllowVtxOffset;
        scGuiMeshCache large;
        reset(dl, 7); int begin = dl.IdxBuffer.Size;
        large.draw(&dl, std::array<double, 1>{20000}, 0, [&] { waveform(dl, 20000); });
        auto original = triangles(dl, begin);
        reset(dl, 100); begin = dl.IdxBuffer.Size;
        large.draw(&dl, std::array<double, 1>{20000}, 0, [&] { assert(false); });
        equal(original, triangles(dl, begin));

        // Empty geometry remains empty, including following a non-empty frame.
        reset(dl);
        large.draw(&dl, std::array<double, 1>{0}, 1, [] {});
        reset(dl);
        large.draw(&dl, std::array<double, 1>{0}, 1, [] { assert(false); });
        assert(dl.IdxBuffer.Size == 0 && dl.VtxBuffer.Size == 0);

        // The mini-EQ's complete 256-point polyline uses the same path.
        std::array<ImVec2, 256> points;
        for(int i = 0; i < 256; ++i) points[i] = ImVec2(i * 0.3f, 20 + std::sin(i * 0.03f) * 12);
        scGuiMeshCache curve;
        reset(dl);
        curve.draw(&dl, std::array<double, 1>{256}, 0, [&] { dl.AddPolyline(points.data(), 256, IM_COL32_WHITE, 0, 1); });
        original = triangles(dl, 0);
        reset(dl, 3); begin = dl.IdxBuffer.Size;
        curve.draw(&dl, std::array<double, 1>{256}, 0, [] { assert(false); });
        equal(original, triangles(dl, begin));

        scGuiMeshCache bench;
        reset(dl);
        bench.draw(&dl, std::array<double, 1>{1200}, 0, [&] { waveform(dl, 1200); });
        auto start = std::chrono::steady_clock::now();
        for(int frame = 0; frame < 1000; ++frame) { reset(dl); waveform(dl, 1200); }
        const auto direct = std::chrono::steady_clock::now() - start;
        start = std::chrono::steady_clock::now();
        for(int frame = 0; frame < 1000; ++frame) {
            reset(dl); bench.draw(&dl, std::array<double, 1>{1200}, 0, [] { assert(false); });
        }
        const auto cached = std::chrono::steady_clock::now() - start;
        std::cout << "1200-column waveform, 1000 frames: "
                  << std::chrono::duration<double, std::milli>(direct).count() << " ms direct / "
                  << std::chrono::duration<double, std::milli>(cached).count() << " ms cached\n";
    }
    ImGui::DestroyContext();
    std::cout << "PASS: identical triangles, colours, AA, clipping and vertex-offset rollover\n";
}
