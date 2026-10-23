(
~synthCreator.value("AudioMerge", {|n, variables|
    var signals = Array.fill(variables[0], {|i|
        In.ar(OceanodeInput.kr(("in" ++ (i + 1)).asSymbol), n)
    });
    Out.ar(OceanodeOutput.kr(\out), signals.sum);
}, description: "Sum audio inputs channel by channel at unity gain", category: "Mixing",
    variables: ["nInputs"], variableDimensions: [32]);
)
