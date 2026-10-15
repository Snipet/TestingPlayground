# README blocks of the effect starter

`scripts/plugin.py clone --type effect` (or `--starter`, for a project that is already an effect) puts each
block below into the new project's README.md, in place of the block of the same name, with the Source/dsp,
Tools/probes/dsp and Tools/probes/plugin/process.cpp of this folder. Keep the marker lines. The template's own
README.md carries the effect's blocks; `scripts/plugin.py starter-diff` compares them.

<!-- starter:intro -->
As it ships it is an effect: a gain and a tone control (a one-pole low-pass), stereo in and out.
<!-- /starter:intro -->

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | renders a sweep through the effect to `dsp.render.wav` (and the input); finite, no clipping, block-size invariant |
| `dsp.response` | each channel against a double-precision model of the effect, sample by sample (a swapped, missing or inverted channel fails); gain and tone against their formulas, within 0.02 and 0.05 dB; the smoothing of a gain step |
| `plugin.process` | the processor as a host drives it: parameters the host writes reach the DSP (GAIN +12 dB brings -12 dBFS to 0 dBFS), both channels are the Engine's output bit for bit, a NaN from the host is ignored |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y, a drag writes the parameter |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->
