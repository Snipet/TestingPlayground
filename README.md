<picture>
  <source media="(prefers-color-scheme: dark)" srcset="Assets/logo/readme-graphite.png">
  <img alt="TestingPlayground: an effect and a synth you write while they play" src="Assets/logo/readme-paper.png"
       width="680">
</picture>

# TestingPlayground

Two plugins (AU, VST3 and Standalone, macOS on Apple silicon) whose DSP you write while they run, built from one
source tree: **TestingPlayground**, an effect, and **TestingPlaygroundSynth**, an instrument. The editor of each is a
text field holding an [FMinus](https://github.com/Snipet/FMinus) program; the plugin compiles it when you press RUN
(or Cmd-Return), and the next block plays it: the effect's crossfaded from the last, the synth's with the keys you
hold started again on the new program while the old one's voices fade out. The program's parameters become the dials
under the text. A chat with Claude at the right of the editor writes and changes the program for you: describe an
effect or a sound and it writes the FMinus, runs it, fixes what does not compile and leaves the dials at good
settings, so a sentence does what a preset did. Built on JUCE 8.0.4, with a GPU editor made with
[FunkGui](https://github.com/Snipet/FunkGui) (its `TextEditor`) and FMinus, the JIT compiler for DSP, linked in.

The two are one code base compiled twice, once for each product (`products/effect.cmake`, `products/synth.cmake`):
what differs is the program each plays (below), its examples and its identity (name, codes, bundle id, its own
folders). The editor, the chat, the projects, the library and the Claude Desktop extension work the same in both.
Where this guide says "the effect" or "the synth", it means that product; the rest holds for both.

<!-- origin -->
The effect began as a feedback delay network reverb (made from FunkPluginTemplate, its FDN ported from the Visage
version, git tag `legacy-visage`), and the synth as the sine voice synth of the Visage version, ported onto FSynth's
DSP core; each one's history before the branch `fminus-editor` holds that plugin and the file-watching script that ran
after it. They were two repositories, TestingPlayground and TestingPlaygroundSynth, until this one took both: the
synth's history is merged in.
<!-- /origin -->

After a build, `build-dev/probe-out/effect/gui.dump.png` and `build-dev/probe-out/synth/gui.dump.png` are pictures of
the two editors.

## Writing a program

FMinus is C with DSP's additions (vectors, modules, parameters); its README has the language, and its
`docs/programs.md` and `docs/extending.md` the details. The effect plays a module named `Effect`, one instance; the
synth plays a module named `Voice`, one instance a note, sixteen at once.

### The effect: module Effect

```c
module Effect
{
    param float gain [[fminus::range(-24, 12)]] [[fminus::default(0)]];   // dB: a dial
    float level;                                                          // state: the module's fields

    void init(float rate)                     // optional: before the first block, and when the sample rate changes
    {
        level = 1;
    }

    void process(float* restrict left, float* restrict right, int n)     // the audio thread: in place
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] *= level * expf(0.115129255f * gain[i]);
            right[i] *= level * expf(0.115129255f * gain[i]);
        }
    }

    void end() {}                             // optional: before the program goes (free what init allocated)
};
```

- **process** gets n samples of both channels and changes them in place (a mono bus: `right` is a copy of the left,
  unheard). A module's fields are its state: they start at 0, live as long as the program, and carry from block to
  block. Keep the state per sample, and the output does not depend on the host's block size.

### The synth: module Voice

```c
module Voice
{
    param float release [[fminus::range(5, 5000)]] [[fminus::default(300)]];   // ms: a dial
    float rate, phase, step, env;                                              // state: the module's fields
    bool held;

    void init(float sampleRate)                // optional: before the first note, and when the sample rate changes
    {
        rate = sampleRate;
    }

    void start(float note, float velocity)     // a note-on: 69 is A, 440 Hz; velocity 0 to 1
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        env = velocity;
        held = true;
    }

    void stop()                                // optional: the note-off
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)   // the voice's next n samples
    {
        const float down = expf(-6907.7553f / (release[0] * rate));
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                env *= down;
            const float out = env * sinf(6.28318531f * phase);
            left[i] += out;
            right[i] += out;
            phase += step;
            if (phase >= 1)
                phase -= 1;
        }
        return held || env > 0.0001f;          // false: silent for good, and the voice is free
    }

    void end() {}                              // optional: before the program goes (free what init allocated)
};
```

- **start** begins a note on a voice, which may have played another before: set what the note needs (a voice's
  fields keep their values between notes, and start as 0 when the program does). **stop** releases it. **process**
  adds the voice's next n samples into both channels (they arrive at 0, so writing works too) and returns false once
  the voice is silent for good, which frees it. Keep the state per sample, and the output does not depend on the
  host's block size: the plugin calls the voices in parts that end on multiples of 16 samples, and frees a finished
  voice only there.
- **Voices.** Sixteen sound at once. A seventeenth note steals the oldest released voice (else the oldest), fading it
  out over 2 ms while the new note starts on another instance. A key pressed again while its note still sounds stops
  that voice first. The voices are summed, and a mono output gets their two channels' average. The sustain pedal is
  the plugin's: a key let go while it is down gets its stop when the pedal comes up.

### Both

- **Dials.** The main module's first 32 parameters, in the order they are declared, are the dials, and the host's
  parameters P1 to P32 (a host shows "1 gain", and the value in the program's units); the synth's voices all read the
  same values. A `float` glides when its dial turns: it is read sample by sample, `gain[i]`, as a stream of 20 ms
  ramps, and held still otherwise (`release[0]` reads it once a call). An `int` or a `bool` is read once a call
  (`mode[0]`, or any index). `fminus::range` sets the dial's span (a float without one spans 0 to 1, an int 0 to 10)
  and `fminus::default` where it starts and where a double-click returns it. A run that brings a parameter the slot
  did not have moves that dial to its default; one with the same name keeps it.
- **Frozen dials.** Once no dial has moved for 3 seconds, the plugin compiles the program again in the background with
  each dial's value as a constant (FMinus's frozen binding) and in FMinus's fast math (LLVM's own sinf, cosf, tanf,
  expf, tanhf and logf), and plays that copy on the same instance (the synth's on the same voices), at a block's
  start, until a dial moves (that block it plays its own code again; 3 s later it freezes again); the top bar shows a
  snowflake beside the CPU load while the copy plays (under the pointer, the status bar says what it means). The two
  sound the same, so the switch cannot be heard: fast math moves a result's last bits, never its level or its spectrum
  (`dsp.freeze` plays the examples and 64 of RANDOM's programs for ten seconds each: every octave band within 0.1 dB,
  the peaks within 0.01 dB). The samples are not the same bits, though (an FM voice's pitch, from an expf an ulp
  apart, drifts its phase so slowly that the worst of them parts from the program's samples 97 dB under the signal in
  ten seconds), and while the copy plays the output may depend on where the host's blocks end. On those programs the
  copy takes about 92% of the program's time in the effect and 90% in the synth (the geometric means, on a quiet Mac;
  single programs 74% to 163% and 76% to 130%). A copy in exact math, the program's own, would take 94% and 93% and
  play the program's very bits: since FMinus c75caab its math is kernels of its own, inline, which fold a frozen
  dial's math exactly, where fast math calls the system's functions. So most of the gain is the dials' folding (FMinus
  also makes copies of a method for a bool's or a small int's values and moves a still dial's arithmetic out of its
  loops), and a program that calls `tanhf` or `sinf` a sample at a time in a loop with state runs slower frozen: the
  effect's Drive (124% to 168%), the synth's FM bell (105% to 127%) and a few of RANDOM's (`dsp.freeze` names each
  more than 5% slower, and measures the copy in exact math beside it).
- **The audio thread.** process (and the synth's start and stop) may not allocate: FMinus refuses a program whose
  methods could reach `calloc`, `malloc` or `free` from them, and names the line. Allocate in init (a delay line:
  `calloc`, zeroed; in the synth each voice's own, as the Pluck example's) and free in end, both off the audio thread;
  or use a fixed array, `float line[48000];`, a field like any other.
- **Modules inside modules.** The main module may hold others (`Svf l;`, `Osc osc[3];`): their methods are its
  helpers, and their parameters start at their defaults (they have no dials).
- **Shared data.** A module named `Shared`, when the program has one, is made once for it, not once a voice: its init
  runs before the main module's (with the rate), its end after theirs, and every field of the main module of type
  `Shared*` points at it. So the synth's voices read one wavetable, lookup table or sample that init built once, where
  a voice's own init would build it for every voice instance; they read it on the audio thread and never free it. The
  effect, one instance, may as well hold what it needs.

  ```c
  module Shared
  {
      Wavetable table;                                       // wavetable.fm
      void init(float rate) { table.allocate(1); table.shape(0, 2); }
      void end() { table.release(); }
  };

  module Voice
  {
      Shared* shared;                                        // the plugin sets it before init
      WaveOsc osc;
      void init(float rate) { osc.table = &shared->table; }
      ...
  };
  ```
- **The host: tempo, position, wheels.** `#include "host.fm"` (the plugin provides it: no tab of its own) and declare
  `Host* host;` in the main module (Shared may have one too): the plugin points it at one `Host`, filled before every
  call (process; the synth's start and stop too). It holds the host's tempo (`bpm`), the song's position at the call's
  first sample in quarter notes (`beat`; sample i is at `beat + i * bpm / (60 * rate)`), the bar's start, the time
  signature and whether the transport runs (`playing`), and, in the synth, the player's mod wheel (CC 1, 0 to 1),
  pitch wheel (-1 to 1; the program picks the range), channel pressure and sustain pedal (the effect's stay at rest).
  A host that says nothing (the Standalone) gets 120 BPM and a position that runs on from the start. Tempo sync is
  the program's: a delay of `beats * 60 / bpm` seconds, an LFO at `bpm / 60 / beats` Hz, or one that follows the song
  with `lfo.follow(position, beats)` (`lfo.fm`); a voice's key tracking comes from start's note. Read it, never write
  it.

  ```c
  #include "host.fm"

  module Effect
  {
      Host* host;
      float line[96000];
      int w;
      void process(float* restrict left, float* restrict right, int n)
      {
          const int d = (int)(0.5f * 48000 * 60 / host->bpm);              // an eighth note
          ...
      }
  };
  ```

  ```c
  #include "host.fm"

  module Voice
  {
      Host* host;
      float step, phase;
      void start(float note, float velocity) { step = 440 * expf(0.0577622650f * (note - 69)) / 48000; }
      bool process(float* restrict left, float* restrict right, int n)
      {
          const float f = step * expf(0.0577622650f * 2 * host->pitchbend);     // a bend of 2 semitones
          ...
      }
  };
  ```
- **The FFT.** FMinus has one, real and complex: a plan from `fft_create(n)` in init (n a multiple of 32 made of 2s,
  3s and 5s: 256, 1024, 2048), `fft_forward(plan, in, bins)` and `fft_inverse(plan, bins, out)` anywhere, the audio
  thread too (n floats to n / 2 + 1 bins and back, unscaled: the round trip is n times the input), `fft_destroy(plan)`
  in end. `wavetable.fm` builds band-limited wavetables with it.
- **Several files.** The editor has a tab for each file, the program's first (`program.fm`). The program includes
  another by its name, `#include "filters.fm"`, as C does: the file is read where the line is (a module there is a
  type after it), and once only, as if every file began with `#pragma once`, so two files may both include a third.
  Its errors name it ("filters.fm:3:12: error: ..."). FMinus has no other directive: no macros, no `#if`.
- **The library.** Ready-made building blocks, tested by `fminus.library`: filters (`svf.fm`, `ladder.fm`,
  `onepole.fm`, `biquad.fm`), delays and reverb (`delay.fm`, `reverb.fm`), sources (`osc.fm`, `wavetable.fm`,
  `noise.fm`), modulation (`adsr.fm`, `lfo.fm`, `follower.fm`), shaping and utilities (`shaper.fm`, `util.fm`), and
  circuits (`diodetone.cir`, `screamer.cir`, `otalp.cir`: netlists, below). LIBRARY (the editor) or Claude adds one as
  a tab of the program's own; `#include` it and use its modules (each file's header says how). It is then the
  program's file like any other, saved with it, and yours to change. They live in
  `Source/fminus/library/<n>-<category>/`, the same in both products.
- **Circuits: a .cir file.** A file whose name ends in `.cir` is a SPICE-style netlist of an analog circuit (resistors,
  capacitors, inductors, pots, switches, diodes, OTAs, ideal op amps, DC and controlled sources; FMinus's
  `docs/circuits.md` has the language), which the plugin turns into one FMinus module with FMinus's circuit compiler
  (`fminus/Circuit.h`; here `Source/fminus/Circuits.h`): an `#include` of it reads that module, generated the first
  time an `#include` reaches it in a compile. The module is named by the netlist's `.title`:

  ```c
  #include "screamer.cir"

  module Effect
  {
      param float drive [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
      Screamer pedal;                           // the module the netlist generates
      int grid;                                 // samples to the next set()
      void init(float rate) { pedal.init(rate); }
      void process(float* restrict left, float* restrict right, int n)
      {
          for (int i = 0; i < n; ++i)
          {
              if (grid == 0)                    // the pots, 0 to 1, on the 16-sample grid
              {
                  pedal.set(drive[i], 0.5f, 0.7f);
                  grid = 16;
              }
              grid -= 1;
              left[i] = pedal.tick(left[i]);    // volts in, volts out: 1.0 is 1 V
              right[i] = left[i];
          }
      }
  };
  ```

  `set` takes the pots (0 to 1, in `.pot` order), then the switches' positions; call it on the absolute 16-sample grid
  (a counter from init, a voice's from start), every grid tick while a dial glides or a switch ramps, and the output
  never depends on the host's blocks. `tick` takes the signal, then the law inputs (`.input iabc - kind=current
  range=10u:2m`: an OTA's bias current, which a voice computes every sample through its own expo converter, an `expf`).
  `clear` returns the circuit to rest (a voice's start). A netlist's mistakes are its lines' ("screamer.cir:37:1: error:
  ...") and stop the compile; FMinus's messages on the generated module are cited at the netlist's line they come from
  ("(in the module generated from screamer.cir, line 11)"); a netlist no `#include` reaches is not compiled (its tab
  still gets the check's squiggles), and the program itself, the first tab, is FMinus. A netlist names its configuration
  (`.oversample 4 iir`, `.adaa on`, `.solver omega(1)`), or leaves it to FMinus's self-test, which chooses it on the
  circuit's stress tests; left out, a provisional configuration plays (8x oversampling for diodes, 2x for an OTA: safe,
  and costly), with a note saying so, and `fminus x.cir --self-test` (FMinus's command line) runs the self-test and
  prints the line to pin its answer with. The library's netlists are pinned, the line in each header. One nonlinear part
  a stage (a diode pair, an OTA) and ideal op amps: resonant filters, transistors, tubes and biased junctions are
  FMinus's Phase 2.
- **The interface: a .gui file.** A file named `panel.gui` (any name ending in `.gui`) beside the program gives it
  an interface richer than the `#pragma panel` lines below, which it replaces: containers laid out as CSS's flexbox
  lays them out, pages behind tabs, every kind of control with its own label, unit and taper, displays and text
  (FMinus never reads it; `Source/fminus/GuiSpec.h` has the language, `Source/editor/GuiLayout.h` the layout):

  ```
  class "main" size=72 arc=orange               // properties an element takes with class="main"
  defaults knob look=soft                       // properties every knob takes (its own win)
  panel accent=orange background=#1d1b19 gradient=#121110 {   // around everything
      row gap=10 {                              // left to right; column: top to bottom; grid cols=4
          section "FILTER" color=orange look=card radius=10 {   // frame (the default), card, inset or plate
              knob cutoff class="main" taper=log unit="Hz" live=cutoffnow   // live=: where a field has it now
              knob resonance label="RESO" look=vintage
          }
          section "ENVELOPE" {
              slider attack; slider decay; slider sustain scale=100 unit="%" decimals=0; slider release
              envelope attack decay sustain release          // their shape, drawn
          }
          box {                                 // a filled frame without a title
              buttons wave "SAW" "SQR" "TRI" horizontal
              switch sync "OFF" "ON"
              menu mode "POLY" "MONO"
          }
      }
      tabs look=bar {                           // pages, one shown at a time: tabs, bar, pills or side
          page "MOD" { xy level pan }           // a pad that moves two parameters
          page "METERS" { meter out; vu out; led gate; number count; scope out; spectrum out grow=1 }
      }
      keyboard from=48 keys=25 wheels           // the synth's keys and its pitch and mod wheels, played with the mouse
  }
  ```

  An element is a line (or several, between `;`): its name, its arguments, `key=value` properties, its children
  between `{` and `}`; `//` and `/* */` are comments. Every container takes `gap=`, `pad=`, `align=` (stretch, start,
  center, end), `justify=` (start, center, end, between, around), `grow=`, `width=`, `height=` and `wrap` (a row's
  children onto more lines when it is narrow); any element `margin=`, `self=` and `minwidth=` to `maxheight=`;
  `spacer`, `divider thickness=3` and `label "TEXT" size=title` (or a size in px, `align=right`) fill and dress it.
  Sections and boxes take a look (frame, card, inset, plate), `radius=`, `fill=`, `border=`, `borderwidth=`,
  `shadow=` and a title's colour, alignment and size; tabs a look (tabs, bar: a header bar of pages as Serum has,
  pills, side: a list at the left), and the page you pick stays picked when the program runs again and in the
  session. A knob has nine looks (modern, vintage, flat, minimal, ring, led, chicken, soft, dot), a size (small,
  medium, large or a face in px), colours for its arc, track, cap, pointer, ticks, label and value, a pointer
  (line, dot, triangle, none), `sweep=`, `ticks=`, `thickness=`, `bipolar`, `labelpos=`, `value=` and `shadow=`;
  `live=field` marks on its ring where the program has the parameter now (its modulation, from a field).
  `class "NAME" ...` and `defaults KIND ...` lines give properties to many elements at once.
  `shader "glow.sc" inputs="cutoff out"` draws a GPU shader of the program's own: `glow.sc`, a tab like any file,
  holds the body of a fragment shader in bgfx's GLSL (a `main()` that sets `gl_FragColor`, reading `v_uv`, `v_px`,
  `u_size`, `u_time` and its inputs as `in0`..`in7` or `in_cutoff`, `in_out`: a parameter's 0..1, a field's value, the
  audio's level). The plugin builds it with the shaderc it carries (`Contents/Resources/shaderc`, bgfx's) when the
  program runs; one that does not compile is a note on its own line, and draws nothing until it does
  (`Source/fminus/Shaders.h`). Four a panel at most. `taper=log` gives a frequency's or a time's knob the same
  turn for each octave, and the host's automation follows it; `unit=`, `decimals=` and `scale=` say how a value reads,
  to the host too (`Source/dsp/Controls.h`). Displays are those of the lines below and `spectrum` (an analyser of `in`
  or `out`), sixteen at most. What the plugin cannot use is a note on its line and column ("panel.gui:12:5: note:
  ..."), and the rest stands; a parameter no control names comes last, in a section without a title.
- **The panel.** `#pragma panel` lines lay the dials out as an analog synth's panel is printed, in titled sections,
  framed, in rows; they choose how each dial looks and add displays that show what the program does as it plays
  (FMinus ignores every `#pragma`; the plugin reads these from the program and what it includes):

  ```c
  #pragma panel section "OSCILLATORS" shape detune      // a section: its title, then its dials and displays in order
  #pragma panel section "FILTER" cutoff resonance amount
  #pragma panel row                                     // the sections after it start a new row
  #pragma panel section "ENVELOPE" attack decay sustain release
  #pragma panel section "OUTPUT" level out              // out: the VU below
  #pragma panel label amount "ENV AMOUNT"               // a dial's label (else its name)
  #pragma panel small detune                            // a small knob: its label under it, no value
  #pragma panel style cutoff vintage                    // knob (the default), vintage, fader, switch or buttons
  #pragma panel style attack decay sustain release fader
  #pragma panel style shape buttons
  #pragma panel options shape "SAW" "SQUARE" "TRI"      // an int's values' names, from its range's low end
  #pragma panel color "FILTER" orange                   // a section's colour, or a dial's or a display's
  #pragma panel vu out "OUTPUT"                         // a display: meter, vu, led, number or scope
  ```

  - **Dial styles**: `knob`; `vintage`, a black knob over a printed scale; `fader`, a slider; `switch`, a toggle (a
    bool, or an int of two values); `buttons`, a lit button a value (an int or a bool, up to eight). A `style` line
    names one dial or several, the style last. `options` names an int's values (a bool's: off, on): its buttons show
    them, and its knob and the host say them.
  - **Displays** show the plugin's audio, `in` (the effect's input; the synth has none) or `out` (the output; left and
    right, in dBFS), or a float, int or bool field of the main module that process writes (`gr = reduction;`; the
    synth's from the newest voice that sounds), across a range given after it (0 to 1 unless it says:
    `#pragma panel meter gr 0 24 "GR"`). `meter`: LED bars, the highest held a second (the audio's peaks); `vu`: an
    analog VU meter's needle (0 VU at -18 dBFS RMS); `led`: a light, a field's value its brightness (of the audio: a
    clip light); `number`: a readout of the field (of the audio: its peak in dBFS); `scope`: an oscilloscope (of a
    field: its trace over the last 2.5 seconds). A section names a display by its source, or by the name `as` gives
    it, which a second display of one source needs: `#pragma panel scope out as wave "WAVE"`. Eight displays at most.
    The colours: red, orange, yellow, green, teal, blue, violet, pink, cream, grey.

  A row too wide for the window wraps, its sections shared out evenly; shorter rows widen toward the widest; when
  the sections need more height than the window gives, every knob is drawn small. Dials and displays no section
  names come last, untitled. A line the plugin cannot use (a name that is no dial, a dial in two sections, a field
  that is not there, a style the dial does not suit) is a note on its line, and the rest stands
  (`Source/fminus/PanelSpec.h`). The examples and RANDOM's programs use these lines; Claude writes a `panel.gui`.
- **Its types are checked.** The plugin calls each method through a line of its own with the type it calls it by, so
  a `process` with other parameters (or, the synth's, one that returns nothing) is an error ("Effect::process: the
  plugin calls it as ..."), never a crash.
- **What the plugin guards, and what it cannot.** Every sample that leaves the plugin is finite and within +-4
  (+12 dBFS): the effect turns a NaN or an infinity into 0, the synth drops a voice whose output is not finite, a
  louder sample is clipped, and the toolbar says so. A NaN in the effect's state stays there: RUN again starts it from
  scratch. But the program is native code: a write through a pointer past its memory, or a loop that never ends,
  takes the host with it. Try a new program in the Standalone.
- **Subnormals** are flushed to zero (the plugin compiles with FMinus's `Denormals::Flush`, the mode the audio thread
  runs in), and the math builtins (`sinf`, `cosf`, `tanf`, `expf`, `tanhf`, `logf`; also `fabsf`, `sqrtf`, `floorf`,
  `fminf`, `fmaxf`) are FMinus's default math mode's, kernels of its own: one result for a float wherever it is
  computed, on any machine. `tanf` is a builtin, so a program's own function of that name is an error.
- **Cost.** A math call in a loop that carries state from sample to sample (a filter, an envelope, a phase) computes
  one sample for the price of four; in a loop with no state FMinus makes one call for four samples. So put the math
  in a loop of its own, in parts of 16 samples: first the sources and the dials into arrays, then the math, then the
  oscillators and filters. The filters take their cutoff as a coefficient for that (`g[i] = svfG(fc[i], rate)` in the
  math loop, `filter.setG(g[i], res)` beside `tick`; `ladderG` and `onePoleG` likewise), and the Modular voice in
  Claude's instructions (`Source/plugin/ChatPrompt.cpp`) is built so: 35% fewer instructions than with its math in the
  one loop, the same samples. `lfo.fm`'s sine is a polynomial, within 0.0011 of `sinf`; a filter's `set` takes its
  sine and cosine in one call (`sinf` of a `float2`).

The EXAMPLES menu holds each product's own to start from, `Source/fminus/examples/<key>/`. The first, **Init**, is the
program a new instance plays (and what the Standalone's File, Reset to Default State, and a session saved without a
program play): the classic init patch, a neutral start that sounds right as it is, laid out to be edited.

- **The effect's seven** (`effect/`): Init (an INPUT and an OUTPUT level in dB and a dry/wet MIX in %, gliding as every
  float dial does, around an empty place for your processing, marked; with every dial at its default the output is the
  input, bit for bit; input and output meters), Gain and pan (a level and a stereo position, the smallest effect),
  Tremolo (init and the sample rate; SYNC follows the song in notes, host.fm), Filter (a state-variable filter as a
  module of its own), Echo (a delay line from calloc in init, freed in end; a VU and a switch; SYNC sets its time in
  notes at the host's tempo), Drive (tanh saturation; input and output meters, a vintage knob) and Tube screamer (the
  library's `screamer.cir`, an overdrive pedal's circuit, one for each channel: DRIVE, TONE and LEVEL its pots, set on
  the 16-sample grid, INPUT and OUTPUT trims in dB).
- **The synth's six** (`synth/`): Init (one band-limited oscillator, a sawtooth or a square, a triangle or a sine,
  through a resonant low-pass wide open, CUTOFF in octaves above 20 Hz from 0 to 11 (at 11 as open as the filter goes,
  at any sample rate), its RESONANCE and ENV AMOUNT at 0; an ADSR on the level, a 2 ms attack, full sustain and a 100 ms
  release, so a note starts and ends without a click, a stolen voice's instance too; the velocity on the level, the
  pitch wheel bending 2 semitones, a VOLUME in dB; its oscillator, filter and envelope the library's `osc.fm`, `svf.fm`
  and `adsr.fm`, copied in), Sine (an attack and a release; the pitch wheel bends it, host.fm), Saw and filter (a
  polyBLEP sawtooth through a resonant low-pass, an ADSR on both: eight dials, the envelope's on faders, a light that
  follows it and a VU; the pitch wheel bends it and the mod wheel opens the filter), FM bell (two operators; a meter of
  its brightness and a scope; the mod wheel adds index), Pluck (Karplus-Strong, each voice's delay line from calloc
  in init) and OTA filter (an oscillator through the library's `otalp.cir`, an analog two-pole OTA low-pass whose
  cutoff is its bias current: each voice computes Iabc every sample from its note, CUTOFF and the envelope through one
  `expf`, and clears the circuit in start; the library's `osc.fm` and `adsr.fm` beside it).

An example may `#include` library files (the circuit examples do): EXAMPLES adds them as tabs, as LIBRARY would.

To add one, add a file there whose first line is `// <Title>: what it is`.

## The editor

- **The layout**: a top bar (the product; the project's pill, which opens PROJECTS; the **Code / Panel**
  switch; the CPU load; the theme's button), then three cards: the explorer's at the left (below), the program's (its
  status and its actions, **Examples**, **Library**, **Random** and **Run**, over the files' tabs, the editor or the
  panel, the problems and the dials) and the chat's, at the right. The chat's **>** folds it into a rail at the right
  edge, and the editor takes the room; the rail's button opens it again, and a dot on the rail says Claude answered
  meanwhile (the session keeps the fold). The status bar at the bottom says what the control under the pointer does;
  the zoom is its pill at the right.
- **The explorer**, at the left, lists in sections that fold (a click on a section's title): **Files**, the program's
  files (the one shown marked, one the last run's errors name with a red dot; a click shows a file, a double click
  renames it, the cross on a row closes it, asking first when it has text; **+** makes a new file); **LLVM IR**, what
  FMinus compiled the program into (below: a click opens a view of it, read only); **Presets**, the
  presets of the project open (a click applies one, its dials at once; **+** saves the dials as a new one);
  **Projects**, the ones on this Mac, the newest first, each with its newest version (a click opens it, asking first
  when the editor has changes no version holds; **+** opens PROJECTS to save); **Library**, the library's building
  blocks by category (a tick on those the program has; a click adds one as a tab, `#include`d, or shows it); and
  **Examples** (a click puts one in the program and runs it; Cmd-Z gives yours back). Its **<** folds it away, the
  program's card taking the room, and the button at the left of the program's card's header opens it again; the
  session keeps the fold. A window too narrow for it beside the chat leaves it folded, and its button then folds the
  chat to make room. The **Panel** view has no explorer: the plugin's interface keeps the whole left column.
- **Run** compiles the text and plays it once it compiles; **Cmd-Return** does the same from the keyboard. The effect
  crossfades from the last program over 20 ms; in the synth the sounding voices fade out over 20 ms, and the keys
  still held start again on the new program. Compiling takes some tens of milliseconds, off the audio thread and off
  the editor's.
- **Examples** replaces the program's text with an example (Cmd-Z gives yours back) and runs it; the other files
  stay, and the library files it `#include`s that the program lacks come as tabs (never over a tab of that name; with
  no room for them it says so and changes nothing).
- **Library** lists the library's files by category, a tick on those the program has. One it lacks becomes a tab,
  shown, and the program `#include`s it (a line after its description; Cmd-Z in the program takes it back); one it
  has is shown. With twelve tabs, the rest are greyed.
- **Random** writes a new program for inspiration, from the library's building blocks, with random settings, a panel
  and a name: in the effect, two to four of a drive (tanh, a wavefolder, a crusher or a tube's curve), a filter
  (still, swept by an LFO, or opened by the input's level), a chorus, a flanger, a tremolo or an auto-pan, and an echo
  or a reverb, then a dry/wet mix; in the synth, a voice of one or two oscillators (a band-limited morph, a wavetable
  built once in `Shared`, or two operators of FM), noise, a filter its envelope opens, an envelope of a character (a
  pluck, keys, a pad, a bass, brass, a bell), an LFO and a drive. It replaces the program's text (Cmd-Z gives yours
  back), adds the library files it needs as tabs, and runs it; each click makes another. Its first line names it and
  its seed (`random 4821`).
- **Projects** (the top bar's pill, named after the project open, its version and the preset applied) keep programs on
  this Mac: the editor's files (every tab, and the one shown) and the dials' values, under a name, in each product's
  own folder, `~/Library/Audio/Presets/Funk/TestingPlayground/` (the effect's) and
  `~/Library/Audio/Presets/Funk/TestingPlaygroundSynth/` (the synth's; a JSON file a project, `<name>.fmproject`:
  copy, rename or share them in the Finder). Every save is a **version** of
  the project (v1, v2, ...), with when it was saved and what changed since the one before ("program.fm +12 -3, 2
  dials"); an older version opens again, and saving it makes it the newest. A project keeps its last 100 versions. A
  project's **presets** are dial settings for its program, no code: applying one turns the dials at once, with nothing
  to compile. The sounds a program makes by its dials alone are presets of one project; a sound that needs other code
  is a project of its own. The card lists the projects at the left (a click chooses one, a double-click opens its
  newest version) and the chosen project's versions and presets at the right (a double-click opens a version, or
  applies a preset; a click under the presets starts a new one). The field names what **Save** saves: a project (a
  new version of the project named, or a new project) or, while the presets are chosen from, a preset of the project
  open. **Open** opens the chosen project or version, **Apply** the chosen preset, **Delete** deletes the chosen
  project, version or preset. Saving into another project, opening over changes no version holds, replacing a preset
  and deleting each ask first (a second click). **Cmd-S** saves a new version of the project open (where the host
  passes the key on), or opens PROJECTS while none is. The pill says (edited) once the files change, or the dials move
  from the preset's. The presets saved before projects (`<name>.fmpreset`) become projects of one version the first
  time the folder is read, and the old files move into its `Presets before projects` folder.
- **Export** (PROJECTS, **Export...**) keeps a project's description, author, category (a synth's: Bass, Lead, Pad,
  ...; an effect's: Reverb, Delay, ...), tags and genres with it, and takes it off this Mac two ways: **Export source
  (.zip)**, its code to read and edit anywhere (`<Name>/README.md` says what it is; `project.json` keeps the dials, the
  presets and every version's place; the newest version's files at the top, each older one's in `versions/v<N>/`),
  and **Export package (.fmpack)**, everything in one compressed file for another playground (xz, by macOS's own
  Compression framework: a project of 30 versions, 33 KB of text, is a 1.3 KB package; the header is 20 bytes and the
  rest a plain .xz stream). Every version goes, or the newest only. **Import...** in PROJECTS, or a .zip or a .fmpack
  dropped on the editor, adds one as a project of its own ("<name> 2" when the name is taken); a synth's export does
  not open in the effect, nor an effect's in the synth.
- **The files' tabs**, over the text: a click shows a file; a double-click edits its name in place (Return keeps it,
  Escape cancels; letters, digits, `_`, `-` and `.`, and no two the same); **+** adds a file (up to twelve; the tabs
  narrow to fit), its name ready to type; a tab's **x** closes its file, asking first when it has text. The program's
  tab cannot be closed. A tab whose file the last run's errors name has a mark before its name. Each file keeps its
  own caret, scroll and undo. The IR view's tab (below), while it is open, sits apart at the bar's right end, outlined,
  a chip before its name.
- **The status**, left of the card's actions (its light green while the program plays as written), says whether the
  program plays, whether the text has changed since it was run, and how many errors the last run had; a program that
  does not compile leaves the last one that did playing. The top bar has the audio thread's load (the busiest recent
  block's share of its time), red while the output guard acts.
- **The text** of the file shown is a code editor (FunkGui's TextEditor, made one by `Source/editor/CodeAssist`):
  FMinus highlighted (keywords, types and the program's module names, functions, members, numbers, comments,
  directives, attributes), line numbers, a column for the problems' marks, the caret's line, indent guides, the bracket
  pair at the caret boxed, a minimap at the right (a press or a drag there scrolls), and a status line under it (the
  problems' count, the caret's line and column, the indent, the language, **Format on Run**). `( [ {` and the quotes
  close themselves (typed over, the closer is stepped over); Return after an opener indents, a closer typed outdents.
  Undo (Cmd-Z, Cmd-Shift-Z), the clipboard, word and line moves (Alt and Cmd with the arrows), Tab to indent (four
  spaces), Escape to leave the text (the panel's keys then move the focus over the dials). It has the caret when the
  editor opens.
- **A netlist's tab** (`.cir`, a circuit: above) is highlighted as SPICE (its elements' names, directives, values,
  an expression's operators, comments; `lang::lexCirLine`), gets the problems from RUN and from the check while typing
  (which generates every netlist, one the program does not include too), and none of the FMinus features below:
  no completion, signature help, hover cards of symbols, Format or Cmd-/; its status line says SPICE. In the FMinus
  tabs a netlist's module is a type like any other: completion offers its methods after `pedal.` (with its header's
  usage line), a library netlist's module by name (adding its tab and its `#include`), and go to the definition opens
  the netlist at its line.
- **Completion and help.** Typing a name (or `.` or `->` after one, or Ctrl-Space anywhere) opens a list of what fits
  there: the locals and arguments in scope, the module's own fields, params and methods, the functions and modules of
  the files the program reads, a module's members after `.` (a vector's lanes), host.fm's fields after `host->`, the
  builtins with their signatures, the keywords, a loop and the contract's methods as snippets, the attributes after
  `[[`, the files in an `#include`'s quotes, and the library's modules the program does not read yet (choosing one
  adds its tab and its `#include`). Up and Down choose, Return or Tab accept (a function with its parentheses, the
  caret in them), Escape closes; a word typed whole needs no list, so Return then breaks the line. In a call's
  parentheses its signature shows, the parameter the caret is at underlined (Shift-Cmd-Space shows it again). The
  pointer still over a name for half a second shows what it is: its declaration, its documentation (the comments over
  it, or its library file's usage line) and any problem there.
- **Commands.** Shift-Alt-F formats the file in the house style of the examples and the library (Allman braces, four
  spaces a level, C's spacing, continuation lines under their parenthesis; lines re-indented and re-spaced, never
  joined): it checks that every token stays as it was, so the program means the same, and it is one undo step. Format
  on Run, the status line's switch (a preference of this Mac), formats every FMinus tab before each run. Cmd-F finds,
  Cmd-Alt-F finds and replaces (a bar at the text's top right: Return or Cmd-G the next, Shift-Return or Shift-Cmd-G
  the one before, Aa and ab for case and whole words, Tab the other field, Alt-Return in the replacement replaces them
  all, Escape closes), the matches marked in the text. Cmd-/ comments the lines or uncomments them; Alt-Up and
  Alt-Down move them, with Shift they are copied; Shift-Cmd-K deletes them, Cmd-L selects the line, Cmd-D the word and
  then its next, Cmd-] and Cmd-[ indent and outdent. Cmd-click a name, or Cmd-B, to go to where it is declared (in
  another tab too); a plugin gets no function keys, so F12 is Cmd-B here. Ctrl-G (or a click on the status line's
  `Ln, Col`) goes to a line: its number, or `line:column`, and Return. The caret resting on a name marks its other
  places in the file.
- **Problems.** The last run's messages are listed under the text, with their file, line and column; a click on one
  shows its file and puts the caret there. In the text they are squiggles (errors red, warnings amber, notes in the
  colour of the types) with a mark in the margin, and the status line counts them (a click there goes to the next). They
  come from RUN, and from a check while you type: 0.6 s after the last edit the files go to the compiler's thread,
  which reads them as far as finding what is wrong (FMinus's pass over the text at -O0 and the main module's methods,
  a few milliseconds; no code is made and nothing plays: the program that plays plays on). The squiggles move with the
  text until the next check replaces them. What only RUN finds (a method the audio thread calls that could allocate,
  one declared otherwise than the plugin calls it, the notes) waits for RUN, and stays: a check of the files RUN
  compiled (an edit undone) shows RUN's messages again.
- **The IR view**: what FMinus made of the program, as LLVM IR, the compiler's own language, in a read-only tab. The
  explorer's **LLVM IR** section has a row a view: `program.ll`, **optimised**, the IR after FMinus's `-O2` pipeline
  (LLVM's passes and FMinus's own), the very text the JIT made the program's machine code from (what `fminus --emit ll`
  prints); `program.lowered.ll`, **lowered**, the IR as FMinus's code generator wrote it for this Mac, before any pass
  ran; and, while the frozen copy plays (the snowflake, "Frozen dials" above), `program.frozen.ll`, its optimised IR,
  the dials folded in as constants (the names follow the program's file). A click opens the tab on that view (one
  tab: a row shows another view in it, each keeping its selection and scroll), at the right end of the tabs, apart from
  the files'; its **x** closes it. A few comment lines at its top say what it is and how the program stands (none
  compiled yet, the IR being made, the last RUN failed and the program before it playing on, the files changed since
  RUN, the frozen copy not playing), and RUN brings it up to date, as does a new frozen copy (the frozen view's: the
  program's views stay as they are, a selection in them too). It is LLVM IR highlighted (instructions, types,
  `%locals`, `@functions`, labels, constants, among them a float LLVM cannot write exactly in decimal, written as its
  bits, `f0x3D6C9820`, comments, attributes and metadata faint), with line numbers, the minimap, the find bar (no
  replace) and Ctrl-G; typing, pasting, undo and the line commands change nothing (the status says it is read only),
  and FMinus's completion, hover cards, squiggles and Format are not there. It is never part of the program: it is not
  compiled, no `#include` reads it, it counts against no tab rule, and it is not kept in the session, a project or an
  export; the chat does not see it. It costs nothing until you look: only while the tab is open does a RUN keep the IR
  (a few milliseconds more), and opening it compiles the program that plays once more for its IR alone, off the
  editor's thread, swapping nothing; closed, nothing is kept. A text past 2 MB is cut at a line's end, and says so.
  Worth a look to see what the compiler did with your code: a loop vectorised, a library module's method inlined, a
  dial read from its stream while it moves and as one value while it holds still, a frozen copy with its filter's
  coefficients folded to constants and its branches gone.
- **Code and Panel**, the top bar's switch: Code shows the editor with the program's interface under it (a .gui file's
  takes half the column at most and scrolls with the wheel past that; over a control the wheel turns it); Panel gives
  the interface the whole program card, the editor out of the way. The session keeps the choice.
- **The dials** are the program's parameters (above): in the sections its `#pragma panel` lines ask for (above),
  in their styles and beside its displays, or in order, in as few rows as the width holds (sixteen in two rows of
  eight at the default size, in one row on a wide window, 32 in three; every knob small when the window is too short
  for them); drag, scroll, or use the keys once Tab has focused one. A switch or a button takes a click (Return or
  Space steps it). The displays move with the sound, as hardware does. A program with a `panel.gui` has its interface
  instead (above): knobs, sliders and the XY pad take a drag (Shift is finer), the wheel and the keys, and a
  double-click for the default; buttons, switches and menus a click; a tab shows its page; in the synth, a keyboard's
  key plays its note (louder lower on the key; a drag slides across the keys).
- **The session** keeps the program that plays (all its files), the files as you left them (run or not), the tab shown,
  the dials and the project open (its version, the preset applied). A session whose program no longer compiles (an older
  FMinus, say) leaves the program that played playing (in a new instance, the first example, Init), and the session's
  files in the editor. A session saved before files loads as a program of one file.
- **The window** resizes in both directions, from 1000 x 600 (logical px, before the zoom) up: the editor and the chat's
  conversation take the height, the chat about a third of the width. The session keeps the size.
- **The Standalone** (each product's app) is a Mac window of its own kind: no title bar over the editor, its close,
  minimise and zoom buttons sit at the left end of the top bar, which keeps them room, and the top bar's empty parts
  drag the window (a double-click there zooms or minimises it, as System Settings, Desktop & Dock, has it). The menu bar
  holds what JUCE's Options button held: **Settings…** (Cmd-,) in the app menu brings the window forward and opens the
  editor's **Settings** card over it (the rest dimmed; Done, Escape or a click beside it closes it; the menu's own Cmd
  keys still work while it is open): **Output**, the device (a pill whose menu lists them), **Test** (a tone through it)
  and its channels in pairs, a switch each, when it has more than one pair;
  **Input** (the effect's), its device (or None), its pairs and **Mute input**; **Audio**, the sample rate, the buffer
  size with its milliseconds and the latency the device reports (the device type too, where there are several); **MIDI**
  (the synth's), each input with a switch. A choice the device refuses says why under its section, a device plugged in
  or out shows at once, Tab and Return reach every control, and the choices are kept as JUCE's dialog kept them, saved
  as soon as they change. While the effect's input is muted (JUCE's guard against a feedback loop through the speakers,
  as it starts), the status bar says so at its right, beside a **Settings…** button that opens the card. **File** loads
  a saved state, saves the state as a file, or resets to the default state; **Edit** undoes, redoes, cuts, copies,
  pastes and selects all in the text field you are typing in (its shortcuts work there as always, as do Cmd-Return and
  Cmd-S); **Window** minimises and zooms. The plugins in a host have the host's window, as before, and no Settings card.
- The theme (the top bar's moon or sun: Graphite or Paper) and the zoom (the status bar's pill) are machine-wide
  preferences, each product's own.

## The chat

The rounded card at the right is a conversation with Claude, through the [Claude API](https://docs.claude.com) with
your own Anthropic API key (billed to you). Claude sees the plugin's files, whether they play, the last compile's
messages and the dials (sent with a message when they changed; a file it has already seen goes as one line, not again
in full), and it can act on them: read, write, edit, rename and delete the files (the editor's tabs
follow, and Cmd-Z in a file undoes its change), add the library's building blocks, run the program (RUN; the compile's
messages come back to it), set the dials, measure what the program puts out, see the panel (view_panel: a picture of
it, drawn headless on the GPU as your screen draws it, shaders included, which goes to Anthropic with the request, so
that Claude can check its layout and fix it; the session keeps no picture), see the sound (view_sound), hold its work to checks it sets and tune the dials toward goals
(below, **Checking its work**), read the CPU load and the output guard's alerts, and ask you questions you answer with
a click. Ask the effect for an effect ("a tape delay with wow and flutter") and the synth for a sound ("a warm detuned
pad", "a plucked string"), either for a change ("make the filter resonate more", "a slower attack"), an explanation
("what does this program do?"), or help with an error or a click. It knows FMinus, the plugin's contract, the library
(each file's header) and how to build a large program from it, and it lays the dials out in a panel (its instructions
are in `Source/plugin/ChatPrompt.cpp`).

- **The conversation**: your messages in bubbles at the right; Claude's answers in light markdown (lists, headings,
  **bold**, `code`, and code blocks with a **Copy** of their own); a **Copy** under each finished answer (it shows
  under the pointer); Claude's thinking as a row that opens and closes; what it did (a file written, a run, a dial
  set, a sound measured) as a card of rows; errors in a tinted card. When a request leaves choices only you can make
  (above all a first, open one: "make me an effect", "make me a synth"), Claude asks: a card of up to four questions,
  each with its options as buttons (a ring for one choice, a box for several, the one it recommends first). One
  question of one choice is answered by its click, more by **Send answers**; or write your own answer in the message
  box instead. Its turn waits for the answer, and the session keeps a question that waits. While Claude works,
  pulsing dots say what it is doing ("Writing osc.fm (12 KB)"). The wheel scrolls; a round button brings you back to
  the end. An empty chat offers suggestions: a click puts one in the message box, and nothing is sent until you send
  it.
- **Pictures**: the picture button at the left of the message box's bar (or a drop on the chat) adds pictures to your
  next message: an effect's or a synth's panel to take after, a sketch, a screenshot, colours you like. PNG, JPEG,
  GIF, WebP, HEIC and TIFF; up to six a message and twenty a conversation (each request carries them all: a new chat
  makes room). They show as thumbnails in the box (each with a remove button) and above your message once sent, and go
  to Claude before your words, scaled to at most 1568 px on their long side (the API's advice), so a large photo costs
  what a small one does. The session keeps them, and their thumbnails.
- **Checking its work**: Claude cannot hear the plugin, so it measures, looks and checks instead.
  - **view_sound** renders the program offline and draws it as a picture, which goes to Claude and shows in the chat
    under its row: the waveform and the level over time, a spectrogram (20 Hz to 20 kHz on a log scale), the spectrum
    while the input plays or the notes are held, and a close-up of a few cycles. The effect's render has an input (a
    sweep from 20 Hz to 20 kHz by default; noise, an impulse for a reverb's or a delay's tail, a sine, or silence),
    and its spectrogram shows a filter's cut, what a drive adds and aliasing folding back down; the synth's holds the
    notes, then releases them, its spectrogram shows aliasing as lines folding back down and a click as a thin
    vertical line, and its spectrum marks the note's harmonics. About 1,200 input tokens a picture.
    `Source/dsp/SoundView.h`, `Source/plugin/SoundPicture.h`.
  - **set_checks**: from your request, Claude sets checks the work must pass, as numbers the plugin measures (the
    effect's gain, distortion (THD+N), brightness in and out, the impulse response's delay and tail and what comes
    out of silence; the synth's level, brightness, pitch, aliasing, clicks, attack, decay and release, when a voice
    ends and a chord's peak; the alerts and the CPU in both), each at least, at most or near a value, in a render of
    its own. The plugin measures them after every run of the program (the results go to Claude with the run's) and
    when Claude ends a turn in which it changed something: while one fails, the turn goes on with the results, twice
    at most, so Claude fixes the program or says why a check cannot pass (a turn that only answers a question is never
    sent on). The chat shows them as a card (a tick or a cross a check, its value against its goal), brought up to
    date as the turn goes; **Clear** on the card stops them. The session keeps them. `Source/dsp/Goals.h`.
  - **tune_dials** finds dial values for goals by rendering the program many times (up to 400 renders, a few seconds)
    and searching, instead of guessing: "the drive at -30 dB THD", "unity gain", "the release at 1.2 s", "the level
    at -18 dBFS RMS". It tunes up to eight float dials and sets the best values; the stop button ends the search with
    the best so far. `Source/dsp/Tune.h`.
  - **A scenario** plays the program as you would, in each of these and in measure_output: the synth's notes in time
    (a key played again before its release ends, fast runs, a seventeenth note that takes a voice, legato), its wheels
    and pedal; for both, dials moved while it plays (once a host block, as a DAW's automation), the transport stopped
    or moved, your host's sample rate and block size. A check keeps its scenario, so "no click when I play fast" guards
    every later run. The report's largest steps then each name the moment nearest them ("0.41 at 0.300 s, 0.4 ms after
    note-on 52"). **Trial edits** (`try_edits`) render a change on a copy of the files: Claude tests a fix before it
    makes it, and your editor, its undo and what plays stay as they are. `Source/plugin/ChatScenario.h`.
  - **compare** renders two versions alike (the editor's files, what plays, a saved version or a preset of the
    project open, other dial settings) and says whether they are the same to the last bit, or how and where they
    differ: the difference's level and when, a gain or a delay that explains it, the octave bands, the measurements
    that moved, the code and the dials that differ; with a picture of both on request. `Source/dsp/Compare.h`.
  - **trace** finds where a problem starts ("it blows up when I push the resonance", "it went silent", "it clicks"):
    a NaN, a click, a runaway, silence or a DC offset, with its moment, the voice and note, the program's field that
    went bad first and the lines that write it; or it watches fields you name over the render. It renders offline,
    twice (the second time a sample a call before the problem), and says when the output depends on how the host
    splits its blocks. `Source/dsp/Debug.h`.
- **Recording what you play (capture)**: for a problem that shows only as you play (a crackle when a chord changes, a
  glitch with the pedal, an effect on your own vocal), Claude can record it: it tells you first, arms the recorder, you
  play the passage, and it stops it and reads the recording as numbers (the notes, the pedal and the wheels, the dials
  you moved, the host's tempo and transport, the output guard's alerts, the CPU, the levels in and out). It keeps the
  last seconds before it stops (up to 20), or starts at your next note (the effect: your input's first sound) or when
  the host starts playing. The recording stays in the plugin's memory: never on disk, in the session or in a project,
  and only numbers go to Claude; it goes when Claude forgets it or records again, or after ten minutes. While it is
  armed the program's own code plays rather than its frozen copy (so the recording can be replayed exactly), which may
  cost a little more CPU. The report also gives the octave bands, the brightness and the stereo width in and out, and
  the largest steps (clicks), each with the moment nearest it ("0.38 at 4.213 s, 2.0 ms after note-on 64"); a picture
  on request. Then the recording **replays** offline, as you played it (your notes, pedal, wheels, dials, the host's
  transport and blocks; the effect: your input): measure_output, view_sound, a check and compare take it as a scenario
  ("capture", or a part of it), so Claude measures its fix on your own playing, and each says how closely a replay of
  what played matches it ("bit for bit", "from 0.20 s"). For an exact take Claude may ask to restart the program first
  (it cuts what still sounds). `Source/plugin/Capture.h`, `Source/plugin/CaptureReplay.h`.
- **A planner**: the model menu has two parts. **Build with** is the model that does the work (writes the files,
  runs, measures, tunes, answers); **Plan with** can add a second model that plans each of your requests first, Claude
  Fable 5.1 for the DSP and the architecture, say, with Claude Sonnet 5.5 building. Its plan (what to build, the signal
  flow, the dials, the files, the DSP's methods, what to measure and check) shows as a card that folds, and joins your
  message for the builder, which may ask the planner again mid-way (`ask_planner`, four times a request at most). Each
  plan is a request of its own to the planning model (the footer counts its tokens); **No planner** goes back to one
  model. The pill shows both ("Fable 5.1 > Sonnet 5.5"); the choice is machine-wide, as the model's is.
- **The message box** grows with its text (up to eight lines). Return sends, Shift-Return breaks the line. In its bar:
  **the model** (a menu of the models your key can use that think adaptively), **Thinking** (low to max:
  how much Claude thinks before it answers, and how hard it works), on Claude Opus 5.5 **Fast** (the API's fast
  mode: the same model writing its answers up to 2.5 times as fast, at twice the price), and **Web** (web search:
  Claude searches the web and reads pages and PDFs, a part's data sheet, a paper, a manual; the API runs both, five
  searches and five pages a request at most, $10 a thousand searches; what it searched and read shows as rows, and
  the pages its answer cites are listed under it), all four machine-wide and shared by the two products (higher
  levels and larger models answer better and cost more), and the round **send** button, which turns into **stop**
  while Claude works (what it did stays). The line under the box says what Claude is doing, then the last reply's
  tokens: in (and how many were read from the cache) and out, and "fast" when fast mode wrote it. When fast mode stays
  busy a turn goes on at standard speed; when your key cannot use it, it turns itself off and says why.
- **The header**: the **>** folds the chat into its rail; the pencil starts a **new chat** (it asks first); the key
  opens the **API key** dialog: paste your key (from console.anthropic.com, API keys) with Cmd-V and **Save**.
  **Remove key** forgets it. The empty chat has an **Add your API key** button, and a message sent before a key is
  stored opens the same dialog and goes once the key is saved.
- **Large work**: a complete synthesizer is a few hundred lines in several files (up to twelve tabs). Claude plans,
  takes building blocks from the library, writes its own files together, gives the dials a panel, runs them until
  they compile, then **measures** the result (a render made offline: levels, pitch, aliasing, the release and whether
  a voice ends, clicks, NaNs, the CPU cost, at a tempo and, in the synth, with the wheels where it asks;
  `Source/dsp/Measure.h`), since it cannot hear it, and sets the dials. An answer may be as long as the model allows
  (128K tokens, its thinking included), a turn may take 80 requests, a dropped connection or an overloaded server is
  asked again, and a long conversation is summarized by the API (compaction) when it nears 150K tokens, so it never
  runs out of room. An answer cut off at its limit goes on by itself, Claude told why; a second one in a row ends the
  turn (send "continue" to go on). At **Thinking: Max** (and XHigh) Claude thinks the longest, so a hard design is
  more likely to reach the limit: **High** leaves it more room to write.
- **The session** keeps the conversation, so a project reopens with its chat; the key is never in it.
- **Network**: the chat is the only part of the plugin that uses the network, and only when you send a message: HTTPS
  to `api.anthropic.com` (the Messages API, streamed, and the models list once per key). Requests are billed to your
  key. A turn goes on while the editor is closed.

**Claude Desktop as the AI (MCP).** Claude Desktop can do what the chat does, on your Claude plan instead of an API
key, and you talk to it as you would to the chat: "a tape delay with a little wow", "make a warm pad", "darker,
please", "what does the third dial do?". Install it once for each product: open `build-release/TestingPlayground.mcpb`
(the effect's) or `build-release/TestingPlaygroundSynth.mcpb` (the synth's), or, after the installer (below), the copy
in `/Library/Application Support/Funk/<name>/`, and Claude Desktop offers to add the extension (each product has its
own, with its own icon). Then open the plugin in your DAW or as its Standalone and ask Claude in Desktop for an effect
or a sound; its **+** menu also has ready-made requests (Make an effect or Make a sound, Improve the sound, Explain the
sound, and Fix what I hear: say what sounds wrong, a click on fast notes or a crackle in the chorus, and Claude listens
to what plays, records you playing it when it happens only as you play, reproduces it, finds where it starts, tries a
fix on a copy, puts it in, sets a check so it stays fixed and tells you what to play to hear that it is gone). Each
open instance has a name, the lowest number free: "Effect 1", "Effect 2", or "Synth 1", "Synth 2"; while Claude Desktop
is connected the chat card's subtitle shows it ("Claude Desktop calls this Effect 2"). With several open, say which you
mean by its track, its project or that name ("the one on my bass track", "the Moog one"); Claude asks when it cannot
tell. The plugin's chat card notes what Claude Desktop does; while it works nothing can be sent there, and Claude
Desktop's requests are refused while one of the chat's own turns runs (a look at the plugin's state still answers).
Claude Desktop's Stop reaches the plugin: a tuning of the dials stopped that way leaves them as they were. A request
may take up to 200 seconds (tuning the dials stops itself before, at 120 seconds or the limit Claude gives it, with the
best it found); one that takes longer is answered then and the plugin asked to stop it. While one runs, Claude can
still look at what the plugin is doing. Pictures are made small enough for Claude Desktop (the whole picture when
Claude asks for it). How it works: each instance listens on a private socket (in your account's temp folder,
`Funk-<name>-mcp/<n>.sock`: `Funk-TestingPlayground-mcp/` and `Funk-TestingPlaygroundSynth-mcp/`, each folder 0700,
each socket 0600), and `<name>MCP` (`TestingPlaygroundMCP`, `TestingPlaygroundSynthMCP`), the MCP server the build
makes for each product, passes Claude's requests to it, with the chat's own tools (each titled in words, those that
only look marked read-only, each described in a sentence or two, so that they take little room in every conversation)
and the chat's instructions as a guide that teaches FMinus, in parts: Claude reads its start (about 7,900 tokens for
the effect, 9,000 for the synth) and then only the part the work needs (a large program, the panel, the library's
building blocks, whose code it can read without adding a tab to your editor, the tools for a problem you hear, the
examples, the projects). Without the extension, Claude Desktop's `claude_desktop_config.json` can name the servers
itself:
`"mcpServers": { "testingplayground": { "command": "<this folder>/build-release/TestingPlaygroundMCP" },
"testingplaygroundsynth": { "command": "<this folder>/build-release/TestingPlaygroundSynthMCP" } }` (each finds its
sandbox's helper, `<name>GuiSandbox`, beside it).

**What plays, and what you changed.** Claude Desktop can listen to the plugin as it plays in your song, for a few
seconds (its levels in and out, the spectrum, the panel's displays, the host's tempo and transport, the notes arriving
and the voices, the CPU per block, the alerts), which tells it why something is too quiet, why nothing plays (the host
not calling the plugin, no MIDI arriving, or the program) or when it crackles, without rendering anything; those
numbers are live, and it says so. It also asks the plugin what you changed since its last look (each conversation
since its own): the dials you moved (in their units), what you typed (as a diff), your runs, versions and presets, the
host's bypass, transport and sample rate, and what the plugin's own chat did; so it works from where you are, and does
not write over your edits. Its other answers begin with a line when you changed something meanwhile.

**Designing a panel (the sandbox).** Claude Desktop can also design the plugin's panel with you, its interface file
(panel.gui), with the plugin open or not: "sketch a panel for a four-band EQ", "try my synth's panel as a dark
hardware look", "make the panel in ~/Desktop/bass/ fit on one page". It works in a sandbox: it starts from a blank
panel of the dials it means to have, from the panel of the plugin you have open (read, never changed), or from a .gui
file or a program's folder you name; it changes the panel, gets back what the plugin would make of it (its notes on a
line it cannot use, and where every element lands: overlaps, what sticks out, text too wide, what scrolls, a control
that names no dial, a dial with no control) and looks at it drawn exactly as the plugin's PANEL view draws it (the same
layout, controls, looks, fonts and shaders, on the GPU; the meters show a steady demo tone), in the dark or the light
theme (yours unless it asks for the other), at any size, a page at a time, with the dials where it sets them. Reading
the plugin's panel leaves no trace there: what you change in the plugin meanwhile still reaches Claude. Nothing
reaches your plugin until you ask: then it writes the panel into the program open and runs it, and says what changed
(and what you changed since); with none open it gives you the text to paste. The dials come from the program's
parameter declarations, read without compiling, or from the list Claude gives (a unit shows only where the panel's
file says unit=, as in the plugin). Claude Desktop's conversations share the sandbox: each design has a number
(Sandbox #48213: they start at random, so a number from before Claude Desktop restarted names none), the newest eight
are kept, and Claude names its own in every call, so two conversations designing at once never work on, or apply, each
other's. The extension carries a second program for it, `<name>GuiSandbox` (beside the
server in the extension's `server/`, and beside `<name>MCP` in the build), which draws panels and nothing else: it
holds the editor's drawing code and FunkGui, not the plugin (no FMinus, no audio; about 4 MB), starts when Claude
first uses the sandbox, is ended by the server after ten idle minutes (or when the server goes, even mid-picture) and
comes back with the panel as it was. The extension's `licences/` holds the licences of what it draws with (bgfx, bx,
bimg, bgfx.cmake, the JetBrains Mono font), as each plugin bundle's Resources do. view_panel stays the way to see the
panel you have now. `Source/sandbox/`, `Source/editor/GuiView.h`.

**Where the key is.** `~/Library/Application Support/Funk/Playground/anthropic-key.bin` (the folder 0700, the file
0600), shared by the two products, in any host and in their Standalones; `chat.json` beside it keeps the model, the
level, fast mode, web search and the planner. The file holds the key encrypted with AES-256-CBC and authenticated with
HMAC-SHA256 (CommonCrypto), under keys derived with PBKDF2 (100,000 rounds, a fresh salt each save) from this Mac's
device ID and your account: copied to another Mac or read by another account it is useless, and nothing on disk shows
the key in clear. A program running as you on this Mac could derive the same keys, as it could read your other files:
the encryption protects the key at rest (backups, a copied folder, a stray search), not from such a program. To revoke
a key, delete it at console.anthropic.com.
<!-- origin -->
Until 2026-10-28 the chat could also sign in to a TestingPlayground server instead of using a key; a plugin from then on
deletes what that kept here (`account-token.bin`, and the sign-in's lines in `chat.json`) when it opens.
<!-- /origin -->

## Build and run

Requirements: macOS 14 or later on Apple silicon, Xcode or its command-line tools, CMake 3.30 or later, Ninja,
Python 3. JUCE, bgfx and the shader compiler come from the machine cache `~/audio/.deps` (FCompressor's
`Scripts/deps.sh` fills it), FunkGui from `~/audio/libraries/FunkGui` at the pinned commit (`cmake/Deps.cmake`); a
machine without them fetches them from GitHub instead. FMinus comes from `~/audio/libraries/FMinus` at the pinned
commit, with the LLVM, Bison and Flex its own build left in its deps cache: build FMinus once first ("FMinus" below).

One configure builds both products; each of the presets below builds and tests both:

```sh
cmake --workflow --preset dev-verify     # configure, build everything, run every test (build-dev/)
open build-dev/TestingPlayground_artefacts/RelWithDebInfo/Standalone/TestingPlayground.app
open build-dev/TestingPlaygroundSynth_artefacts/RelWithDebInfo/Standalone/TestingPlaygroundSynth.app
```

`cmake --workflow --preset effect-verify` (or `synth-verify`) builds and tests one product alone; the build presets
`dev-effect` and `dev-synth`, and the targets `effect` and `synth`, build one (`cmake --build build-dev --target
synth`).

In the effect's Standalone, **Settings…** (Cmd-, in the app menu) picks the audio input and output; it starts with its
input muted (JUCE's guard against a feedback loop through the speakers, which the status bar says at its right, beside a
**Settings…** button): unmute it there, with headphones on. In the synth's, it picks the audio output and the MIDI
inputs. The settings, the plugin's state and the window's place are kept where JUCE's Standalone kept them
(`~/Library/Application Support/<name>.settings`, its keys `audioSetup` and `shouldMuteInput` for the devices).

### Install

Three ways, for three cases: the installer app (another Mac, or yours, with a window), `scripts/install.sh` (your own
build onto your own Mac, from the terminal) and the `.pkg` (every user of a Mac, through macOS's Installer). Keep one
copy of each plugin: a host that finds two lists both.

**The installer.** Every build also makes **TestingPlayground Installer.app** (`<build>/Installer_artefacts/<type>/`):
one app for both products, with both products' Audio Unit, VST3, app and Claude Desktop extension inside it (its
payload, `Contents/Resources/payload/`), in the editor's look (GRAPHITE or PAPER, as macOS's appearance is). For another
Mac, put it on a disk image with a short Read Me:

```sh
cmake --workflow --preset release-build  # Release with LTO, both products and the installer (build-release/)
scripts/package.sh --installer           # build-release/dist/TestingPlayground-Installer-<version>.dmg
```

Open it (from the disk image: it installs itself nowhere) and it goes through four pages. **Welcome** shows the two
products and the versions it holds; **Choose** has each product's four parts as switches, with what is installed now
beside each (its version, and what installing does: install, upgrade, keep a newer one unless you switch it on, already
up to date, or why it cannot), and whom for:

- **Just for me**: the plugins in `~/Library/Audio/Plug-Ins/Components` and `VST3`, the apps in `/Applications` (or
  `~/Applications` when you may not write there, as `install.sh` does), the extensions in `~/Library/Application
  Support/Funk/<name>/`. No password.
- **All users**: `/Library/Audio/Plug-Ins`, `/Applications` and `/Library/Application Support/Funk/<name>/`, where the
  `.pkg` puts them. macOS asks for an administrator's password in its own prompt; the installer never sees it.

**Installing** shows each part's progress, and **Done** what went where, with **Open TestingPlayground** (or the synth) for each app it installed
and **Add to Claude Desktop** for each extension (it opens the extension with Claude Desktop, which asks before adding
it; only when you click it, and only when Claude Desktop is installed, else the page says how), and a reminder to rescan
plugins in your DAW. **Uninstall...** (on Choose) lists every copy it finds, for you and for all users, and moves the
ones you leave switched on to the Trash. Return presses the page's main button (once: holding it presses nothing more),
Escape goes back (or stops an install between two parts), Tab moves the focus and Space flips a switch; every control
has a name for VoiceOver.

How it installs a part: a copy (`ditto`, which keeps the signature and the extended attributes; the quarantine flag is
dropped, as a `.pkg`'s files have none) into a staging folder beside its place, then one rename into place, so that a
failure at any point leaves the old copy whole where it was. The old copy goes to the Trash: the installer deletes
nothing (an uninstall moves to the Trash too; drag a copy back to undo). A failure stops the run, and the Done page says
in plain words what went wrong, what was installed before it and what was left as it was, with the log's place; Back
returns to Choose to try again (or for all users). A staging copy a crash left behind goes to the Trash on the next
install. For all users, the script macOS runs as root is handed to it as text, never a file, and writes only into
folders it checked and pinned, so nothing you run can change what root does while the password prompt waits. After an Audio Unit
changes, macOS's AU registrar is restarted so that hosts see it. It warns, at the top of Choose (and of Uninstall), when
the app it replaces is open, or a DAW (restart it to load the new plugins), or a product is installed both for you and
for all users (hosts list both). It writes only into the folders above, never through a link: a plugin folder that
is a link (to another disk, say) is left alone, and said so. What it did is logged in
`~/Library/Logs/TestingPlayground Installer.log`. Its name is the products' names' common start and " Installer"
(`cmake/Installer.cmake`), so renaming the products renames it.

Unsigned, macOS asks before the installer opens (Control-click it, **Open**). `scripts/package.sh --installer --sign
"Developer ID Application: <you> (<team>)" --notarize <profile>` signs a copy of it under
`build-release/dist/stage-installer/`, its payload inside out first (as the `.pkg`'s bundles, below), then the disk
image, which it notarizes and staples.

**From your own build.** `scripts/install.sh` copies your build into your own Library:

```sh
cmake --workflow --preset release-build  # Release with LTO, both products (build-release/)
scripts/install.sh                       # both products' AU, VST3 (~/Library/Audio/Plug-Ins) and apps (/Applications)
scripts/install.sh synth                 # or one product's (its key, or its name)
```

Builds never install anything by themselves; `scripts/install.sh build-dev` installs a dev build instead, and
`--dry-run` says what it would copy. The apps go to `/Applications`, or to `~/Applications` when you may not write
there (it says which). A bundle it may not replace (an app the `.pkg` installed is root's) stops it before it copies
anything: remove that one first.

**The .pkg.** For every user of another Mac through macOS's Installer, `scripts/package.sh` (after the release build,
or with `--build`) makes one package of both products,
`build-release/dist/TestingPlayground-<version>.pkg`, in which each product is a choice of its own (**Customize**) made
of four: the Audio Unit (into `/Library/Audio/Plug-Ins/Components`), the VST3 (`/Library/Audio/Plug-Ins/VST3`), the
app (`/Applications`) and the Claude Desktop extension (`/Library/Application Support/Funk/<name>/<name>.mcpb`; the
installer's last page says to double-click it, which adds it to Claude Desktop: an installer never opens Claude Desktop
itself). It installs for every user, beside what `install.sh` put in your own `~/Library`: keep one or the other.
`scripts/package.sh synth` packages one product. Look inside without installing: `pkgutil --payload-files <pkg>`,
`pkgutil --expand <pkg> <folder>` (its `Distribution` is the choices).

The `.pkg` is unsigned for now: the bundles keep the build's ad hoc signatures and the package is not signed, so macOS
asks before it opens (Control-click it, **Open**). Signing is ready and off unless asked for:

```sh
scripts/package.sh --sign "Developer ID Application: <you> (<team>)" \
                   --sign-installer "Developer ID Installer: <you> (<team>)" \
                   --notarize <profile>      # a profile stored once: xcrun notarytool store-credentials <profile>
```

(or the environment's `DEVELOPER_ID_APPLICATION`, `DEVELOPER_ID_INSTALLER`, `NOTARY_PROFILE`). Every bundle and binary
(the plugins, the apps, the shaderc inside each, each extension's server) is signed with the hardened runtime and a
timestamp, on copies under `build-release/dist/stage/`; each app with `scripts/entitlements/<kind>.plist`
(`com.apple.security.cs.allow-unsigned-executable-memory`, which FMinus's JIT needs: below; the effect's also
`com.apple.security.device.audio-input`). Then `productbuild --sign`, `notarytool submit --wait` and `stapler staple`
on the .pkg. Each product's plugin code (`products/<key>.cmake`; `scripts/plugin.py info`
prints both identities) is its own, so the two load in one host beside each other and your other plugins. A host that
keeps keys for itself (some do for the space bar or the arrows, or play notes from the computer keyboard) may need its
"keyboard to plugin" option on for typing in the editor.

Each block the plugin reads the host's tempo, position, time signature and transport (and passes them to the program
through host.fm); the synth also reads from the MIDI the notes, the mod wheel (CC 1), the pitch wheel, channel
pressure, the sustain pedal (CC 64) and All Notes Off (CC 123).

## Test

```sh
scripts/test.sh                       # build both products' probes, run them; outputs in build-dev/probe-out/<key>/
scripts/test.sh synth                 # the synth's only
scripts/test.sh -R fminus.contract -V # one probe, for both products, every row printed
ctest --test-dir build-dev -L effect  # every probe of the effect (after a build)
```

Every probe runs for both products, as `effect:<probe>` and `synth:<probe>` (labelled `effect` and `synth`, and
`probe` and the probe's layer), each against its own build of the plugin; the lints run once, and so do the installer's
probes (`installer.engine`, `installer.gui`, labelled `probe` and `installer`), which install into a folder of their own
only. Where the kinds
differ, a row says what each product's run checks. Run the probes through ctest (or `scripts/test.sh`): outside CTest
a probe would find the user's own folders.

| Probe | Checks |
| --- | --- |
| `fminus.examples` | every example (with the library files it `#include`s, as EXAMPLES adds them) compiles with no message, its dials named, with ranges and defaults inside them; plays (the effect's two seconds over noise; the synth's a chord for a second, then released), finite, audible and with no alert; the same bits in blocks of 64 and 333; the synth's voices end within eight seconds, and a note on an instance a stolen voice left starts as on a fresh one (no skipped attack) |
| `fminus.init` | the default program, the first example, Init: titled so and compiling with no message. The effect's, at its defaults, gives its input back bit for bit (noise and a sine, in irregular blocks of 1 to 512 samples); OUTPUT at -6 dB halves the level and INPUT at +6 dB doubles it (within 0.1 dB), MIX at 50 % gives the average, at 0 % the dry input bit for bit; OUTPUT turned 24 dB down between two blocks does not click. The synth's: a held C4 at its pitch within a cent, bent 2 semitones by the pitch wheel; its level following the ADSR (full from 5 ms and steady, the first sample 0, a half SUSTAIN 6 dB down, a slow ATTACK, -60 dB in RELEASE after the key's release, the voice ending with its last sample more than 70 dB down); a note on the instance a stolen voice left starting as a fresh one does (the same bits, its attack not skipped); velocity and VOLUME on the level; CUTOFF turned down lowering the spectral centroid below half; the sweep its header describes (CUTOFF and SUSTAIN down, ENV AMOUNT up) falling in brightness over the note, and holding still at full SUSTAIN |
| `fminus.contract` | what a program needs, and what the plugin says when it does not fit: no module `Effect` (the synth: `Voice`), no `process` (the synth: `start`), a `process` of another type (refused, never called), one that could allocate, a syntax error with its line; 32 dials and a note for the 33rd parameter; a held module's parameters at their defaults; init gets the rate; a module Shared made once, its init before the main module's (the synth: every instance's), which reads what it made through a `Shared*` field, again at a new rate, read on the audio thread, a note when nothing points at it, a `Shared::init` of another type refused; several files: an included function plays, the main module in an included file, a file two files include read once, an included file's error with its name and include stack, a file not found, the plugin's message for a wrong `process` after an `#include` |
| `fminus.library` | every library file named on its first line, with a usage header, compiling alone and all together (a netlist in its own `*` comments, generating with no message and a configuration of its own); each measured: the filters' responses (SVF and biquad -3 dB at their cutoffs, notches under -60 dB, the ladder's 24 dB/octave and its self-oscillation at the cutoff), the delay exact to the sample and its four-point read, the reverb's decay times, width and level, the oscillators' levels, DC and pitch and the saw's aliasing against a naive one, the noises' levels and colours, the envelope's times and its retrigger without a step, the LFO (its sine, a polynomial, within 0.0011 of sin; and one following the song's position), the filters' coefficients (`svfG`, `ladderG`, `onePoleG`: tan, clamped as `set` clamps) through `setG` playing what `set` plays within 1e-6 of the peak, the follower and the compressor's curve, the shapers' bounds, the crusher's steps and the conversions; the wavetable's shapes (RMS, no DC, pitch), nothing inharmonic above -80 dB from 1 to 11 kHz where a naive saw's aliases are above -40 dB, the 16th harmonic at full strength at the end of a fade, a glide's copies continuous and rising, a saw-to-square morph that keeps its fundamental, every call safe before build |
| `fminus.circuit` | circuit netlists in a program (`fminus/Circuits.h`): each library netlist played by a program of the product's kind (an Effect over noise, a Voice over a saw), its pots dials set on the 16-sample grid, its law inputs swept every sample: it compiles with no message and no self-test wanted; 2 s in blocks of 64 and of 333 give the same bits, finite and sounding; its frozen copy keeps the layout, plays the program's bits in exact math and is 80 dB under the signal in fast math; trace's second pass gives the first's bits and the field map lists `pedal.x_C1`; the language service's init, set, tick and clear are the compiled module's. A broken netlist's error at its line, in compile's and check's messages, stopping the compile; an `#include` of no such netlist FMinus's `file not found`; a netlist as the program the plugin's error; FMinus's error on a generated module (a module named twice) cited at the netlist's `.title` line; a netlist included twice generated once, one never included not compiled (no job) but checked (its squiggles); freeze, ir and frozenIr generating nothing; a netlist with its configuration open (Yeh's clipper) compiling at once with one job and the provisional note, word for word; after its self-test (`fminus::circuit::selfTest`) stored, compiling with no job and no note, the module `generate`'s with that answer, the provisional program's frozen copy keeping its own layout; the answers' store (find, of, the in-flight set, 64 entries) |
| `fminus.random` | RANDOM's programs, seeds 1 to 160: one program a seed, every seed's another; its first line names it and its seed, and it #includes its library files; each compiles with no message, at most sixteen dials, every one in a titled section; the effect's: noise and a sine through it play finite, audible and under +6 dBFS with no alert, and silence through a fresh one stays silent; the synth's: a chord held a second and released plays finite, audible and under 0 dBFS with no alert, every voice ended within five seconds; the first sixteen the same bits in blocks of 64 and 333; every part the generator can choose chosen by some seed |
| `fminus.gui` | a `.gui` file: a rich file's every element in its tree (the panel's colours, containers and their properties, sections, a box, tabs and pages, labels, a divider, a spacer, every control and display), options and labels for the host; comments, `;`, a `{` on the next line, an escaped quote, `#rgb`; a log taper's middle (the geometric mean) and back, units (Hz to kHz), decimals and scale, a typed text back to its value; displays (kinds, taps, two of one source, a range); the parameters left out, last with a note; a note on its line and column for each thing it cannot use (an unknown element, property or value, no such parameter, one twice, a control that does not suit its parameter, a pad on an int, an envelope of three, a label without text, tabs without pages, a page outside tabs, a colour, a unit after a number, a log taper from 0, decimals out of range, a field that is not there, an open quote, a stray `}`, an unclosed `{`), the rest standing; a second `.gui` file and `#pragma panel` lines beside one, noted; pragmas alone as before; classes and defaults (an element's own property beating its class's and the defaults'), every look and style property in its node (bar tabs, a plate section, a row that wraps, a knob's class, look, ticks, bipolar, label and value, a slider's colours, an inset box, self= and minwidth=, a label in px, a divider's thickness, the gradient; the synth's a keyboard of three octaves with wheels), live= tapping its field once, and their mistakes noted on their line and column |
| `fminus.panel` | the `#pragma panel` lines: sections, titles, order, a row break, labels and small knobs, comments and commas; an untitled section; the dials left over, last and noted; a note on its line for each line it cannot use (an unknown word, no such dial, a dial twice, an open quote, a missing label, an empty section, a title or label too long), the program still playing; lines in an included file, after the program's, and none from a file no `#include` reads; labels without a section; displays (each kind, of `in` (the effect's), `out` and the main module's fields: name, `as`, label, range, the fields' taps in order; a display in a section and one left over; notes for a field that is not there, an array, a dial as a source, a name taken, a range from high to low, the synth's `in`, which an instrument lacks); styles where they suit and a note where not, several dials a line; options; colours; displays without a section line |
| `dsp.swap` | the Engine. The effect's: no program passes the input; the first program fades in from it, a second crossfades from the first over 20 ms with no step beyond the signal's own, bit for bit outside the fades, and the old one is handed back. The synth's: a held note's voice fades out when another program arrives, which starts the key again; seventeen notes steal the oldest; a voice that returns false is free. Both: a real dial glides over 20 ms sample by sample, an integer one steps at the block; a NaN, an infinity and 10 leave as 0, 0 and 4, with their alerts |
| `dsp.tune` | the numbers checks and tune_dials read, and view_sound's analysis (Measure.h, Goals.h, Tune.h, SoundView.h), on programs whose answers are known: the effect's gain as its dial (sine, noise, the impulse's peak), its THD+N rising with the drive, a darker tone a lower centroid, the impulse's delay, silence silent; the synth's pitch, attack, release and voice end as its dials set them, a brighter filter a higher centroid, a high naive saw's aliasing, infinity for a release that never ends, sixteen voices measured at once costing more than one, the CPU line saying the dials were held still; every metric measured and worded; one compile measured again the same, a render after another (Engine::restart) the same as a fresh compile's, `want` measuring only what it asks, run()'s report the Session's; goals' bounds, targets, tolerances, infinity and a value not measured, a goal that cannot be refused; checks passing and failing (the synth's also not measured), a program that does not compile; tune_dials meeting a gain and a THD+N (the synth: a level and a release) and two goals at once within its budget, goals met already in one render, an int, an unknown dial and no goal refused, progress stopping it, the same dials every time; the spectrogram's and the spectrum's loudest place a 440 Hz tone's, the close-up four of its cycles, the effect's sweep rising through the rows, the waveform's peak, the synth's note-off, a close-up asked for; a scenario's render (the synth's phrase with a retrigger, fast notes, a steal and legato, its wheels and pedal; the effect's transport stopped and moved) the same bits in blocks of 64 and 333 and again, and with a dial moving the same again at each, a hook that cuts every call to 16 samples changing nothing, the click locator naming the note-on (the synth) or the dial's step at a block's start (the effect); compare: a program against itself and a recording of it identical (a shorter recording too), a gain dial a gain, a delay dial a lag (also of a note still sounding past the correlation's 2^17 samples); a hook that stops a render leaving the rest silent; a move after the held part rendered |
| `dsp.meters` | what the Engine publishes for the displays: the effect's input and output peaks and RMS, left and right, within 1 % (the synth's output; no input: 0); the peaks falling 10 dB in half a second and the RMS 40 dB; the fields the displays show (a float, an int, a bool, as written last; the synth's from the newest sounding voice, then the one left, then none); the scope's samples bit for bit; the frame count; the program the fields are of, not one offered and not yet taken; with no reader (an editor open, a tool that listens) the levels read 0 and the scope stands still while the fields and the frame go on, and a reader back finds the next block's own levels and the scope silent before it; LISTEN, what listen reads besides: the samples, the host's transport, a block a ring entry (its length, its share, its flags: metering, a dial moving, the guard acting), the synth's note-ons counted (not the held keys a new program starts), the keys held, the voices sounding, held by the pedal and fading (a swap, a steal at a block's end), a steal counted, a voice dropped for going non-finite with its key and sample |
| `dsp.freeze` | frozen dials: what the Engine says of them (the program, how long they have held still, from its arrival or the end of a glide, their values); a frozen copy (fast math) at the dials' values plays from the next block, the same samples as the program's own code (finite, the difference 80 dB under the signal), in blocks of 64 and of 333 too; a dial moved drops it that block, the same samples still; a copy at the new values plays again; a copy at other values, or of another program, is not taken; each example frozen at its defaults plays the same spectrum (every octave band within 0.1 dB each half second); LONG: the examples and RANDOM's seeds 1 to 64, ten seconds frozen in irregular blocks, finite, every half second's level and the peak within 0.1 dB, the same spectrum; the time live and frozen take, and the programs more than 5% slower frozen, by name (printed); and beside it a copy in exact math (the program's Math::Vector, `Patch::freeze`'s `exactMath`, which the plugin never plays): its time, and how many play the program's bits (printed) |
| `dsp.host` | the program's Host block (host.fm) as the Engine fills it: host.fm compiles with no file of that name, and a module Host nothing points at is a note; the host at 90 BPM in 3/4 from beat 4 (the tempo, the signature, playing, the position of each call's first sample); in blocks of 48 and 80 the output the same as of 256 and the position that of the last call's first sample; a stopped host's position standing; a host that says nothing at 120 BPM, its position running on. The effect's: its wheels at rest; the Tremolo example at SYNC 1/4 taking its phase from the song's position, the Echo repeating after TIME, and at SYNC 1/4 after a beat at 90 BPM. The synth's: the mod wheel, the pitch wheel and aftertouch from their events, the wheel up bending a sine 2 semitones; a key let go under the sustain pedal sounding until the pedal comes up; the Sine example bent 2 semitones by the pitch wheel |
| `dsp.debug` | trace (Debug.h) on programs with planted bugs, each product's kind: a state-variable filter past the Nyquist frequency (not a number after its cause, in the synth's voice of note 72; its first culprit one of the filter's fields, with the line that writes it and its values around it; find any also finds the runaway before it), a phase that starts again now and then (a click at its sample, its culprit the phase, the line that resets it among its writers), a division by zero into the output alone (not a number at its sample, no field gone bad, the verdict naming a local); the first example clean, the self-check's 1-sample calls the same bits; a level smoothed once a call found by the self-check; fields watched by path and glob, the map listed, raw values a sample each; the synth's other voice alone clean; the same lines twice; silence (the synth: a voice that ends while its key is held; the effect: a gain stepped to 0), a DC offset found at the start; a sentinel init sets past 1e6 noted, the click after it still found; a delay line fed back at 1.1 (its buffer named, not a gain or a local blamed); two fields bad in one sample named together; a note-on's click the note-on's (the synth); a low saw's wrap no click; a Shared field listed and watched; a cancel in pass 1 (the NaN listed, not traced, no "Nothing found") and in pass 2, the time limit; a NaN from the first sample (the self-check says it had nothing to compare) |
| `plugin.process` | the processor as a host drives it: a new one plays the first example, Init, its dials at their defaults (the effect gives its input back bit for bit, the synth's A4 sounds at 440 Hz within a cent); P1 to P32 named and worded as the program's, host writes reach the program (within 0.05 dB), a host's NaN ignored (its text a dash), a block of 0 samples (the synth's with a note-on in it) and a block larger than announced change nothing; the effect's bypass passes the input bit for bit (the synth's is silent) and its edges do not click; a block before prepareToPlay passes the input with NaN and inf as 0 (the synth's is silent); the synth's note-on at velocity 0 and All Notes Off release; the host's playhead (100 BPM, 7/8, from beat 8, playing) reaching the Host block, the effect's wheels at rest, the synth's mod wheel, pitch wheel at both ends, channel pressure and sustain pedal from MIDI, and the keyboard's wheels (`EditorPort::playWheel`); the freezer (its dials still for its delay, the frozen copy plays, the same sound as a processor that never freezes (the difference 80 dB under the signal), a host write drops it that block and it comes back) |
| `plugin.capture` | the recorder (`plugin/Capture.h`) as a host drives the processor, in blocks of every size (0, 1, over the announced): armed through the tool, mode last keeps the last second in whole blocks, its sizes, dials, transport, events (MIDI and the keyboard's) and the input and output bit for bit what the host gave and got, and a processor that records sounds as one that never does, bit for bit; mode next starts at the first note-on (the effect: the input's first sound) or the host starting to play and fills once; the host preparing again goes on before it starts and ends it after; small blocks and 40,000 events in a block (the synth) end or shorten it with a flag and never hold the audio up; a frozen copy dropped while armed and back after; forget frees (at once, or at the audio thread's next block); a 20 s recording's size and how long status and stop take; the tool's words (live, at most 25 lines), and nothing of a recording in the session; the report's bands, steps and replay line, its picture; a take replayed offline bit for bit (off the voices' grid, in parts, a sustain-pedal phrase, the effect's input), through measure_output, view_sound, compare and trace (through the program that played it) too, and a part of it; the synth's eighteen keys at once, the Engine's steals in the take's blocks and the report; a take started mid-playing named so; a restart's take bit for bit; status copied on another thread while blocks of 16 samples play, every take whole; a host's blocks of 65,536 samples; without a host transport, a program that reads the song's position replayed bit for bit, twice in one Session, after an arm with restart just after a run; a take without MIDI replays no notes; no picture of a recording kept in the chat's session |
| `plugin.projects` | the projects, in a folder of the probe's own (under CTest the default is the sandbox's): names tidied; a program of two files and its two dials saved as a new project, version 1, listed with a date, the JSON naming this plugin and its kind; saved again unchanged, no version added; an edit and a dial each a version, with their notes; another processor opening version 1 (its files, tab and dials) and the newest; an older version saved, the newest; a preset of the dials (from its version), a dial moved and the preset applied back at once, applied in a processor with no project open (its project opened, the dials after the compile), refused with no project open, replaced case aside; dials by name in any order (an unknown one ignored); a name in another case the same project; the session keeps the project, its version and the preset, an old session's preset a project; an old preset file migrated (one version, the file moved aside); the newest 100 versions kept; another kind's project, a damaged file, a missing name and a missing version refused, the draft untouched; a preset, a version (never the only one) and a project deleted |
| `plugin.export` | a project's info and its exports, in a folder of the probe's own: the description, author, category, tags and genres kept tidied (tags split at commas, once each case aside; a long description cut) and in the file; a project of 30 versions as a .fmpack (FMPK, format 1, xz on macOS, the header's size and CRC-32 the JSON's, an .xz stream, an eighth of its text at most), imported as "<name> 2" with the same versions, presets and info and opened with its newest files and dials; the newest version only; a source .zip (README with the description, tags and presets; project.json; the newest files on top, older ones under versions/v<N>/) imported the same; a zlib package; another kind's package, a changed byte, a header cut short, a file that is no export and a zip with no project.json refused, nothing added |
| `plugin.mcp` | Claude Desktop's way in, in a folder of the probe's own: a processor's socket (0600, its folder 0700) and the tools.json and guide.md it leaves; info, the tools (the chat's but ask_user and ask_planner, each MCP description its first sentence, a setting's description given once unless it has a number: tools.json's size printed, at most 25,000 bytes) and the guide through it, in parts (start, its size printed, large-program, interface, library, a file's header, a file's code with no tab added, debugging (listen's, capture's, trace's and compare's notes, not in start), examples, projects, circuits (a guide from before it still cuts), all; an unknown part refused; each part written to `probe-out/<key>/plugin.mcp.guide.<part>.md`); write_file, run_program, read_file and view_panel (a PNG, drawn by the GPU) through it, the chat's card noting Claude Desktop once, an unknown tool and ask_user errors; on a second connection with a slow program, get_status and info answered while a tune runs (whole lines), the tune cancelled with its dial as it was, a call waiting behind it cancelled at once, a tune with a time limit of a second stopping with its best, the window's and the sound's pictures at most 100,000 base64 characters (and whole when asked); the instances named Effect 1, Effect 2 (the synth: Synth 1, Synth 2), the product's server program over stdio as Claude Desktop runs it: initialize (2025-11-25 too; instructions that keep tools and ids from the user), tools/list (each tool with `instance` and a title in words, read-only ones marked, gui_sandbox among them), list_instances, get_status and the guide (and a part) routed, the plugin then showing it is connected, the prompts (one filled in, Fix what I hear naming no tool and carrying the plugin's state); a server whose calls may take 1.5 s answering a tune past it once ("still running") while list_instances and get_status answer, the tune stopped with its dial kept, the plugin still reached; a large request sent while the link answers the guide's start, without reading, going through (the link keeps reading), and a server whose only instance never reads still answering a ping, the call answered at its deadline; a guide request naming no part (a 0.2.0 server's) getting the whole guide; a second processor on a track with a project reached by its name, its number, its track and part of its project, a word matching nothing and two open with none named each asking the user; a processor gone, its socket gone; with none open, a call says to open the plugin, the tools still list and the guide's parts come from guide.md |
| `plugin.sandbox` | the GUI sandbox through the product's MCP server over stdio and its real helper, in a folder of the probe's own, to `gui.sandbox.png`, `.paper.png`, `.knob.png`, `.shader.png`, `.example.png` and `.example.view.png`: gui_sandbox listed with no instance open (titled, not read-only, its schema small), a render before an open refused; a blank panel of the dials given; an edit naming no dial noted on its line and listed, a dial no control names listed, an old_text that does not occur refused with nothing changed; an overlap, what sticks out of its container, text too wide and a panel that scrolls reported; a render of the size asked (1.5 pixels a px) in both themes; a dial's value moving its knob and nothing else; a large render made smaller to fit the MCP cap; a second sandbox laid out at the PANEL view's size, a dial's unit shown only from the .gui's unit=, the first edited by its number and the second drawn as it was, an unknown number refused, apply without a number refused; a program's folder opened from disk, its dials from their declarations, its displayable fields listed (not an array), its shader drawn by the GPU, not as the placeholder; the helper killed between calls coming back with the panel as it was, at the last render's size; apply with no instance giving the text back, by number (an older sandbox's while another is open); close ending the helper, a sandbox kept coming back after it; the prompt's example opened from an instance and drawn in the user's theme as view_panel draws it, to the pixel but the displays and live= marks, the read leaving the plugin's record of what MCP was told alone; apply writing panel.gui into the instance's program and running it, the user's change said first; the .mcpb holding the server, the helper and the licences; a helper that hangs answered at the deadline and the next call working; the helper idle ended by the server and back quietly; the server gone while its helper hangs, the helper stopped |
| `plugin.live` | listen and get_status's since over a processor a host thread plays in real time, in folders of the probe's own: listen live (the host's blocks, 100 BPM in 7/8, playing, the rate and the largest block; the effect's input -15.1 dBFS RMS, its gain -6 dB, the output's peak at 1 kHz; the synth's three note-ons, their range, velocity and keys held, three voices, the spectrum's peak at a note; a display's field; the CPU per block; text and JSON; the readers raised while it listens and back after); the host's bypass said; a host that calls nothing said; through the socket a listen of 5 s with get_status answered meanwhile, cancelled at once; get_status's dial lines in their units and options, its token, and with since the editor's drag in one line, the host's automation, a typed edit as a diff against the file at the token, RUN, a version and a preset saved and applied (its dials no move), the bypass, the transport starting, a new sample rate; nothing after that; another instance's token the whole state; the hint on another tool's result while changes wait; each token its own (two from before an edit both shown it; Claude Desktop's own write no change, but in the diff); a host move just before a preset that leaves that dial alone still a move; a bypass ending after a token said once, never as still on; two cancels before a listen starts, the first one's listen stopped at once; New chat while a call of Claude Desktop's runs keeping the chat busy until it ends |
| `plugin.state` | the session round-trips (a program of two files, the draft's three files and its tab, the dials, the output bit for bit); a run moves a renamed parameter's dial to its default and keeps a kept one's; a session whose program does not compile leaves the last one playing; one without a program plays the first example; one saved before files loads as one file; one with too many files or bad names loads tidied; a NaN dial loads as 0.5 |
| `plugin.chatstore` | the key on disk, in a folder of the probe's own: a key's shape; saved, the file is the format above with no 12 characters of the key in it, 0600 in a 0700 folder, and loads back; saved twice it differs (fresh salt and IV); one byte changed anywhere, or the file cut short, and it does not load; forgotten, it is gone; the model, level and fast mode round-trip, a damaged chat.json loads as none; what an earlier build kept of a sign-in (its token's file, chat.json's two fields) deleted, the choice and the key kept, nothing made when there is none, a damaged file no trouble |
| `plugin.chat` | the chat's agent against a scripted API (no network): the event stream however it is cut, a message rebuilt (thinking and its signature, text, a tool call's streamed input, a compaction block, the usage over its iterations); no key, no request (and nothing asked but a key); a turn that writes a program (the synth: a voice), runs it, sets its dial and answers; each request's shape (model, the model's own max_tokens, adaptive thinking, effort, the cached system prompt, the twenty tools, fallbacks and compaction with their betas, the API version, to api.anthropic.com with the key alone) and the history append-only; another model and level; fast mode (`speed` and no fallbacks, the footer's "fast", standard speed when it stays busy, off when the API refuses it, the beta on Opus 4.8, none on Sonnet); a tool input that is not JSON; an answer cut off at its limit (the turn goes on, Claude told; twice in a row it ends; a tool call cut off runs nothing; the limit note at max effort, none at medium); a retry after 529; a 401; a refusal; a fallback; a file Claude has seen named but not resent; Claude Desktop's own record of the files apart from the chat's (a file it wrote or read goes to the chat with its text, and one the chat wrote is new to it: get_status since shows the chat's edit and its lines, and with the next token nothing); measure_output through the agent; view_panel through the agent (a PNG of the panel and one of the whole editor, drawn on the message thread by the GPU, with a line of what each shows and that the GPU drew it; the user's view kept; with the GPU forced off, a picture still and the note that each shader shows as a striped placeholder, and why; no picture in the saved session); view_sound (a PNG of 1120 x 800 and what it shows, the tool's row with its thumbnail, the session with the thumbnail and not the PNG, nothing drawn for a program that does not compile); set_checks (measured at once, a failing check sending the turn on with the results as the user's line, a dial set and a run fixing it, the run's result ending with the checks, one card a turn, checks that cannot pass going on twice and then ending, a metric that is none refused, the session keeping them, Clear); tune_dials (a gain (the synth: a level or a gain) met and set, the dial then measuring as asked, an unknown dial refused); trial edits (the first line saying so, the editor untouched, an edit that does not match named) and a scenario through the agent (one that cannot be refused, naming its part); compare through the agent (a trial edit that halves the level a gain of 0.5, version:N with no project open, dials "current" keeping a {dials} side's own); trace through the agent (nothing in the program that sounds, a trial edit's NaN found at the first sample in a local, the recording and a find it does not know refused); a check that would play the recording refused; pictures (a PNG kept as its bytes, a large JPEG scaled to 1568 px, thumbnails, a text file and a broken picture refused with a note, detach, the pictures before the words as base64 image blocks, a message of pictures alone, six a message, the session keeping the thumbnails, a new chat dropping the attached); web search (off at first; on, web_search and web_fetch in the request and the thinking dropped, what Claude searched and read as rows, the sources under the answer, the footer's search count, the API's blocks back as they came with their encrypted parts, a paused reply sent back as it is, a 400 that wants the tools called directly, an organisation without web search); ask_user (the turn waiting with the question open, the other calls' results kept and a second ask refused, a session saved meanwhile opening with it still waiting, the answer's labels as its result after the others, a question answered by a message instead, one of one option refused); add_library_file (the tab, its text in the result and seen after it, an unknown name, the same file twice); a stream cut short asked again and shown once; a compaction (the history starting at the summary, the files sent again), in the middle of a turn too (every tool result with its call, the files with the results), and a session saved with the old cut repaired; a session saved with another system prompt or none loading without its thinking; STOP; the session; what an earlier build kept of a sign-in (its token's file and chat.json's fields deleted when a processor opens, the choice kept, a session from then loading, the next turn to api.anthropic.com with the key, the history valid; the key removed, a message asking for the key alone and sending nothing); NEW |
| `plugin.chatexample` | the system prompt has the large-program section, the .gui file's language (THE INTERFACE), every library file's header (a netlist's `*` lines), CIRCUITS (the netlists, last) with the circuit example naming the library files it uses, and the program built from the library for the product's kind and its `panel.gui`, word for word; that program compiles with no message (its interface too), every dial a control in a titled section, two displays, plays finite and audible, the same in blocks of 64 and 333, its voices ending; measure_output's numbers on programs whose answer is known (a level, a pitch, a gain, an impulse), and its notes for a voice that never ends or an effect that makes sound from silence; the synth's Inharmonic line (a sine's clean; at C7 a naive saw's aliases above -40 dB and a wavetable voice's clean, its table built once in `Shared`) and a saw's own edges no click, a phase that jumps once a click; the prompt's FFT and `Shared`; THE HOST and host.fm word for word, measure_output's Host line and the effect's delay of a beat at 90 BPM, the synth's voice on the pitch wheel 2 semitones up; the example in a scenario the same bits in blocks of 64 and 333 and again (with a dial moving, again at each), its report saying so |
| `gui.chat` | the chat column and resizing, headless, to `gui.chat.png`, `.empty.png`, `.key.png`, `.stored.png`, `.paper.png`, `.small.png`, `.large.png`, `.folded.png`, `.ask.png`, `.answered.png`, `.pictures.png`, `.sent.png`, `.checks.png` and `.checks-fail.png`: the empty chat (the key button and nothing else to get in, suggestions); the key dialog (opened by a message with no key, Escape, modal, a key refused, pasted, never spoken, Save sends the waiting message to api.anthropic.com with the key, no account or credit anywhere, a stored key's line and Remove); the message box growing with its rows and shrinking when sent; a turn shown (thinking that opens and closes, what Claude did, the answer, a code block and its Copy, an answer's Copy, the usage), its files as tabs, the program playing, Cmd-Z undoing Claude's change; the model and thinking menus; the Fast switch (on Claude Opus 5.5 only, a switch to a screen reader) and the Web switch after it; stop; Claude's question as a card (the message box saying so, the options as radio buttons and toggles to a screen reader, a pick that replaces or adds, Send answers waiting for each question, the labels sent, the card then without buttons and the answers in the transcript; one question of one choice answered by its click); pictures (the picture button's file chooser, thumbnails in the box drawn with none missing, a remove button, a drop on the chat's card taken and one on the editor not, a text file no drop, sent before the words and shown above the message, FunkGui's registry holding them until a new chat); Claude's checks as a card (a row each, the sound's picture under its row, drawn; brought up to date when a fix makes them pass; one failing; Clear spoken as a button, forgetting them); new chat; a suggestion into the box, unsent; the wheel and the jump to the end; the chat folded into its rail (the editor wider, the chat's items gone, an answer's dot, the session keeping it) and opened again; the least size with twelve narrowed tabs and a large size, every item inside; the session keeps the size |
| `fminus.lang` | the language service (`fminus/Lang.h`): FMinus's tokens; a netlist's (`lexCirLine`: comments, directives, elements' names, SPICE's values, an expression's operators, every line of the library's netlists) and the module it generates, read line by line (Screamer's init, set, tick and clear; a law input in tick; no set without pots; its methods after `pedal.`; the library's module offered with its `#include`); a program of three files and host.fm indexed (modules, params with their ranges, fields, methods, a function of another file, locals with their scopes); completion at given places (a prefix's field first, a module's members after `.`, host.fm's after `->`, the attributes, an `#include`'s files, locals in and out of scope, a builtin's signature, tanf's among them, a library module with its `#include`, nothing in a comment); signature help (a method's, a builtin's, another file's function's, a vector literal's, the parameter at the caret); hover (a method with its usage line, a Host field, a module, a local, a param, a keyword, a builtin); go to definition (another file, a field, a library module, an `#include`); occurrences (a field's, a loop variable's and not another loop's); a module whose `{` has no partner (a closer missing, the file cut short) holding the rest of the file. The formatter: every example and library file is the house style already; three scrambled copies of each (no indent; tokens single-spaced, K&R braces; no space at all) format back to it, and formatting again changes nothing; a messy program comes out as the house style says; a character FMinus does not read leaves the text as it is. Formatted, each example renders the same bits (Measure's render), and so do RANDOM programs with every file formatted, until every library file the product's RANDOM uses has been through; the others compile alone with the same modules. `Patch::check`: the examples clean, a syntax error, an undeclared name, an `#include` of no file and a missing method said as RUN says them, its time against RUN's; `tanf`, a builtin since FMinus c75caab: a call compiles clean, and a program's own `tanf` is FMinus's error, the check's as RUN's |
| `gui.code` | the editor as a code editor, headless, to `gui.code.png`, `.completion.png`, `.signature.png`, `.hover.png`, `.problems.png`, `.find.png`, `.paper.png` and `.cir.png` (a netlist's tab: SPICE highlighted, no completion or Format, a broken line squiggled by the check): its looks on (guides, bracket pair, auto-closing, brace indent, minimap, mark column) and drawn, the chat's message box without them; the caret on a field marking its three places; completion (a prefix's list, Tab, `.` and a method accepted with its signature, `)` stepping over, Ctrl-Space and Escape, a library module adding its tab and `#include`, one Cmd-Z); signature help on `(` and `,`; the hover card; a check while typing (an undeclared name squiggled and marked within a second, without RUN, the card saying the message, cleared when undone); a note before its error, each card saying its own; a note only RUN gives kept after RUN, gone with an edit's check and back when the edit is undone; Shift-Alt-F and its one undo, Format on Run; find (count, Return, Cmd-G, Shift-Cmd-G, the matches marked, replace all with Alt-Return and its undo, Escape, no match left marked in a tab shown while the bar was open); Cmd-/, Alt-Up and Down, Shift-Alt-Down, Shift-Cmd-K; Cmd-click into the library's tab and Cmd-B to a field; PAPER's palette |
| `gui.dump` | the panel headless to `gui.dump.png` (the default program, Init), `.paper.png`, `.error.png`, `.edited.png`, `.files.png`, `.dials.png`, `.dials.small.png`, `.dials.most.png`, `.dials.most.small.png`, `.panel.png`, `.panel.paper.png`, `.panel.small.png`, `.example.png`, `.displays.png`, `.displays.paper.png`, `.random.png`, `.projects.png`, `.export.png`, `.frozen.png`, `.frozen.paper.png` and `.1x.png`: settles, every glyph, the draft in the editor and a dial for each parameter; typing, Cmd-Return, a syntax error (status, problem line, the line's mark, the click that jumps to it), the EXAMPLES menu and its undo; the files' tabs (+, renaming in place, a taken name refused, Escape, typing into a second file, an `#include` of it that runs, its error's problem and marks, the click that shows its file, closing with and without asking); a dial's drag and double-click; sixteen dials in rows (two at the default and the least size, one when wide; in order, evenly spaced, inside the left column, the editor giving up a row's height; 32 in three rows, every knob small at the least size); sixteen dials in five sections (two rows of frames, titles in order, every dial inside its frame, the rows even, the labels and small knobs asked for, at the least size too; a bad panel line noted on its line); the LIBRARY menu (every file under its heading; a file added as a tab and #included after the program's description, undoable, still running; ticked and not added twice; greyed with twelve tabs); the program Claude's instructions build from the library, its files added through the menu's path and its `panel.gui` in a tab of its own, running with no note, its interface's every dial in a titled section; a program with every display and dial style, coloured, audio through the processor (the synth's a note held: each item in its frame; a meter at the Engine's peak within 0.3 dB, a field's meter, light, number and trace; a button's click chooses its value, the switch flips, a fader's drag raises its dial, the host says an option's name; another program's light dark until the audio thread plays it); RANDOM (a seed's program in place of the text, its library files as tabs, run with no note, Cmd-Z back in one step; the button's another; with twelve tabs, a notice and nothing changed); PROJECTS (the card opened with the program's title, a name typed and saved as a new project: the status and the pill say so, EDITED after an edit; Cmd-S a version, again nothing new; a preset saved in the project open; listed by name, the open one chosen with its versions and presets, another's older version opened over changes after asking; saving into another project asking (Add version); a project and a version deleted, each asking; Escape; EXPORT from Export..., its info pasted, typed and chosen, a package written where the save chooser says and the info kept, Import... adding it back as a project of its own); Escape and Tab; the frozen copy playing (blocks with no dial moving, one wait): the top bar's snowflake drawn left of the load, clear of the CODE / PANEL switch, its accessibility title, and no FROZEN in the load's text; the top bar's mark (MARK), at 2x and at 1x hinted for each (its stem, crest and trough on whole device px, in its numbers and in its pixels: at most 0.2 of the accent outside each edge, 0.8 inside, the effect's floor solid for 3 px, the synth's trough round), and the 2x drawing at a zoom's other scales |
| `gui.ir` | the IR view (`editor/IrView.h`), headless, to `gui.ir.png`, `.lowered.png`, `.frozen.png`, `.paper.png` and `.tabs.png`: with no IR tab open a RUN keeps no IR (and `Patch::compile` keeps it only when asked); the highlighter's numbers (decimal, a float's bits as `f0x3D6C9820`, `-inf`, `+qnan`, `+nan(0x...)`); the explorer's LLVM IR caption and rows (optimised, lowered; frozen only while a copy plays); a click opening the read-only tab apart at the bar's right (no file added, the draft the same, the selected tab to a screen reader) and the compiler's thread making the IR of the program that plays without a RUN, holding `define ... @playground_process(`; a text past 2 MB cut at a line's end and said so, set in the view in milliseconds; edits refused and taken (typing, Space, Return, Backspace, Delete, paste, cut, undo, Cmd-/, Alt-Down, Shift-Cmd-K, Cmd-], Shift-Alt-F) with READ ONLY said, whole; no completion, signature, hover or squiggle, the status line's LLVM IR; a RUN of a changed program changing the text, the caret kept, the new function's define in the lowered view; find (`define` counted and marked, Return, no replace, Escape clearing them); Ctrl-G to a line, a line past the end refused, in a file's tab too; a file's tab and back, the caret kept, a replace row open in the file's tab closed in the IR's; a RUN that fails said, the playing program's IR kept; an `#include` of the view's name finding no file; not in the session's state, a saved project or its source export; the frozen copy coming leaving the lowered view's text and selection alone; the frozen copy's view while it plays (its dials as constants), and when a dial moves (said, the selection on the same text); PAPER; closing it (nothing wanted or held, a RUN then keeping nothing) and a Panel going with it open; the least window with twelve files and a long program name, the files' tabs clear of the IR tab |
| `gui.explorer` | the explorer at the editor's left, headless, to `gui.explorer.png`, `.folded.png`, `.paper.png` and `.small.png`: open at first, its card left of the program's and the chat's at the right; a row a file, the shown one marked, one the errors name dotted until it compiles; a click shows a file, a double click renames it, + makes one, a row's cross closes an empty one (none on the program); every library file under its category's caption, a click adding it as a tab (the program #including it, its row ticked) and a second showing it; an example loaded and run; PRESETS saying to save a project while none is open; two projects (the open one marked, with its version) and its two presets (the applied one marked), a preset's click turning its dial, a project's click opening it (asking first when the editor is edited), the presets following the project, the + of each opening PROJECTS for a preset or a project; a section folding and opening, the explorer folding away (the program's card wider by it and its gap, its button in the program's card's header) and that button opening it, the session keeping the fold; none in the PANEL view; a list to a screen reader, rows as buttons, a row pressed doing what its click does; at the least size with the chat open away (the program's card keeping its room), its button folding the chat to show it; the wheel scrolling it at the least size |
| `gui.shaders` | a `.gui` file's shaders, built with the build's shaderc: the system prompt's example builds and FunkGui takes it; a program with three (one still, one that moves, one broken) plays, the two good ones built under their bytes' hash and the same again from the cache; the broken one a note on its own line (the name it does not know found in the file); a shader of no file and a fifth one noted; in the panel the built ones registered with FunkGui and drawn over their cells, their inputs following a parameter and the output's level, the still one settling and the moving one not, the broken one saying why (to a screen reader too), to `gui.shaders.png`; view_panel's pictures (`editor/Picture.h`): the frame on the GPU on this Mac, at SoftRaster's size, the still shader's cell its colours where SoftRaster's is the striped placeholder, the rest within FunkGui's bounds of SoftRaster's (no channel off by more than 4), the headless GPU context held and let go once idle (`picturesIdle`), view_panel's picture saying the GPU drew it and, with the GPU forced off, a picture still saying each shader is a placeholder and why, to `gui.shaders.gpu.png`, `.picture.png` and `.picture-soft.png` |
| `gui.panel` | a `.gui` interface in the Panel, headless, to `gui.panel.code.png`, `gui.panel.png`, `.paper.png`, `.meters.png` and `.small.png`: in the CODE view under the editor, every item inside its area and none over another, the dials' own items hidden; the Panel switch (the editor gone, the interface from the card's header down, fitting; the processor and its state keep the choice); a large log knob's drag, double-click and middle (632 Hz, the host's text); a row of buttons, the switch, the menu (the host's, ticked; chosen), a slider's drag, the pad's two parameters; the envelope's corners; the tabs (one page shown, a click shows the other; a radio group to a screen reader); audio through the processor (the meter, the light, the number, the spectrum's peak at the sine's frequency and level); at the least size the wheel scrolls the interface, and turns a knob under it; a `#pragma panel` program in the PANEL view from the column's top |
| `gui.styles` | what a `.gui` file can make an interface look like, in the PANEL view, to `gui.styles.png`, `.fx.png`, `.mixer.png` and `.narrow.png`: a defaults line and a class reaching their elements (an element's own property winning); the knobs' nine looks, a small knob's label under it and no value, a face in px; card, inset and plate sections side by side, a frame's own corners, fill and border; a bar of three pages (a click shows the third, the processor keeps its title, its state carries it to another processor whose panel opens on it, an edited interface keeps it), pills and a side list switching their pages; the synth's keyboard (a press plays its note at its pitch, a drag slides to the next key, letting go is silent) and its wheels (the pitch wheel up reaching the Host block and springing back when let go, the mod wheel staying where it is left); a knob's live= mark at its field's value; at the least size the knobs' row wrapping onto two lines |
| `gui.settings` | the Standalone's Settings card (`editor/SettingsCard.h`) over a real processor whose window hooks hand it a made-up `DevicePort` (every call recorded), to `gui.settings.png`, `.paper.png`, `.small.png`, `.error.png` and `.muted.png`: in a host (no devices) no card and no notice, the app's request doing nothing; the effect's muted notice in the status bar, clear of the zoom, its button opening the card (the synth has none); the app's request opening it, modal; at the default size, the least and a large one every control inside the card and none over another or over a text, each with an accessibility title; the effect's Input section and no MIDI, the synth's MIDI and no Input; Tab over every control in order, Return on Test; a pair's switch, a MIDI input's, the mute's (the notice gone when unmuted) and Test calling the port; the output device, the rate and the buffer chosen through the host's menus (the current one ticked) calling it with those values and shown; a device plugged in while open listed; an error under its section, spoken, and JUCE's error with line breaks in lines of its own; a Cmd key given back to the app's menu; at the least size with more than it holds, the scroll (Done in the card, Tab scrolling to its control, the wheel); Done, Escape and a click outside closing it, the click reaching nothing under it (nor a double-click's second press: a dial keeps its value); a second editor in PAPER; the hooks let go: no card, no notice |
| `gui.window` | the Standalone's own window, as far as it is checked without opening one, to `gui.window.png` and `.host.png`: its menu bar as data (the app menu with About, Settings… Cmd-, , Services, Hide, Hide Others, Show All and Quit; File's Load State…, Save State As… and Reset to Default State; Edit's Undo Cmd-Z, Redo Cmd-Shift-Z, Cut, Copy, Paste and Select All; Window's Minimize Cmd-M, Zoom and Bring All to Front; every shortcut its own, none the editor's: Cmd-Return, Cmd-S); in a host's window no room and no drag; with the window's buttons (60 x 14 points) the top bar keeps them room (the product's mark after them, the project's pill clear), says once where they go (its left inner edge, its middle), keeps them their size in points at 200 % and none in full screen; a press on the top bar where nothing is drags the window and a double-click's second press asks for its double-click action (whose release and double-click, wherever a zoom then puts them, leave the panel alone: a dial keeps its value), a press on a button, a right-click, one under the top bar or with PROJECTS open drags nothing; each Edit item's shortcut, sent to the focused component as the app sends it, does its work in the editor's text field (Select All, Copy, Cut, Paste, Undo, Redo) and nothing with no text field focused |
| `installer.engine` | the installer's engine (`Source/installer/Engine.h`) against a root of its own (its name has spaces and both quotes) with a made-up payload of two products, nothing outside it written (ditto copies inside it, its Trash is a folder there, the AU registrar and the administrator prompt are stand-ins): the payload read (each part's version from its Info.plist or manifest; a missing payload an error in words); a clean install of every part "Just for me", each where it belongs and whole (its signature holding), no staging folder left, the registrar restarted once, the log written; the same again: every part "already installed", nothing copied; another build of the same version (the bundle's bytes; a re-signed bundle's CDHash) replaces only those, the old copies in the Trash; an older and a newer copy told apart; a folder made unwritable mid-plan: the parts before it installed, its part failed with words that say why, the old copy whole, the rest not run; a folder that is a link and a target that is a link refused, nothing written where they point; a staging folder an earlier run left goes to the Trash with the next install (another's is left); uninstall to the Trash; "All users": the script quotes every path it names, reaches the prompt as text, renames only with `mv -h`, and run (as the user, by the stand-in) installs every part, replacing an older copy, its status folder gone after; a cancelled prompt changes nothing; run directly, a planted status folder stops it before it changes anything, and the uninstall's script refuses a parent folder swapped for a link and a Trash that is a link; a running Standalone and a running host named in the notes, a product in both scopes one note, an uninstall's notes |
| `installer.gui` | the installer's window (`Source/installer/Panel.h`) headless at its size over that engine, to `gui.installer.welcome.png`, `.choose.png`, `.choose.allusers.png`, `.installing.png` (a run half done), `.done.png`, `.done.noclaude.png`, `.uninstall.png`, `.error.png`, `.uninstalled.png`, `.choose.notes.png` (a DAW and a Standalone open, copies in the other scope), `.choose.notes.allusers.png` and `.uninstall.notes.png`, each in PAPER too (`.paper.png`): on every page every control inside the window and none over another, every text inside it and clear of the controls, everything with an accessibility title, every glyph in the font; the window's buttons placed over the sidebar; a newer installed part's switch off at first; Return (once: its repeat, a held Return and a double-click on Continue press nothing more), Escape, Tab and Space; Install's progress, then Done with the report; Open <name> and Add to Claude Desktop open the installed app and extension (and nothing without Claude Desktop, whose absence a note explains); Done quits, Back and Escape return to Choose; a failed run's page names the log and how to get Claude Desktop; the notes on Choose in both scopes and on Uninstall, whole; an uninstall's switch kept across the page's refresh, and Move to Trash |
| `lint.layers`, `lint.exports`, `lint.signature` | once, for both products: Source/dsp, Source/fsdsp and Source/fminus without JUCE or FunkGui, FMinus through its public header only; every product's bundles export their entry points and nothing else, and their signatures hold (the installer app's too, over its payload) |

A probe is one file in `Tools/probes/<layer>/` whose first line registers it (`// PROBE name=dsp.mytest timeout=60`)
and whose body uses spec rows (`P.near`, `P.le`, `P.ge`, `P.in`, `P.eq`): each states what must hold, with its
tolerance. It runs for both products (`product::kIsInstrument` says which kind it runs as).
`Tools/probes/common/Audio.h` has the signal helpers, and `Tools/probes/common/Rig.h` runs a program in an Engine as
the processor does (the synth's notes at absolute samples).

## FMinus

[FMinus](https://github.com/Snipet/FMinus) (`~/audio/libraries/FMinus`) comes in through its public API,
`fminus/FMinus.h` (`fminus::Program`): `Source/fminus/Patch.cpp` compiles a program twice (once to read its module
and parameters, once with each float parameter bound as a stream and the plugin's calls appended), at `-O2`, flushing
subnormals, and in the synth makes 32 instances of Voice. Its circuit compiler comes in through `fminus/Circuit.h`
(`fminus::circuit`: generate, the self-test's key and answers), in `Source/fminus/Circuits.cpp` alone.

- **Which FMinus**: the commit `FP_FMINUS_SHA` in `cmake/Deps.cmake`, from `~/audio/libraries/FMinus`. To try a change
  to FMinus before committing it, configure with `-DFETCHCONTENT_SOURCE_DIR_FMINUS=$HOME/audio/libraries/FMinus`:
  the working tree is built as it is. LLVM, Bison and Flex are the ones FMinus's own build put in
  `~/audio/libraries/FMinus/.deps`, read and never rebuilt here: build FMinus once
  (`cmake --workflow --preset dev` there) before configuring this project. A Release build here takes LLVM without
  assertions when FMinus's release preset has made it, and the dev flavor otherwise.
- **The cost**: LLVM is linked into each binary, about 80 MB more in a Release build, and stays private to it: each
  bundle exports only its entry points (`lint.exports` checks), so two plugins with LLVM in one host do not share it,
  nor do the two products. A DAW's hardened runtime must allow the JIT's executable memory: FMinus's LLVM maps its code
  without `MAP_JIT` (JITLink's in-process memory manager), so it needs
  `com.apple.security.cs.allow-unsigned-executable-memory` (Ableton Live 12 has it); `com.apple.security.cs.allow-jit`
  alone is not enough (measured: a hardened binary with only that is killed at its first compile). The Standalones
  signed by `scripts/package.sh` have it; unsigned builds and the probes have no hardened runtime. FMinus's README,
  "Notes for running inside a plugin", has the rest.

## What is where

```
products/<key>.cmake     a product's identity: effect.cmake (TestingPlayground, an effect), synth.cmake
                         (TestingPlaygroundSynth, an instrument): name, kind, version, codes, bundle id, prefixes
Source/fminus/           Patch: a program compiled for the plugin (the contract, the calls, the instances, the fields
                         its displays read), JUCE-free;
                         PanelSpec (its #pragma panel lines read: sections, dial styles, displays);
                         GuiSpec (a .gui file read: its interface's tree, the controls' tapers and units);
                         HostFile.cpp (host.fm, the one file the plugin provides);
                         Circuits (a .cir netlist as the module FMinus's circuit compiler generates, on #include;
                         the self-test's answers, kept for every instance; FMinus's messages cited at the netlist);
                         LibraryIncludes.h (a program's files and the library files it #includes: the examples');
                         Random (RANDOM: a program put together from the library's building blocks);
                         Lang (FMinus's language service: its tokens, the program's symbols, completion, signature
                         help, hover, definitions, occurrences; LangFormat.cpp, the formatter);
                         examples/<key>/*.fm, a product's EXAMPLES (compiled in as generated/<key>/Examples.h);
                         library/<n>-<category>/*.fm and *.cir, the LIBRARY (as generated/<key>/Library.h;
                         6-circuits/ its netlists)
Source/dsp/              the audio side, plain C++20 without JUCE: Engine (an instrument's voices, programs swapped
                         without a click, the dials glided into streams, the output guard, the levels, fields and
                         scope the displays read: Meters.h), Compiler (compiles off the audio thread), Controls,
                         Events, Sources (a program's files), Measure (a program rendered offline and measured, in
                         words and in numbers, for the chat), Goals (checks and goals on those numbers), Tune
                         (tune_dials' search), SoundView (view_sound's render and analysis), Compare (compare's null
                         test), Debug (trace: where a problem starts, the fields watched)
Source/fsdsp/core/       FSynth's DSP core, vendored at the tag in SEED.tsv (scripts/sync-dsp-core.sh)
Source/plugin/           the JUCE AudioProcessor: an instrument's MIDI to events, the playhead to the transport, P1 to
                         P32, the program's files and the draft in the session, the bypass;
                         EditorPort.h, what the editor reaches besides the parameters; DevicePort.h, the
                         Standalone's audio and MIDI devices as the Settings card sees them (plain data, its actions);
                         the chat: ChatAgent (the conversation, its turns and Claude's tools), ChatPrompt.cpp (Claude's
                         instructions: the panel, the library, a program built from it), ClaudeApi (the Messages API
                         over HTTPS, to Anthropic),
                         ChatStore (the key, encrypted), ChatPort.h (what the editor reaches of it),
                         SoundPicture (view_sound's picture, drawn with JUCE);
                         ChatScenario (the render tools' scenario and trial edits), ChatCompare (compare),
                         ChatTrace (trace),
                         ChatCaptured (the user's recording as a render: the capture's seam), CaptureReplay (its
                         replay, null test and analysis);
                         ProjectStore (the projects on this Mac: their versions, presets and info),
                         ProjectExport (a project as a source .zip or a compressed .fmpack, and back),
                         McpLink (the instance's socket for the MCP server: Claude Desktop through the chat's tools,
                         a call at a time on its worker, quick reads beside it);
                         StandaloneMenu (the Standalone's menu bar, as data);
                         standalone/, the Standalone app, its target only: App.cpp (JUCE's custom standalone hook:
                         the app, its window, the devices and the state through StandalonePluginHolder), Devices
                         (the DevicePort over the holder's AudioDeviceManager and its input's mute, saved on change)
                         and NativeWindow.mm (the Mac window's look, its buttons over the top bar, its drag, the menu
                         bar)
Source/mcp/              <Name>MCP, each product's MCP server Claude Desktop starts (Main.cpp: JSON-RPC on stdio, one
                         loop over stdin, the instances' sockets, found by their sockets, and the sandbox's helper;
                         gui_sandbox), McpProtocol.h (the folder, the sockets, the lines), McpGuide.h (the guide in
                         parts), and icon-<key>.png, each product's extension's icon (Tools/logo/logo.swift writes
                         it)
Source/sandbox/          <Name>GuiSandbox, the GUI sandbox's helper, which the server starts (Main.cpp: JSON lines on
                         its stdin and stdout, SandboxProtocol.h), Sandbox (the panel being designed: its notes, its
                         layout report, its picture), Dials (the dials, from the program's declarations or Claude's
                         list), View (the editor's GuiView in a panel of its own, demo meters): the editor's drawing
                         code and FunkGui, no plugin code
Source/editor/           the FunkGui panel (Panel.cpp: the top bar, the explorer's card (it folds), the program's card
                         with its actions, the files' tabs and TextEditors, the problems and the dials, the chat's card
                         (it folds) and the key dialog; its layout follows the window's size), CodeAssist (the
                         tabs' TextEditors as a code editor: the highlighter, the problems' squiggles and marks, the
                         check while typing, completion, signature help, hover cards, Format Document, the find bar,
                         the line commands, go to definition, go to a line, the status line), IrView (the IR view's
                         views, their texts and the LLVM IR highlighter), Explorer (the
                         explorer's list: files, the IR, presets, projects, library, examples), Picture.cpp (view_panel's
                         picture: on the GPU, else SoftRaster; PictureDraw.cpp, the frame drawn and its PNG),
                         DialLayout (the dials' and displays' places: plain rows, or the panel's sections), GuiView (a
                         .gui file's interface: its items made, laid out, fed and drawn; the sandbox's too),
                         ProjectBrowser (PROJECTS' card: the projects, the chosen one's versions and presets, the
                         name, Save, Open, Delete, Import, Export), ExportCard (EXPORT: the info, the two exports),
                         SettingsCard (the Standalone's Settings: its devices, rates, buffers, channels, MIDI inputs),
                         Looks (the dial styles and the displays drawn: vintage knobs, faders, switches, buttons, LED
                         meters, a VU, lights, readouts, a scope), DisplayItem (a display's motion: VU ballistics,
                         peak hold),
                         ChatView (the conversation), ChatItems (the message box, the chat's buttons), ChatStyle.h
                         (the chat's rounded look, its palette from the theme), Items.h, Theme.h (each product's
                         accent), LogoMark.h (the top bar's mark, generated by Tools/logo/logo.swift);
                         gpu/ starts it in the plugin
Source/installer/        the installer, one app for both products (cmake/Installer.cmake; its payload is their
                         bundles and extensions): Engine (no GUI: the payload, what is installed where, the plan,
                         installing and uninstalling, the script "All users" runs as root), System.cpp (the system's
                         tools: ditto, pgrep, killall, the Trash, osascript's administrator prompt), Panel (its window,
                         a FunkGui Panel in the editor's look: Welcome, Choose, Installing, Done, Uninstall) and
                         App.cpp (the app: an EditorHost in the Standalone's native window)
Tools/logo/              logo.swift, the mark's one source: its construction and every number, and the script that
                         writes every asset from them (`swift Tools/logo/logo.swift`, after a build: it takes
                         JetBrains Mono from FunkGui's checkout there, or --font <ttf>)
Assets/logo/             what it writes: <kind>/ (effect, instrument) the app icon (icon.iconset, 16 to 1024 px;
                         icon.icns, which each bundle carries; icon.svg; installer-1024.png), extension-512.png, the
                         mark's SVGs and lockups; readme-*.png (the header above: both marks), construction.png (the
                         geometry, measured), sheet.png (every size, both themes, the top bar in FunkGui's pixels)
Tools/probes/            tests that measure, each run for both products: fminus/, dsp/, plugin/, gui/ (the panel,
                         headless); installer/, the installer's, run once
scripts/                 plugin.py (the products' identities: info, add, rename, codesign, notarize), test.sh,
                         sync-dsp-core.sh, install.sh, package.sh (the installer .pkg; --installer: the installer
                         app's disk image), entitlements/ (the Standalones' hardened runtime, by kind)
cmake/                   Deps.cmake (the pins), Flags.cmake, Probes.cmake (the probes and lints), Product.h.in,
                         Installer.cmake (the installer, its payload, its probes) and InstallerPayload.cmake (the
                         payload put in the app), the lints
```

## A new plugin, or a new product

Start new plugins from FunkPluginTemplate (`../FunkPluginTemplate/scripts/plugin.py clone --name MyPlugin`, effect or
instrument). A new product of this tree, compiled from the same sources, is
`scripts/plugin.py add --from synth --key bass --name MyBass --code Mbas`: it writes `products/bass.cmake` and copies
the synth's examples for it to change, and its icon (it keeps the kind of the product it copies; its app icon is its
kind's, `Assets/logo/<kind>/`, and `swift Tools/logo/logo.swift` writes its extension's icon from the same mark). It
refuses a plugin code another product here or another of your plugins already uses (any project in `~/audio/plugins`):
two plugins with one code hide each other in a host. `scripts/plugin.py rename --product effect --name ...` gives a
product a new identity in place (its file, and its names in the docs); an installed product renamed is a new plugin to
hosts, with new folders.

## Update the DSP core

```sh
scripts/sync-dsp-core.sh <fsynth-tag>     # e.g. fsyn-s3 (the seed, SEED.tsv) or a later fsyn-sN
```

It takes `Source/fsdsp/core` from FSynth at that tag (`git tag` in FSynth lists them) and records it in SEED.tsv.
It refuses to overwrite a core file you edited (`--force` overwrites). Build and test after a sync.

## Theme, zoom, diagnostics

The theme's button (the top bar) and the zoom's pill (the status bar) are machine-wide preferences, stored by FunkGui in
each product's own `~/Library/Application Support/<name>/` (`TestingPlayground/`, `TestingPlaygroundSynth/`). Colours
are tokens, and each product has its own accent, which its mark's caret, RUN and the editor's caret share
(`Source/editor/Theme.h`): the effect's is FunkGui's orange and its errors (the theme's `signal`) cyan; the synth's is
a blue and its errors orange, so an error never reads as the accent. The code's types and its notes take a colour of
their own (`codeSecond`): the effect's cyan, as its errors, the synth's a teal, apart from its blue keywords and its
orange errors. `<PREFIX>UI_THEME=1` opens the Standalone on the
light theme without saving it, `<PREFIX>GPU_LOG=1` logs the GPU surface; the prefix is the product's
(`PRODUCT_ENV_PREFIX`: `TSTN_` for the effect, `TSYN_` for the synth).
