(
File.mkdir(d ++ "/" ++ "bufferscope");

// 1. Segment Reader
// Reads 'nPoints' from 'buf' starting at 'startPhase' (0..1) to 'endPhase' (0..1)
// outputting to 'out' control bus.
~makeSegment = { |nPoints|
    SynthDef(("bufferscopeSegment_" ++ nPoints).asSymbol, {
        |buf=0, out=0, startPhase=0, endPhase=1|

        var frames = BufFrames.kr(buf);
        var start  = startPhase * frames;
        var end    = endPhase * frames;

        // Generate 'nPoints' indices interpolated between start and end
        var idx = LinLin.kr(Array.series(nPoints, 0, 1), 0, nPoints-1, start, end);

        // Read and Output
        Out.kr(out, BufRd.kr(1, buf, idx, 1, 1));
    }).writeDefFile(d);
};

// Create a standard segment size (e.g., 64 is network safe)
[32, 64, 128, 256].do(~makeSegment);

"✓ bufferscopeSegment SynthDefs written to ".postln; d.postln;
)