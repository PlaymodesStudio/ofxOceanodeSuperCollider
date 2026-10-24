#pragma once
#include <algorithm>
#include <cstring>
#include <utility>

// The allpass sits INSIDE a feedback resonator. Mixing its internal delay taps
// changes loop phase and can move the dominant resonance abruptly. During a
// fractional activation run both complete loop states and blend their outputs.
// Buffers are allocated by the UGen constructor; idle endpoints run one kernel.
template<class Kernel>
void sampleAllpassMorph(Kernel& main, Kernel& other, bool& active, int& mode, size_t cells) {
    const double amount=std::clamp(main._allpass_on,0.0,1.0);
    // A control-rate Lag can settle a few float ulps away from its endpoint.
    const bool partial=amount>1e-6 && amount<1.0-1e-6;
    if(partial && other.buf) {
        if(!active) {
            auto* buffer=other.buf;
            other=main;
            other.buf=buffer;
            std::memcpy(other.buf,main.buf,cells*sizeof(double));
            active=true;
        }
        main._allpass_on=mode;
        other._allpass_on=1-mode;
        main.sample(); other.sample();
        const double weight=mode ? 1-amount : amount;
        main.out0=main.out0*(1-weight)+other.out0*weight;
        main.out1=main.out1*(1-weight)+other.out1*weight;
        return;
    }
    const int target=amount>=0.5 ? 1 : 0;
    if(active && target!=mode) std::swap(main,other);
    active=false; mode=target;
    // Allocation failure retains the original fractional single-loop behavior.
    main._allpass_on=other.buf ? double(target) : amount;
    main.sample();
}
