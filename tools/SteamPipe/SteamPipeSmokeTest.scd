// Boots a temporary server, registers the EEL2 sidecar, loads SteamPipe1,
// exercises note-on/note-off, and exits. Server-side DynGen parse errors make
// this test fail visibly in the post output.

var here = thisProcess.nowExecutingPath.dirname;
var repository = (here +/+ "../..").standardizePath;
var sourcePath = repository +/+ "example/bin/data/Supercollider/DynGenDefs/SteamPipe.eel2";
var synthDefPath = repository +/+ "example/bin/data/Supercollider/Synthdefs/PhysicalModeling/SteamPipe/SteamPipe1.scsyndef";

s.options.numOutputBusChannels = 2;
s.options.numInputBusChannels = 0;
s.options.numWireBufs = 8192;
s.options.memSize = 262144;

s.waitForBoot({
    var definition = DynGenDef(\steamPipeCore, File.readAllString(sourcePath));
    var synth;
    definition.send(s);
    s.sendMsg(\d_load, synthDefPath);
    s.sync;

    synth = Synth(\SteamPipe1, [
        \out, 0,
        \pitch, 60,
        \velocity, 0.8,
        \gate, 1,
        \gain, -12
    ]);

    Routine({
        0.75.wait;
        synth.set(\gate, 0);
        0.75.wait;
        s.quit;
        0.exit;
    }).play(AppClock);
});

s.boot;
