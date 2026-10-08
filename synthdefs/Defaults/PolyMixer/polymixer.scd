(
var d, polymixerDir;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
polymixerDir = d +/+ "/polymixer";
File.mkdir(d);
File.mkdir(polymixerDir);

[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,20,21,24,25,30,32,40].do({|numChannels|
    var synthName = ("polyMixerTrack" ++ numChannels).asSymbol;
    SynthDef(synthName, {
        arg in=0, out=0, mute=0, balance=0, vuBus=(-1);
        var sig, level, masterLevel, vuSig, processedSig, inputSig, cleaned;
        var vuAttackTime, vuReleaseTime;
        var normPositions, rightFavor, leftFavor, balanceGains;
        var balanceSq, panPos, leftGain, rightGain;

        // --- INPUT ---
        inputSig = In.ar(in, numChannels);
        sig = Gate.ar(inputSig, (in >= 0));

        // --- PARAMETERS ---
        level = NamedControl.kr(\level, Array.fill(numChannels, 0.75));
        masterLevel = NamedControl.kr(\masterLevel, Array.fill(numChannels, 1.0));
        vuAttackTime = NamedControl.kr(\vuAttackTime, 10.0) * 0.001;
        vuReleaseTime = NamedControl.kr(\vuReleaseTime, 300.0) * 0.001;

        // --- BALANCE LOGIC ---
        balanceGains = if(numChannels == 2, {
            panPos = balance * 0.5 + 0.5;
            leftGain = (1 - panPos).sqrt;
            rightGain = panPos.sqrt;
            [leftGain, rightGain];
        }, {
            normPositions = Array.series(numChannels, 0, 1/(numChannels-1).max(1));
            balanceSq = balance.sign * (balance.abs.squared);
            leftFavor = 1.0 - (normPositions * balanceSq.min(0).abs);
            rightFavor = 1.0 - ((1.0 - normPositions) * balanceSq.max(0));
            (leftFavor * rightFavor).squared;
        });

        sig = sig * balanceGains;

        // --- LEVELS & OUTPUT ---
        processedSig = sig * level.lag(0.05);
        processedSig = processedSig * (1 - mute);

        // --- NaN/inf PROTECTION (same fix as vumeter) ---
        // CheckBadValues returns 1 on bad, 0 on clean — so (1 - cbv) gates out bad samples
        cleaned = numChannels.collect({ |i|
            var ch = processedSig.asArray[i];
            ch * (1 - CheckBadValues.ar(ch, id: i, post: 0));
        });

        // VU — now operates on sanitized signal
        vuSig = LagUD.kr(Amplitude.kr(cleaned), vuAttackTime, vuReleaseTime);
        Out.kr(vuBus, vuSig.max(0).min(2) * (vuBus >= 0));

        // Master & Out — also use cleaned signal so NaN never reaches output bus
        processedSig = cleaned * masterLevel.lag(0.05);
        Out.ar(out, processedSig);
    }).writeDefFile(polymixerDir);
    ("Created: " ++ synthName).postln;
});
"DONE".postln;
)