// Source/sandbox/Dials.cpp: see Dials.h.
#include "sandbox/Dials.h"

#include "fminus/Lang.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace playground::sandbox
{
    namespace
    {
        std::string lower(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool isFm(const std::string& name) { return name.size() > 3 && name.compare(name.size() - 3, 3, ".fm") == 0; }

        // The numbers between the parentheses after `attribute` in a declaration's text ("fminus::range(- 36, 0)": -36
        // and 0; Lang's tokens are spaced, so a minus may stand apart from its number). Empty when it is not there.
        std::vector<double> numbersOf(const std::string& signature, const char* attribute)
        {
            std::vector<double> out;
            const std::size_t at = signature.find(attribute);
            if (at == std::string::npos)
                return out;
            const std::size_t open = signature.find('(', at), close = signature.find(')', at);
            if (open == std::string::npos || close == std::string::npos || close < open)
                return out;
            std::string inside;
            for (const char c : signature.substr(open + 1, close - open - 1))
                if (c != ' ')
                    inside += c;
            std::size_t from = 0;
            while (from <= inside.size())
            {
                const std::size_t comma = inside.find(',', from);
                const std::string part = inside.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                if (part == "true" || part == "false")
                    out.push_back(part == "true" ? 1.0 : 0.0);
                else
                {
                    char* end = nullptr;
                    const double v = std::strtod(part.c_str(), &end);
                    if (end != part.c_str() && std::isfinite(v))
                        out.push_back(v);
                }
                if (comma == std::string::npos)
                    break;
                from = comma + 1;
            }
            return out;
        }

        bool number(const juce::var& v, double& out)
        {
            if (v.isDouble() || v.isInt() || v.isInt64())
            {
                out = static_cast<double>(v);
                return std::isfinite(out);
            }
            if (v.isBool())
            {
                out = static_cast<bool>(v) ? 1.0 : 0.0;
                return true;
            }
            return false;
        }

        std::string quoted(const std::string& s)
        {
            std::string out = "\"";
            for (const char c : s)
                if (c != '"' && c != '\\' && c != '\n')
                    out += c;
            return out + "\"";
        }

        std::string upperAscii(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        bool identifier(const std::string& s)
        {
            if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }
    }

    Declared readProgram(const Sources& files, bool instrument)
    {
        Declared out;
        Sources programs;
        for (const SourceFile& f : files)
            if (isFm(f.name))
                programs.push_back(f);
        if (programs.empty())
            return out;
        lang::Service service;
        lang::Options o;
        o.instrument = instrument;
        service.setOptions(o);
        service.setFiles(programs);
        const std::string module = instrument ? "Voice" : "Effect";
        std::vector<const lang::Symbol*> params;
        for (const lang::Symbol& s : service.symbols())
        {
            const bool mine = s.file >= 0 && static_cast<std::size_t>(s.file) < service.files().size() && service.files()[static_cast<std::size_t>(s.file)].program;
            if (!mine)
                continue;
            if (s.kind == lang::Kind::module && s.name == module && out.module.empty())
            {
                out.module = module;
                out.file = service.files()[static_cast<std::size_t>(s.file)].name;
            }
            if (s.container != module)
                continue;
            if (s.kind == lang::Kind::param)
                params.push_back(&s);
            else if (s.kind == lang::Kind::field)
                out.fields.push_back({ s.name, s.type });
        }
        std::stable_sort(params.begin(), params.end(), [](const lang::Symbol* a, const lang::Symbol* b) {
            return a->file != b->file ? a->file < b->file : a->at < b->at;
        });
        for (const lang::Symbol* s : params)
        {
            if (out.dials.size() >= static_cast<std::size_t>(kMaxControls))
            {
                ++out.beyond;
                continue;
            }
            Dial d;
            d.name = s->name;
            d.type = s->type == "bool" ? ControlType::toggle : s->type == "int" ? ControlType::integer : ControlType::real;
            const std::vector<double> range = numbersOf(s->signature, "fminus::range");
            const std::vector<double> def = numbersOf(s->signature, "fminus::default");
            if (d.type == ControlType::toggle)
            {
                d.min = 0.0f;
                d.max = 1.0f;
            }
            else
            {
                d.min = range.size() >= 2 ? static_cast<float>(range[0]) : 0.0f;
                d.max = range.size() >= 2 ? static_cast<float>(range[1]) : (d.type == ControlType::integer ? 10.0f : 1.0f);
            }
            d.def = def.empty() ? d.min : static_cast<float>(def[0]);
            out.dials.push_back(std::move(d));
        }
        return out;
    }

    std::vector<Dial> parseDials(const juce::var& list, std::string& notes)
    {
        std::vector<Dial> out;
        const juce::Array<juce::var>* a = list.getArray();
        if (a == nullptr)
        {
            if (!list.isVoid())
                notes += "dials: a list of dials ([{ name, range, default, ... }]); left out\n";
            return out;
        }
        for (const juce::var& v : *a)
        {
            Dial d;
            d.given = !v.hasProperty("given") || static_cast<bool>(v["given"]);
            d.name = v["name"].toString().trim().toStdString();
            if (!identifier(d.name) || d.name.size() >= sizeof(Control{}.name))
            {
                notes += "dials: '" + d.name + "' is not a parameter's name (letters, digits and _, under 32); left out\n";
                continue;
            }
            const std::string type = lower(v["type"].toString().trim().toStdString());
            if (const juce::Array<juce::var>* o = v["options"].getArray())
                for (const juce::var& name : *o)
                    d.options.push_back(name.toString().trim().toStdString());
            d.type = type == "bool" ? ControlType::toggle
                   : type == "int" || (type.empty() && !d.options.empty()) ? ControlType::integer
                                                                           : ControlType::real;
            if (!type.empty() && type != "bool" && type != "int" && type != "float")
                notes += "dials: " + d.name + "'s type '" + type + "' is none of float, int, bool: a float\n";
            double lo = 0.0, hi = 0.0;
            const juce::Array<juce::var>* range = v["range"].getArray();
            if (d.type == ControlType::toggle)
            {
                d.min = 0.0f;
                d.max = 1.0f;
            }
            else if (range != nullptr && range->size() == 2 && number((*range)[0], lo) && number((*range)[1], hi))
            {
                d.min = static_cast<float>(lo);
                d.max = static_cast<float>(hi);
            }
            else
            {
                if (range != nullptr || v.hasProperty("range"))
                    notes += "dials: " + d.name + "'s range is two numbers, [lo, hi]: the default range instead\n";
                d.min = 0.0f;
                d.max = d.type == ControlType::integer ? std::max(1.0f, static_cast<float>(d.options.size()) - 1.0f) : 1.0f;
            }
            double def = 0.0;
            float plain = 0.0f;
            if (number(v["default"], def))
                d.def = static_cast<float>(def);
            else if (v["default"].isString() && valueOf(d, v["default"], plain))
                d.def = plain;
            else
                d.def = d.min;
            d.unit = v["unit"].toString().trim().toStdString().substr(0, sizeof(Control{}.unit) - 1);
            d.style = lower(v["style"].toString().trim().toStdString());
            if (!d.style.empty() && d.style != "knob" && d.style != "slider" && d.style != "switch" && d.style != "buttons"
                && d.style != "menu")
            {
                notes += "dials: " + d.name + "'s style '" + d.style + "' is none of knob, slider, switch, buttons, menu: a knob\n";
                d.style.clear();
            }
            out.push_back(std::move(d));
        }
        return out;
    }

    juce::var toJson(const std::vector<Dial>& dials)
    {
        juce::Array<juce::var> out;
        for (const Dial& d : dials)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String(d.name));
            o->setProperty("type", d.type == ControlType::toggle ? "bool" : d.type == ControlType::integer ? "int" : "float");
            juce::Array<juce::var> range{ static_cast<double>(d.min), static_cast<double>(d.max) };
            o->setProperty("range", range);
            o->setProperty("default", static_cast<double>(d.def));
            if (!d.unit.empty())
                o->setProperty("unit", juce::String(d.unit));
            if (!d.options.empty())
            {
                juce::Array<juce::var> names;
                for (const std::string& n : d.options)
                    names.add(juce::String(n));
                o->setProperty("options", names);
            }
            if (!d.style.empty())
                o->setProperty("style", juce::String(d.style));
            o->setProperty("given", d.given);
            out.add(juce::var(o));
        }
        return out;
    }

    std::vector<Dial> merge(std::vector<Dial> base, const std::vector<Dial>& given)
    {
        for (const Dial& g : given)
        {
            const auto it = std::find_if(base.begin(), base.end(), [&](const Dial& b) { return lower(b.name) == lower(g.name); });
            if (it != base.end())
                *it = g;
            else
                base.push_back(g);
        }
        return base;
    }

    Controls controlsOf(const std::vector<Dial>& dials)
    {
        Controls out{};
        for (std::size_t k = 0; k < dials.size() && k < out.size(); ++k)
        {
            const Dial& d = dials[k];
            Control& c = out[k];
            std::snprintf(c.name, sizeof c.name, "%s", d.name.c_str());
            std::snprintf(c.unit, sizeof c.unit, "%s", d.unit.c_str());
            c.used = true;
            c.type = d.type;
            c.min = d.type == ControlType::toggle ? 0.0f : d.min;
            c.max = d.type == ControlType::toggle ? 1.0f : d.max;
            if (c.max < c.min)
                std::swap(c.min, c.max);
            if (!(c.max > c.min))
                c.max = c.min + 1.0f;
            c.def = plainOf(c, normOf(c, d.def));
        }
        return out;
    }

    std::string blankGui(const std::vector<Dial>& dials, bool instrument)
    {
        std::string controls;
        int inSection = 0, sections = 0;
        const auto open = [&] {
            ++sections;
            controls += std::string(sections > 1 ? "\n" : "") + "        section \"" + (sections == 1 ? std::string("CONTROLS")
                                                                                                  : "CONTROLS " + std::to_string(sections))
                      + "\" {\n";
        };
        for (std::size_t k = 0; k < dials.size() && k < static_cast<std::size_t>(kMaxControls); ++k)
        {
            const Dial& d = dials[k];
            if (inSection == 0)
                open();
            std::string kind = d.style;
            if (kind.empty())
                kind = d.type == ControlType::toggle                                  ? "switch"
                     : d.type == ControlType::integer && !d.options.empty()           ? (d.options.size() > 8 ? "menu" : "buttons")
                                                                                      : "knob";
            std::string line = "            " + kind + " " + d.name;
            if (kind == "switch" || kind == "buttons" || kind == "menu")
                for (const std::string& o : d.options)
                    line += " " + quoted(upperAscii(o));
            if (kind == "switch" && d.options.empty())
                line += " \"OFF\" \"ON\"";
            if ((kind == "knob" || kind == "slider") && d.type == ControlType::real && !d.unit.empty())
                line += " unit=" + quoted(d.unit);
            controls += line + "\n";
            if (++inSection == 6)
            {
                controls += "        }\n";
                inSection = 0;
            }
        }
        if (inSection > 0)
            controls += "        }\n";
        std::string meters = instrument ? "            meter out label=\"OUT\"\n"
                                        : "            meter in label=\"IN\"\n            meter out label=\"OUT\"\n";
        return "// A panel to start from (the GUI sandbox's blank): the dials, then the output.\n"
               "panel {\n"
               "    row wrap {\n"
             + controls + (controls.empty() ? "" : "\n") + "        section \"OUTPUT\" {\n" + meters + "        }\n"
               "    }\n"
               "}\n";
    }

    std::vector<std::string> fieldsNamed(const std::string& gui)
    {
        // The words of the file, comments and "texts" left out; a display's word, then its source.
        std::vector<std::string> words;
        std::string w;
        bool lineComment = false, blockComment = false, text = false;
        const auto flush = [&] {
            if (!w.empty())
                words.push_back(w);
            w.clear();
        };
        for (std::size_t i = 0; i < gui.size(); ++i)
        {
            const char c = gui[i];
            const char next = i + 1 < gui.size() ? gui[i + 1] : '\0';
            if (lineComment)
            {
                if (c == '\n')
                    lineComment = false;
                continue;
            }
            if (blockComment)
            {
                if (c == '*' && next == '/')
                {
                    blockComment = false;
                    ++i;
                }
                continue;
            }
            if (text)
            {
                if (c == '"' || c == '\n')
                    text = false;
                continue;
            }
            if (c == '/' && next == '/')
            {
                flush();
                lineComment = true;
                continue;
            }
            if (c == '/' && next == '*')
            {
                flush();
                blockComment = true;
                ++i;
                continue;
            }
            if (c == '"')
            {
                flush();
                text = true;
                words.push_back("\"");
                continue;
            }
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '=')
                w += c;
            else
            {
                flush();
                if (c == ';' || c == '{' || c == '}' || c == '\n')
                    words.push_back(";");
            }
        }
        flush();
        std::vector<std::string> out;
        const auto add = [&](const std::string& name) {
            if (identifier(name) && name != "in" && name != "out" && std::find(out.begin(), out.end(), name) == out.end())
                out.push_back(name);
        };
        static constexpr const char* kDisplays[] = { "meter", "vu", "led", "number", "scope", "spectrum" };
        for (std::size_t k = 0; k < words.size(); ++k)
        {
            const std::string& x = words[k];
            const bool startsElement = k == 0 || words[k - 1] == ";";
            if (startsElement && k + 1 < words.size()
                && std::any_of(std::begin(kDisplays), std::end(kDisplays), [&](const char* d) { return x == d; }))
                add(words[k + 1]);
            if (x.rfind("live=", 0) == 0)
                add(x.substr(5));
        }
        return out;
    }

    bool valueOf(const Dial& d, const juce::var& v, float& plain)
    {
        double x = 0.0;
        if (number(v, x))
        {
            plain = static_cast<float>(x);
            return true;
        }
        if (!v.isString())
            return false;
        const std::string s = lower(v.toString().trim().toStdString());
        for (std::size_t k = 0; k < d.options.size(); ++k)
            if (lower(d.options[k]) == s)
            {
                plain = (d.type == ControlType::toggle ? 0.0f : d.min) + static_cast<float>(k);
                return true;
            }
        if (s == "on" || s == "true")
        {
            plain = 1.0f;
            return true;
        }
        if (s == "off" || s == "false")
        {
            plain = 0.0f;
            return true;
        }
        char* end = nullptr;
        x = std::strtod(s.c_str(), &end);
        if (end != s.c_str() && std::isfinite(x))
        {
            plain = static_cast<float>(x);
            return true;
        }
        return false;
    }
}
