(
~synthCreator.value("HardSync", {|n|
    var freq, syncFreqRatio, masterOsc, slaveOsc, sync, amp;
    
    // Parameters
    freq = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true).midicps;
    syncFreqRatio = OceanodeParameterLag.ar(\ratio, 1, n, 0.1, 10, "vf", 1/30, true);
    amp = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true);
    
    // Generate oscillators
    masterOsc = Pulse.ar(freq, 0.5);
    slaveOsc = Saw.ar((freq * syncFreqRatio).min(SampleRate.ir * 0.45));   // above Nyquist Saw gave NaN
    
    // Apply hard sync
    sync = slaveOsc * (masterOsc < 0);
    
    // Output
    Out.ar(OceanodeOutput.kr(\out), LeakDC.ar(sync, 0.9995) * amp);   // gating the saw left a DC offset
    
}, description: "Hard sync oscillator with sawtooth slave", category: "Source/Sync");
)