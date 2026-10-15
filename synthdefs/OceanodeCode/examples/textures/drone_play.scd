// {}.play works too: it becomes a persistent synth with an Out port,
// and the function's arguments become parameters.
{ |freq = 55, spread = 0.4, amp = 0.4|
    var sig = Splay.ar(Array.fill(6, { |i| LFSaw.ar(freq * (i + 1) * LFNoise1.kr(0.1, spread * 0.01, 1)) / (i + 1) }));
    LPF.ar(sig, 1200) * amp
}.play;
