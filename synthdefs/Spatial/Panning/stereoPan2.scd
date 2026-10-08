(
// A single mono voice panned to stereo with the signed -1..1 Pan2 control.
// This keeps legacy BubbleA/B pan automation when their new sources are mono.
~synthCreator.value("StereoPan2", {|n|
    var source, pan, signal;
    source = In.ar(OceanodeInput.kr(\in), n);
    pan = OceanodeParameter.ar(\pan, 0, n, -1, 1, "vf");
    signal = Pan2.ar(source, pan);
    Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Signed stereo panner for a mono source", category: "Spatial/Panning");
)
