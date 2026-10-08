(
SynthDef(\customBufferPlayer, {
	arg out=0, freq=200, amp=0.5,
	bufA=0, bufB=1, // The two ping-pong buffers
	mix=0;          // 0.0 = hear bufA, 1.0 = hear bufB

	var sigA, sigB, crossfade, signal;

	// Smooth the mix parameter linearly over ~1 frame (60fps = 0.0166s)
	// This creates the "Morphing" effect.
	crossfade = VarLag.kr(mix, 0.017, warp:\linear);

	// Read both buffers simultaneously
	sigA = Osc.ar(bufA, freq);
	sigB = Osc.ar(bufB, freq);

	// Linear interpolation (morph) between the two shapes
	signal = (sigA * (1.0 - crossfade)) + (sigB * crossfade);

	Out.ar(out, signal * amp);
}).add;
)