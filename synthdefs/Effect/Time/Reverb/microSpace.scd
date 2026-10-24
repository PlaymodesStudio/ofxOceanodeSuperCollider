(
~synthCreator.value("MicroSpace", {|n|
    var input, output;
    var size, mod, lo, hi, position, mix;
    /*
    Native Instruments "Micro Space" (Reaktor 6 macro), rebuilt from its Core cell
    "Space Master Deluxe" with tools/ReaktorPatchExtractor (core_view.py).

    Panel -> cell (the "Master Adaptor" macro):
        PreDel = Size * 0.00022 + 0.002 s, PDSym = 0
        ErSz = 60 - 0.42 Size, ERSym = -2, ErDff = 0.67      (pitches, P2T = 1 / P2F)
        LtSz = 48 - 0.72 Size, LtSym = 1.5, LtDffs = 0.67
        RT60 = 10^(Size / 20) ms
        damping shelves: high 4186 Hz (P108) min(0, Hi) * 0.5 dB, low 65.4 Hz (P36) min(0, Lo) * 0.72 dB
        EQ shelves: same frequencies, Hi dB and Lo dB
        ModF = Size * Mod / 100 Hz, ModA = Size * Mod / 100 ms
        E/L = Position, D/W = Mix
    Cell, per side (L uses Size - Sym, R uses Size + Sym):
        predelay (integer samples) -> 3 early diffusers (P+8, P+4, P), integer delay
        -> damping shelf = early out
        -> + other side's tank feedback -> 3 late diffusers (P+9, P+6, P+3), Hermite delay,
           modulated by 8Phase -> damping shelf = late out
        -> tank delay (P, Hermite, modulated) * RT60 gain -> other side.
    Diffuser: v = x + g y; y = z^-1 Shelf(Delay(v, T)) - g x (damping inside every loop).
    Output: parabolic crossfade early/late, EQ shelves, parabolic crossfade dry/wet.

    Every recursive loop goes through LocalIn/LocalOut: one block of the loop's
    delay comes from there and DelayN/DelayC supply the rest, so the loop
    lengths match Reaktor's exactly (all loops are longer than a 64-sample block).
    DelayC interpolates with the same 4-point Hermite as Reaktor's Delay H.
    */

    size = OceanodeParameterLag.ar(\size, 50, n, 0, 100, "vf", 1/30, true).asArray;
    mod = OceanodeParameterLag.ar(\mod, 0.5, n, 0, 1, "vf", 1/30, true).asArray;
    lo = OceanodeParameterLag.ar(\lo, 0, n, -12, 12, "vf", 1/30, true).asArray;
    hi = OceanodeParameterLag.ar(\hi, 0, n, -12, 12, "vf", 1/30, true).asArray;
    position = OceanodeParameterLag.ar(\position, 0.5, n, 0, 1, "vf", 1/30, true).asArray;
    mix = OceanodeParameterLag.ar(\mix, 0.5, n, 0, 1, "vf", 1/30, true).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;

    // The Box inserts use this same DSP class with legacy-control adapters.
    output = OceanodeMicroSpace.ar(input, size, mod, lo, hi, position, mix);

    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "NI Micro Space reverb rebuilt from its Reaktor Core cell", category: "Effect/Time/Reverb");
)
