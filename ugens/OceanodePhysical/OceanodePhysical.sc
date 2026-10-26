// PhysicalBox's voice: see OceanodePhysical.cpp. Two outputs (the two pickups).
// PhysicalVoice.ar(ext, [\freq, 110, \gate, gateSignal, ...]): the inputs by name,
// any left out at its default. Order and defaults: physical::PHYSICAL_PARAMS
// (PhysicalModes.h).
PhysicalVoice : MultiOutUGen {
    *names { ^[\freq, \gate, \hold, \strk, \atk, \rel, \strike, \hardness, \mass, \bounce, \bow, \bowpress, \bowspeed, \rosin, \blow, \breath, \blowtype, \jetratio, \turb, \air, \scrape, \scrapespeed, \rough, \grit, \chatter, \ext, \pos, \pick1, \pick2, \posy, \pick1y, \pick2y, \atune, \adecay, \abright, \abore, \astiff, \btune, \bdecay, \bfamily, \bshape, \bstretch, \bmodes, \bbright, \split, \beat, \material, \excite, \listen, \couple, \mutual, \rattle, \rattleth, \tension, \cascade, \drift, \driftrate, \freeze, \damper, \radiator, \radmix, \radtune, \tremrate, \tremdepth, \jawari, \symp, \sympset, \sympdecay, \sympbright, \sympshimmer] }
    *defaults { ^[220, 0, 0, 0, 0.08, 0.6, 1, 0.5, 0.3, 0, 0, 0.5, 0.5, 0.2, 0, 0.75, 0, 0.48, 0.2, 0, 0, 0.5, 0.5, 0.3, 0, 0, 0.3, 0.2, 0.58, 0.37, 0.23, 0.71, 0, 3, 0.7, 0, 0, 0, 3, 1, 0, 0, 32, 0.6, 0, 0, 0.5, 0.5, 0.5, 0, 0, 0, 0.3, 0, 0, 0, 0.3, 0, 0, 0, 0.5, 0, 0, 0.5, 0, 0, 0, 3, 0.5, 3] }
    *ar { arg ext = 0.0, pairs, familyWeights = #[0,0,0,0,0,0];
        var values = this.defaults.copy, names = this.names;
        pairs.pairsDo { |k, v|
            var i = names.indexOf(k.asSymbol);
            if(i.isNil) { Error("PhysicalVoice: no input %".format(k)).throw };
            values[i] = v;
        };
        ^this.multiNewList(['audio', ext] ++ values ++ familyWeights.asArray.wrapExtend(6))
    }
    init { arg ... theInputs;
        inputs = theInputs;
        ^this.initOutputs(2, rate)
    }
}
