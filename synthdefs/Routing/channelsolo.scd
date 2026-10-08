

(
~synthCreator.value("ChannelSolo", {|n|
	var input, soloChan, fillAll, bypass, selectedChannel, output;

	// Input signal
	input = In.ar(OceanodeInput.kr(\in), n);
	soloChan = OceanodeParameter.kr(\solochan, 0, 1, 0, 79, "i"); // single value (size n gave n*n outputs)

    // Select the channel based on soloch parameter
    selectedChannel = Select.ar(soloChan, input);

   // Output the result
	Out.ar(OceanodeOutput.kr(\out), selectedChannel.dup(n));
}, description: "Solos a selected channel with options to fill all outputs or bypass", category: "Routing");
)