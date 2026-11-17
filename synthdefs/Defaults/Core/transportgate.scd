// "Mute When Stopped" (inspector switch of every SC node; scNode,
// serverManager): one of these sits right after a node that has it on, for
// each of its outputs, and fades that output's bus to silence while the
// transport is stopped. The node itself keeps running, so it comes back where
// it was, and whatever follows it (a reverb) still rings out.
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);

SynthDef(\OceanodeTransportGate, {
    var bus = \bus.kr(0);
    ReplaceOut.ar(bus, In.ar(bus, 128) * \gate.kr(1, 0.03));
}).writeDefFile(d);
)
