// FlutePro's loop (synthdefs/Source/PhysicalModelling/flutepro.scd): see OceanodeFlute.cpp.
// exc is the breath, audio rate. mode 1 is flute.scd's own loop, for tests.
FluteVoice : UGen {
    *ar { arg exc = 0, pitch = 60, fb1 = 0.4, fb2 = 0.4, lpf = 2000, jet = 0.5, jetoffs = -1, overblow = 0,
            track = 3, limit = 2, mode = 0;
        ^this.multiNew('audio', exc, pitch, fb1, fb2, lpf, jet, jetoffs, overblow, track, limit, mode)
    }
}
