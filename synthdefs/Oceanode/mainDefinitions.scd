//Make shure you have OceanodeParameter pseudo-Ugen in user extensions folder
Platform.userExtensionDir;
//You should have a class containing the file oceanodeParameter.sc with the following contents:

/*
OceanodeParameter {
	*ar {arg name, default, size, min, max, units;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: "a"++units))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}

	*kr {arg name, default, size, min, max, units;
		^name.asSymbol.kr(default!size, spec: ControlSpec(min, max, default: default, units: units));
	}
}

OceanodeParameterLag {
	*ar {arg name, default, size, min, max, units, lagtime = 0, fixedLag = false;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, lag: lagtime, fixedLag: fixedLag, spec: ControlSpec(min, max, default: default, units: "a"++units))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}

	*kr {arg name, default, size, min, max, units, lagtime = 0, fixedLag = false;
		^name.asSymbol.kr(default!size, lag: lagtime, fixedLag: fixedLag, spec: ControlSpec(min, max, default: default, units: units));
	}
}

OceanodeParameterDropdown {
	*ar {arg name, default, size, options;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "ad:"++options))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
	*kr {arg name, default, size, options;
		^name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "d:"++options));
	}
}

OceanodeParameterFloatDropdown {
	*ar {arg name, default, size, options;
		^Select.ar(
			(name ++ "_sel").asSymbol.kr(0),
			[
				K2A.ar(name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "adf:"++options))),
				(name ++ "_ar").asSymbol.ar(default!size);
			]
		)
	}
	*kr {arg name, default, size, options;
		^name.asSymbol.kr(default!size, spec: ControlSpec(default: default, units: "df:"++options));
	}
}


OceanodeInput{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "input"));
	}
}

OceanodeOutput{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "output"));
	}
}

OceanodeBuffer{
  *kr {arg name, default = -1;
    ^name.asSymbol.kr(default, spec: ControlSpec(units: "buffer"));
  }
}

OceanodeInternalBuffer{
	*kr {arg name;
		^name.asSymbol.kr(0, spec: ControlSpec(units: "internalbuffer"));
	}
}
*/

//Default params
d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
~maxVoices = 16;

//Helper funtion to create at once a number of synths from 1 to maxVoices
(
~synthCreator = {|name, func, description = "", category = "", variables, variableDimensions|
	var createSynths, variableNames, variableDimensionString, placeholderArgs;
	("Writing Synth " ++ name ++ " --------------------------").postln;
	File.mkdir(d ++ "/" ++ name);
	description = description.replace(" ", "_");
	description = description.replace(",", "|");
	if(variables.size == 0){
		variableNames = "";
		variableDimensionString = "";
	}{
		variableNames = variables.join(":");
		variableDimensionString = variableDimensions.collect(_.asString).join(":");
	};
	//Create first synth for metadata.
	(1..variables.size).do{
		placeholderArgs = placeholderArgs.add(1);
	};
	SynthDef.new(name, {
			var sig = SynthDef.wrap(func, prependArgs: [1, placeholderArgs]);
	}, metadata: (name: name, type: "source", description: description, category: category, variables: variableNames, variableDimensions: variableDimensionString)).writeDefFile(d ++ "/" ++ name);

	 // Recursive function to handle variable number of variables
    createSynths = { |n, vars, varDim, args|
		var synthdefName = name ++ (n).asSymbol;
        if (args.size == vars.size) {
            // Base case: No more variables to process
			if(args.size > 0){
				(1..args.size).do{|a|
					synthdefName = synthdefName ++ "_" ++ (args[a-1]).asSymbol;
				};
			}{};
		    synthdefName.postln;
            SynthDef.new(synthdefName, {
                var sig = SynthDef.wrap(func, prependArgs: [n, args]);
            }).writeDefFile(d ++ "/" ++ name, mdPlugin: AbstractMDPlugin); //AbstractMDPlugin to disable metadata
			args = [];
		}{
            // Recursive case: Process current variable and call next
			args = args.add(1);
			(1..varDim[args.size-1]).do { |varValue|
                // Recurse to handle the next variable
				args[args.size-1] = varValue;
				createSynths.(n, vars, varDim, args);
            };
			args = args.removeAt(args.size-1);
        };
    };

    // Create synths for each voice number
    (1..~maxVoices).do { |n|
        createSynths.(n, variables, variableDimensions);
    };
	"Writing finished".postln;
};
);

////////////////////////////////////////////////////////////////////

~maxVoices = 1;
// Helper function to create a single synth with maxVoices
(
~synthCreator = {|name, func, description = "", category = "", variables, variableDimensions|
    var createSynth, variableNames, variableDimensionString, placeholderArgs;

    ("Writing Single Synth " ++ name ++ " with " ++ ~maxVoices ++ " voices --------------------------").postln;
    File.mkdir(d ++ "/" ++ name);

    description = description.replace(" ", "_");
    description = description.replace(",", "|");

    if(variables.size == 0){
        variableNames = "";
        variableDimensionString = "";
    }{
        variableNames = variables.join(":");
        variableDimensionString = variableDimensions.collect(_.asString).join(":");
    };

    // Create placeholder args for metadata
    (1..variables.size).do{
        placeholderArgs = placeholderArgs.add(1);
    };

    // Create metadata synth
    SynthDef.new(name, {
        var sig = SynthDef.wrap(func, prependArgs: [~maxVoices, placeholderArgs]);
    }, metadata: (
        name: name,
        type: "source",
        description: description,
        category: category,
        variables: variableNames,
        variableDimensions: variableDimensionString
    )).writeDefFile(d ++ "/" ++ name);

    // Function to create a single synth with variable combinations
    createSynth = { |n, vars, varDim, args|
        var synthdefName = name ++ (n).asSymbol;

        if (args.size == vars.size) {
            // Base case: No more variables to process
            if(args.size > 0){
                (1..args.size).do{|a|
                    synthdefName = synthdefName ++ "_" ++ (args[a-1]).asSymbol;
                };
            };

            synthdefName.postln;
            SynthDef.new(synthdefName, {
                var sig = SynthDef.wrap(func, prependArgs: [n, args]);
            }).writeDefFile(d ++ "/" ++ name, mdPlugin: AbstractMDPlugin); // AbstractMDPlugin to disable metadata
            args = [];
        }{
            // Recursive case: Process current variable and call next
            args = args.add(1);
            (1..varDim[args.size-1]).do { |varValue|
                // Recurse to handle the next variable
                args[args.size-1] = varValue;
                createSynth.(n, vars, varDim, args);
            };
            args = args.removeAt(args.size-1);
        };
    };

    // Create only one synth with the specified number of voices from global maxVoices
    createSynth.(~maxVoices, variables, variableDimensions);

    "Writing finished".postln;
};
);




//////////////////////////////////////


~numChans = [1, 2, 4, 6, 21, 126];
~numChans = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,20,21,24,25,30,32,40];

(
~synthCreator = {|name, func, description = "", category = "", variables, variableDimensions|
    var createSynths, variableNames, variableDimensionString, placeholderArgs, channelsToCreate;


    channelsToCreate = ~numChans;


    ("Writing Synth " ++ name ++ " for channels: " ++ channelsToCreate ++ " --------------------------").postln;
    File.mkdir(d ++ "/" ++ name);
    description = description.replace(" ", "_");
    description = description.replace(",", "|");
    if(variables.size == 0){
        variableNames = "";
        variableDimensionString = "";
    }{
        variableNames = variables.join(":");
        variableDimensionString = variableDimensions.collect(_.asString).join(":");
    };
    //Create first synth for metadata.
    (1..variables.size).do{
        placeholderArgs = placeholderArgs.add(1);
    };
    SynthDef.new(name, {
            var sig = SynthDef.wrap(func, prependArgs: [1, placeholderArgs]);
    }, metadata: (name: name, type: "source", description: description, category: category, variables: variableNames, variableDimensions: variableDimensionString)).writeDefFile(d ++ "/" ++ name);

     // Recursive function to handle variable number of variables
    createSynths = { |n, vars, varDim, args|
        var synthdefName = name ++ (n).asSymbol;
        if (args.size == vars.size) {
            // Base case: No more variables to process
            if(args.size > 0){
                (1..args.size).do{|a|
                    synthdefName = synthdefName ++ "_" ++ (args[a-1]).asSymbol;
                };
            }{};
            synthdefName.postln;
            SynthDef.new(synthdefName, {
                var sig = SynthDef.wrap(func, prependArgs: [n, args]);
            }).writeDefFile(d ++ "/" ++ name, mdPlugin: AbstractMDPlugin); //AbstractMDPlugin to disable metadata
            args = [];
        }{
            // Recursive case: Process current variable and call next
            args = args.add(1);
            (1..varDim[args.size-1]).do { |varValue|
                // Recurse to handle the next variable
                args[args.size-1] = varValue;
                createSynths.(n, vars, varDim, args);
            };
            args = args.removeAt(args.size-1);
        };
    };

    // Create synths for each specified channel number
    channelsToCreate.do { |n|
        createSynths.(n, variables, variableDimensions);
    };
    "Writing finished".postln;
};
);

///////
// Modified synthCreator that preserves metadata
(
~synthCreatorWithMetadata = {|name, func, description = "", category = "", variables, variableDimensions|
    var createSynths, variableNames, variableDimensionString, placeholderArgs;
    ("Writing Synth " ++ name ++ " --------------------------").postln;
    File.mkdir(d ++ "/" ++ name);
    description = description.replace(" ", "_");
    description = description.replace(",", "|");
    if(variables.size == 0){
        variableNames = "";
        variableDimensionString = "";
    }{
        variableNames = variables.join(":");
        variableDimensionString = variableDimensions.collect(_.asString).join(":");
    };
    //Create first synth for metadata.
    (1..variables.size).do{
        placeholderArgs = placeholderArgs.add(1);
    };
    SynthDef.new(name, {
        var sig = SynthDef.wrap(func, prependArgs: [1, placeholderArgs]);
    }, metadata: (name: name, type: "source", description: description, category: category, variables: variableNames, variableDimensions: variableDimensionString)).writeDefFile(d ++ "/" ++ name);

    // Recursive function to handle variable number of variables
    createSynths = { |n, vars, varDim, args|
        var synthdefName = name ++ (n).asSymbol;
        if (args.size == vars.size) {
            // Base case: No more variables to process
            if(args.size > 0){
                (1..args.size).do{|a|
                    synthdefName = synthdefName ++ "_" ++ (args[a-1]).asSymbol;
                };
            }{};
            synthdefName.postln;
            SynthDef.new(synthdefName, {
                var sig = SynthDef.wrap(func, prependArgs: [n, args]);
            }).writeDefFile(d ++ "/" ++ name); // REMOVED mdPlugin: AbstractMDPlugin
            args = [];
        }{
            // Recursive case: Process current variable and call next
            args = args.add(1);
            (1..varDim[args.size-1]).do { |varValue|
                // Recurse to handle the next variable
                args[args.size-1] = varValue;
                createSynths.(n, vars, varDim, args);
            };
            args = args.removeAt(args.size-1);
        };
    };

    // Create synths for each voice number
    (1..~maxVoices).do { |n|
        createSynths.(n, variables, variableDimensions);
    };
    "Writing finished".postln;
};
);


////////////////////////////////////////////////////////////////////

// Helper to create one exact channel-count / variable combination.
//
// Example:
//     ~useExactSynthCreator.(78, [6]);
//     "/path/to/panaz.scd".load;
//
// This writes only PanAz78_6.scsyndef. It deliberately does not write the
// unnumbered metadata SynthDef, so existing model metadata is left untouched.
(
~synthCreatorExact = {|name, func, description = "", category = "", variables, variableDimensions|
    var variableNames = variables ? [];
    var dimensions = variableDimensions ? [];
    var args = ~exactVariableValues ? [];
    var synthdefName;

    if(~exactNumChannels.isNil || { ~exactNumChannels.asInteger < 1 }) {
        Error("~exactNumChannels must be an integer greater than zero").throw;
    };

    if(dimensions.size != variableNames.size) {
        Error(
            "SynthDef % declares % variables but % variable dimensions"
            .format(name, variableNames.size, dimensions.size)
        ).throw;
    };

    if(args.size != variableNames.size) {
        Error(
            "SynthDef % expects % variable values but received %"
            .format(name, variableNames.size, args.size)
        ).throw;
    };

    args.do { |value, index|
        if((value.asInteger < 1) || { value.asInteger > dimensions[index].asInteger }) {
            Error(
                "SynthDef % variable % value % is outside 1..%"
                .format(name, variableNames[index], value, dimensions[index])
            ).throw;
        };
    };

    synthdefName = name ++ (~exactNumChannels.asInteger).asSymbol;
    args.do { |value|
        synthdefName = synthdefName ++ "_" ++ (value.asInteger).asSymbol;
    };

    File.mkdir(d +/+ name);
    ("Writing exact SynthDef " ++ synthdefName).postln;

    SynthDef.new(synthdefName, {
        var sig = SynthDef.wrap(
            func,
            prependArgs: [~exactNumChannels.asInteger, args.collect(_.asInteger)]
        );
    }).writeDefFile(d +/+ name, mdPlugin: AbstractMDPlugin);

    ("Writing exact SynthDef finished").postln;
};

~useExactSynthCreator = {|numChannels, variableValues = #[]|
    if(numChannels.asInteger < 1) {
        Error("Exact SynthDef channel count must be greater than zero").throw;
    };

    ~exactNumChannels = numChannels.asInteger;
    ~exactVariableValues = variableValues.collect(_.asInteger);
    ~synthCreator = ~synthCreatorExact;
};
);
