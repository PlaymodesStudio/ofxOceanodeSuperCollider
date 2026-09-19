# DynGen Poly — Mono-Authored, Multi-Instantiated EEL2

`DynGen Poly` lets you write a **single mono DynGen script** and have Oceanode
instantiate it once per channel behind the scenes.

## Mental model

- You always write **mono** code.
- Read audio from `in(0)`.
- Read params from `in(1)`, `in(2)`, `in(3)`, ...
- Write output to `out0`.
- Oceanode creates one mono SC synth per channel and routes each channel's
  audio and param slice into its own instance.

## Param behavior

Each `//@param` slider still accepts Oceanode vectors:

- 1 value: broadcast to all channels
- `n` values: one value per channel
- fewer than `n`: last value repeats

So a stereo node can run the same mono DSP on left and right while still
receiving different per-channel modulation values.

## Example

```eel
//@param Gain : 1.0, 0.0, 4.0

@sample
out0 = in(0) * in(1);
```

With `Num Channels = 4`, this same script runs four times:

- voice 0 gets audio ch0 and `Gain[0]`
- voice 1 gets audio ch1 and `Gain[1]`
- voice 2 gets audio ch2 and `Gain[2]`
- voice 3 gets audio ch3 and `Gain[3]`

## Practical consequence

This is ideal for effects and instruments where each channel should run the
same DSP topology independently.

It is **not** the right model when the code itself must directly reference
other channels, because every channel instance has isolated script state.
