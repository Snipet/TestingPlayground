// Source/fminus/Random.cpp: see Random.h.
//
// A program is written as a Writer's parts (the library files, the panel's lines, the parameters, the fields, and
// the lines of each method), which each choice adds to, then assembled in one fixed form. Every choice's lines name
// only what that choice declares, so any combination compiles; fminus.random checks the combinations its seeds reach.
#include "fminus/Random.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace testingplayground::random
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;

        // splitmix64: the same seed, the same draws, on every machine.
        class Rng
        {
        public:
            explicit Rng(uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ull ^ 0xD1B54A32D192ED03ull) {}
            uint64_t next()
            {
                uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }
            double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
            double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
            double logRange(double lo, double hi) { return lo * std::pow(hi / lo, uniform()); }
            int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
            bool chance(double p) { return uniform() < p; }
            // An index by weight.
            int choose(std::initializer_list<double> weights)
            {
                double total = 0.0;
                for (const double w : weights)
                    total += w;
                double at = uniform() * total;
                int k = 0;
                for (const double w : weights)
                {
                    if (at < w)
                        return k;
                    at -= w;
                    ++k;
                }
                return k - 1;
            }
            template <typename T, std::size_t N>
            const T& pick(const T (&items)[N]) { return items[below(static_cast<int>(N))]; }

        private:
            uint64_t s_;
        };

        // A number as FMinus writes one: `digits` significant figures, no exponent, no trailing zeros.
        std::string num(double v, int digits = 3)
        {
            if (v == 0.0 || !std::isfinite(v))
                return "0";
            const int magnitude = static_cast<int>(std::floor(std::log10(std::fabs(v))));
            const int decimals = std::clamp(digits - 1 - magnitude, 0, 6);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
            std::string s = buf;
            if (s.find('.') != std::string::npos)
            {
                while (s.back() == '0')
                    s.pop_back();
                if (s.back() == '.')
                    s.pop_back();
            }
            return s == "-0" ? "0" : s;
        }

        // A value rounded as a dial's default reads well.
        double nice(double v) { return std::stod(num(v, 2)); }

        struct Param
        {
            std::string type, name, lo, hi, def, comment;
        };

        // The parts of a program, each choice adding its own.
        struct Writer
        {
            std::vector<std::string> library, panel, fields, init, end, start, startSilent, stop, call, loop, after;
            std::vector<Param> params;
            std::vector<std::string> parts, said;          // the choices; what the first line says, in order
            std::string shared;                            // the module Shared, or ""

            void use(const std::string& file)
            {
                if (std::find(library.begin(), library.end(), file) == library.end())
                    library.push_back(file);
            }
            void real(const std::string& name, double lo, double hi, double def, const std::string& comment = "")
            {
                params.push_back({ "float", name, num(lo), num(hi), num(std::clamp(def, lo, hi)), comment });
            }
            void integer(const std::string& name, int lo, int hi, int def, const std::string& comment = "")
            {
                params.push_back({ "int", name, std::to_string(lo), std::to_string(hi), std::to_string(std::clamp(def, lo, hi)),
                                   comment });
            }
            void field(const std::string& line) { fields.push_back(line); }
            void line(const std::string& l) { panel.push_back("#pragma panel " + l); }
        };

        // The library's files in its own order, for the #include lines.
        constexpr const char* kLibraryOrder[] = { "osc.fm",    "wavetable.fm", "noise.fm", "svf.fm",    "ladder.fm",
                                                  "onepole.fm", "adsr.fm",     "lfo.fm",   "follower.fm", "shaper.fm",
                                                  "delay.fm",  "reverb.fm",    "util.fm" };

        void sortLibrary(std::vector<std::string>& files)
        {
            const auto rank = [](const std::string& f) {
                for (std::size_t k = 0; k < std::size(kLibraryOrder); ++k)
                    if (f == kLibraryOrder[k])
                        return k;
                return std::size(kLibraryOrder);
            };
            std::stable_sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        }

        constexpr const char* kAdjectives[] = { "Copper", "Velvet", "Glass", "Rust", "Amber", "Hollow", "Silver", "Dusty",
                                                "Neon", "Paper", "Iron", "Lunar", "Tidal", "Static", "Honey", "Frozen",
                                                "Burnt", "Electric", "Quiet", "Wild", "Golden", "Cobalt", "Ivory", "Smoky",
                                                "Crystal", "Woolen", "Magnetic", "Distant", "Bright", "Sunken" };
        // Nouns that suit what it is: an instrument's by its character (kCharacters' order), an effect's by whether it
        // has an echo or a reverb.
        [[maybe_unused]] constexpr const char* kVoiceNouns[][6] = {
            { "Pluck", "Harp", "Mallet", "String", "Drop", "Koto" },          // pluck
            { "Keys", "Organ", "Piano", "Lantern", "Clavier", "Celeste" },    // keys
            { "Bloom", "Drift", "Choir", "Tide", "Garden", "Orbit" },         // pad
            { "Bass", "Engine", "Pulse", "Machine", "Tank", "Undertow" },     // bass
            { "Reed", "Horn", "Siren", "Signal", "Brass", "Fanfare" },        // brass
            { "Bell", "Chime", "Comet", "Gong", "Glass", "Carillon" },        // bell
        };
        [[maybe_unused]] constexpr const char* kSpaceNouns[] = { "Echo", "Tape", "Cloud", "Spring", "Tunnel", "Cathedral",
                                                                 "Chamber", "Cavern", "Halo", "Canyon", "Room", "Hall" };
        [[maybe_unused]] constexpr const char* kTextureNouns[] = { "Machine", "Furnace", "Radio", "Engine", "Static", "Haze",
                                                                   "Filter", "Ripple", "Grinder", "Circuit", "Wobble", "Fuzz" };
        constexpr const char* kColours[] = { "red", "orange", "yellow", "green", "teal", "blue", "violet", "pink", "cream" };

        // "a, b and c"
        std::string listed(const std::vector<std::string>& items)
        {
            std::string s;
            for (std::size_t k = 0; k < items.size(); ++k)
                s += (k == 0 ? "" : (k + 1 == items.size() ? " and " : ", ")) + items[k];
            return s;
        }

        // The panel's colours: one section's (often), a second's (now and then).
        void colour(Rng& g, Writer& w, const std::vector<std::string>& sections)
        {
            if (sections.empty() || !g.chance(0.8))
                return;
            const int first = g.below(static_cast<int>(sections.size()));
            w.line("color \"" + sections[static_cast<std::size_t>(first)] + "\" " + g.pick(kColours));
            if (sections.size() > 2 && g.chance(0.3))
            {
                const int second = (first + 1 + g.below(static_cast<int>(sections.size()) - 1)) % static_cast<int>(sections.size());
                w.line("color \"" + sections[static_cast<std::size_t>(second)] + "\" " + g.pick(kColours));
            }
        }

        // The parameters in the order the panel's sections show them (the host's P1, P2, ... read the same way).
        std::vector<Param> panelOrder(const std::vector<Param>& params, const std::vector<std::string>& panel)
        {
            std::vector<std::string> names;
            for (const std::string& l : panel)
            {
                const std::string key = "#pragma panel section \"";
                if (l.rfind(key, 0) != 0)
                    continue;
                std::size_t at = l.find('"', key.size());
                while (at != std::string::npos && at + 1 < l.size())
                {
                    const std::size_t from = l.find_first_not_of(' ', at + 1);
                    if (from == std::string::npos)
                        break;
                    const std::size_t to = l.find(' ', from);
                    names.push_back(l.substr(from, to == std::string::npos ? std::string::npos : to - from));
                    at = to;
                }
            }
            const auto rank = [&](const Param& p) {
                const auto it = std::find(names.begin(), names.end(), p.name);
                return it == names.end() ? names.size() : static_cast<std::size_t>(it - names.begin());
            };
            std::vector<Param> out = params;
            std::stable_sort(out.begin(), out.end(), [&](const Param& a, const Param& b) { return rank(a) < rank(b); });
            return out;
        }

        std::string paramLines(const std::vector<Param>& params)
        {
            std::string s;
            for (const Param& p : params)
            {
                std::string l = "    param " + p.type + " " + p.name + " [[fminus::range(" + p.lo + ", " + p.hi + ")]] [[fminus::default(" + p.def + ")]];";
                if (!p.comment.empty())
                {
                    l.resize(std::max<std::size_t>(l.size() + 1, 84), ' ');
                    l += "// " + p.comment;
                }
                s += l + "\n";
            }
            return s;
        }

        std::string block(const std::vector<std::string>& lines, const std::string& indent)
        {
            std::string s;
            for (const std::string& l : lines)
                s += l.empty() ? "\n" : indent + l + "\n";
            return s;
        }

        // Text as // comment lines of at most `width` characters.
        std::string comment(const std::string& text, std::size_t width)
        {
            std::string out, line = "//";
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t end = text.find(' ', at);
                if (end == std::string::npos)
                    end = text.size();
                const std::string word = text.substr(at, end - at);
                if (line.size() > 2 && line.size() + 1 + word.size() > width)
                {
                    out += line + "\n";
                    line = "//";
                }
                line += " " + word;
                at = end + 1;
            }
            return out + line + "\n";
        }

        std::string header(const Program& p, const std::string& what, const Writer& w)
        {
            std::string s = comment(p.title + " (random " + std::to_string(p.seed) + "): " + what + ".", 118);
            s += "// RANDOM put it together from the library's building blocks: change anything, then RUN (Cmd-Return).\n";
            for (const std::string& f : w.library)
                s += "#include \"" + f + "\"\n";
            for (const std::string& l : w.panel)
                s += l + "\n";
            return s;
        }

        // ---- an instrument ---------------------------------------------------------------------------------------
        struct Character
        {
            const char* name;
            double a0, a1, d0, d1, s0, s1, r0, r1;          // the amplitude envelope's ranges (ms, ms, 0..1, ms)
            double fa, fd0, fd1, fs0, fs1, fr;              // the filter's (or FM index's) envelope
        };
        [[maybe_unused]] constexpr Character kCharacters[] = {
            { "pluck", 1, 4, 150, 700, 0.0, 0.15, 120, 450, 1, 120, 450, 0.0, 0.05, 250 },
            { "keys", 2, 10, 400, 1500, 0.2, 0.5, 200, 700, 2, 300, 900, 0.1, 0.3, 400 },
            { "pad", 250, 1400, 800, 2500, 0.6, 0.9, 900, 2800, 600, 1200, 3000, 0.3, 0.6, 1500 },
            { "bass", 1, 6, 120, 450, 0.4, 0.8, 60, 220, 1, 90, 320, 0.0, 0.2, 150 },
            { "brass", 25, 90, 300, 900, 0.6, 0.85, 150, 500, 40, 300, 800, 0.3, 0.5, 300 },
            { "bell", 1, 3, 1500, 5000, 0.0, 0.0, 700, 2400, 1, 400, 1500, 0.0, 0.0, 600 },
        };

        [[maybe_unused]] Program instrument(Rng& g, Program p)
        {
            Writer w;
            w.use("adsr.fm");
            w.use("util.fm");
            const int source = g.choose({ 0.45, 0.3, 0.25 });           // analog, wavetable, FM
            const Character& ch = source == 2 ? kCharacters[std::size_t(g.choose({ 0.2, 0.3, 0, 0, 0, 0.5 }))]
                                              : kCharacters[std::size_t(g.choose({ 1, 1, 1, 1, 1, 0 }))];
            w.parts.push_back(ch.name);
            const int filter = source == 2 ? (g.chance(0.3) ? 1 : 0) : 1 + g.choose({ 0.35, 0.1, 0.05, 0.4, 0.1 });
            // filter: 0 none, 1 svf low-pass, 2 svf band-pass, 3 svf high-pass, 4 ladder, 5 none
            const bool hasFilter = filter >= 1 && filter <= 4;
            const bool hasLfo = g.chance(0.75);
            const bool hasNoise = source != 2 && g.chance(0.25);
            const bool hasDrive = g.chance(0.35);
            const bool fenvUsed = hasFilter || source == 2;
            const double ratios[] = { 1.0, 0.5, 2.0, 1.49830708 };
            const char* ratioSaid[] = { "in unison", "an octave down", "an octave up", "a fifth up" };
            const int ratio2 = g.below(4);
            std::vector<std::string> sourceItems, sections;
            std::string sourceSection, sourceSaid;
            bool waveButtons = false;

            // The envelopes.
            w.real("attack", 1, 3000, nice(g.logRange(ch.a0, ch.a1)), "ms");
            w.real("decay", 10, 6000, nice(g.logRange(ch.d0, ch.d1)), "ms, to 60 dB down");
            w.real("sustain", 0, 1, nice(g.range(ch.s0, ch.s1)));
            w.real("release", 10, 6000, nice(g.logRange(ch.r0, ch.r1)), "ms");
            w.field("Adsr amp;");
            w.call.push_back("amp.set(attack[0], decay[0], sustain[0], release[0], rate);");
            w.loop.push_back("const float a = amp.tick();");
            if (fenvUsed)
            {
                w.field("Adsr fenv;");
                w.start.push_back("fenv.gate(true);");
                w.stop.push_back("fenv.gate(false);");
                w.call.push_back("fenv.set(" + num(ch.fa) + ", " + num(nice(g.logRange(ch.fd0, ch.fd1))) + ", "
                                 + num(nice(g.range(ch.fs0, ch.fs1))) + ", " + num(ch.fr) + ", rate);"
                                 + (source == 2 ? "   // the FM index's envelope" : "   // the filter's envelope"));
                w.loop.push_back("const float f = fenv.tick();");
            }

            // The LFO and what it moves.
            std::string target;
            if (hasLfo)
            {
                std::vector<std::string> targets{ "pitch", "level" };
                if (hasFilter)
                    targets.insert(targets.end(), { "cutoff", "cutoff" });
                if (source == 0)
                    targets.push_back("shape");
                if (source == 1)
                    targets.insert(targets.end(), { "position", "position" });
                if (source == 2)
                    targets.insert(targets.end(), { "index", "index" });
                target = targets[static_cast<std::size_t>(g.below(static_cast<int>(targets.size())))];
                const int shape = target == "cutoff" ? g.choose({ 0.5, 0.3, 0.2 }) : g.choose({ 0.6, 0.4, 0 });
                const char* shapes[] = { "sine", "triangle", "random" };
                const char* reads[] = { "lfo.sine(0)", "lfo.triangle(0)", "lfo.random()" };
                w.parts.push_back(std::string("lfo>") + target);
                w.parts.push_back(std::string("lfo-") + shapes[shape]);
                w.use("lfo.fm");
                w.real("lforate", 0.05, 12, nice(target == "pitch" ? g.range(4, 6.5) : g.logRange(0.15, 5)), "Hz");
                w.real("lfodepth", 0, 1, nice(target == "pitch" ? g.range(0.1, 0.3) : g.range(0.2, 0.6)));
                w.field("Lfo lfo;");
                w.field("float lfolight;");
                if (g.chance(0.5))
                    w.start.push_back("lfo.reset(0);");
                w.loop.push_back("lfo.advance(lforate[i] / rate);");
                w.loop.push_back(std::string("const float l = ") + reads[shape] + ";");
                w.after.push_back(std::string("lfolight = 0.5f * (1 + ") + reads[shape] + ");");
                w.said.push_back(std::string(shape == 2 ? "a random-step LFO" : (shape == 1 ? "a triangle LFO" : "an LFO"))
                                 + " on the " + (target == "level" ? "level (tremolo)" : (target == "pitch" ? "pitch (vibrato)" : target)));
            }
            w.loop.push_back(target == "pitch" ? "const float step = hz * cents(30 * lfodepth[i] * l) / rate;"
                                               : "const float step = hz / rate;");

            // The sources.
            if (source == 0)
            {
                w.parts.push_back("analog");
                w.use("osc.fm");
                waveButtons = target != "shape" && g.chance(0.5);
                if (waveButtons)
                {
                    w.parts.push_back("wave-buttons");
                    w.integer("wave", 0, 2, g.below(3), "0 saw, 1 square, 2 triangle");
                    sourceItems.push_back("wave");
                }
                else
                {
                    w.parts.push_back("shape-dial");
                    w.real("shape", 0, 1, nice(g.range(0, 1)), "0 saw, 0.5 square, 1 triangle");
                    sourceItems.push_back("shape");
                }
                w.real("detune", 0, 50, nice(g.range(3, 25)), "cents");
                w.real("mix", 0, 1, nice(g.range(0.3, 0.9)), "the second oscillator's level");
                sourceItems.insert(sourceItems.end(), { "detune", "mix" });
                w.field("Osc osc1;");
                w.field("Osc osc2;");
                w.startSilent.insert(w.startSilent.end(), { "osc1.reset(0);", "osc2.reset(0.25f);" });
                w.call.push_back("const float spread = cents(detune[0]);");
                const std::string sh = waveButtons ? "0.5f * wave[0]"
                                                   : (target == "shape" ? "clampf(shape[i] + 0.4f * lfodepth[i] * l, 0, 1)" : "shape[i]");
                w.loop.push_back("const float sh = " + sh + ";");
                w.loop.push_back("float s = 0.7f * (osc1.morph(step, sh) + mix[i] * osc2.morph(step * " + num(ratios[ratio2], 6)
                                 + " * spread, sh));");
                sourceSection = "OSCILLATORS";
                sourceSaid = std::string("two band-limited oscillators, the second ") + ratioSaid[ratio2] + " and detuned";
            }
            else if (source == 1)
            {
                w.parts.push_back("wavetable");
                w.use("wavetable.fm");
                // The table: two to four frames, the library's shapes and perhaps one drawn from its harmonics.
                const int frames = 2 + g.below(3);
                const bool drawn = g.chance(0.5);
                if (drawn)
                    w.parts.push_back("drawn-frame");
                std::vector<int> shapes{ 0, 1, 2, 3 };
                for (int k = 3; k > 0; --k)
                    std::swap(shapes[static_cast<std::size_t>(k)], shapes[static_cast<std::size_t>(g.below(k + 1))]);
                const char* shapeNames[] = { "a sine", "a triangle", "a saw", "a square" };
                std::string s = "module Shared\n{\n    Wavetable table;                               // every voice reads it\n\n"
                                "    void init(float rate)\n    {\n        table.allocate(" + std::to_string(frames) + ");\n";
                std::vector<std::string> frameSaid;
                const int drawnAt = drawn ? g.below(frames) : -1;
                int next = 0;
                for (int f = 0; f < frames; ++f)
                {
                    if (f == drawnAt)
                    {
                        // A partial's amplitude falls as 1/k^tilt, some partials left out; their sum peaks under 1.
                        const int count = 6 + g.below(10);
                        const double tilt = g.range(0.5, 1.6);
                        const bool odd = g.chance(0.3);
                        std::vector<double> amps;
                        double total = 0.0;
                        for (int k = 1; k <= count; ++k)
                        {
                            double a = (k == 1 || (!(odd && k % 2 == 0) && g.chance(0.75))) ? std::pow(k, -tilt) * g.range(0.5, 1.0) : 0.0;
                            amps.push_back(a);
                            total += a;
                        }
                        s += "        // frame " + std::to_string(f) + ": drawn from " + std::to_string(count) + " partials" +
                             (odd ? ", the odd ones" : "") + "\n";
                        s += "        float* c = table.cycle(" + std::to_string(f) + ");\n";
                        s += "        for (int i = 0; i < 2048; ++i)\n        {\n";
                        s += "            const float x = 6.28318531f * i / 2048.0f;\n";
                        std::string sum;
                        for (std::size_t k = 0; k < amps.size(); ++k)
                            if (amps[k] > 0.0)
                                sum += std::string(sum.empty() ? "" : " + ") + num(amps[k] / total, 3) + " * sinf(" +
                                       (k == 0 ? std::string("x") : std::to_string(k + 1) + " * x") + ")";
                        s += "            c[i] = -(" + sum + ");\n        }\n";
                        s += "        table.build(" + std::to_string(f) + ");\n";
                        frameSaid.push_back("one drawn from its partials");
                    }
                    else
                    {
                        const int which = shapes[static_cast<std::size_t>(next++)];
                        s += "        table.shape(" + std::to_string(f) + ", " + std::to_string(which) + ");             // "
                           + shapeNames[which] + "\n";
                        frameSaid.push_back(shapeNames[which]);
                    }
                }
                s += "    }\n\n    void end()\n    {\n        table.release();\n    }\n};\n\n";
                w.shared = s;
                w.real("position", 0, 1, nice(g.range(0, 1)), "across the frames");
                w.real("detune", 0, 50, nice(g.range(3, 25)), "cents");
                w.real("mix", 0, 1, nice(g.range(0.3, 0.9)), "the second oscillator's level");
                sourceItems.insert(sourceItems.end(), { "position", "detune", "mix" });
                w.field("Shared* shared;                                // the plugin points it at the program's one Shared");
                w.field("WaveOsc osc1;");
                w.field("WaveOsc osc2;");
                w.init.insert(w.init.end(), { "osc1.table = &shared->table;", "osc2.table = &shared->table;" });
                w.startSilent.insert(w.startSilent.end(), { "osc1.reset(0);", "osc2.reset(0.25f);" });
                w.call.push_back("const float spread = cents(detune[0]);");
                w.loop.push_back(std::string("const float pos = ")
                                 + (target == "position" ? "clampf(position[i] + 0.5f * lfodepth[i] * l, 0, 1)" : "position[i]") + ";");
                w.loop.push_back("float s = 0.7f * (osc1.tick(step, pos) + mix[i] * osc2.tick(step * " + num(ratios[ratio2], 6)
                                 + " * spread, pos));");
                sourceSection = "WAVETABLE";
                sourceSaid = "a wavetable (" + listed(frameSaid) + ") built once for every voice, read by two oscillators, the second "
                           + ratioSaid[ratio2];
            }
            else
            {
                w.parts.push_back("fm");
                const double fmRatios[] = { 0.5, 1, 1.41, 2, 3, 3.5, 4, 5, 7 };
                const double r = g.pick(fmRatios);
                w.real("ratio", 0.5, 8, r, "the modulator's frequency over the carrier's");
                w.real("index", 0, 10, nice(g.range(1.5, 6)), "how far the modulator moves the carrier's phase");
                sourceItems.insert(sourceItems.end(), { "ratio", "index" });
                w.field("float cphase;");
                w.field("float mphase;");
                w.startSilent.insert(w.startSilent.end(), { "cphase = 0;", "mphase = 0;" });
                std::string depth = "index[i] * (0.2f + 0.8f * f)";
                if (target == "index")
                    depth += " * (1 + 0.5f * lfodepth[i] * l)";
                w.loop.push_back("const float m = sinf(6.28318531f * mphase);");
                w.loop.push_back("float s = sinf(6.28318531f * cphase + " + depth + " * m);");
                w.loop.push_back("cphase += step;");
                w.loop.push_back("cphase -= floorf(cphase);");
                w.loop.push_back("mphase += step * ratio[i];");
                w.loop.push_back("mphase -= floorf(mphase);");
                sourceSection = "OPERATORS";
                sourceSaid = "two-operator FM, the modulator at " + num(r) + " times the carrier, its index falling with an envelope";
            }
            if (hasNoise)
            {
                w.parts.push_back("noise");
                w.use("noise.fm");
                const bool pink = g.chance(0.6);
                w.real("air", 0, 0.5, nice(g.range(0.04, 0.2)), "noise");
                sourceItems.push_back("air");
                w.field("Noise noise;");
                w.start.push_back("noise.seed(1 + (int)note);");
                w.loop.push_back(std::string("s += air[i] * noise.") + (pink ? "pink" : "white") + "();");
                sourceSaid += std::string(", with ") + (pink ? "pink" : "white") + " noise";
            }

            // The filter.
            std::string filterSaid;
            if (hasFilter)
            {
                const char* names[] = { "", "svf-lp", "svf-bp", "svf-hp", "ladder" };
                w.parts.push_back(names[filter]);
                w.use(filter == 4 ? "ladder.fm" : "svf.fm");
                w.real("cutoff", 40, 12000, nice(g.logRange(filter == 3 ? 150 : 250, filter == 3 ? 900 : 3500)), "Hz");
                w.real("resonance", 0, 1, nice(g.range(0.1, filter == 2 ? 0.5 : 0.65)));
                w.real("envamount", 0, 6, nice(g.range(filter == 3 ? 0.5 : 1, 4)), "octaves the envelope opens it");
                w.field(filter == 4 ? "Ladder filter;" : "Svf filter;");
                w.startSilent.push_back("filter.clear();");
                std::string octaves = "envamount[i] * f";
                if (target == "cutoff")
                    octaves += " + 2 * lfodepth[i] * l";
                w.loop.push_back("const float fc = clampf(cutoff[i] * expf(0.693147181f * (" + octaves + ")), 20, 0.45f * rate);");
                w.loop.push_back("filter.set(fc, resonance[i], rate);");
                if (filter == 4)
                    w.loop.push_back("float y = 2 * filter.tick(0.5f * s);");
                else
                {
                    w.loop.push_back("filter.tick(s);");
                    w.loop.push_back(std::string("float y = filter.") + (filter == 1 ? "lp" : (filter == 2 ? "bp" : "hp")) + ";");
                }
                filterSaid = filter == 4 ? "a ladder low-pass" : (filter == 1 ? "a resonant low-pass" : (filter == 2 ? "a band-pass" : "a high-pass"));
                filterSaid += " its envelope opens";
            }
            else
            {
                w.parts.push_back("no-filter");
                w.loop.push_back("float y = s;");
            }
            if (hasDrive)
            {
                w.parts.push_back("drive");
                w.use("shaper.fm");
                w.real("heat", 0, 1, nice(g.range(0.15, 0.6)), "a tanh drive");
                w.loop.push_back("y = drive(y, heat[i]);");
            }
            w.real("level", -36, 0, nice(g.range(-12, -6)), "dB");
            w.call.push_back(std::string("const float gain = ") + (source == 2 ? "0.22f" : "0.25f") + " * velocity * dbToGain(level[0]);");
            w.loop.push_back(target == "level" ? "const float out = gain * a * (1 - 0.5f * lfodepth[i] * (1 - l)) * y;"
                                               : "const float out = gain * a * y;");

            // The panel.
            const bool scope = g.chance(0.3);
            if (scope)
            {
                w.parts.push_back("scope");
                sourceItems.push_back("trace");
            }
            std::string items;
            for (const std::string& it : sourceItems)
                items += " " + it;
            w.line("section \"" + sourceSection + "\"" + items);
            sections.push_back(sourceSection);
            if (hasFilter)
            {
                w.line("section \"FILTER\" cutoff resonance envamount");
                sections.push_back("FILTER");
            }
            w.line("row");
            w.line("section \"ENVELOPE\" attack decay sustain release");
            sections.push_back("ENVELOPE");
            if (hasLfo)
            {
                w.line("section \"LFO\" lforate lfodepth lfolight");
                sections.push_back("LFO");
            }
            w.line(std::string("section \"OUTPUT\"") + (hasDrive ? " heat" : "") + " level out");
            sections.push_back("OUTPUT");
            if (source != 2)
                w.line("label mix \"OSC 2\"");
            if (hasNoise)
                w.line("label air \"NOISE\"");
            if (hasFilter)
                w.line("label envamount \"ENV AMT\"");
            if (hasLfo)
            {
                w.line("label lforate \"RATE\"");
                w.line("label lfodepth \"DEPTH\"");
            }
            if (hasDrive)
                w.line("label heat \"DRIVE\"");
            if (g.chance(0.5))
            {
                w.parts.push_back("faders");
                w.line("style attack decay sustain release fader");
            }
            if (hasFilter && g.chance(0.3))
                w.line("style cutoff resonance vintage");
            if (waveButtons)
            {
                w.line("style wave buttons");
                w.line("options wave \"SAW\" \"SQUARE\" \"TRI\"");
            }
            if (source != 2 && g.chance(0.3))
                w.line("small detune");
            if (hasLfo)
                w.line("led lfolight \"LFO\"");
            if (scope)
                w.line("scope out as trace \"WAVE\"");
            if (g.chance(0.6))
            {
                w.parts.push_back("vu");
                w.line("vu out \"OUTPUT\"");
            }
            else
            {
                w.parts.push_back("meter");
                w.line("meter out \"OUT\"");
            }
            colour(g, w, sections);

            // The voice, assembled.
            sortLibrary(w.library);
            std::vector<std::string> what{ sourceSaid };
            if (hasFilter)
                what.push_back(filterSaid);
            for (const std::string& s : w.said)
                what.push_back(s);
            if (hasDrive)
                what.push_back("a drive");
            p.title = std::string(g.pick(kAdjectives)) + " "
                    + kVoiceNouns[static_cast<std::size_t>(&ch - kCharacters)][static_cast<std::size_t>(g.below(6))];
            const std::string kind = ch.name;
            std::string t = header(p, listed(what) + "; " + (kind == "keys" || kind == "brass" ? kind : "a " + kind), w);
            t += w.shared;
            t += "module Voice\n{\n" + paramLines(panelOrder(w.params, w.panel));
            t += block(w.fields, "    ");
            t += "    float rate;\n    float hz;\n    float velocity;\n\n";
            t += "    void init(float sampleRate)\n    {\n        rate = sampleRate;\n" + block(w.init, "        ") + "    }\n\n";
            t += "    void start(float note, float vel)\n    {\n        hz = noteToHz(note);\n        velocity = vel;\n";
            t += "        if (!amp.active())                         // a silent voice starts afresh; a sounding one goes on\n";
            t += "        {\n" + block(w.startSilent, "            ") + "        }\n";
            t += "        amp.gate(true);\n" + block(w.start, "        ") + "    }\n\n";
            t += "    void stop()\n    {\n        amp.gate(false);\n" + block(w.stop, "        ") + "    }\n\n";
            t += "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + block(w.call, "        ");
            t += "        for (int i = 0; i < n; ++i)\n        {\n" + block(w.loop, "            ");
            t += "            left[i] += out;\n            right[i] += out;\n        }\n" + block(w.after, "        ");
            t += "        return amp.active();\n    }\n};\n";
            p.text = t;
            p.library = w.library;
            p.parts = w.parts;
            return p;
        }

        // ---- an effect -------------------------------------------------------------------------------------------
        [[maybe_unused]] Program effect(Rng& g, Program p)
        {
            Writer w;
            w.use("util.fm");
            bool drive = g.chance(0.5), filter = g.chance(0.65), mod = g.chance(0.5), time = g.chance(0.7);
            if (static_cast<int>(drive) + static_cast<int>(filter) + static_cast<int>(mod) + static_cast<int>(time) < 2)
            {
                if (!filter)
                    filter = true;
                else
                    time = true;
            }
            std::vector<std::string> sections, said, rows;
            std::vector<std::string> sectionLines;
            const auto section = [&](const std::string& title, const std::string& items) {
                sectionLines.push_back("section \"" + title + "\" " + items);
                sections.push_back(title);
            };

            if (drive)
            {
                const int kind = g.choose({ 0.4, 0.2, 0.2, 0.2 });     // tanh, fold, crush, tube
                const char* names[] = { "tanh", "fold", "crush", "tube" };
                w.parts.push_back(names[kind]);
                w.use("shaper.fm");
                std::string items, title;
                if (kind == 0)
                {
                    w.real("heat", 0, 1, nice(g.range(0.3, 0.8)), "a tanh drive");
                    w.loop.push_back("l = drive(l, heat[i]);");
                    w.loop.push_back("r = drive(r, heat[i]);");
                    items = "heat";
                    title = "DRIVE";
                    w.line("label heat \"DRIVE\"");
                    said.push_back("a tanh drive");
                }
                else if (kind == 1)
                {
                    w.real("folds", 0, 1, nice(g.range(0.2, 0.7)), "a sine wavefolder");
                    w.loop.push_back("l = fold(l, folds[i]);");
                    w.loop.push_back("r = fold(r, folds[i]);");
                    items = "folds";
                    title = "FOLDER";
                    w.line("label folds \"FOLD\"");
                    said.push_back("a wavefolder");
                }
                else if (kind == 2)
                {
                    w.real("bits", 1, 16, nice(g.range(4, 10)), "the crusher's bits (16: clean)");
                    w.real("hold", 1, 16, nice(g.range(1, 6)), "samples each value is held");
                    w.field("Crusher crushL;");
                    w.field("Crusher crushR;");
                    w.loop.push_back("l = crushL.tick(l, bits[i], hold[i]);");
                    w.loop.push_back("r = crushR.tick(r, bits[i], hold[i]);");
                    items = "bits hold";
                    title = "CRUSHER";
                    said.push_back("a bit crusher");
                }
                else
                {
                    w.use("onepole.fm");
                    w.real("warmth", 0, 1, nice(g.range(0.3, 0.8)), "a tube's even harmonics");
                    w.field("DcBlock dcL;");
                    w.field("DcBlock dcR;");
                    w.init.insert(w.init.end(), { "dcL.set(rate);", "dcR.set(rate);" });
                    w.loop.push_back("l = dcL.tick(asym(l, warmth[i]));");
                    w.loop.push_back("r = dcR.tick(asym(r, warmth[i]));");
                    items = "warmth";
                    title = "TUBE";
                    said.push_back("a tube's curve");
                }
                if (g.chance(0.5))
                {
                    w.parts.push_back("tone");
                    w.use("onepole.fm");
                    w.real("tone", 400, 18000, nice(g.logRange(2500, 12000)), "Hz: a low-pass after it");
                    w.field("OnePole toneL;");
                    w.field("OnePole toneR;");
                    w.call.insert(w.call.end(), { "toneL.set(tone[0], rate);", "toneR.set(tone[0], rate);" });
                    w.loop.push_back("l = toneL.lowpass(l);");
                    w.loop.push_back("r = toneR.lowpass(r);");
                    items += " tone";
                }
                if (g.chance(0.4))
                    w.line("style " + items.substr(0, items.find(' ')) + " vintage");
                section(title, items);
            }

            if (filter)
            {
                const int kind = 1 + g.choose({ 0.45, 0.15, 0.1, 0.3 });   // 1 svf lp, 2 bp, 3 hp, 4 ladder
                const char* names[] = { "", "svf-lp", "svf-bp", "svf-hp", "ladder" };
                w.parts.push_back(names[kind]);
                w.use(kind == 4 ? "ladder.fm" : "svf.fm");
                const int motion = g.choose({ 0.3, 0.4, 0.3 });          // still, an LFO, the input's level
                const char* motions[] = { "static", "wah", "follow" };
                w.parts.push_back(motions[motion]);
                w.real("cutoff", 40, 16000, nice(g.logRange(kind == 3 ? 200 : 300, kind == 3 ? 1500 : 4000)), "Hz");
                w.real("resonance", 0, 1, nice(g.range(0.1, kind == 4 ? 0.7 : 0.6)));
                std::string items = "cutoff resonance", octaves;
                if (motion == 1)
                {
                    w.use("lfo.fm");
                    const bool tri = g.chance(0.4);
                    w.real("wahrate", 0.05, 10, nice(g.logRange(0.2, 4)), "Hz");
                    w.real("wahdepth", 0, 4, nice(g.range(0.8, 2.5)), "octaves each way");
                    w.field("Lfo wah;");
                    w.field("float wahlight;");
                    w.loop.push_back("wah.advance(wahrate[i] / rate);");
                    w.loop.push_back(std::string("const float sweep = wah.") + (tri ? "triangle" : "sine") + "(0);");
                    w.after.push_back(std::string("wahlight = 0.5f * (1 + wah.") + (tri ? "triangle" : "sine") + "(0));");
                    octaves = "wahdepth[i] * sweep";
                    items += " wahrate wahdepth wahlight";
                    w.line("label wahrate \"RATE\"");
                    w.line("label wahdepth \"DEPTH\"");
                    w.line("led wahlight \"SWEEP\"");
                }
                else if (motion == 2)
                {
                    w.use("follower.fm");
                    w.real("sens", 0, 6, nice(g.range(1.5, 4)), "octaves the input's level opens it");
                    w.field("Follower follow;");
                    w.field("float envlight;");
                    w.call.push_back("follow.set(" + num(nice(g.logRange(2, 15))) + ", " + num(nice(g.logRange(60, 300))) + ", rate);");
                    w.loop.push_back("const float e = follow.peak(0.5f * (l + r));");
                    w.loop.push_back("envlight = e;");
                    octaves = "sens[i] * fminf(e, 1)";
                    items += " sens envlight";
                    w.line("meter envlight \"ENV\"");
                }
                w.field(kind == 4 ? "Ladder filterL;" : "Svf filterL;");
                w.field(kind == 4 ? "Ladder filterR;" : "Svf filterR;");
                w.loop.push_back(octaves.empty() ? "const float fc = clampf(cutoff[i], 20, 0.45f * rate);"
                                                 : "const float fc = clampf(cutoff[i] * expf(0.693147181f * " + octaves + "), 20, 0.45f * rate);");
                w.loop.push_back("filterL.set(fc, resonance[i], rate);");
                w.loop.push_back("filterR.set(fc, resonance[i], rate);");
                if (kind == 4)
                {
                    w.loop.push_back("l = 2 * filterL.tick(0.5f * l);");
                    w.loop.push_back("r = 2 * filterR.tick(0.5f * r);");
                }
                else
                {
                    const char* out = kind == 1 ? "lp" : (kind == 2 ? "bp" : "hp");
                    w.loop.push_back("filterL.tick(l);");
                    w.loop.push_back("filterR.tick(r);");
                    w.loop.push_back(std::string("l = filterL.") + out + ";");
                    w.loop.push_back(std::string("r = filterR.") + out + ";");
                }
                const char* kindSaid[] = { "", "a resonant low-pass", "a band-pass", "a high-pass", "a ladder low-pass" };
                said.push_back(std::string(kindSaid[kind]) + (motion == 1 ? " an LFO sweeps" : (motion == 2 ? " the input's level opens" : "")));
                if (g.chance(0.3))
                    w.line("style cutoff vintage");
                section(motion == 1 ? "SWEEP" : (motion == 2 ? "ENV FILTER" : "FILTER"), items);
            }

            if (mod)
            {
                const int kind = g.choose({ 0.35, 0.25, 0.2, 0.2 });     // chorus, flanger, tremolo, auto-pan
                const char* names[] = { "chorus", "flanger", "tremolo", "autopan" };
                const char* titles[] = { "CHORUS", "FLANGER", "TREMOLO", "AUTO-PAN" };
                w.parts.push_back(names[kind]);
                w.use("lfo.fm");
                w.field("Lfo mod;");
                w.field("float modlight;");
                std::string items = "speed depth";
                if (kind <= 1)
                {
                    w.use("delay.fm");
                    w.real("speed", 0.03, 5, nice(kind == 0 ? g.logRange(0.2, 1.5) : g.logRange(0.08, 0.5)), "Hz");
                    w.real("depth", 0, 1, nice(g.range(0.3, 0.9)));
                    w.field("Delay modL;");
                    w.field("Delay modR;");
                    w.init.insert(w.init.end(), { "modL.allocate((int)(0.05f * rate));", "modR.allocate((int)(0.05f * rate));" });
                    w.end.insert(w.end.end(), { "modL.release();", "modR.release();" });
                    const double base = kind == 0 ? g.range(7, 12) : g.range(0.6, 1.5);
                    const double span = kind == 0 ? g.range(2, 5) : g.range(1, 3);
                    w.loop.push_back("mod.advance(speed[i] / rate);");
                    w.loop.push_back("const float dl = (" + num(base) + " + " + num(span) + " * depth[i] * (1 + mod.sine(0))) * 0.001f * rate;");
                    w.loop.push_back("const float dr = (" + num(base) + " + " + num(span) + " * depth[i] * (1 + mod.sine(0.25f))) * 0.001f * rate;");
                    w.loop.push_back("const float wl = modL.readCubic(dl);");
                    w.loop.push_back("const float wr = modR.readCubic(dr);");
                    if (kind == 1)
                    {
                        w.real("regen", 0, 0.9, nice(g.range(0.3, 0.7)), "the flanger's feedback");
                        w.loop.push_back("modL.write(l + regen[i] * wl);");
                        w.loop.push_back("modR.write(r + regen[i] * wr);");
                        items += " regen";
                        w.line("label regen \"FEEDBACK\"");
                    }
                    else
                    {
                        w.loop.push_back("modL.write(l);");
                        w.loop.push_back("modR.write(r);");
                    }
                    w.loop.push_back("l = 0.70710678f * (l + wl);");
                    w.loop.push_back("r = 0.70710678f * (r + wr);");
                    said.push_back(kind == 0 ? "a stereo chorus" : "a flanger");
                }
                else if (kind == 2)
                {
                    w.real("speed", 0.3, 12, nice(g.logRange(2, 7)), "Hz");
                    w.real("depth", 0, 1, nice(g.range(0.4, 0.8)));
                    w.loop.push_back("mod.advance(speed[i] / rate);");
                    w.loop.push_back("const float t = 1 - 0.5f * depth[i] * (1 - mod.sine(0));");
                    w.loop.push_back("l *= t;");
                    w.loop.push_back("r *= t;");
                    said.push_back("a tremolo");
                }
                else
                {
                    w.real("speed", 0.05, 6, nice(g.logRange(0.2, 2)), "Hz");
                    w.real("depth", 0, 1, nice(g.range(0.5, 1)));
                    w.loop.push_back("mod.advance(speed[i] / rate);");
                    w.loop.push_back("const float pan = depth[i] * mod.sine(0);");
                    w.loop.push_back("l *= 1.41421356f * panLeft(pan);");
                    w.loop.push_back("r *= 1.41421356f * panRight(pan);");
                    said.push_back("an auto-pan");
                }
                w.after.push_back("modlight = 0.5f * (1 + mod.sine(0));");
                w.line("label speed \"RATE\"");
                w.line("led modlight \"MOTION\"");
                section(titles[kind], items + " modlight");
            }

            if (time)
            {
                const bool echo = g.chance(0.55);
                if (echo)
                {
                    const bool pingpong = g.chance(0.4);
                    w.parts.push_back("echo");
                    if (pingpong)
                        w.parts.push_back("pingpong");
                    w.use("delay.fm");
                    w.use("onepole.fm");
                    w.real("time", 20, 1500, nice(g.logRange(90, 650)), "ms");
                    w.real("feedback", 0, 0.95, nice(g.range(0.25, 0.6)), "of each echo in the next");
                    w.real("echo", 0, 1, nice(g.range(0.25, 0.55)), "the echoes' level");
                    w.field("Delay lineL;");
                    w.field("Delay lineR;");
                    w.field("OnePole dampL;");
                    w.field("OnePole dampR;");
                    const std::string damp = num(nice(g.logRange(1800, 8000)));
                    w.init.insert(w.init.end(), { "lineL.allocate((int)(1.6f * rate));", "lineR.allocate((int)(1.6f * rate));",
                                                  "dampL.set(" + damp + ", rate);", "dampR.set(" + damp + ", rate);" });
                    w.end.insert(w.end.end(), { "lineL.release();", "lineR.release();" });
                    w.loop.push_back("const float d = clampf(time[i] * 0.001f * rate, 1, 1.5f * rate);");
                    w.loop.push_back("const float el = lineL.read(d);");
                    w.loop.push_back("const float er = lineR.read(d);");
                    w.loop.push_back(std::string("lineL.write(l + feedback[i] * dampL.lowpass(") + (pingpong ? "er" : "el") + "));");
                    w.loop.push_back(std::string("lineR.write(r + feedback[i] * dampR.lowpass(") + (pingpong ? "el" : "er") + "));");
                    w.loop.push_back("l += echo[i] * el;");
                    w.loop.push_back("r += echo[i] * er;");
                    w.line("label feedback \"REPEATS\"");
                    w.line("label echo \"LEVEL\"");
                    said.push_back(pingpong ? "a ping-pong echo" : "an echo");
                    section("ECHO", "time feedback echo");
                }
                else
                {
                    w.parts.push_back("reverb");
                    w.use("reverb.fm");
                    w.real("size", 0, 1, nice(g.range(0.3, 0.85)), "the decay: a room to a hall");
                    w.real("space", 0, 1, nice(g.range(0.2, 0.5)), "the reverb's level");
                    w.field("Reverb verb;");
                    w.init.push_back("verb.allocate(rate);");
                    w.end.push_back("verb.release();");
                    w.call.push_back("verb.set(size[0], " + num(nice(g.range(0.2, 0.7))) + ", " + num(nice(g.range(0.6, 1))) + ");");
                    w.loop.push_back("verb.tick(l, r);");
                    w.loop.push_back("l += space[i] * verb.left;");
                    w.loop.push_back("r += space[i] * verb.right;");
                    w.line("label space \"LEVEL\"");
                    said.push_back("a reverb");
                    section("REVERB", "size space");
                }
            }

            w.real("mix", 0, 1, nice(g.range(0.6, 1)), "dry to wet");
            w.real("output", -24, 12, nice(g.range(-3, 0)), "dB");
            const bool meters = g.chance(0.6);
            w.parts.push_back(meters ? "meters" : "vu");
            section("OUTPUT", meters ? "mix output in out" : "mix output out");
            if (meters)
            {
                w.line("meter in \"IN\"");
                w.line("meter out \"OUT\"");
            }
            else
                w.line("vu out \"OUTPUT\"");
            if (g.chance(0.4))
                w.line("style mix fader");
            // The sections, a row of three at most.
            std::vector<std::string> panel;
            const std::size_t rowAt = sectionLines.size() >= 5 ? 3 : (sectionLines.size() == 4 ? 2 : sectionLines.size());
            for (std::size_t k = 0; k < sectionLines.size(); ++k)
            {
                if (k == rowAt)
                    panel.push_back("#pragma panel row");
                panel.push_back("#pragma panel " + sectionLines[k]);
            }
            panel.insert(panel.end(), w.panel.begin(), w.panel.end());
            w.panel = panel;
            colour(g, w, sections);

            sortLibrary(w.library);
            p.title = std::string(g.pick(kAdjectives)) + " " + (time ? g.pick(kSpaceNouns) : g.pick(kTextureNouns));
            std::string t = header(p, listed(said), w);
            t += "module Effect\n{\n" + paramLines(panelOrder(w.params, w.panel));
            t += block(w.fields, "    ");
            t += "    float rate;\n\n";
            t += "    void init(float sampleRate)\n    {\n        rate = sampleRate;\n" + block(w.init, "        ") + "    }\n\n";
            if (!w.end.empty())
                t += "    void end()\n    {\n" + block(w.end, "        ") + "    }\n\n";
            t += "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + block(w.call, "        ");
            t += "        for (int i = 0; i < n; ++i)\n        {\n";
            t += "            float l = left[i];\n            float r = right[i];\n";
            t += "            const float dryL = l;\n            const float dryR = r;\n";
            t += block(w.loop, "            ");
            t += "            const float g = dbToGain(output[i]);\n";
            t += "            left[i] = g * (dryL + mix[i] * (l - dryL));\n";
            t += "            right[i] = g * (dryR + mix[i] * (r - dryR));\n        }\n" + block(w.after, "        ");
            t += "    }\n};\n";
            p.text = t;
            p.library = w.library;
            p.parts = w.parts;
            return p;
        }
    }

    Program make(uint32_t seed)
    {
        Program p;
        p.seed = seed >= 1 && seed <= kMaxSeed ? seed : 1 + seed % kMaxSeed;
        Rng g(p.seed);
        if constexpr (kInstrument)
            return instrument(g, p);
        else
            return effect(g, p);
    }

    std::vector<std::string> allParts()
    {
        if constexpr (kInstrument)
            return { "pluck", "keys", "pad", "bass", "brass", "bell", "analog", "wave-buttons", "shape-dial", "wavetable",
                     "drawn-frame", "fm", "noise", "svf-lp", "svf-bp", "svf-hp", "ladder", "no-filter", "drive",
                     "lfo>pitch", "lfo>level", "lfo>cutoff", "lfo>shape", "lfo>position", "lfo>index", "lfo-sine",
                     "lfo-triangle", "lfo-random", "faders", "scope", "vu", "meter" };
        else
            return { "tanh", "fold", "crush", "tube", "tone", "svf-lp", "svf-bp", "svf-hp", "ladder", "static", "wah",
                     "follow", "chorus", "flanger", "tremolo", "autopan", "echo", "pingpong", "reverb", "meters", "vu" };
    }
}
