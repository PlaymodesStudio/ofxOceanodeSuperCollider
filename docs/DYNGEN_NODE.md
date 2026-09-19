# DynGen — Live EEL2 DSP Node

**DynGen** is an Oceanode node that lets you write real-time DSP code in the
**EEL2** scripting language (same language used by REAPER's JSFX plug-ins) and
hear it run inside SuperCollider with sub-second feedback.
Scripts can be edited directly in the node's built-in code editor, or loaded
from `.eel2` files. Changes are sent to SC automatically after a short debounce
(0.4 s of inactivity).

---

## The Node at a Glance

| Control | What it does |
|---|---|
| **Num Channels** | Number of audio channels (1 – 16). Affects both the audio I/O bus width and the param memory layout. |
| **P0 … P7** | Modulation sliders. Only appear when the script contains `@param` annotations. Can receive Oceanode vector connections. |
| **EEL2 Editor** | The code editor. Toggle **Wrap / Edit** to switch between word-wrap view and edit mode. |
| **Open** | Open system file dialog — load any `.eel2` file from anywhere. |
| **Save** | Save the current buffer as a `.eel2` file. Saved scripts also appear in the dropdown. |
| **Dropdown + Load** | Browse scripts stored in `data/Supercollider/dyngenScripts/` and load the selected one. |

### Technical limits

| Parameter | Value |
|---|---|
| Max simultaneous DynGen nodes | 16 (slot pool) |
| Max param sliders per node | 8 (`p0` – `p7`) |
| Max channels | 16 |
| Code buffer size | 32 768 chars |
| Debounce before send | 0.4 s |

---

## EEL2 Primer for DynGen

EEL2 is a lightweight procedural language.  Every statement ends with `;`.
Variables are floating-point scalars and are global to the script by default —
no declaration is needed, just assign them.

### Code sections

DynGen recognises three optional section markers:

```eel
@init
  // Runs ONCE when the script is (re)compiled.
  // Use for initialising variables, clearing buffers, computing constants.

@sample
  // Runs ONCE PER AUDIO SAMPLE — this is your hot path.
  // Write your DSP here.

@block
  // Runs once per audio BLOCK (before the samples in that block).
  // Useful for things you only need to update at block rate.
```

You can use any combination, in any order. If you skip a marker the code before
any marker runs as `@init`.

### Built-in functions

| Function | Description |
|---|---|
| `sin(x)`, `cos(x)`, `tan(x)` | Trigonometry (radians) |
| `asin(x)`, `acos(x)`, `atan(x)`, `atan2(y,x)` | Inverse trig |
| `exp(x)`, `log(x)`, `log10(x)` | Exponential / log |
| `sqrt(x)`, `sqr(x)` | Square root / square |
| `abs(x)` | Absolute value |
| `floor(x)`, `ceil(x)`, `round(x)` | Rounding |
| `min(a,b)`, `max(a,b)` | Clamp helpers |
| `sign(x)` | −1, 0 or 1 |
| `pow(x,y)` | x raised to y |
| `rand(x)` | Random in `[0, x)` |

### Special variables

| Variable | Description |
|---|---|
| `srate` | Current sample rate in Hz |
| `in(i)` | Read input index `i` — see layout below |
| `out0`, `out1`, … `outN` | Write output for channel N |
| `buf[i]` | Shared heap memory — index with integers. Useful for delay lines, tables, etc. |

> **`num_ch` is NOT available.** DynGen bakes the channel count into the
> SynthDef name at compile time (`DynGenWrapper_2_0`, etc.) and does not inject
> it as a runtime EEL2 variable. Using `num_ch` evaluates to `0`, which silently
> breaks any code that depends on it (loops run 0 times, param indices point to
> audio instead of params). **Always hardcode `n` to the current Num Channels
> setting in your param index formulas.**

---

## Input Memory Layout

DynGen packs audio channels and parameter values into a single flat `in()` array.

### Formula

```
in(0)         .. in(n-1)              → audio channel 0 .. n-1
in(n + pi*n + ci)                     → param pi, channel ci
```

Where:
- `n`  = **Num Channels** — **hardcode this literal in your script**
- `pi` = param index (0 for p0, 1 for p1, …)
- `ci` = channel index (0 for left, 1 for right, …)

> **Do not use `num_ch` for `n`** — it is always 0 in DynGen's EEL2 runtime.
> Write the actual number directly: `in(1)`, `in(2)`, etc.

### Mono shortcut (n = 1)

When Num Channels = 1 the formula reduces to:

```
in(0)           → audio in ch 0
in(1)           → p0 (= in(1 + 0*1 + 0))
in(2)           → p1
in(3)           → p2
...
in(1 + pi)      → param pi
```

This is backward-compatible with older mono scripts.

### Stereo example (n = 2)

```
in(0)   → audio left
in(1)   → audio right

in(2)   → p0 left   (n + 0*2 + 0 = 2)
in(3)   → p0 right  (n + 0*2 + 1 = 3)
in(4)   → p1 left   (n + 1*2 + 0 = 4)
in(5)   → p1 right  (n + 1*2 + 1 = 5)
in(6)   → p2 left
in(7)   → p2 right
...
```

A helper macro makes this less error-prone — see *Patterns* below.

---

## @param Annotations

Annotations drive the live slider UI from inside the code. They are parsed
every time the code changes.

```eel
//@param Name : default, min, max
```

- **Name** — label shown on the slider in the Oceanode panel.
- **default, min, max** — initial value and range. All are optional; omitting
  them produces range `[0, 1]` with default `0`.
- Annotations are read in top-to-bottom order; the first one becomes `p0`, the
  second `p1`, etc.
- A maximum of **8** annotations are used; extras are ignored.
- When the count changes the node adds or removes sliders automatically.
- When only the name changes the slider is renamed in place (value preserved).

### Examples

```eel
//@param Gain : 1.0, 0.0, 4.0
//@param Cutoff : 0.5, 0.0, 1.0
//@param Feedback
```

---

## Per-Channel Parameter Modulation

Each param slider accepts an Oceanode **vector** connection.
DynGen automatically expands the vector to `n` channels before sending it to
SuperCollider:

| Connected vector length | Behaviour |
|---|---|
| 1 element | Broadcast to all channels (same value for ch 0, 1, …) |
| n elements | Each channel receives its own value |
| k < n elements | Channels 0 … k-1 get their value; channels k … n-1 repeat the last element |

This lets you automate left and right (or any per-channel dimension) with a
single Oceanode connection.

---

## Output Variables

Output channels are written via the **named global variables** `out0`, `out1`,
`out2`, … up to `out(n-1)`.

```eel
// Mono
out0 = processed_signal;

// Stereo
out0 = left_result;
out1 = right_result;
```

Unwritten `outN` variables default to **0** each sample.

> **Important — no runtime indexing for outputs.**
> `out` is not a special base pointer. Writing `out(ch)` with a loop variable
> `ch` does **not** write to `out0`, `out1`, etc. — it writes to raw EEL2 heap
> memory at address `ch` and the SC output stays silent.
> Since `num_ch` is always 0, avoid loops over channels entirely.
> Write to named outputs directly — one line per channel:
>
> ```eel
> // Stereo example — just two lines, no loop needed
> out0 = process(in(0));
> out1 = process(in(1));
> ```

---

## Patterns and Recipes

### Passthrough

```eel
// Mono (Num Channels = 1)
@sample
out0 = in(0);

// Stereo (Num Channels = 2)
@sample
out0 = in(0);
out1 = in(1);
```

> Scripts are written for a **specific** Num Channels value. `num_ch` is always
> 0 at runtime — hardcode the channel count instead.

---

### Gain with per-channel modulation

```eel
//@param Gain : 1.0, 0.0, 4.0

// Mono (Num Channels = 1)
// p0 ch0 = in(1 + 0*1 + 0) = in(1)
@sample
out0 = in(0) * in(1);

// Stereo (Num Channels = 2)
// p0 ch0 = in(2 + 0*2 + 0) = in(2)
// p0 ch1 = in(2 + 0*2 + 1) = in(3)
@sample
out0 = in(0) * in(2);
out1 = in(1) * in(3);
```

---

### Simple low-pass filter (one-pole, mono)

```eel
//@param Cutoff : 0.1, 0.0001, 0.499

// Num Channels = 1
// in(1) = p0 ch0 = Cutoff (n=1: p0 at index 1)

@init
z = 0;

@sample
a = 6.28318530 * in(1);   // 2π × cutoff (use literal — $pi may not be defined)
k = exp(-a);
z = in(0) + k * (z - in(0));
out0 = z;
```

---

### Delay line (mono, max 1 s)

```eel
//@param Time   : 0.1, 0.001, 1.0
//@param Feedback : 0.5, 0.0, 0.95

@init
buf_size = ceil(srate);   // 1 second at current rate
write_pos = 0;

@sample
time_s   = in(1);         // p0
fb       = in(2);         // p1
delay_s  = floor(time_s * srate);

read_pos = write_pos - delay_s;
read_pos < 0 ? read_pos += buf_size;

delayed  = buf[read_pos];
buf[write_pos] = in(0) + delayed * fb;
write_pos = (write_pos + 1) % buf_size;
out0 = delayed;
```

---

### Stereo ping-pong delay

```eel
//@param Time     : 0.2, 0.001, 1.0
//@param Feedback : 0.5, 0.0, 0.95

// Num Channels = 2
// p0 ch0 = in(2 + 0*2 + 0) = in(2)   Time
// p1 ch0 = in(2 + 1*2 + 0) = in(4)   Feedback

@init
buf_size = ceil(srate);
wL = 0;  wR = buf_size;    // two separate heap regions

@sample
time_s  = in(2);    // p0 ch0 (delay time)
fb      = in(4);    // p1 ch0 (feedback)

dL = floor(time_s * srate);
dR = floor(time_s * srate * 0.7);   // slightly shorter on right

rL = (wL - dL + buf_size) % buf_size;
rR = (wR - dR + buf_size) % buf_size + buf_size;

dlyL = buf[rL];
dlyR = buf[rR];

buf[wL] = in(0) + dlyR * fb;
buf[wR] = in(1) + dlyL * fb;

wL = (wL + 1) % buf_size;
wR = (wR + 1) % buf_size + buf_size;

out0 = dlyL;
out1 = dlyR;
```

---

### Frequency-domain trick — counting zero crossings

```eel
//@param Threshold : 0.0, -1.0, 1.0

@init
prev   = 0;
zcrate = 0;
count  = 0;
block_count = 0;

@sample
thresh = in(1);     // p0
(in(0) > thresh && prev <= thresh) ? count += 1;
prev = in(0);

@block
zcrate  = count * srate / 512;  // rough Hz estimate (assumes 512-sample blocks)
count   = 0;
out0    = zcrate / 20000;       // normalise ~0-1 for audio-rate output
```

---

## Working with the UI

### Editor toolbar (left → right)

```
[ Open ]  [ Save ]  [ Wrap/Edit ]  [ ──── dropdown ──── ]  [ Load ]
```

| Button | Action |
|---|---|
| **Open** | Open any `.eel2` file from the filesystem. The script is loaded into the buffer and sent immediately. |
| **Save** | Save the current buffer. A file dialog lets you choose the destination. |
| **Wrap / Edit** | Toggle between word-wrap read-only view and the normal editable mode. Click anywhere in wrap view to return to edit mode. |
| **Dropdown** | Lists all `.eel2` files found in `data/Supercollider/dyngenScripts/`. |
| **Load** | Load the script currently selected in the dropdown. |

### Inspector parameters

Right-click the node → Inspector to access:

| Inspector param | Description |
|---|---|
| **EEL2 Code** | The full script text (persisted in presets). |
| **Editor Width** | Horizontal size of the editor panel (160 – 800 px). |
| **Editor Height** | Vertical size of the editor panel (80 – 600 px). |

---

## Preset Behaviour

- The **EEL2 code** and **active param values** are saved with the preset.
- Param values are stored under stable index-based keys (`dyngen_p0`,
  `dyngen_p1`, …) in addition to the named keys. This means values survive even
  if you rename a `@param` between saves.
- When a preset is loaded, the correct number of sliders is reconstructed
  *before* connections are restored, so wired param lanes always reconnect
  cleanly.

---

## Architecture Notes (for developers)

### Slot pool

Up to **16** DynGen nodes can be active at the same time. Each node occupies a
*slot* (0 – 15). A SynthDef named `DynGenWrapper_<n>_<slot>` is pre-compiled
for every (channel count × slot) combination by `dyngen.scd`; the slot symbol's
hash is baked in at compile time.
If all 16 slots are in use, creating another DynGen node logs an error and the
node produces silence.

### Sending code to SC

Code is sent via a custom `/cmd dyngenscript` OSC message. If the message
would exceed ~16 kB, the code is written to a temp file and sent via
`/cmd dyngenfile` instead.
Parameter names from `@param` annotations are forwarded alongside the code so
SuperCollider can correlate the EEL2 `@param` names with the `p<i>`
NamedControls.

### SynthDef regeneration

If you change `kMaxSlots`, `kMaxChans`, or `kMaxParams`, you must re-evaluate
`dyngen.scd` in SuperCollider, copy the printed hash table into
`scDynGenNode.cpp`, and re-deploy the compiled SynthDefs.

---

## Quick Reference Card

```
── Inputs  (n = Num Channels, HARDCODED — num_ch is always 0) ───
in(0) .. in(n-1)          audio channels 0 .. n-1
in(n + pi*n + ci)         param pi, channel ci
in(n + pi)                param pi, mono shortcut (n=1 only)

// Mono (n=1): in(1)=p0  in(2)=p1  in(3)=p2  …
// Stereo(n=2): in(2)=p0L  in(3)=p0R  in(4)=p1L  in(5)=p1R  …

── Outputs ──────────────────────────────────────────────────────
out0 = v                  write channel 0
out1 = v                  write channel 1  (out2, out3, …)
// ⚠ out(ch) with a variable does NOT work — use named vars above

── Special vars ─────────────────────────────────────────────────
srate                     sample rate in Hz
// ⚠ num_ch is NOT available — always 0. Hardcode n instead.

── Param annotation syntax ──────────────────────────────────────
//@param Label : default, min, max
//@param Label : default, min      (max defaults to 1)
//@param Label                     (default 0, range 0-1)

── Sections ─────────────────────────────────────────────────────
@init    runs once on compile
@sample  runs every sample
@block   runs every block
```
