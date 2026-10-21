// PhysicalBox's voice: see OceanodePhysical.cpp. Two outputs (the two pickups).
// PhysicalVoice.ar(ext, [\freq, 110, \gate, gateSignal, ...]): the inputs by name,
// any left out at its default. Order and defaults: physical::PHYSICAL_PARAMS
// (PhysicalModes.h).
PhysicalVoice : MultiOutUGen {
    *names { ^[\freq, \gate, \hold, \strk, \atk, \rel, \strike, \hardness, \mass, \bounce, \bow, \bowpress, \bowspeed, \rosin, \blow, \breath, \blowtype, \jetratio, \turb, \scrape, \scrapespeed, \rough, \grit, \chatter, \ext, \pos, \pick1, \pick2, \atune, \adecay, \abright, \abore, \astiff, \btune, \bdecay, \bfamily, \bshape, \bstretch, \bmodes, \bbright, \split, \beat, \material, \excite, \listen, \couple, \mutual, \rattle, \rattleth, \tension, \cascade, \drift, \driftrate, \freeze] }
    *defaults { ^[220, 0, 0, 0, 0.08, 0.6, 1, 0.5, 0.3, 0, 0, 0.5, 0.5, 0.2, 0, 0.75, 0, 0.48, 0.2, 0, 0.5, 0.5, 0.3, 0, 0, 0.3, 0.2, 0.58, 0, 3, 0.7, 0, 0, 0, 3, 1, 0, 0, 32, 0.6, 0, 0, 0.5, 0.5, 0.5, 0, 0, 0, 0.3, 0, 0, 0, 0.3, 0] }
    *ar { arg ext = 0.0, pairs;
        var values = this.defaults.copy, names = this.names;
        pairs.pairsDo { |k, v|
            var i = names.indexOf(k.asSymbol);
            if(i.isNil) { Error("PhysicalVoice: no input %".format(k)).throw };
            values[i] = v;
        };
        ^this.multiNewList(['audio', ext] ++ values)
    }
    init { arg ... theInputs;
        inputs = theInputs;
        ^this.initOutputs(2, rate)
    }
}
