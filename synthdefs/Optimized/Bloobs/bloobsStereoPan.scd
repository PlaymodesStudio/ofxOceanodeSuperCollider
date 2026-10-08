(
// Signed Pan2 per voice followed by a stereo mix, as in stereopan78.
~synthCreator.value("BloobsStereoPan", {|n|
    var input, position, panned, signal;
    input = In.ar(OceanodeInput.kr(\in), n);
    position = OceanodeParameter.kr(\position, 0, n, -1, 1, "vf");
    panned = Array.fill(n, {|i| Pan2.ar(input[i], position[i]) });
    signal = Mix.new(panned);
    Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "BLOOBS signed stereo pan mixer", category: "Optimized/Bloobs");
)
