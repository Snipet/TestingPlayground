# README blocks of the instrument starter

`scripts/plugin.py clone --type instrument` (or `--starter`, for a project that is already an instrument) puts each
block below into the new project's README.md, in place of the block of the same name, with the Source/dsp,
Tools/probes/dsp and Tools/probes/plugin/process.cpp of this folder. Keep the marker lines. The template's own
README.md carries the effect's blocks; `scripts/plugin.py starter-diff` compares them.

<!-- starter:intro -->
As it ships it is an instrument: eight voices, each a sine or a triangle under an attack/release envelope, played
by MIDI, stereo out.
<!-- /starter:intro -->

<!-- starter:probes -->
| Probe | Checks |
| --- | --- |
| `dsp.render` | plays an arpeggio and a chord (sine, then triangle) to `dsp.render.wav`; finite, no clipping, block-size invariant |
| `dsp.response` | pitch within 0.005 Hz; level, GAIN and velocity within 0.02 dB; the attack and release shapes; each of eight held notes in the output; voice stealing; no click when a voice is retriggered or stolen |
| `plugin.process` | the processor as a host drives it: a note-on sounds from its sample offset at its velocity, the note-off releases it to silence with no voice left, GAIN written by the host reaches the DSP, a NaN from the host is ignored, both channels are the Engine's output bit for bit |
| `gui.dump` | the panel headless to `gui.dump.png` and `gui.dump.paper.png`; settles, every glyph, a11y, a drag writes the parameter |
| `plugin.state` | the host's parameters equal the table; the state round-trips; processBlock gives finite output |
<!-- /starter:probes -->
