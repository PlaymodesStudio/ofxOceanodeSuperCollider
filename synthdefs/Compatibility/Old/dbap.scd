d = thisProcess.nowExecutingPath.dirname; // Define d before using it



(
SynthDef(\dbaprouter13, {
    arg in=0, out=0;
    var input, output, ampArray;

    // Read 13 input channels
    input = In.ar(in, 13);

    // Array of 169 amplitudes (13x13 matrix)
    ampArray = \amplitudes.kr(Array.fill(169, 1.0));

    // Create array to store output channels
    output = Array.fill(13, 0);

    // For each input channel
    13.do({ |inputChan|
        var baseIndex = inputChan * 13;

        // Get the 13 amplitude values for this input channel
        var amps = Array.fill(13, { |i|
            ampArray[baseIndex + i]
        });

        // Add this input's contribution to each output channel
        output = output + (input[inputChan] * amps);
    });

    // Output all channels
    Out.ar(out, output);
}).writeDefFile(d);
)

(
SynthDef(\dbap13, {
    arg in=0, out=0;
    var speakerXPos, speakerYPos, sourceXPos, sourceYPos, rolloff;
    var inputs, distances, amplitudes, normFactors, outputSignals;
	var numspeakers=16;
	var numsources=13;

    // Input parameters
    speakerXPos = \speakerx.kr(0!numspeakers);  // Default spreads speakers evenly
    speakerYPos = \speakery.kr(0!numspeakers);
	sourceXPos = \x.kr(0!numsources);  // Default center
    sourceYPos = \y.kr(0!numsources);
    rolloff = \rolloff.kr(6.0);  // Spatial rolloff factor

    // Input audio signals - 13 channels
    inputs = In.ar(in, numsources);

    // Calculate distances and amplitudes
    distances = Array.fill(numspeakers, { |speaker|
        Array.fill(numsources, { |source|
            var dx = speakerXPos[speaker] - sourceXPos[source];
            var dy = speakerYPos[speaker] - sourceYPos[source];
            sqrt(dx.squared + dy.squared);
        });
    });

    // Calculate amplitude for each speaker/source combination
    amplitudes = distances.collect({ |speakerDists|
        speakerDists.collect({ |dist|
            var amp = 1.0 / pow(max(dist, 0.00001), rolloff);  // Avoid division by zero
            amp
        });
    });

    // Normalize amplitudes
    normFactors = Array.fill(numsources, { |source|
        var sumSquared = 0;
        numspeakers.do({ |speaker|
            sumSquared = sumSquared + amplitudes[speaker][source].squared;
        });
        1.0 / sqrt(max(sumSquared, 0.00001));  // Avoid division by zero
    });

    // Apply normalization
    amplitudes = amplitudes.collect({ |speakerAmps, speaker|
        speakerAmps.collect({ |amp, source|
            amp * normFactors[source];
        });
    });

    // Mix and output
    outputSignals = Array.fill(numspeakers, { |speaker|
        Mix.new(
            Array.fill(numsources, { |source|
                inputs[source] * amplitudes[speaker][source];
            })
        );
    });

    Out.ar(out, outputSignals);
}).writeDefFile(d);
)


