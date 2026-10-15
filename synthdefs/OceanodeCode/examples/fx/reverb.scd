// Reverb (FreeVerb2) with pre-delay and a low-cut on the tail. Persistent effect: connect the In port.
// Mangle: room, damp, mix, predelay.
(
SynthDef(\reverb, { |in = 0, out = 0, room = 0.85, damp = 0.4, mix = 0.35, predelay = 0.02, lowcut = 150|
    var dry = In.ar(in, 2);
    var wet = DelayN.ar(HPF.ar(dry, lowcut), 0.2, predelay);
    wet = FreeVerb2.ar(wet[0], wet[1], 1, room, damp);
    Out.ar(out, XFade2.ar(dry, wet, mix * 2 - 1));
}, metadata: (specs: (room: [0, 1], damp: [0, 1], mix: [0, 1], predelay: [0, 0.2], lowcut: [20, 1000, \exp]))).add;
)
