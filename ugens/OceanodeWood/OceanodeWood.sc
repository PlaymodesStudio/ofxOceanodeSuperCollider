// MarimbaPro's voice: see OceanodeWood.cpp. Defaults: SteamPipeBox's
// GlassWoodTemplate snapshot, played in tune.
WoodPipe : UGen {
    *ar { arg gate = 0, pitch = 60, hardness = 0.7585, strike = 4.04, decay = 2.67, cutp = 100.92,
            diffusion = 0.417, ratio = 3.23, push = 0.646, symmetry = 0.669, drive = 1,
            vibrato = 0, vibfreq = 0,
            velamp = 1, velbright = 0.354, keybright = 0.315, keycut = 0.803, keydecay = 0.819, raw = 0;
        ^this.multiNew('audio', gate, pitch, hardness, strike, decay, cutp, diffusion, ratio,
            push, symmetry, drive, vibrato, vibfreq, velamp, velbright, keybright, keycut, keydecay, raw)
    }
}
