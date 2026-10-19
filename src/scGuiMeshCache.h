#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Cache tessellated, non-interactive primitives (lines/polylines only).
// Widgets, animations and draw-command state stay with the caller. Capture
// absolute positions so a cache hit reproduces the original vertices exactly.
class scGuiMeshCache {
public:
    void invalidate() { valid = false; }

    template<size_t N, class Build>
    void draw(ImDrawList* dl, const std::array<double, N>& key,
              std::uint64_t revision, Build build) {
        const ImVec2 white = dl->_Data->TexUvWhitePixel;
        const ImVec4 line = dl->_Data->TexUvLines ? dl->_Data->TexUvLines[1] : ImVec4();
        const std::array<float, 8> context{(float)dl->Flags, dl->_FringeScale,
            white.x, white.y, line.x, line.y, line.z, line.w};
        if(valid && revision == savedRevision && context == savedContext &&
           savedKey.size() == N && std::equal(key.begin(), key.end(), savedKey.begin())) {
            for(size_t c = 0; c < activeChunks; ++c) {
                const auto& chunk = chunks[c];
                dl->PrimReserve((int)chunk.indices.size(), (int)chunk.vertices.size());
                const unsigned int base = dl->_VtxCurrentIdx;
                std::copy(chunk.vertices.begin(), chunk.vertices.end(), dl->_VtxWritePtr);
                dl->_VtxWritePtr += chunk.vertices.size();
                for(unsigned int index : chunk.indices)
                    *dl->_IdxWritePtr++ = (ImDrawIdx)(base + index);
                dl->_VtxCurrentIdx += (unsigned int)chunk.vertices.size();
            }
            return;
        }

        const unsigned int firstIndex = (unsigned int)dl->IdxBuffer.Size;
        build();
        const unsigned int endIndex = (unsigned int)dl->IdxBuffer.Size;
        activeChunks = 0;
        // A large waveform can cross ImGui's 16-bit vertex-offset boundary.
        // Save each command's portion independently so replay can reserve it
        // safely at a different offset in a later frame's draw list.
        for(const ImDrawCmd& cmd : dl->CmdBuffer) {
            const unsigned int begin = std::max(firstIndex, cmd.IdxOffset);
            const unsigned int end = std::min(endIndex, cmd.IdxOffset + cmd.ElemCount);
            if(begin >= end) continue;
            unsigned int lo = ~0u, hi = 0;
            for(unsigned int i = begin; i < end; ++i) {
                const unsigned int vertex = cmd.VtxOffset + dl->IdxBuffer[i];
                lo = std::min(lo, vertex);
                hi = std::max(hi, vertex);
            }
            if(activeChunks == chunks.size()) chunks.emplace_back();
            auto& chunk = chunks[activeChunks++];
            chunk.vertices.assign(dl->VtxBuffer.Data + lo, dl->VtxBuffer.Data + hi + 1);
            chunk.indices.resize(end - begin);
            for(unsigned int i = begin; i < end; ++i)
                chunk.indices[i - begin] = cmd.VtxOffset + dl->IdxBuffer[i] - lo;
        }
        savedKey.assign(key.begin(), key.end());
        savedRevision = revision;
        savedContext = context;
        valid = true;
    }

private:
    struct Chunk {
        std::vector<ImDrawVert> vertices;
        std::vector<unsigned int> indices;
    };
    std::vector<Chunk> chunks;
    size_t activeChunks = 0;
    std::vector<double> savedKey;
    std::array<float, 8> savedContext{};
    std::uint64_t savedRevision = 0;
    bool valid = false;
};
