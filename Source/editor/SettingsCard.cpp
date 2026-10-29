// Source/editor/SettingsCard.cpp: see SettingsCard.h.
#include "editor/SettingsCard.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 600.0f;
        constexpr float kPad = 24.0f;
        constexpr float kHeaderH = 84.0f;                // the title and its line, over the body
        constexpr float kFooterH = 66.0f;                // Done, under it
        constexpr float kButtonH = 32.0f;
        constexpr float kLabelW = 108.0f;                // a row's label, before its controls
        constexpr float kRowH = 28.0f;                   // a pill's, a switch's
        constexpr float kRowStep = 36.0f;
        constexpr float kFlowStep = 34.0f;               // switches that wrap: the next line
        constexpr float kTextRow = 22.0f;                // a value's or a note's row
        constexpr float kHeadingH = 26.0f;               // a section's heading and the gap under it
        constexpr float kSectionGap = 14.0f;
        constexpr float kDeviceW = 330.0f;               // a device's pill, at most
        constexpr float kSwitchW = 30.0f, kSwitchH = 17.0f;
        constexpr uint32_t kLineIds = SettingsCard::kA11yBase + 0xF000u;
        // Why an effect's input starts muted (JUCE's guard), under its switch.
        constexpr const char* kMuteWhy = "A loop through the speakers would feed back: unmute with headphones on.";

        float textW(const std::string& s, const funkgui::TextStyle& st)
        {
            return funkgui::text::width(funkgui::FontService::get().atlas(), s.c_str(), st);
        }

        // `s` in lines of at most `maxW` px of `st`, words kept whole while they fit. A run of spaces, tabs or CRs
        // is one space, and a line break (JUCE's "Can't open the audio device!\n\nThis may be...") starts a new
        // line, never an empty one: the font has no glyph for either.
        std::vector<std::string> wrapped(const std::string& s, const funkgui::TextStyle& st, float maxW)
        {
            std::vector<std::string> out;
            std::string line;
            std::size_t at = 0;
            while (at < s.size())
            {
                std::size_t end = s.find_first_of(" \t\r\n", at);
                if (end == std::string::npos)
                    end = s.size();
                const std::string word = s.substr(at, end - at);
                if (!word.empty())
                {
                    const std::string longer = line.empty() ? word : line + " " + word;
                    if (!line.empty() && textW(longer, st) > maxW)
                    {
                        out.push_back(line);
                        line = word;
                    }
                    else
                        line = longer;
                }
                if (end < s.size() && s[end] == '\n' && !line.empty())
                {
                    out.push_back(line);
                    line.clear();
                }
                at = end + 1;
            }
            if (!line.empty())
                out.push_back(line);
            return out;
        }

        std::string rateLabel(double rate)
        {
            char b[32];
            std::snprintf(b, sizeof b, "%ld Hz", std::lround(rate));
            return b;
        }

        std::string bufferLabel(int samples, double ms)
        {
            char b[48];
            std::snprintf(b, sizeof b, "%d samples (%.1f ms)", samples, ms);
            return b;
        }

        // The latency the device reports, in and out (each with its buffer, as CoreAudio counts it).
        std::string latencyText(const DeviceSetup& s)
        {
            char b[96];
            if (s.hasInput)
                std::snprintf(b, sizeof b, "%.1f ms through: input %.1f ms, output %.1f ms", s.inputLatencyMs + s.outputLatencyMs,
                              s.inputLatencyMs, s.outputLatencyMs);
            else
                std::snprintf(b, sizeof b, "%.1f ms output", s.outputLatencyMs);
            return b;
        }

        // A pill's width for its label (a chevron when a menu opens, an icon before it).
        float pillW(const std::string& label, bool menu, bool icon)
        {
            return std::ceil(14.0f + (icon ? 18.0f : 0.0f) + textW(label, chat::kSmall) + (menu ? 34.0f : 14.0f));
        }

        bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
        {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        }

        uint32_t idOf(SettingsCard::Act act, int index)
        {
            return SettingsCard::kA11yBase + 0x100u * (static_cast<uint32_t>(act) + 1u) + static_cast<uint32_t>(index);
        }

        const char* sectionName(DeviceSection s)
        {
            switch (s)
            {
                case DeviceSection::output: return "Output";
                case DeviceSection::input:  return "Input";
                case DeviceSection::audio:  return "Audio";
                case DeviceSection::midi:   return "MIDI";
            }
            return "";
        }
    }

    bool SettingsCard::open()
    {
        sync();
        if (!has_)
            return false;
        open_ = true;
        focus_ = {};
        ring_ = false;
        scroll_ = 0.0f;
        ++revision_;
        return true;
    }

    void SettingsCard::close()
    {
        open_ = false;
        focus_ = {};
        ++revision_;
    }

    bool SettingsCard::sync()
    {
        DevicePort* d = port_.devices();
        if (d == nullptr)
        {
            if (!has_ && !open_)
                return false;
            has_ = open_ = false;                        // the window let go of its devices: no card
            setup_ = {};
            ++revision_;
            return true;
        }
        const uint64_t serial = d->serial();
        if (has_ && serial == seen_)
            return false;
        setup_ = d->setup();
        seen_ = serial;
        has_ = true;
        ++revision_;
        return true;
    }

    void SettingsCard::changed()
    {
        sync();
        ++revision_;
    }

    SettingsCard::Places SettingsCard::places(float width, float height) const
    {
        Places at;
        const DeviceSetup& s = setup_;
        const float w = std::min(kBoxW, width - 32.0f);
        const float inner = w - 2.0f * kPad;
        const float ctrlX = kLabelW, ctrlW = inner - kLabelW;

        // The sections, from the body's top (y 0), its left (x 0); placed on the card below.
        float y = 0.0f;
        const auto line = [&](Line::Kind kind, std::string text, funkgui::Rect r) { at.lines.push_back({ kind, std::move(text), r }); };
        const auto heading = [&](const char* title) {
            if (y > 0.0f)
                y += kSectionGap;
            line(Line::Kind::heading, title, { 0.0f, y, inner, 16.0f });
            y += kHeadingH;
        };
        const auto label = [&](const char* text) { line(Line::Kind::label, text, { 0.0f, y, kLabelW - 10.0f, kRowH }); };
        const auto control = [&](Act act, int index, funkgui::Rect r, std::string shown, std::string title, bool enabled) -> Control& {
            Control c;
            c.act = act;
            c.index = index;
            c.r = r;
            c.label = std::move(shown);
            c.title = std::move(title);
            c.enabled = enabled;
            c.menu = act == Act::type || act == Act::output || act == Act::input || act == Act::rate || act == Act::buffer;
            c.toggle = act == Act::outputPair || act == Act::inputPair || act == Act::mute || act == Act::midi;
            c.id = idOf(act, index);
            at.controls.push_back(std::move(c));
            return at.controls.back();
        };
        // A switch for each, as many in a row as fit (wrapping under the first), from the controls' column.
        const auto switches = [&](Act act, const std::vector<ChannelPair>& pairs, const char* what) {
            float x = ctrlX;
            for (std::size_t i = 0; i < pairs.size(); ++i)
            {
                const std::string name = asciiText(pairs[i].name);
                const float sw = std::ceil(kSwitchW + 10.0f + textW(name, chat::kSmall) + 8.0f);
                if (x + sw > inner && x > ctrlX)
                {
                    x = ctrlX;
                    y += kFlowStep;
                }
                control(act, static_cast<int>(i), { x, y, std::min(sw, inner - x), kRowH }, name,
                        std::string(what) + " channels " + pairs[i].name, true)
                    .on = pairs[i].on;
                x += sw + 18.0f;
            }
            y += kRowStep;
        };
        const auto errors = [&](DeviceSection section) {
            if (s.error.empty() || s.errorOf != section)
                return;
            for (std::string& l : wrapped(asciiText(s.error), chat::kSmall, inner))
            {
                line(Line::Kind::error, std::move(l), { 0.0f, y, inner, 16.0f });
                y += 16.0f;
            }
            y += 6.0f;
        };

        // OUTPUT: the device, Test; its pairs when it has more than one.
        heading("OUTPUT");
        {
            label("Device");
            const float testW = pillW("Test", false, true);
            const float devW = std::max(120.0f, std::min(kDeviceW, ctrlW - testW - 8.0f));
            control(Act::output, 0, { ctrlX, y, devW, kRowH }, s.output.empty() ? "None" : asciiText(s.output), "Output device",
                    !s.outputs.empty());
            control(Act::test, 0, { ctrlX + devW + 8.0f, y, testW, kRowH }, "Test", "Play a test tone", s.open && !s.output.empty());
            y += kRowStep;
            if (s.outputPairs.size() > 1)
            {
                label("Channels");
                switches(Act::outputPair, s.outputPairs, "Output");
            }
            errors(DeviceSection::output);
        }

        // INPUT (an effect's): the device, its pairs, the mute and why.
        if (s.hasInput)
        {
            heading("INPUT");
            label("Device");
            if (s.separateInputs)
                control(Act::input, 0, { ctrlX, y, std::max(120.0f, std::min(kDeviceW, ctrlW)), kRowH },
                        s.input.empty() ? "None" : asciiText(s.input), "Input device", true);
            else
                line(Line::Kind::value, "The output device's own", { ctrlX, y, ctrlW, kRowH });
            y += kRowStep;
            if (s.inputPairs.size() > 1)
            {
                label("Channels");
                switches(Act::inputPair, s.inputPairs, "Input");
            }
            if (s.muteApplies)
            {
                label("Feedback");
                const std::string mute = "Mute input";
                control(Act::mute, 0, { ctrlX, y, std::ceil(kSwitchW + 10.0f + textW(mute, chat::kSmall) + 8.0f), kRowH }, mute,
                        "Mute input", true)
                    .on = s.muted;
                y += kRowH + 2.0f;
                for (std::string& l : wrapped(kMuteWhy, chat::kSmall, ctrlW))
                {
                    line(Line::Kind::note, std::move(l), { ctrlX, y, ctrlW, 16.0f });
                    y += 16.0f;
                }
                y += kRowStep - kRowH - 2.0f;
            }
            errors(DeviceSection::input);
        }

        // AUDIO: the type (when there are several), the rate, the buffer, the latency.
        heading("AUDIO");
        if (s.types.size() > 1)
        {
            label("Device type");
            control(Act::type, 0, { ctrlX, y, std::min(ctrlW, pillW(asciiText(s.type), true, false)), kRowH }, asciiText(s.type),
                    "Device type", true);
            y += kRowStep;
        }
        if (s.open)
        {
            label("Sample rate");
            const std::string rate = rateLabel(s.rate);
            control(Act::rate, 0, { ctrlX, y, std::min(ctrlW, std::max(130.0f, pillW(rate, true, false))), kRowH }, rate,
                    "Sample rate", !s.rates.empty());
            y += kRowStep;
            label("Buffer size");
            const std::string buffer = bufferLabel(s.buffer, s.bufferMs);
            control(Act::buffer, 0, { ctrlX, y, std::min(ctrlW, pillW(buffer, true, false)), kRowH }, buffer, "Buffer size",
                    !s.buffers.empty());
            y += kRowStep;
            label("Latency");
            line(Line::Kind::value, latencyText(s), { ctrlX, y, ctrlW, kRowH });
            y += kRowStep;
        }
        else
        {
            line(Line::Kind::value, "No audio device is open.", { 0.0f, y, inner, kTextRow });
            y += kTextRow + 8.0f;
        }
        errors(DeviceSection::audio);

        // MIDI (an instrument's): each input with a switch.
        if (s.takesMidi)
        {
            heading("MIDI");
            if (s.midi.empty())
            {
                line(Line::Kind::value, "No MIDI inputs", { 0.0f, y, inner, kTextRow });
                y += kTextRow + 8.0f;
            }
            else
            {
                label("Inputs");
                for (std::size_t i = 0; i < s.midi.size(); ++i)
                {
                    const std::string name = asciiText(s.midi[i].name);
                    control(Act::midi, static_cast<int>(i), { ctrlX, y, std::min(ctrlW, std::ceil(kSwitchW + 10.0f + textW(name, chat::kSmall) + 8.0f)), kRowH },
                            name, "MIDI input " + s.midi[i].name, true)
                        .on = s.midi[i].on;
                    y += i + 1 < s.midi.size() ? kFlowStep : kRowStep;
                }
            }
            errors(DeviceSection::midi);
        }
        for (const Control& c : at.controls)                // as tall as what it holds (a row's step after the last
            at.contentH = std::max(at.contentH, c.r.bottom());   // is no part of it)
        for (const Line& l : at.lines)
            at.contentH = std::max(at.contentH, l.r.bottom());
        at.contentH = std::ceil(at.contentH);

        // The card: as tall as its sections, within the window; the body scrolls past that.
        const float h = std::min(height - 32.0f, kHeaderH + at.contentH + kFooterH);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        at.body = { at.box.x + kPad, at.box.y + kHeaderH, inner, h - kHeaderH - kFooterH };
        at.maxScroll = std::max(0.0f, at.contentH - at.body.h);
        at.scroll = std::clamp(scroll_, 0.0f, at.maxScroll);
        const float dx = at.body.x, dy = at.body.y - at.scroll;
        for (Control& c : at.controls)
        {
            c.r.x += dx;
            c.r.y += dy;
        }
        for (Line& l : at.lines)
        {
            l.r.x += dx;
            l.r.y += dy;
        }
        control(Act::done, 0, { at.box.right() - kPad - 96.0f, at.box.bottom() - 22.0f - kButtonH, 96.0f, kButtonH }, "Done",
                "Done", true);
        return at;
    }

    int SettingsCard::focusIndex(const Places& at) const
    {
        for (std::size_t i = 0; i < at.controls.size(); ++i)
            if (at.controls[i].act == focus_.act && at.controls[i].index == focus_.index)
                return static_cast<int>(i);
        return -1;
    }

    std::optional<funkgui::Point> SettingsCard::centreOf(Act act, int index, float width, float height) const
    {
        const Places at = places(width, height);
        for (const Control& c : at.controls)
            if (c.act == act && c.index == index)
            {
                const funkgui::Point p{ c.r.centreX(), c.r.centreY() };
                if (c.act != Act::done && !at.body.contains(p))
                    return std::nullopt;
                return p;
            }
        return std::nullopt;
    }

    std::optional<SettingsCard::Control> SettingsCard::focused(float width, float height) const
    {
        const Places at = places(width, height);
        const int i = focusIndex(at);
        if (i < 0)
            return std::nullopt;
        return at.controls[static_cast<std::size_t>(i)];
    }

    void SettingsCard::act(const Control& c, funkgui::HostServices* host)
    {
        if (!c.enabled)
            return;
        if (c.act == Act::done)
        {
            close();
            return;
        }
        DevicePort* d = port_.devices();
        if (d == nullptr)
            return;
        switch (c.act)
        {
            case Act::type:
            case Act::output:
            case Act::input:
            case Act::rate:
            case Act::buffer:     openMenu(c, host); return;
            case Act::test:       d->playTestTone(); break;
            case Act::outputPair: d->switchPair(false, c.index, !c.on); break;
            case Act::inputPair:  d->switchPair(true, c.index, !c.on); break;
            case Act::mute:       d->setMuted(!c.on); break;
            case Act::midi:
                if (c.index >= 0 && c.index < static_cast<int>(setup_.midi.size()))
                    d->switchMidi(setup_.midi[static_cast<std::size_t>(c.index)].identifier, !c.on);
                break;
            case Act::done:       break;
        }
        changed();
    }

    // The host's menu beside the pill: the choices, the current one ticked. The choice reaches the devices when it is
    // made, if the card is still open.
    void SettingsCard::openMenu(const Control& c, funkgui::HostServices* host)
    {
        if (host == nullptr)
            return;
        constexpr int kNone = 1000;                      // the input's None
        funkgui::MenuRequest request;
        const DeviceSetup& s = setup_;
        const auto items = [&](const std::vector<std::string>& names, const std::string& current) {
            for (std::size_t i = 0; i < names.size(); ++i)
                request.items.push_back({ static_cast<int>(i) + 1, names[i], true, names[i] == current, false });
        };
        switch (c.act)
        {
            case Act::type:   items(s.types, s.type); break;
            case Act::output: items(s.outputs, s.output); break;
            case Act::input:
                request.items.push_back({ kNone, "None", true, s.input.empty(), false });
                if (!s.inputs.empty())
                    request.items.push_back({ 0, {}, true, false, true });
                items(s.inputs, s.input);
                break;
            case Act::rate:
                for (std::size_t i = 0; i < s.rates.size(); ++i)
                    request.items.push_back({ static_cast<int>(i) + 1, rateLabel(s.rates[i]), true,
                                              std::lround(s.rates[i]) == std::lround(s.rate), false });
                break;
            case Act::buffer:
                for (std::size_t i = 0; i < s.buffers.size(); ++i)
                    request.items.push_back({ static_cast<int>(i) + 1, bufferLabel(s.buffers[i].samples, s.buffers[i].ms), true,
                                              s.buffers[i].samples == s.buffer, false });
                break;
            case Act::test:
            case Act::outputPair:
            case Act::inputPair:
            case Act::mute:
            case Act::midi:
            case Act::done:
                return;
        }
        request.anchor = c.r;
        request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
        const Act act = c.act;
        const DeviceSetup choices = s;                   // what the menu lists, whatever changes meanwhile
        host->showMenu(request, [this, act, choices](int chosen) {
            DevicePort* d = port_.devices();
            if (!open_ || d == nullptr || chosen <= 0)
                return;
            const auto pick = [chosen](const auto& list) -> const auto* {
                return chosen >= 1 && chosen <= static_cast<int>(list.size()) ? &list[static_cast<std::size_t>(chosen - 1)] : nullptr;
            };
            switch (act)
            {
                case Act::type:
                    if (const auto* t = pick(choices.types))
                        d->chooseType(*t);
                    break;
                case Act::output:
                    if (const auto* o = pick(choices.outputs))
                        d->chooseOutput(*o);
                    break;
                case Act::input:
                    if (chosen == kNone)
                        d->chooseInput({});
                    else if (const auto* i = pick(choices.inputs))
                        d->chooseInput(*i);
                    break;
                case Act::rate:
                    if (const auto* r = pick(choices.rates))
                        d->chooseRate(*r);
                    break;
                case Act::buffer:
                    if (const auto* b = pick(choices.buffers))
                        d->chooseBuffer(b->samples);
                    break;
                case Act::test:
                case Act::outputPair:
                case Act::inputPair:
                case Act::mute:
                case Act::midi:
                case Act::done:
                    break;
            }
            changed();
        });
    }

    void SettingsCard::pointerDown(funkgui::Point p, float width, float height, funkgui::HostServices* host)
    {
        const Places at = places(width, height);
        ring_ = false;
        if (!at.box.contains(p))
        {
            close();                                     // a click outside the card
            return;
        }
        for (const Control& c : at.controls)
            if (c.r.contains(p) && (c.act == Act::done || at.body.contains(p)))
            {
                focus_ = { c.act, c.index };
                act(c, host);
                break;
            }
        ++revision_;
    }

    void SettingsCard::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        const Places at = places(width, height);
        if (at.maxScroll <= 0.0f)
            return;
        const float k = e.smooth ? 512.0f : 292.0f;     // px a unit of the wheel (the chat's)
        const float dy = e.reversed ? -e.dy : e.dy;
        scroll_ = std::clamp(at.scroll - dy * k, 0.0f, at.maxScroll);
        ++revision_;
    }

    void SettingsCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        using funkgui::Key;
        if (e.key == Key::escape)
        {
            close();
            return;
        }
        const Places at = places(width, height);
        const int n = static_cast<int>(at.controls.size());
        const int i = focusIndex(at);
        if (e.key == Key::tab || e.key == Key::down || e.key == Key::up)
        {
            if (n == 0)
                return;
            const bool back = e.key == Key::up || (e.key == Key::tab && e.mods.shift);
            const int next = i < 0 ? (back ? n - 1 : 0) : (i + (back ? n - 1 : 1)) % n;
            const Control& c = at.controls[static_cast<std::size_t>(next)];
            focus_ = { c.act, c.index };
            ring_ = true;
            // The body scrolled to show it.
            if (c.act != Act::done)
            {
                if (c.r.y < at.body.y)
                    scroll_ = std::max(0.0f, at.scroll - (at.body.y - c.r.y) - 4.0f);
                else if (c.r.bottom() > at.body.bottom())
                    scroll_ = std::min(at.maxScroll, at.scroll + (c.r.bottom() - at.body.bottom()) + 4.0f);
            }
            ++revision_;
            return;
        }
        if ((e.key == Key::enter || e.key == Key::space) && i >= 0)
        {
            ring_ = true;
            act(at.controls[static_cast<std::size_t>(i)], host);
            ++revision_;
        }
    }

    funkgui::Cursor SettingsCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const Control& c : at.controls)
            if (c.enabled && c.r.contains(p) && (c.act == Act::done || at.body.contains(p)))
                return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void SettingsCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                            bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));   // the panel dimmed
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)                     // a soft shadow under the card
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);

        // The title and its line.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::gear, x + 15.0f, box.y + 37.0f, 16.0f, p.accent);
        c.text("Settings", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        c.text(fitText(c, "The audio and MIDI devices this app plays through, kept for the next time it opens.", chat::kSmall,
                       box.w - 2.0f * kPad).c_str(),
               x, c.capCentreTop(box.y + 62.0f, chat::kSmall), chat::kSmall, p.soft);

        const int focus = ring_ ? focusIndex(at) : -1;
        const auto hovered = [&](const Control& k) {
            return hasPointer && k.enabled && k.r.contains(pointer) && (k.act == Act::done || at.body.contains(pointer));
        };
        const auto ring = [&](const Control& k, float radius) {
            c.rrect(k.r.x - 2.0f, k.r.y - 2.0f, k.r.w + 4.0f, k.r.h + 4.0f, radius + 2.0f, p.card.withAlpha(0.0f), 1.5f, p.accent);
        };
        {
            const funkgui::Canvas::ClipScope clip(c, { box.x + 1.0f, at.body.y, box.w - 2.0f, at.body.h });
            for (const Line& l : at.lines)
            {
                const float mid = l.r.centreY();
                switch (l.kind)
                {
                    case Line::Kind::heading:
                    {
                        const std::string t = fitText(c, l.text, chat::kSmallStrong, l.r.w);
                        c.text(t.c_str(), l.r.x, c.capCentreTop(mid, chat::kSmallStrong), chat::kSmallStrong, p.dim);
                        const float after = l.r.x + c.textWidth(t.c_str(), chat::kSmallStrong) + 10.0f;
                        c.hairlineH(after, std::round(mid), std::max(0.0f, l.r.right() - after), p.cardEdge);
                        break;
                    }
                    case Line::Kind::label:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.soft);
                        break;
                    case Line::Kind::value:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.text);
                        break;
                    case Line::Kind::note:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint);
                        break;
                    case Line::Kind::error:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.danger);
                        break;
                }
            }
            for (std::size_t i = 0; i + 1 < at.controls.size(); ++i)   // Done, the last, is outside the body
            {
                const Control& k = at.controls[i];
                const funkgui::Rect& r = k.r;
                const bool hover = hovered(k);
                const funkgui::Col ink = k.enabled ? p.text : p.faint;
                if (k.menu)
                {
                    c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.composer, 1.0f, p.composerEdge);
                    c.text(fitText(c, k.label, chat::kSmall, r.w - 38.0f).c_str(), r.x + 14.0f, c.capCentreTop(r.centreY(), chat::kSmall),
                           chat::kSmall, ink);
                    chat::drawIcon(c, chat::Icon::chevronDown, r.right() - 15.0f, r.centreY() + 0.5f, 8.0f, k.enabled ? p.soft : p.faint);
                    if (static_cast<int>(i) == focus)
                        ring(k, r.h * 0.5f);
                }
                else if (k.toggle)
                {
                    // A switch: its track (the accent when on), its knob, its name after it.
                    const float ty = r.centreY() - 0.5f * kSwitchH;
                    const funkgui::Col track = k.on ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.composer);
                    c.rrect(r.x, ty, kSwitchW, kSwitchH, 0.5f * kSwitchH, track, k.on ? 0.0f : 1.0f, p.composerEdge);
                    const float kx = k.on ? r.x + kSwitchW - 0.5f * kSwitchH : r.x + 0.5f * kSwitchH;
                    c.disc(kx, r.centreY(), 0.5f * kSwitchH - 2.5f, k.on ? p.onAccent : p.soft);
                    c.text(fitText(c, k.label, chat::kSmall, r.w - kSwitchW - 10.0f).c_str(), r.x + kSwitchW + 10.0f,
                           c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, k.on ? p.text : p.soft);
                    if (static_cast<int>(i) == focus)
                        c.rrect(r.x - 2.0f, ty - 2.0f, kSwitchW + 4.0f, kSwitchH + 4.0f, 0.5f * kSwitchH + 2.0f, p.card.withAlpha(0.0f), 1.5f,
                                p.accent);
                }
                else
                {
                    // Test: an outlined pill with its icon.
                    c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.card.withAlpha(0.0f), 1.0f, p.composerEdge);
                    chat::drawIcon(c, chat::Icon::play, r.x + 20.0f, r.centreY(), 11.0f, k.enabled ? p.soft : p.faint);
                    c.text(k.label.c_str(), r.x + 32.0f, c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall,
                           k.enabled ? (hover ? p.text : p.soft) : p.faint);
                    if (static_cast<int>(i) == focus)
                        ring(k, r.h * 0.5f);
                }
            }
        }
        // The body's scroll bar, when the window is too short for it.
        if (at.maxScroll > 0.0f)
        {
            const funkgui::Rect& b = at.body;
            const float h = std::max(24.0f, b.h * b.h / (b.h + at.maxScroll));
            const float y = b.y + (b.h - h) * at.scroll / at.maxScroll;
            c.rrect(box.right() - 9.0f, y, 3.0f, h, 1.5f, th.ink32);
            c.hairlineH(box.x + 1.0f, b.bottom() + 6.0f, box.w - 2.0f, p.cardEdge);
        }

        // Done.
        {
            const Control& k = at.controls.back();
            const funkgui::Rect& r = k.r;
            const bool hover = hovered(k);
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
            c.text("Done", r.centreX(), c.capCentreTop(r.centreY(), chat::kSmallStrong), chat::kSmallStrong, p.onAccent, funkgui::Align::centre);
            if (focus == static_cast<int>(at.controls.size()) - 1)
                ring(k, r.h * 0.5f);
        }
    }

    void SettingsCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kA11yBase;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Settings";
        box.description = "The audio and MIDI devices";
        out.push_back(box);
        uint32_t n = 0;
        for (const Line& l : at.lines)
        {
            if (l.kind == Line::Kind::label)
                continue;                                // each control's title says it
            funkgui::A11yItem it;
            it.id = kLineIds + n++;
            it.parent = kA11yBase;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = l.r;
            it.title = l.kind == Line::Kind::error ? std::string(sectionName(setup_.errorOf)) + " error: " + l.text : l.text;
            it.readOnly = true;
            it.visible = meet(at.body, l.r);
            out.push_back(std::move(it));
        }
        for (const Control& c : at.controls)
        {
            funkgui::A11yItem it;
            it.id = c.id;
            it.parent = kA11yBase;
            it.bounds = c.r;
            it.title = c.title;
            it.enabled = c.enabled;
            it.visible = c.act == Act::done || meet(at.body, c.r);
            if (c.menu)
            {
                it.role = funkgui::A11yRole::comboBox;
                it.value = c.label;
            }
            else if (c.toggle)
            {
                it.role = funkgui::A11yRole::toggleButton;
                it.checkable = true;
                it.checked = c.on;
                if (c.act == Act::mute)
                    it.help = kMuteWhy;
            }
            else
                it.role = funkgui::A11yRole::button;
            out.push_back(std::move(it));
        }
    }

    bool SettingsCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height, funkgui::HostServices* host)
    {
        const Places at = places(width, height);
        for (const Control& c : at.controls)
        {
            if (c.id != id)
                continue;
            focus_ = { c.act, c.index };
            if (a == funkgui::A11yAction::press || a == funkgui::A11yAction::toggle || a == funkgui::A11yAction::showMenu)
                act(c, host);
            ++revision_;
            return true;
        }
        return false;
    }
}
