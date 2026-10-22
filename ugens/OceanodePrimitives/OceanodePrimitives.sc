// Reusable sample-rate DSP primitives. See README.md for contracts.
CongruentialRandomKernel : UGen {
    *ar { arg clock=0, seed=0, reset=0, mode=0;
        ^this.multiNew('audio', clock, seed, reset, mode)
    }
    checkInputs { ^this.checkValidInputs }
}
CompensatedSVF : UGen {
    *ar { arg in=0, freq=1000, res=0, type=0;
        ^this.multiNew('audio', in, freq, res, type)
    }
    checkInputs { ^this.checkValidInputs }
}
FilteredFeedbackDelay : UGen {
    *ar { arg in=0, time=8, feedback=0.874, freq=4000, res=0.14, type=0, mix=1;
        ^this.multiNew('audio', in, time, feedback, freq, res, type, mix)
    }
    checkInputs { ^this.checkValidInputs }
}
EventADSRKernel : UGen {
    *ar { arg trigger=0, gate=1, attack=1, decay=100, sustain=0, release=1, peak=1;
        ^this.multiNew('audio', trigger, gate, attack, decay, sustain, release, peak)
    }
    checkInputs { ^this.checkValidInputs }
}

AsymmetricFollower : UGen {
    *ar { arg in=0, attack=0.001, release=0.1;
        ^this.multiNew('audio', in, attack, release)
    }
    checkInputs { ^this.checkValidInputs }
}
