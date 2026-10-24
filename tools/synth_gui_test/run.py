#!/usr/bin/env python3
"""Test cached geometry using the installed ImGui, without a window or GPU."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
imgui = root.parent / "ofxImGui/libs/imgui/src"
with tempfile.TemporaryDirectory(prefix="synth_gui_test_") as directory:
    # Compile the real ImGui renderer with a standalone configuration, without
    # the OF adapter's platform/GL dependencies. Production TUs are checked
    # separately against that adapter's actual configuration.
    pathlib.Path(directory, "imconfig.h").write_text("#pragma once\n")
    # Exercise the production ring-to-display update as well: no incoming
    # data must preserve the revision; every dirty update must keep age order.
    source = (root / "src/nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp").read_text()
    start = source.index("void scGrainBoxPoly::updateLivePeaks(")
    end = source.index("\n}\n", start) + 2
    live_test = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
class scGrainBoxPoly { public:
    static constexpr int WAVEFORM_BINS=4096;
    struct Playhead {
        struct { std::vector<float> peaks,lows; std::uint64_t waveformRevision=0; } live;
        std::vector<float> liveRing,liveRingMin;
        float liveHead=0;
        bool livePeaksDirty=true;
    };
    void updateLivePeaks(Playhead&);
};
''' + source[start:end] + r'''
int main() {
    scGrainBoxPoly node;
    scGrainBoxPoly::Playhead ph;
    node.updateLivePeaks(ph);
    assert(ph.live.waveformRevision==1 && !ph.livePeaksDirty);
    for(int head : {0,1,2048,4095}) {
        for(int b=0;b<4096;++b) { ph.liveRing[b]=float(b); ph.liveRingMin[b]=-float(b); }
        ph.liveHead=float(head)/4096; ph.livePeaksDirty=true;
        node.updateLivePeaks(ph);
        auto revision=ph.live.waveformRevision;
        for(int b=0;b<4096;++b) {
            int r=((head-b)%4096+4096)%4096;
            assert(ph.live.peaks[b]==float(r) && ph.live.lows[b]==-float(r));
        }
        node.updateLivePeaks(ph); assert(ph.live.waveformRevision==revision);
        ph.liveRing[head]=0.75f; ph.livePeaksDirty=true;
        node.updateLivePeaks(ph);
        assert(ph.live.peaks[0]==0.75f && ph.live.waveformRevision==revision+1);
    }
    ph.live.peaks.clear(); node.updateLivePeaks(ph);
    assert(ph.live.peaks.size()==4096);
    std::cout << "PASS: live waveform age order, dirty updates and idle reuse\n";
}
'''
    live_source = pathlib.Path(directory, "live_test.cpp")
    live_source.write_text(live_test)
    live_output = pathlib.Path(directory, "live_test")
    subprocess.run(["clang++", "-std=c++17", "-O2", str(live_source), "-o", str(live_output)], check=True)
    subprocess.run([str(live_output)], check=True)
    for bits in (16, 32):
        output = pathlib.Path(directory) / f"test{bits}"
        command = ["clang++", "-std=c++17", "-O2", "-I" + directory,
                   "-DIMGUI_DISABLE_OSX_FUNCTIONS", "-DIMGUI_DISABLE_WIN32_FUNCTIONS",
                   "-I" + str(imgui), "-I" + str(root / "src")]
        if bits == 32:
            command += ["-DImDrawIdx=unsigned int"]
        command += [str(pathlib.Path(__file__).with_name("test.cpp"))]
        command += [str(imgui / name) for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")]
        subprocess.run(command + ["-o", str(output)], check=True)
        print(f"ImDrawIdx {bits}-bit", flush=True)
        subprocess.run([str(output)], check=True)
