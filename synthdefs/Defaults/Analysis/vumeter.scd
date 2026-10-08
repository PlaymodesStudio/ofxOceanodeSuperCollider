// vumeter.scd - Simple VU meter SynthDef
var d, vumeterDir;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
vumeterDir = d +/+ "/vumeter";
File.mkdir(d);
File.mkdir(vumeterDir);

(1..40).do({|numChannels|
    var synthName = ("vumeter" ++ numChannels).asSymbol;
    SynthDef(synthName, {
        arg in=0, out=0, vubus=(-1);
        var sig, vuSig, cleaned;
        var vuattacktime, vureleasetime;

        sig = In.ar(in, numChannels);

        vuattacktime = NamedControl.kr(\vuattacktime, 10.0) * 0.001;
        vureleasetime = NamedControl.kr(\vureleasetime, 300.0) * 0.001;

        // NaN/inf protection: CheckBadValues returns 0 on bad values.
        // Multiply sig by it so bad samples become silence.
        // sig.asArray.collect returns an Array — wrap in UGen graph with
        // a simple channel-wise approach that stays in UGen land.
        cleaned = numChannels.collect({ |i|
            var ch = sig.asArray[i];
            ch * (1 - CheckBadValues.ar(ch, id: i, post: 0));
        });

        // cleaned is now an Array of UGens — SC handles Array args to
        // Amplitude.kr and Out.ar just fine (multichannel expansion).
        vuSig = LagUD.kr(Amplitude.kr(cleaned), vuattacktime, vureleasetime);
        vuSig = vuSig.max(0).min(2);

        Out.kr(vubus, vuSig);
        Out.ar(out, cleaned);
    }).writeDefFile(vumeterDir);

    if([1, 2, 4, 8, 16].includes(numChannels)) {
        ("✅ Created: " ++ synthName ++ " (" ++ numChannels ++ " channels)").postln;
    };
});

("✅ VU Meter SynthDefs created successfully").postln;