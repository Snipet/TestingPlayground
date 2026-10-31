// Source/fminus/CirTemplates.cpp: see CirTemplates.h.
#include "fminus/CirTemplates.h"

#include "fminus/CirDoc.h"
#include "fminus/Lang.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace playground::cir
{
    namespace
    {
        // A float literal FMinus reads as a float: "1e-05f", "0.5f", "2.0f".
        std::string literal(double v)
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.9g", v);
            std::string s = buf;
            if (s.find_first_of(".e") == std::string::npos)
                s += ".0";
            return s + "f";
        }

        // A law input as tick takes it: its name and the argument's range (its range= over its scale=; cv's 0:1 by
        // default).
        struct Law
        {
            std::string name;
            double lo = 0.0, hi = 1.0;
        };

        std::vector<Law> lawsOf(const Netlist& nl)
        {
            std::vector<Law> out;
            for (const Terminal& t : nl.laws)
            {
                Law law;
                law.name = t.name;
                double scale = 1.0;
                for (const Token& k : t.tokens)
                {
                    const std::string& x = k.text;
                    if (x.rfind("scale=", 0) == 0)
                    {
                        if (const std::optional<double> v = lang::spiceNumber(x.substr(6)); v && *v != 0.0 && std::isfinite(*v))
                            scale = *v;
                    }
                    else if (x.rfind("range=", 0) == 0)
                    {
                        const std::size_t colon = x.find(':', 6);
                        if (colon == std::string::npos)
                            continue;
                        const std::optional<double> lo = lang::spiceNumber(x.substr(6, colon - 6));
                        const std::optional<double> hi = lang::spiceNumber(x.substr(colon + 1));
                        if (lo && hi && *lo < *hi)
                        {
                            law.lo = *lo;
                            law.hi = *hi;
                        }
                    }
                }
                law.lo /= scale;
                law.hi /= scale;
                if (law.hi < law.lo)
                    std::swap(law.lo, law.hi);
                out.push_back(law);
            }
            return out;
        }

        // The law inputs' params, and their values a sample (the lines that compute them, the arguments after the
        // signal).
        void lawLines(const std::vector<Law>& laws, std::string& params, std::string& values, std::string& args,
                      const std::string& indent)
        {
            for (const Law& l : laws)
            {
                const bool geometric = l.lo > 0.0;
                const std::string lo = literal(l.lo), hi = literal(l.hi);
                params += "    param float " + l.name + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];   // " + l.name
                        + ": " + lo.substr(0, lo.size() - 1) + " to " + hi.substr(0, hi.size() - 1)
                        + (geometric ? ", geometrically" : "") + "\n";
                const std::string v = l.name + "Now";
                values += indent + "const float " + v + " = "
                        + (geometric ? literal(l.lo) + " * expf(" + literal(std::log(l.hi / l.lo)) + " * " + l.name + "[i]);\n"
                                     : literal(l.lo) + " + " + literal(l.hi - l.lo) + " * " + l.name + "[i];\n");
                args += ", " + v;
            }
        }
    }

    HostProgram hostProgram(std::string_view file, std::string_view netlist, bool instrument)
    {
        HostProgram out;
        const std::string name(file);
        // The module and its tick, as Lang reads the netlist (the module FMinus generates from it).
        lang::Service service;
        service.setFiles({ { "program.fm", "" }, { name, std::string(netlist) } });
        const int index = service.fileIndex(name);
        bool two = false;
        for (const lang::Symbol& s : service.symbols())
            if (s.file == index && s.kind == lang::Kind::module && out.module.empty())
                out.module = s.name;
        for (const lang::Symbol& s : service.symbols())
            if (s.file == index && s.kind == lang::Kind::method && s.container == out.module && s.name == "tick")
                two = !s.params.empty() && s.params.front().rfind("float2 ", 0) == 0;
        if (out.module.empty())
        {
            out.why = name + " makes no module to play";
            return out;
        }
        const std::vector<Law> laws = lawsOf(read(netlist));
        const std::string& m = out.module;
        std::string params, values, args;
        lawLines(laws, params, values, args, "            ");
        std::string& s = out.text;
        if (instrument)
        {
            s = "// Play " + name + ": its module, " + m + ", played by an oscillator and an envelope, its pots the plugin's\n"
                "// dials (Play this circuit).\n"
                "#include \"osc.fm\"\n#include \"adsr.fm\"\n#include \"" + name + "\"\n"
                "module Voice\n{\n" + params +
                "    Osc osc;\n    Adsr env;\n    " + m + " circuit;\n    float rate;\n"
                "    float step;                          // the note, cycles a sample\n    float velocity;\n\n"
                "    void init(float sampleRate)\n    {\n        rate = sampleRate;\n        circuit.init(sampleRate);\n    }\n\n"
                "    void start(float note, float vel)\n    {\n        step = 440 * expf(0.0577622650f * (note - 69)) / rate;\n"
                "        velocity = vel;\n"
                "        if (!env.active())               // from silence: the circuit at rest\n        {\n"
                "            osc.reset(0);\n            circuit.clear();\n        }\n        env.gate(true);\n    }\n\n"
                "    void stop()\n    {\n        env.gate(false);\n    }\n\n"
                "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                "        env.set(5, 400, 0.7f, 300, rate);\n        for (int i = 0; i < n; ++i)\n        {\n" + values +
                "            const float x = velocity * env.tick() * osc.saw(step);\n";
            if (two)
                s += "            const float2 y = circuit.tick((float2)(x, x)" + args + ");\n"
                     "            left[i] += y.x;\n            right[i] += y.y;\n";
            else
                s += "            const float y = circuit.tick(x" + args + ");\n            left[i] += y;\n            right[i] += y;\n";
            s += "        }\n        return env.active();\n    }\n};\n";
            return out;
        }
        s = "// Play " + name + ": its module, " + m + ", on both channels, its pots the plugin's dials (Play this circuit).\n"
            "#include \"" + name + "\"\n"
            "module Effect\n{\n" + params;
        if (two)
            s += "    " + m + " amp;                       // both channels: .channels 2\n\n"
                 "    void init(float rate)\n    {\n        amp.init(rate);\n    }\n\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n" + values +
                 "            const float2 y = amp.tick((float2)(left[i], right[i])" + args + ");\n"
                 "            left[i] = y.x;\n            right[i] = y.y;\n        }\n    }\n};\n";
        else
            s += "    " + m + " amp[2];                    // one a channel: the dials drive both\n\n"
                 "    void init(float rate)\n    {\n        amp[0].init(rate);\n        amp[1].init(rate);\n    }\n\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n" + values +
                 "            left[i] = amp[0].tick(left[i]" + args + ");\n"
                 "            right[i] = amp[1].tick(right[i]" + args + ");\n        }\n    }\n};\n";
        return out;
    }
}
