// Source/fminus/GuiSpec.cpp: a .gui file read (GuiSpec.h). Words first (a token each, with its line and column), then
// the elements (a tree of what each line says), then the interface: every element checked against what it takes and
// against the program, its controls bound to the parameters' slots and its displays to their sources.
#include "fminus/GuiSpec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>

namespace testingplaygroundsynth
{
    namespace
    {
        // ---- words ----------------------------------------------------------------------------------------------------
        struct Token
        {
            enum class Kind : uint8_t { name, text, number, color, equals, open, close, end, eof };
            Kind kind = Kind::eof;
            std::string value;
            double number = 0.0;
            int line = 1, col = 1;
            bool is(Kind k) const noexcept { return kind == k; }
            bool isValue() const noexcept
            {
                return kind == Kind::name || kind == Kind::text || kind == Kind::number || kind == Kind::color;
            }
        };

        bool nameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        bool nameChar(char c) { return nameStart(c) || (c >= '0' && c <= '9'); }
        bool digit(char c) { return c >= '0' && c <= '9'; }
        bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

        // The file in tokens. A newline or a ; ends an element (an `end` token); what is no token is a note, and
        // skipped.
        std::vector<Token> lex(std::string_view s, const std::string& file, std::string& notes)
        {
            std::vector<Token> out;
            int line = 1, col = 1;
            std::size_t i = 0;
            const auto note = [&](int l, int c, const std::string& what) {
                notes += file + ":" + std::to_string(l) + ":" + std::to_string(c) + ": note: " + what + "\n";
            };
            const auto advance = [&](std::size_t n) {
                for (std::size_t k = 0; k < n && i < s.size(); ++k, ++i)
                {
                    if (s[i] == '\n')
                    {
                        ++line;
                        col = 1;
                    }
                    else
                        ++col;
                }
            };
            const auto push = [&](Token::Kind kind, std::string value, double number, int l, int c) {
                Token t;
                t.kind = kind;
                t.value = std::move(value);
                t.number = number;
                t.line = l;
                t.col = c;
                out.push_back(std::move(t));
            };
            while (i < s.size())
            {
                const char c = s[i];
                const int l = line, cl = col;
                if (c == '\n' || c == ';')
                {
                    push(Token::Kind::end, "", 0.0, l, cl);
                    advance(1);
                }
                else if (c == ' ' || c == '\t' || c == '\r' || c == ',')
                    advance(1);
                else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
                {
                    while (i < s.size() && s[i] != '\n')
                        advance(1);
                }
                else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
                {
                    const std::size_t close = s.find("*/", i + 2);
                    if (close == std::string_view::npos)
                    {
                        note(l, cl, "a /* comment without its */: the rest of the file is the comment");
                        advance(s.size() - i);
                    }
                    else
                        advance(close + 2 - i);
                }
                else if (c == '"')
                {
                    std::string text;
                    std::size_t k = i + 1;
                    bool closed = false;
                    for (; k < s.size() && s[k] != '\n'; ++k)
                    {
                        if (s[k] == '\\' && k + 1 < s.size() && (s[k + 1] == '"' || s[k + 1] == '\\'))
                        {
                            text += s[++k];
                            continue;
                        }
                        if (s[k] == '"')
                        {
                            closed = true;
                            break;
                        }
                        text += s[k];
                    }
                    if (!closed)
                        note(l, cl, "a \"text\" without its closing \" (it ends with the line)");
                    advance((closed ? k + 1 : k) - i);
                    push(Token::Kind::text, std::move(text), 0.0, l, cl);
                }
                else if (c == '#')
                {
                    std::size_t k = i + 1;
                    while (k < s.size() && hex(s[k]))
                        ++k;
                    const std::size_t n = k - i - 1;
                    if ((n != 6 && n != 3) || (k < s.size() && nameChar(s[k])))
                    {
                        while (k < s.size() && nameChar(s[k]))
                            ++k;
                        note(l, cl, "'" + std::string(s.substr(i, k - i)) + "' is not a colour: #rrggbb or #rgb");
                        advance(k - i);
                        continue;
                    }
                    std::string v(s.substr(i, k - i));
                    if (n == 3)                          // #rgb as #rrggbb
                        v = std::string("#") + v[1] + v[1] + v[2] + v[2] + v[3] + v[3];
                    for (char& ch : v)
                        ch = static_cast<char>(ch >= 'A' && ch <= 'F' ? ch - 'A' + 'a' : ch);
                    advance(k - i);
                    push(Token::Kind::color, std::move(v), 0.0, l, cl);
                }
                else if (nameStart(c))
                {
                    std::size_t k = i;
                    while (k < s.size() && nameChar(s[k]))
                        ++k;
                    std::string v(s.substr(i, k - i));
                    advance(k - i);
                    push(Token::Kind::name, std::move(v), 0.0, l, cl);
                }
                else if (digit(c) || ((c == '-' || c == '+' || c == '.') && i + 1 < s.size()
                                      && (digit(s[i + 1]) || (s[i + 1] == '.' && i + 2 < s.size() && digit(s[i + 2])))))
                {
                    std::size_t k = i + 1;
                    while (k < s.size() && (digit(s[k]) || s[k] == '.'))
                        ++k;
                    const std::string text(s.substr(i, k - i));
                    char* end = nullptr;
                    const double v = std::strtod(text.c_str(), &end);
                    advance(k - i);
                    if (end == nullptr || *end != '\0' || !std::isfinite(v))
                    {
                        note(l, cl, "'" + text + "' is not a number");
                        continue;
                    }
                    if (i < s.size() && nameStart(s[i]))       // 120px: the unit is said by the property, not here
                    {
                        std::size_t e = i;
                        while (e < s.size() && nameChar(s[e]))
                            ++e;
                        note(line, col, "a number takes no unit after it: '" + std::string(s.substr(i, e - i))
                                            + "' is left out (sizes are in px)");
                        advance(e - i);
                    }
                    push(Token::Kind::number, text, v, l, cl);
                }
                else if (c == '=')
                {
                    push(Token::Kind::equals, "=", 0.0, l, cl);
                    advance(1);
                }
                else if (c == '{')
                {
                    push(Token::Kind::open, "{", 0.0, l, cl);
                    advance(1);
                }
                else if (c == '}')
                {
                    push(Token::Kind::close, "}", 0.0, l, cl);
                    advance(1);
                }
                else
                {
                    note(l, cl, std::string("'") + c + "' is not part of an element: names, \"texts\", numbers, "
                                                       "#colours, =, { and } are");
                    advance(1);
                }
            }
            Token eof;
            eof.kind = Token::Kind::eof;
            eof.line = line;
            eof.col = col;
            out.push_back(eof);
            return out;
        }

        // ---- elements -------------------------------------------------------------------------------------------------
        struct Prop
        {
            std::string key;
            Token value;
            bool own = true;                             // the element's own (not its class's or the defaults')
        };

        struct Element
        {
            Token name;
            std::vector<Token> args;
            std::vector<Prop> props;
            std::vector<Element> children;
            bool block = false;                          // it had { }
        };

        class Parser
        {
        public:
            Parser(const std::vector<Token>& t, const std::string& file, std::string& notes)
                : t_(t), file_(file), notes_(notes) {}

            // Elements until a } (which the caller takes) or the end of the file.
            std::vector<Element> list(int depth)
            {
                std::vector<Element> out;
                for (;;)
                {
                    while (peek().is(Token::Kind::end))
                        ++at_;
                    const Token& t = peek();
                    if (t.is(Token::Kind::eof) || t.is(Token::Kind::close))
                        return out;
                    if (!t.is(Token::Kind::name))
                    {
                        note(t, "an element starts with its name (row, knob, ...), not '" + t.value + "'");
                        skipLine();
                        continue;
                    }
                    Element e;
                    e.name = t;
                    ++at_;
                    header(e);
                    // Its children: { on this line, or on the next.
                    std::size_t look = at_;
                    while (t_[look].is(Token::Kind::end))
                        ++look;
                    if (t_[look].is(Token::Kind::open))
                    {
                        const Token open = t_[look];
                        at_ = look + 1;
                        e.block = true;
                        if (depth + 1 > kMaxGuiDepth)
                            note(open, "containers nest " + std::to_string(kMaxGuiDepth) + " deep at most: what is in "
                                                                                            "this one is left out");
                        std::vector<Element> kids = list(depth + 1);
                        if (depth + 1 <= kMaxGuiDepth)
                            e.children = std::move(kids);
                        if (peek().is(Token::Kind::close))
                            ++at_;
                        else
                            note(open, "this { has no } (the file ends inside it)");
                    }
                    out.push_back(std::move(e));
                }
            }

            // The } left over at the top level.
            void strays()
            {
                while (!peek().is(Token::Kind::eof))
                {
                    if (peek().is(Token::Kind::close))
                        note(peek(), "a } that closes nothing");
                    ++at_;
                    std::vector<Element> more = list(0);
                    for (Element& e : more)
                        extra_.push_back(std::move(e));
                }
            }

            std::vector<Element> takeExtra() { return std::move(extra_); }

        private:
            const Token& peek() const { return t_[at_]; }

            void note(const Token& t, const std::string& what)
            {
                notes_ += file_ + ":" + std::to_string(t.line) + ":" + std::to_string(t.col) + ": note: " + what + "\n";
            }

            void skipLine()
            {
                while (!peek().is(Token::Kind::end) && !peek().is(Token::Kind::eof) && !peek().is(Token::Kind::close)
                       && !peek().is(Token::Kind::open))
                    ++at_;
            }

            // The element's arguments and key=value properties, up to the end of its line, a { or a }.
            void header(Element& e)
            {
                for (;;)
                {
                    const Token& t = peek();
                    if (t.is(Token::Kind::end) || t.is(Token::Kind::eof) || t.is(Token::Kind::open) || t.is(Token::Kind::close))
                        return;
                    if (t.is(Token::Kind::equals))
                    {
                        note(t, "an = needs a name before it (key=value)");
                        ++at_;
                        if (peek().isValue())
                            ++at_;
                        continue;
                    }
                    if (t.is(Token::Kind::name) && t_[at_ + 1].is(Token::Kind::equals))
                    {
                        const Token key = t;
                        at_ += 2;
                        if (!peek().isValue())
                        {
                            note(key, key.value + "= needs a value after it");
                            continue;
                        }
                        e.props.push_back({ key.value, peek() });
                        e.props.back().value.line = key.line;   // notes about it point at its key
                        e.props.back().value.col = key.col;
                        ++at_;
                        continue;
                    }
                    e.args.push_back(t);
                    ++at_;
                }
            }

            const std::vector<Token>& t_;
            const std::string& file_;
            std::string& notes_;
            std::size_t at_ = 0;
            std::vector<Element> extra_;
        };

        // ---- the interface ----------------------------------------------------------------------------------------------
        bool isNamedColor(const std::string& name)
        {
            return std::any_of(std::begin(kPanelColors), std::end(kPanelColors), [&](const char* c) { return name == c; });
        }

        std::string list(std::initializer_list<const char*> words)
        {
            std::string out;
            std::size_t k = 0;
            for (const char* w : words)
            {
                out += k == 0 ? "" : (k + 1 == words.size() ? " or " : ", ");
                out += w;
                ++k;
            }
            return out;
        }

        int valuesOf(const Control& c)
        {
            if (c.type == ControlType::toggle)
                return 2;
            if (c.type == ControlType::integer)
                return static_cast<int>(std::lround(c.max) - std::lround(c.min)) + 1;
            return 0;
        }

        std::string typeName(const Control& c)
        {
            return c.type == ControlType::real ? "a float" : (c.type == ControlType::integer ? "an int" : "a bool");
        }

        bool isDisplay(const std::string& w)
        {
            return w == "meter" || w == "vu" || w == "led" || w == "number" || w == "scope" || w == "spectrum";
        }

        DisplayKind displayKind(const std::string& w)
        {
            if (w == "vu")
                return DisplayKind::vu;
            if (w == "led")
                return DisplayKind::led;
            if (w == "number")
                return DisplayKind::number;
            if (w == "scope")
                return DisplayKind::scope;
            if (w == "spectrum")
                return DisplayKind::spectrum;
            return DisplayKind::meter;
        }

        bool isContainerWord(const std::string& w)
        {
            return w == "row" || w == "column" || w == "grid" || w == "section" || w == "box" || w == "tabs" || w == "page";
        }

        bool isElementWord(const std::string& w)
        {
            return isContainerWord(w) || isDisplay(w) || w == "spacer" || w == "divider" || w == "label" || w == "knob"
                || w == "slider" || w == "switch" || w == "buttons" || w == "menu" || w == "xy" || w == "envelope"
                || w == "keyboard" || w == "shader";
        }

        constexpr const char* kElements = "row, column, grid, section, box, tabs, page, spacer, divider, label, knob, "
                                          "slider, switch, buttons, menu, xy, envelope, keyboard, shader, meter, vu, "
                                          "led, number, scope, spectrum, class or defaults";

        constexpr const char* kKnobLooks = "modern, vintage, flat, minimal, ring, led, chicken, soft or dot";

        class Builder
        {
        public:
            Builder(const SourceFile& file, Controls& controls, const std::vector<PanelField>& fields, bool instrument,
                    const std::vector<std::string>& fileNames, std::string& notes)
                : file_(file), controls_(controls), fields_(fields), instrument_(instrument), fileNames_(fileNames),
                  notes_(notes) {}

            PanelSpec build(const std::vector<Element>& top)
            {
                spec_.gui.file = file_.name;
                collect(top);
                GuiNode root;
                root.kind = GuiKind::column;
                root.gap = 10.0f;
                spec_.gui.nodes.push_back(root);
                // `panel` may hold the elements, with the panel's colours.
                std::vector<const Element*> elements;
                for (const Element& e : top)
                {
                    if (e.name.value == "panel")
                    {
                        if (panelSeen_)
                            note(e.name, "one panel a file: this one's children join the first's, its colours are left out");
                        else
                            panelProps(e);
                        panelSeen_ = true;
                        for (const Element& c : e.children)
                            elements.push_back(&c);
                        for (const Token& a : e.args)
                            note(a, "panel takes background=, gradient= and accent=, then its children between { }");
                        continue;
                    }
                    elements.push_back(&e);
                }
                for (const Element* e : elements)
                    add(*e, 0);

                // The dials no control names, last, in a section without a title (each knob as the defaults say).
                std::string rest;
                std::vector<int> left;
                for (int c = 0; c < kMaxControls; ++c)
                    if (controls_[static_cast<std::size_t>(c)].used && !placed_[static_cast<std::size_t>(c)])
                    {
                        left.push_back(c);
                        rest += (rest.empty() ? "" : ", ") + std::string(controls_[static_cast<std::size_t>(c)].name);
                    }
                if (!left.empty())
                {
                    notes_ += file_.name + ": note: " + rest + (left.size() == 1 ? " has" : " have")
                            + " no control in the file; " + (left.size() == 1 ? "it comes" : "they come")
                            + " last, in a section without a title\n";
                    Element section;
                    section.name.kind = Token::Kind::name;
                    section.name.value = "section";
                    for (const int c : left)
                    {
                        Element knob;
                        knob.name.kind = Token::Kind::name;
                        knob.name.value = "knob";
                        Token name;
                        name.kind = Token::Kind::name;
                        name.value = controls_[static_cast<std::size_t>(c)].name;
                        knob.args.push_back(name);
                        section.children.push_back(std::move(knob));
                    }
                    quiet_ = true;                       // what the defaults say of them is the file's to fix
                    add(section, 0);
                    quiet_ = false;
                }
                return std::move(spec_);
            }

        private:
            // ---- classes and defaults ---------------------------------------------------------------------------------
            // `class "NAME" key=value ...` and `defaults KIND key=value ...`, wherever they are: read before anything
            // else, so that an element may use one written after it.
            void collect(const std::vector<Element>& elements)
            {
                for (const Element& e : elements)
                {
                    const std::string& w = e.name.value;
                    if (w == "class" || w == "defaults")
                    {
                        const bool isClass = w == "class";
                        if (e.args.empty() || (isClass ? !(e.args[0].is(Token::Kind::text) || e.args[0].is(Token::Kind::name))
                                                       : !e.args[0].is(Token::Kind::name)))
                        {
                            note(e.name, isClass ? "class takes a \"name\", then the properties it gives: class \"big\" size=96"
                                                 : "defaults takes an element's name, then the properties every one of them "
                                                   "gets: defaults knob look=ring");
                            continue;
                        }
                        const std::string key = e.args[0].value;
                        if (!isClass && !isElementWord(key))
                        {
                            note(e.args[0], "defaults takes an element's name (knob, section, ...), not '" + key + "'");
                            continue;
                        }
                        for (std::size_t k = 1; k < e.args.size(); ++k)
                            note(e.args[k], w + " takes key=value properties after its name: '" + e.args[k].value + "' is left out");
                        if (e.block)
                            note(e.name, w + " holds no children: what is between its { } is left out");
                        std::vector<Prop>& into = isClass ? classes_[key] : defaults_[key];
                        if (isClass && classes_.count(key) > 0 && !into.empty())
                            note(e.args[0], "a class \"" + key + "\" is defined already: this one's properties are added to it");
                        for (Prop p : e.props)
                        {
                            p.own = false;
                            into.push_back(p);
                        }
                        continue;
                    }
                    collect(e.children);
                }
            }

            // An element's properties: its kind's defaults, then its classes' in order, then its own (the last word
            // wins, as later properties set the same field again).
            Element merged(const Element& e)
            {
                Element m;
                m.name = e.name;
                m.args = e.args;
                m.children = e.children;
                m.block = e.block;
                if (const auto d = defaults_.find(e.name.value); d != defaults_.end())
                    m.props = d->second;
                for (const Prop& p : e.props)
                    if (p.key == "class")
                    {
                        std::string names = p.value.value;
                        std::size_t at = 0;
                        while (at < names.size())
                        {
                            const std::size_t end = names.find(' ', at);
                            const std::string one = names.substr(at, end == std::string::npos ? std::string::npos : end - at);
                            at = end == std::string::npos ? names.size() : end + 1;
                            if (one.empty())
                                continue;
                            const auto c = classes_.find(one);
                            if (c == classes_.end())
                            {
                                std::string have;
                                for (const auto& [name, props] : classes_)
                                    have += (have.empty() ? "" : ", ") + name;
                                note(p.value, "there is no class \"" + one + "\"" + (have.empty() ? std::string(" (none is defined)") : " (the classes: " + have + ")"));
                                continue;
                            }
                            m.props.insert(m.props.end(), c->second.begin(), c->second.end());
                        }
                    }
                for (const Prop& p : e.props)
                    if (p.key != "class")
                        m.props.push_back(p);
                return m;
            }

            // ---- notes and values -------------------------------------------------------------------------------------
            void note(const Token& at, const std::string& what)
            {
                if (quiet_)
                    return;
                const std::string line = file_.name + ":" + std::to_string(at.line) + ":" + std::to_string(at.col) + ": note: "
                                       + what + "\n";
                if (said_.insert(line).second)           // a class's mistake is said once, not at every use
                    notes_ += line;
            }

            // A property this element does not take: a note when it is its own (a class's or the defaults' are left
            // out quietly: a class may serve several kinds).
            void unknown(const Prop& p, const std::string& element, const std::string& takes)
            {
                if (p.own)
                    note(p.value, element + " takes " + takes + ": " + p.key + "= is left out");
            }

            int push(GuiNode n, int parent)
            {
                n.parent = parent;
                const int i = static_cast<int>(spec_.gui.nodes.size());
                spec_.gui.nodes.push_back(std::move(n));
                spec_.gui.nodes[static_cast<std::size_t>(parent)].children.push_back(i);
                return i;
            }

            // A number property in [lo, hi], else a note and `fallback`.
            float number(const Prop& p, float lo, float hi, float fallback)
            {
                if (!p.value.is(Token::Kind::number))
                {
                    note(p.value, p.key + "= takes a number, not '" + p.value.value + "'");
                    return fallback;
                }
                const auto v = static_cast<float>(p.value.number);
                if (v < lo || v > hi)
                {
                    note(p.value, p.key + "=" + p.value.value + " is outside " + trim(lo) + " to " + trim(hi) + ": "
                                      + trim(std::clamp(v, lo, hi)) + " instead");
                    return std::clamp(v, lo, hi);
                }
                return v;
            }

            static std::string trim(float v)
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
                return buf;
            }

            // A name property among `words`, else a note and -1.
            int choice(const Prop& p, std::initializer_list<const char*> words)
            {
                int k = 0;
                for (const char* w : words)
                {
                    if (p.value.is(Token::Kind::name) && p.value.value == w)
                        return k;
                    ++k;
                }
                note(p.value, p.key + "= takes " + list(words) + ", not '" + p.value.value + "'");
                return -1;
            }

            // true, false (yes, no, on, off, 1, 0).
            bool flag(const Prop& p)
            {
                const std::string& v = p.value.value;
                if (v == "true" || v == "yes" || v == "on" || (p.value.is(Token::Kind::number) && p.value.number != 0.0))
                    return true;
                if (v == "false" || v == "no" || v == "off" || (p.value.is(Token::Kind::number) && p.value.number == 0.0))
                    return false;
                note(p.value, p.key + "= takes true or false, not '" + v + "'");
                return false;
            }

            std::string color(const Token& v, bool none = false)
            {
                if (v.is(Token::Kind::color))
                    return v.value;
                if (v.is(Token::Kind::name) && isNamedColor(v.value))
                    return v.value;
                if (none && v.is(Token::Kind::name) && v.value == "none")
                    return "none";
                std::string names;
                for (const char* c : kPanelColors)
                    names += (names.empty() ? "" : ", ") + std::string(c);
                note(v, "'" + v.value + "' is not a colour: #rrggbb, " + names + (none ? ", or none" : ""));
                return {};
            }

            std::string text(const Token& v, std::size_t max, const char* what)
            {
                std::string s = v.value;
                if (!v.is(Token::Kind::text))
                    note(v, std::string(what) + " is a \"text\": '" + s + "' is read as one");
                if (s.size() > max)
                {
                    note(v, std::string("the ") + what + " \"" + s + "\" is cut to " + std::to_string(max) + " characters");
                    s.resize(max);
                }
                return s;
            }

            int slotNamed(const std::string& name) const
            {
                for (int c = 0; c < kMaxControls; ++c)
                    if (controls_[static_cast<std::size_t>(c)].used && name == controls_[static_cast<std::size_t>(c)].name)
                        return c;
                return -1;
            }

            std::string dialNames() const
            {
                std::string have;
                for (const Control& c : controls_)
                    if (c.used)
                        have += (have.empty() ? "" : ", ") + std::string(c.name);
                return have.empty() ? "none" : have;
            }

            // The parameter `t` names, as a slot (and a note when it is none, or another control has it).
            int param(const Token& t, const std::string& element)
            {
                if (!t.is(Token::Kind::name))
                {
                    note(t, element + " takes a parameter's name, not '" + t.value + "'");
                    return -1;
                }
                const int c = slotNamed(t.value);
                if (c >= 0 && placed_[static_cast<std::size_t>(c)])
                {
                    note(t, t.value + " has a control already (one a parameter): this " + element + " is left out");
                    return -1;
                }
                if (c < 0)
                    note(t, "there is no parameter " + t.value + " (the dials: " + dialNames() + ")");
                return c;
            }

            // A field of the main module a display, a live= or a shader may read (and why not, when it is none).
            bool fieldOk(const Token& at, const std::string& name, const char* who)
            {
                const auto f = std::find_if(fields_.begin(), fields_.end(), [&](const PanelField& x) { return x.name == name; });
                if (f == fields_.end() || f->type.rfind("param", 0) == 0 || (f->type != "float" && f->type != "int" && f->type != "bool"))
                {
                    std::string have;
                    for (const PanelField& x : fields_)
                        if (x.type == "float" || x.type == "int" || x.type == "bool")
                            have += (have.empty() ? "" : ", ") + x.name;
                    note(at, f == fields_.end() ? "there is no field " + name + " in the main module (" + who + " reads a float, "
                                                  "int or bool field" + (have.empty() ? std::string(")") : ": " + have + ")")
                             : f->type.rfind("param", 0) == 0 ? name + " is a dial; " + who + " reads a field the program "
                                                                           "writes"
                                                              : name + " is a " + f->type + "; " + who + " reads a float, "
                                                                                                         "int or bool field");
                    return false;
                }
                return true;
            }

            // A tap of a field, for a live= or a shader: a display that is never drawn. Its index, or -1.
            int hiddenTap(const Token& at, const std::string& field, const char* who)
            {
                if (!fieldOk(at, field, who))
                    return -1;
                for (std::size_t d = 0; d < spec_.displays.size(); ++d)
                    if (spec_.displays[d].hidden && spec_.displays[d].source == field)
                        return static_cast<int>(d);
                if (static_cast<int>(spec_.displays.size()) >= kMaxDisplays)
                {
                    note(at, "a panel reads " + std::to_string(kMaxDisplays) + " fields and displays at most: " + field
                                 + " is left out");
                    return -1;
                }
                PanelDisplay d;
                d.kind = DisplayKind::number;
                d.source = field;
                d.name = field + "~";
                d.hidden = true;
                d.tap = taps_++;
                spec_.displays.push_back(std::move(d));
                return static_cast<int>(spec_.displays.size()) - 1;
            }

            // ---- the panel's own colours ------------------------------------------------------------------------------
            void panelProps(const Element& e)
            {
                for (const Prop& p : e.props)
                {
                    if (p.key == "background")
                        spec_.gui.background = color(p.value);
                    else if (p.key == "gradient")
                        spec_.gui.gradient = color(p.value);
                    else if (p.key == "accent")
                        spec_.gui.accent = color(p.value);
                    else
                        note(p.value, "panel takes background=, gradient= and accent=: " + p.key + "= is left out");
                }
                if (!spec_.gui.gradient.empty() && spec_.gui.background.empty())
                    note(e.name, "gradient= blends from background= to it: give the panel a background= too");
            }

            // ---- an element and its children ----------------------------------------------------------------------------
            void add(const Element& raw, int parent)
            {
                const std::string& w = raw.name.value;
                if (w == "class" || w == "defaults")
                    return;                              // read by collect()
                if (static_cast<int>(spec_.gui.nodes.size()) >= kMaxGuiNodes)
                {
                    if (!full_)
                        note(raw.name, "a .gui file has " + std::to_string(kMaxGuiNodes) + " elements at most: the rest "
                                                                                            "are left out");
                    full_ = true;
                    return;
                }
                const GuiKind parentKind = spec_.gui.nodes[static_cast<std::size_t>(parent)].kind;
                if (parentKind == GuiKind::tabs && w != "page")
                {
                    note(raw.name, "tabs hold pages (page \"NAME\" { ... }): this " + w + " is left out");
                    return;
                }
                if (w == "page" && parentKind != GuiKind::tabs)
                {
                    note(raw.name, "a page goes in tabs { }: its children join its container instead");
                    for (const Element& c : raw.children)
                        add(c, parent);
                    return;
                }
                if (w == "panel")
                {
                    note(raw.name, "panel goes at the file's top: its children join this container");
                    for (const Element& c : raw.children)
                        add(c, parent);
                    return;
                }
                const Element e = merged(raw);
                if (!isContainerWord(w) && e.block)
                    note(e.name, w + " holds no children: what is between its { } is left out");
                if (isContainerWord(w))
                    addContainer(e, parent);
                else if (w == "spacer" || w == "divider" || w == "label")
                    addDecoration(e, parent);
                else if (w == "knob" || w == "slider" || w == "switch" || w == "buttons" || w == "menu")
                    addControl(e, parent);
                else if (w == "xy")
                    addXy(e, parent);
                else if (w == "envelope")
                    addEnvelope(e, parent);
                else if (w == "keyboard")
                    addKeyboard(e, parent);
                else if (w == "shader")
                    addShader(e, parent);
                else if (isDisplay(w))
                    addDisplay(e, parent);
                else
                    note(e.name, "unknown element '" + w + "': " + kElements);
            }

            // margin=, self=, minwidth=, maxwidth=, minheight=, maxheight=: any element's.
            bool commonProp(GuiNode& n, const Prop& p)
            {
                if (p.key == "margin")
                    n.margin = number(p, 0.0f, 200.0f, 0.0f);
                else if (p.key == "self")
                {
                    if (const int k = choice(p, { "stretch", "start", "center", "end" }); k >= 0)
                        n.self = k;
                }
                else if (p.key == "minwidth")
                    n.minW = number(p, 0.0f, 4000.0f, 0.0f);
                else if (p.key == "maxwidth")
                    n.maxW = number(p, 0.0f, 4000.0f, 0.0f);
                else if (p.key == "minheight")
                    n.minH = number(p, 0.0f, 4000.0f, 0.0f);
                else if (p.key == "maxheight")
                    n.maxH = number(p, 0.0f, 4000.0f, 0.0f);
                else
                    return false;
                return true;
            }

            // The layout properties a container and some leaves take; false when the key is none of them.
            bool layoutProp(GuiNode& n, const Prop& p, bool container)
            {
                if (commonProp(n, p))
                    return true;
                if (p.key == "grow")
                    n.grow = number(p, 0.0f, 100.0f, 0.0f);
                else if (p.key == "width")
                    n.width = number(p, 0.0f, 4000.0f, 0.0f);
                else if (p.key == "height")
                    n.height = number(p, 0.0f, 4000.0f, 0.0f);
                else if (!container)
                    return false;
                else if (p.key == "gap")
                    n.gap = number(p, 0.0f, 200.0f, n.gap);
                else if (p.key == "pad")
                    n.pad = number(p, 0.0f, 200.0f, n.pad);
                else if (p.key == "align")
                {
                    if (const int k = choice(p, { "stretch", "start", "center", "end" }); k >= 0)
                        n.align = static_cast<GuiAlign>(k);
                }
                else if (p.key == "justify")
                {
                    if (const int k = choice(p, { "start", "center", "end", "between", "around" }); k >= 0)
                        n.justify = static_cast<GuiJustify>(k);
                }
                else if (p.key == "wrap" && (n.kind == GuiKind::row || n.kind == GuiKind::column || n.kind == GuiKind::section
                                             || n.kind == GuiKind::box || n.kind == GuiKind::page))
                    n.wrap = flag(p);
                else
                    return false;
                return true;
            }

            // How a container is drawn: radius=, fill=, border=, borderwidth=, shadow=, look= (a section's or a box's:
            // frame, card, inset, plate; tabs': tabs, bar, pills, side), titlecolor=, titlealign=, titlesize=.
            bool decorProp(GuiNode& n, const Prop& p)
            {
                GuiStyle& st = n.style;
                const bool titled = n.kind == GuiKind::section || n.kind == GuiKind::tabs;
                if (p.key == "radius")
                    st.radius = number(p, 0.0f, 200.0f, 0.0f);
                else if (p.key == "fill")
                    st.fill = color(p.value, true);
                else if (p.key == "border")
                    st.border = color(p.value, true);
                else if (p.key == "borderwidth")
                    st.borderWidth = number(p, 0.0f, 12.0f, 1.0f);
                else if (p.key == "shadow")
                    st.shadow = flag(p) ? 1 : 0;
                else if ((p.key == "look" || (p.key == "style" && n.kind == GuiKind::tabs)) && n.kind == GuiKind::tabs)
                {
                    if (const int k = choice(p, { "tabs", "bar", "pills", "side" }); k >= 0)
                        st.look = p.value.value;
                }
                else if (p.key == "look" && (n.kind == GuiKind::section || n.kind == GuiKind::box))
                {
                    if (const int k = choice(p, { "frame", "card", "inset", "plate" }); k >= 0)
                        st.look = p.value.value;
                }
                else if (p.key == "titlecolor" && titled)
                    st.title = color(p.value);
                else if (p.key == "titlealign" && titled)
                {
                    if (const int k = choice(p, { "left", "center", "right" }); k >= 0)
                        st.titleAlign = k;
                }
                else if (p.key == "titlesize" && titled)
                {
                    if (const int k = choice(p, { "small", "normal", "large", "title" }); k >= 0)
                        st.titleSize = k;
                }
                else
                    return false;
                return true;
            }

            void addContainer(const Element& e, int parent)
            {
                const std::string& w = e.name.value;
                GuiNode n;
                n.kind = w == "row"       ? GuiKind::row
                       : w == "column"    ? GuiKind::column
                       : w == "grid"      ? GuiKind::grid
                       : w == "section"   ? GuiKind::section
                       : w == "box"       ? GuiKind::box
                       : w == "tabs"      ? GuiKind::tabs
                                          : GuiKind::page;
                n.line = e.name.line;
                n.vertical = !(n.kind == GuiKind::row || n.kind == GuiKind::section);
                if (n.kind == GuiKind::grid)
                    n.cols = 4;
                if (n.kind == GuiKind::box)
                    n.pad = 8.0f;
                bool titled = false;
                for (const Token& a : e.args)
                {
                    if ((n.kind == GuiKind::section || n.kind == GuiKind::page) && !titled && a.is(Token::Kind::text))
                    {
                        titled = true;
                        n.text = text(a, kMaxSectionTitle, "title");
                    }
                    else if (n.kind == GuiKind::grid && a.is(Token::Kind::number) && !titled)
                    {
                        titled = true;
                        n.cols = std::clamp(static_cast<int>(a.number), 1, 32);
                    }
                    else if (a.is(Token::Kind::name) && a.value == "wrap" && n.kind != GuiKind::grid && n.kind != GuiKind::tabs)
                        n.wrap = true;
                    else
                        note(a, w + (n.kind == GuiKind::section || n.kind == GuiKind::page ? " takes one \"title\"" : " takes no argument")
                                  + ": '" + a.value + "' is left out");
                }
                if (n.kind == GuiKind::page && n.text.empty())
                    n.text = "PAGE " + std::to_string(spec_.gui.nodes[static_cast<std::size_t>(parent)].children.size() + 1);
                for (const Prop& p : e.props)
                {
                    if (layoutProp(n, p, true) || decorProp(n, p))
                        continue;
                    if (p.key == "cols" && n.kind == GuiKind::grid)
                        n.cols = static_cast<int>(number(p, 1.0f, 32.0f, 4.0f));
                    else if (p.key == "layout" && (n.kind == GuiKind::section || n.kind == GuiKind::box || n.kind == GuiKind::page))
                    {
                        if (const int k = choice(p, { "row", "column" }); k >= 0)
                            n.vertical = k == 1;
                    }
                    else if (p.key == "color" && (n.kind == GuiKind::section || n.kind == GuiKind::box || n.kind == GuiKind::tabs))
                        n.color = color(p.value);
                    else
                        unknown(p, w, containerProps(n.kind));
                }
                const int i = push(std::move(n), parent);
                for (const Element& c : e.children)
                    add(c, i);
                if (spec_.gui.nodes[static_cast<std::size_t>(i)].children.empty())
                    note(e.name, w + " has nothing in it");
            }

            static std::string containerProps(GuiKind k)
            {
                std::string out = "gap=, pad=, align=, justify=, grow=, width=, height=, margin=, self=, min/max width= "
                                  "and height=, radius=, fill=, border=, borderwidth=, shadow=";
                if (k == GuiKind::row || k == GuiKind::column || k == GuiKind::section || k == GuiKind::box || k == GuiKind::page)
                    out += ", wrap=";
                if (k == GuiKind::grid)
                    out += ", cols=";
                if (k == GuiKind::section || k == GuiKind::box || k == GuiKind::page)
                    out += ", layout=";
                if (k == GuiKind::section || k == GuiKind::box || k == GuiKind::tabs)
                    out += ", color=, look=";
                if (k == GuiKind::section || k == GuiKind::tabs)
                    out += ", titlecolor=, titlealign=, titlesize=";
                return out;
            }

            void addDecoration(const Element& e, int parent)
            {
                const std::string& w = e.name.value;
                GuiNode n;
                n.kind = w == "spacer" ? GuiKind::spacer : (w == "divider" ? GuiKind::divider : GuiKind::label);
                n.line = e.name.line;
                for (std::size_t k = 0; k < e.args.size(); ++k)
                {
                    if (n.kind == GuiKind::label && k == 0)
                        n.text = text(e.args[k], kMaxGuiText, "label");
                    else
                        note(e.args[k], w + (n.kind == GuiKind::label ? " takes one \"text\"" : " takes no argument") + ": '"
                                            + e.args[k].value + "' is left out");
                }
                if (n.kind == GuiKind::label && n.text.empty())
                {
                    note(e.name, "label takes a \"text\": label \"FILTER\"");
                    return;
                }
                for (const Prop& p : e.props)
                {
                    if (layoutProp(n, p, false))
                        continue;
                    if (p.key == "color" && n.kind != GuiKind::spacer)
                        n.color = color(p.value);
                    else if (p.key == "size" && n.kind == GuiKind::label)
                    {
                        if (p.value.is(Token::Kind::number))
                            n.textPx = number(p, 10.0f, 44.0f, 11.0f);
                        else if (const int k = choice(p, { "small", "normal", "large", "title" }); k >= 0)
                            n.textSize = static_cast<GuiText>(k);
                    }
                    else if (p.key == "align" && n.kind == GuiKind::label)
                    {
                        if (const int k = choice(p, { "left", "center", "right" }); k >= 0)
                            n.textAlign = k;
                    }
                    else if (p.key == "thickness" && n.kind == GuiKind::divider)
                        n.style.thickness = number(p, 1.0f, 12.0f, 1.0f);
                    else
                        unknown(p, w, std::string(n.kind == GuiKind::label ? "size=, align=, color=, " : n.kind == GuiKind::divider ? "color=, thickness=, " : "")
                                          + "grow=, width=, height=, margin=, self=");
                }
                if (n.kind == GuiKind::spacer && n.width == 0.0f && n.height == 0.0f && n.grow == 0.0f)
                    n.grow = 1.0f;                       // a spacer without a size takes the room there is
                push(std::move(n), parent);
            }

            // label=, color=, and for a float unit=, decimals=, scale=, taper=; false when the key is none of them.
            bool controlProp(GuiNode& n, int slot, const Prop& p, bool real)
            {
                Control& c = controls_[static_cast<std::size_t>(slot)];
                if (p.key == "label")
                {
                    spec_.labels[static_cast<std::size_t>(slot)] = text(p.value, kMaxDialLabel, "label");
                    return true;
                }
                if (p.key == "color")
                {
                    n.color = color(p.value);
                    spec_.colors[static_cast<std::size_t>(slot)] = n.color;
                    return true;
                }
                if (p.key != "unit" && p.key != "decimals" && p.key != "scale" && p.key != "taper")
                    return false;
                if (!real)
                {
                    if (p.own)
                        note(p.value, std::string(c.name) + " is " + typeName(c) + ": " + p.key + "= is a float's");
                    return true;
                }
                if (p.key == "unit")
                {
                    const std::string u = text(p.value, sizeof c.unit - 1, "unit");
                    std::snprintf(c.unit, sizeof c.unit, "%s", u.c_str());
                }
                else if (p.key == "decimals")
                    c.decimals = static_cast<int8_t>(number(p, 0.0f, 4.0f, -1.0f));
                else if (p.key == "scale")
                {
                    const float s = number(p, -1.0e6f, 1.0e6f, 1.0f);
                    if (s == 0.0f)
                        note(p.value, "scale=0 would show every value as 0: 1 instead");
                    c.scale = s == 0.0f ? 1.0f : s;
                }
                else if (const int k = choice(p, { "linear", "log" }); k >= 0)
                {
                    Control probe = c;
                    probe.taper = static_cast<ControlTaper>(k);
                    if (probe.taper == ControlTaper::log && !logTaperFits(probe))
                        note(p.value, std::string("a log taper needs a range above 0, and ") + c.name + "'s starts at "
                                          + trim(c.min) + ": it stays linear");
                    else
                        c.taper = probe.taper;
                }
                return true;
            }

            // How a knob or a slider looks (GuiStyle), and its live= field.
            bool knobProp(GuiNode& n, const Prop& p)
            {
                GuiStyle& st = n.style;
                const bool knob = n.kind == GuiKind::knob;
                if ((p.key == "look" || p.key == "style") && knob)
                {
                    if (const int k = choice(p, { "modern", "vintage", "flat", "minimal", "ring", "led", "chicken", "soft", "dot" }); k >= 0)
                    {
                        st.look = p.value.value;
                        n.vintage = st.look == "vintage";
                    }
                }
                else if (p.key == "size" && knob)
                {
                    if (p.value.is(Token::Kind::number))
                        st.px = number(p, 24.0f, 240.0f, 60.0f);
                    else if (const int k = choice(p, { "small", "medium", "large" }); k >= 0)
                    {
                        n.size = static_cast<GuiSize>(k);
                        st.px = 0.0f;
                    }
                }
                else if (p.key == "arc")
                    st.arc = color(p.value, true);
                else if (p.key == "track")
                    st.track = color(p.value, true);
                else if (p.key == "cap")
                    st.cap = color(p.value, true);
                else if (p.key == "pointer" && knob)
                {
                    if (const int k = choice(p, { "line", "dot", "triangle", "none" }); k >= 0)
                        st.pointer = k;
                }
                else if (p.key == "pointercolor" && knob)
                    st.pointerColor = color(p.value);
                else if (p.key == "thickness")
                    st.thickness = number(p, 1.0f, 24.0f, 2.0f);
                else if (p.key == "sweep" && knob)
                    st.sweep = number(p, 60.0f, 360.0f, 270.0f);
                else if (p.key == "ticks" && knob)
                    st.tickCount = static_cast<int>(number(p, 0.0f, 61.0f, 0.0f));
                else if (p.key == "tickcolor" && knob)
                    st.ticks = color(p.value);
                else if (p.key == "bipolar")
                    st.bipolar = flag(p);
                else if (p.key == "labelpos")
                {
                    if (const int k = choice(p, { "top", "bottom", "none" }); k >= 0)
                        st.labelPos = k;
                }
                else if (p.key == "value")
                {
                    if (const int k = choice(p, { "always", "hover", "never" }); k >= 0)
                        st.valueShow = k;
                }
                else if (p.key == "valuecolor")
                    st.value = color(p.value);
                else if (p.key == "labelcolor")
                    st.label = color(p.value);
                else if (p.key == "shadow")
                    st.shadow = flag(p) ? 1 : 0;
                else if (p.key == "live")
                {
                    if (!p.value.is(Token::Kind::name))
                        note(p.value, "live= takes a field's name");
                    else
                        n.live = hiddenTap(p.value, p.value.value, "live=");
                }
                else
                    return false;
                return true;
            }

            // An int's or a bool's value names, from the texts after its name.
            void options(const Element& e, std::size_t from, int slot, const std::string& w)
            {
                const Control& c = controls_[static_cast<std::size_t>(slot)];
                const int values = valuesOf(c);
                std::vector<std::string> names;
                for (std::size_t k = from; k < e.args.size(); ++k)
                {
                    const Token& a = e.args[k];
                    if (a.is(Token::Kind::name) && (a.value == "horizontal" || a.value == "vertical"))
                        continue;
                    if (!a.is(Token::Kind::text))
                    {
                        note(a, w + " takes \"texts\" for its values' names: '" + a.value + "' is left out");
                        continue;
                    }
                    if (values == 0)
                    {
                        note(a, std::string(c.name) + " is a float: names are an int's or a bool's values'");
                        return;
                    }
                    std::string n = a.value;
                    if (n.size() > kMaxOptionLabel)
                    {
                        note(a, "the name \"" + n + "\" is cut to " + std::to_string(kMaxOptionLabel) + " characters");
                        n.resize(kMaxOptionLabel);
                    }
                    names.push_back(std::move(n));
                }
                if (names.empty())
                    return;
                if (static_cast<int>(names.size()) != values)
                {
                    note(e.name, std::string(c.name) + " takes " + std::to_string(values) + " values, and " + w + " names "
                                     + std::to_string(names.size()) + (static_cast<int>(names.size()) > values
                                                                           ? ": the extra ones are left out"
                                                                           : ": the others keep their numbers"));
                    if (static_cast<int>(names.size()) > values)
                        names.resize(static_cast<std::size_t>(values));
                }
                spec_.options[static_cast<std::size_t>(slot)] = std::move(names);
            }

            void addControl(const Element& e, int parent)
            {
                const std::string& w = e.name.value;
                if (e.args.empty())
                {
                    note(e.name, w + " takes a parameter's name: " + w + " cutoff");
                    return;
                }
                const int slot = param(e.args[0], w);
                if (slot < 0)
                    return;
                const Control& c = controls_[static_cast<std::size_t>(slot)];
                const int values = valuesOf(c);
                GuiNode n;
                n.line = e.name.line;
                n.slot = slot;
                if (w == "knob")
                    n.kind = GuiKind::knob;
                else if (w == "slider")
                {
                    n.kind = GuiKind::slider;
                    if (c.type == ControlType::toggle)
                    {
                        note(e.args[0], std::string(c.name) + " is a bool: a switch or buttons suit it, not a slider");
                        return;
                    }
                }
                else if (w == "switch")
                {
                    n.kind = GuiKind::toggle;
                    if (values != 2)
                    {
                        note(e.args[0], "a switch takes a bool, or an int of two values: " + std::string(c.name) + " is "
                                            + typeName(c) + (values > 0 ? " of " + std::to_string(values) + " values" : ""));
                        return;
                    }
                }
                else if (w == "buttons")
                {
                    n.kind = GuiKind::buttons;
                    if (values < 2 || values > kMaxOptions)
                    {
                        note(e.args[0], "buttons take a bool, or an int of 2 to " + std::to_string(kMaxOptions) + " values: "
                                            + std::string(c.name) + " is " + typeName(c)
                                            + (values > 0 ? " of " + std::to_string(values) + " values" : ""));
                        return;
                    }
                }
                else
                {
                    n.kind = GuiKind::menu;
                    if (values < 2 || values > kMaxMenuOptions)
                    {
                        note(e.args[0], "a menu takes a bool, or an int of 2 to " + std::to_string(kMaxMenuOptions)
                                            + " values: " + std::string(c.name) + " is " + typeName(c)
                                            + (values > 0 ? " of " + std::to_string(values) + " values" : ""));
                        return;
                    }
                }
                // After the name: horizontal or vertical (a slider, buttons), bipolar (a knob, a slider), and the
                // values' names (switch, buttons, menu).
                const auto isFlag = [&](const Token& a) {
                    return a.is(Token::Kind::name)
                        && (((a.value == "horizontal" || a.value == "vertical") && (n.kind == GuiKind::slider || n.kind == GuiKind::buttons))
                            || (a.value == "bipolar" && (n.kind == GuiKind::knob || n.kind == GuiKind::slider)));
                };
                for (std::size_t k = 1; k < e.args.size(); ++k)
                    if (isFlag(e.args[k]))
                    {
                        if (e.args[k].value == "bipolar")
                            n.style.bipolar = true;
                        else
                            n.vertical = e.args[k].value == "vertical";
                    }
                if (n.kind == GuiKind::toggle || n.kind == GuiKind::buttons || n.kind == GuiKind::menu)
                    options(e, 1, slot, w);
                else
                    for (std::size_t k = 1; k < e.args.size(); ++k)
                        if (!isFlag(e.args[k]))
                            note(e.args[k], w + " takes one parameter" + (n.kind == GuiKind::slider ? " (then horizontal, vertical or bipolar)" : " (then bipolar)")
                                                + ": '" + e.args[k].value + "' is left out");
                const bool real = c.type == ControlType::real;
                for (const Prop& p : e.props)
                {
                    if (controlProp(n, slot, p, real) || commonProp(n, p))
                        continue;
                    if ((n.kind == GuiKind::knob || n.kind == GuiKind::slider) && knobProp(n, p))
                        continue;
                    if (n.kind == GuiKind::slider && p.key == "length")
                        (n.vertical ? n.height : n.width) = number(p, 40.0f, 1000.0f, 0.0f);
                    else if (n.kind == GuiKind::menu && p.key == "width")
                        n.width = number(p, 40.0f, 600.0f, 0.0f);
                    else if (p.key == "grow")
                        n.grow = number(p, 0.0f, 100.0f, 0.0f);
                    else
                        unknown(p, w, controlProps(n.kind));
                }
                // What the pragmas' drawing knows of it (the host's value names come from `options`).
                DialStyle style = DialStyle::knob;
                if (n.kind == GuiKind::knob && n.vintage)
                    style = DialStyle::vintage;
                else if (n.kind == GuiKind::slider)
                    style = DialStyle::fader;
                else if (n.kind == GuiKind::toggle)
                    style = DialStyle::toggle;
                else if (n.kind == GuiKind::buttons)
                    style = DialStyle::buttons;
                spec_.styles[static_cast<std::size_t>(slot)] = style;
                spec_.small[static_cast<std::size_t>(slot)] = n.kind == GuiKind::knob && n.size == GuiSize::small && n.style.px == 0.0f;
                placed_[static_cast<std::size_t>(slot)] = true;
                push(std::move(n), parent);
            }

            static std::string controlProps(GuiKind k)
            {
                const std::string common = "label=, color=, grow=, margin=, self=";
                if (k == GuiKind::knob)
                    return common + ", size= (small, medium, large or px), look= (" + kKnobLooks + "), arc=, track=, cap=, "
                                    "pointer=, pointercolor=, thickness=, sweep=, ticks=, tickcolor=, bipolar=, labelpos=, "
                                    "value=, valuecolor=, labelcolor=, shadow=, live=, unit=, decimals=, scale=, taper=";
                if (k == GuiKind::slider)
                    return common + ", length=, arc=, track=, cap=, thickness=, bipolar=, labelpos=, value=, valuecolor=, "
                                    "labelcolor=, shadow=, live=, unit=, decimals=, scale=, taper=";
                if (k == GuiKind::menu)
                    return common + ", width=";
                return common;
            }

            void addXy(const Element& e, int parent)
            {
                if (e.args.size() < 2)
                {
                    note(e.name, "xy takes two parameters, x then y: xy cutoff resonance");
                    return;
                }
                const int x = param(e.args[0], "xy");
                if (x < 0)
                    return;
                placed_[static_cast<std::size_t>(x)] = true;   // so that y may not be x
                const int y = param(e.args[1], "xy");
                placed_[static_cast<std::size_t>(x)] = false;
                if (y < 0)
                    return;
                for (const int s : { x, y })
                    if (controls_[static_cast<std::size_t>(s)].type != ControlType::real)
                    {
                        note(e.name, "an xy pad moves floats: " + std::string(controls_[static_cast<std::size_t>(s)].name)
                                         + " is " + typeName(controls_[static_cast<std::size_t>(s)]));
                        return;
                    }
                for (std::size_t k = 2; k < e.args.size(); ++k)
                    note(e.args[k], "xy takes two parameters: '" + e.args[k].value + "' is left out");
                GuiNode n;
                n.kind = GuiKind::xy;
                n.line = e.name.line;
                n.slot = x;
                n.slot2 = y;
                for (const Prop& p : e.props)
                {
                    if (commonProp(n, p))
                        continue;
                    if (p.key == "size")
                        n.width = n.height = number(p, 60.0f, 600.0f, 120.0f);
                    else if (p.key == "label")
                        n.text = text(p.value, kMaxDialLabel * 2, "label");
                    else if (p.key == "color")
                        n.color = color(p.value);
                    else if (p.key == "fill")
                        n.style.fill = color(p.value, true);
                    else if (p.key == "grow")
                        n.grow = number(p, 0.0f, 100.0f, 0.0f);
                    else
                        unknown(p, "xy", "size=, label=, color=, fill=, grow=, margin=, self=");
                }
                placed_[static_cast<std::size_t>(x)] = placed_[static_cast<std::size_t>(y)] = true;
                spec_.colors[static_cast<std::size_t>(x)] = spec_.colors[static_cast<std::size_t>(y)] = n.color;
                push(std::move(n), parent);
            }

            void addEnvelope(const Element& e, int parent)
            {
                if (e.args.size() != 4)
                {
                    note(e.name, "envelope takes four parameters: attack, decay, sustain and release (it draws their "
                                 "shape; each keeps a control of its own)");
                    return;
                }
                GuiNode n;
                n.kind = GuiKind::envelope;
                n.line = e.name.line;
                for (std::size_t k = 0; k < 4; ++k)
                {
                    const Token& a = e.args[k];
                    const int slot = a.is(Token::Kind::name) ? slotNamed(a.value) : -1;
                    if (slot < 0 || controls_[static_cast<std::size_t>(slot)].type != ControlType::real)
                    {
                        note(a, slot < 0 ? "there is no parameter " + a.value
                                         : a.value + " is " + typeName(controls_[static_cast<std::size_t>(slot)])
                                               + ": an envelope draws floats");
                        return;
                    }
                    n.env[k] = slot;
                }
                for (const Prop& p : e.props)
                {
                    if (layoutProp(n, p, false))
                        continue;
                    if (p.key == "label")
                        n.text = text(p.value, kMaxDialLabel * 2, "label");
                    else if (p.key == "color")
                        n.color = color(p.value);
                    else if (p.key == "fill")
                        n.style.fill = color(p.value, true);
                    else
                        unknown(p, "envelope", "label=, color=, fill=, width=, height=, grow=, margin=, self=");
                }
                push(std::move(n), parent);
            }

            // keyboard from=48 keys=25 (or octaves=2): an instrument's keys, which play notes.
            void addKeyboard(const Element& e, int parent)
            {
                if (!instrument_)
                {
                    note(e.name, "an effect plays no notes: a keyboard is an instrument's");
                    return;
                }
                for (const Token& a : e.args)
                    note(a, "keyboard takes from=, keys= or octaves=, and color=: '" + a.value + "' is left out");
                GuiNode n;
                n.kind = GuiKind::keyboard;
                n.line = e.name.line;
                for (const Prop& p : e.props)
                {
                    if (layoutProp(n, p, false))
                        continue;
                    if (p.key == "from")
                        n.from = static_cast<int>(number(p, 0.0f, 108.0f, 48.0f));
                    else if (p.key == "keys")
                        n.keys = static_cast<int>(number(p, 12.0f, 88.0f, 25.0f));
                    else if (p.key == "octaves")
                        n.keys = 12 * static_cast<int>(number(p, 1.0f, 7.0f, 2.0f)) + 1;
                    else if (p.key == "color")
                        n.color = color(p.value);
                    else
                        unknown(p, "keyboard", "from=, keys=, octaves=, color=, width=, height=, grow=, margin=, self=");
                }
                if (n.from + n.keys > 128)
                    n.keys = 128 - n.from;
                push(std::move(n), parent);
            }

            // shader "glow.sc" inputs="cutoff env out": a fragment shader of the program's own (a file among its tabs).
            void addShader(const Element& e, int parent)
            {
                if (e.args.empty() || !e.args[0].is(Token::Kind::text))
                {
                    note(e.name, "shader takes the \"name\" of its file (a tab, .sc): shader \"glow.sc\"");
                    return;
                }
                GuiNode n;
                n.kind = GuiKind::shader;
                n.line = e.name.line;
                n.file = e.args[0].value;
                std::vector<std::string> names;
                if (n.file.size() < 4 || n.file.substr(n.file.size() - 3) != ".sc")
                    note(e.args[0], "a shader's file ends in .sc: \"" + n.file + "\" is read as one all the same");
                if (std::find(fileNames_.begin(), fileNames_.end(), n.file) == fileNames_.end())
                    note(e.args[0], "there is no file " + n.file + " among the program's: write it beside the program "
                                    "(it draws nothing until then)");
                for (std::size_t k = 1; k < e.args.size(); ++k)
                    note(e.args[k], "shader takes one file: '" + e.args[k].value + "' is left out (inputs=\"...\")");
                for (const Prop& p : e.props)
                {
                    if (layoutProp(n, p, false))
                        continue;
                    if (p.key == "inputs")
                    {
                        // Parameters (their 0..1), fields (their values), in and out (their levels), up to eight.
                        names.clear();
                        std::size_t k = 0, at = 0;
                        const std::string& all = p.value.value;
                        while (at < all.size())
                        {
                            const std::size_t end = all.find(' ', at);
                            const std::string one = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
                            at = end == std::string::npos ? all.size() : end + 1;
                            if (one.empty())
                                continue;
                            if (k >= n.inputs.size())
                            {
                                note(p.value, "a shader takes eight inputs at most: " + one + " is left out");
                                break;
                            }
                            const std::size_t before = k;
                            if (one == "in" || one == "out")
                            {
                                if (one == "in" && !instrument_)
                                    n.inputs[k++] = one;
                                else if (one == "out")
                                    n.inputs[k++] = one;
                                else
                                    note(p.value, "an instrument has no input: in is left out");
                            }
                            else if (const int slot = slotNamed(one); slot >= 0)
                                n.inputs[k++] = "param:" + std::to_string(slot);
                            else if (const int d = hiddenTap(p.value, one, "a shader's input"); d >= 0)
                                n.inputs[k++] = "field:" + std::to_string(d);
                            if (k > before)
                                names.push_back(one);
                        }
                    }
                    else
                        unknown(p, "shader", "inputs=, width=, height=, grow=, margin=, self=");
                }
                if (spec_.shaders.size() >= kMaxGuiShaders)
                {
                    note(e.name, "a panel draws " + std::to_string(kMaxGuiShaders) + " shaders at most: this one is left out");
                    return;
                }
                n.shader = static_cast<int>(spec_.shaders.size());
                spec_.shaders.push_back({ n.file, std::move(names), {}, 0 });
                push(std::move(n), parent);
            }

            void addDisplay(const Element& e, int parent)
            {
                const std::string& w = e.name.value;
                if (e.args.empty() || !e.args[0].is(Token::Kind::name))
                {
                    note(e.name, w + " takes a source: in, out, or a field of the main module (" + w + " out)");
                    return;
                }
                PanelDisplay d;
                d.kind = displayKind(w);
                d.source = e.args[0].value;
                d.name = d.source;
                for (std::size_t k = 1; k < e.args.size(); ++k)
                    note(e.args[k], w + " takes one source: '" + e.args[k].value + "' is left out (label=, min=, max=)");
                GuiNode n;
                n.kind = GuiKind::display;
                n.line = e.name.line;
                bool lo = false, hi = false;
                const bool sized = d.kind == DisplayKind::scope || d.kind == DisplayKind::spectrum;
                for (const Prop& p : e.props)
                {
                    if (commonProp(n, p))
                        continue;
                    if (p.key == "label")
                        d.label = text(p.value, kMaxDialLabel, "label");
                    else if (p.key == "color")
                        d.color = n.color = color(p.value);
                    else if (p.key == "min")
                    {
                        d.lo = number(p, -1.0e9f, 1.0e9f, 0.0f);
                        lo = true;
                    }
                    else if (p.key == "max")
                    {
                        d.hi = number(p, -1.0e9f, 1.0e9f, 1.0f);
                        hi = true;
                    }
                    else if (sized && (p.key == "width" || p.key == "height"))
                        layoutProp(n, p, false);
                    else if (p.key == "grow")
                        n.grow = number(p, 0.0f, 100.0f, 0.0f);
                    else
                        unknown(p, w, "label=, color=, min=, max=, grow=, margin=, self=" + std::string(sized ? ", width=, height=" : ""));
                }
                if (d.label.empty())                     // its source's name ("OUT"), not the one it is known by
                    d.label = d.source.substr(0, kMaxDialLabel);
                if ((lo || hi) && !(d.hi > d.lo))
                {
                    note(e.name, "min= must be under max=: " + d.source + " shows 0 to 1");
                    d.lo = 0.0f;
                    d.hi = 1.0f;
                }
                if (d.source == "in" && instrument_)
                {
                    note(e.args[0], "an instrument has no input: in shows nothing");
                    return;
                }
                if (d.kind == DisplayKind::spectrum && !d.audio())
                {
                    note(e.args[0], "a spectrum analyses in or out, not a field");
                    return;
                }
                if (!d.audio() && !fieldOk(e.args[0], d.source, "a display"))
                    return;
                if (static_cast<int>(spec_.displays.size()) >= kMaxDisplays)
                {
                    note(e.name, "a panel shows " + std::to_string(kMaxDisplays) + " displays at most: this " + w
                                     + " is left out");
                    return;
                }
                int same = 0;
                for (const PanelDisplay& x : spec_.displays)
                    same += x.source == d.source && !x.hidden ? 1 : 0;
                if (same > 0)
                    d.name = d.source + std::to_string(same + 1);
                if (!d.audio())
                    d.tap = taps_++;
                n.display = static_cast<int>(spec_.displays.size());
                spec_.displays.push_back(std::move(d));
                push(std::move(n), parent);
            }

            const SourceFile& file_;
            Controls& controls_;
            const std::vector<PanelField>& fields_;
            bool instrument_;
            const std::vector<std::string>& fileNames_;
            std::string& notes_;
            PanelSpec spec_;
            std::array<bool, kMaxControls> placed_{};
            std::map<std::string, std::vector<Prop>> classes_, defaults_;
            std::set<std::string> said_;
            int taps_ = 0;
            bool panelSeen_ = false, full_ = false, quiet_ = false;
        };
    }

    bool isGuiFile(std::string_view name)
    {
        return name.size() > 4 && name.substr(name.size() - 4) == ".gui";
    }

    PanelSpec readGuiSpec(const SourceFile& gui, Controls& controls, const std::vector<PanelField>& fields,
                          bool instrument, const std::vector<std::string>& fileNames, std::string& notes)
    {
        const std::vector<Token> tokens = lex(gui.text, gui.name, notes);
        Parser parser(tokens, gui.name, notes);
        std::vector<Element> top = parser.list(0);
        parser.strays();
        for (Element& e : parser.takeExtra())
            top.push_back(std::move(e));
        Builder b(gui, controls, fields, instrument, fileNames, notes);
        return b.build(top);
    }
}
