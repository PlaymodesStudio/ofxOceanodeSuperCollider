(
~synthCreator.value("ChannelExtractor", {|nch|
	var input, channel, output;

	// Read the multichannel input defined by this SynthDef's channel count.
	input = In.ar(OceanodeInput.kr(\in), nch);

	// Select one channel using a zero-based scalar integer index.
	// Keep the metadata range independent of nch: SynthCreator derives metadata
	// from its one-channel variant, where an nch-based maximum would become 0.
	channel = OceanodeParameter.kr(\channel, 0, 1, 0, 79, "i");
	output = Select.ar(channel.clip(0, nch - 1), input);

	// ChannelExtractor always produces one mono output channel.
	Out.ar(OceanodeOutput.kr(\out), output);

}, description: "Extracts one zero-based channel from a multichannel input as mono", category: "Routing");
)
