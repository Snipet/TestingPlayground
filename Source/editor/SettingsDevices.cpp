// Source/editor/SettingsDevices.cpp: the settings card's Audio and MIDI page (SettingsPages.h), the Standalone's audio
// and MIDI devices (EditorPort::devices, plugin/DevicePort.h) as the card had them before it had pages: listed only
// where there are devices (the Standalone; never in a host).
//
//   OUTPUT  the output device (a select: its menu, the host's, lists the type's devices), Test (a tone through it); its
//           channels as switches, a pair each, when it has more than one pair
//   INPUT   an effect's only: the input device (None among them), its pairs, and Mute input with a line on why (a
//           loop through the speakers would feed back); unmuting is the user's click alone
//   AUDIO   the device type (when there are several), the sample rate, the buffer size with its ms, and the latency the
//           device reports (in and out)
//   MIDI    an instrument's only: each MIDI input with a switch (a racing wheel's among them, its status under its
//           name), or "No MIDI inputs"
// A device's error (a choice it refused) is under the section whose choice it was, in the danger colour. The card reads
// the devices again whenever DevicePort::serial moves (a device plugged in, a change made elsewhere).
#include "editor/SettingsPages.h"

#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground::ui::settings
{
    namespace
    {
        constexpr float kDeviceW = 330.0f;               // a device's pill, at most

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

        void build(Builder& b)
        {
            const DeviceSetup& s = b.ctx.card.setup();
            const float inner = b.inner, ctrlX = b.ctrlX, ctrlW = b.ctrlW;
            float& y = b.y;
            const auto label = [&](const char* text) { b.label(text); };
            const auto control = [&](Act act, int index, funkgui::Rect r, std::string shown, std::string title, bool enabled) -> Control& {
                Control& c = b.control(act, index, r, std::move(shown), std::move(title), enabled);
                c.menu = act == Act::type || act == Act::output || act == Act::input || act == Act::rate || act == Act::buffer;
                c.toggle = act == Act::outputPair || act == Act::inputPair || act == Act::mute || act == Act::midi;
                return c;
            };
            // A switch for each, as many in a row as fit (wrapping under the first), from the controls' column.
            const auto switches = [&](Act act, const std::vector<ChannelPair>& pairs, const char* what) {
                float x = ctrlX;
                for (std::size_t i = 0; i < pairs.size(); ++i)
                {
                    const std::string name = asciiText(pairs[i].name);
                    const float sw = Builder::switchW(name);
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
                b.notes(asciiText(s.error), 0.0f, inner, Line::Kind::error);
                y += 6.0f;
            };

            // OUTPUT: the device, Test; its pairs when it has more than one.
            b.heading("OUTPUT");
            {
                label("Device");
                const float testW = Builder::pillW("Test", false, true);
                const float devW = std::max(120.0f, std::min(kDeviceW, ctrlW - testW - 8.0f));
                control(Act::output, 0, { ctrlX, y, devW, kRowH }, s.output.empty() ? "None" : asciiText(s.output), "Output device",
                        !s.outputs.empty());
                control(Act::test, 0, { ctrlX + devW + 8.0f, y, testW, kRowH }, "Test", "Play a test tone", s.open && !s.output.empty())
                    .icon = chat::Icon::play;
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
                b.heading("INPUT");
                label("Device");
                if (s.separateInputs)
                    control(Act::input, 0, { ctrlX, y, std::max(120.0f, std::min(kDeviceW, ctrlW)), kRowH },
                            s.input.empty() ? "None" : asciiText(s.input), "Input device", true);
                else
                    b.line(Line::Kind::value, "The output device's own", { ctrlX, y, ctrlW, kRowH });
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
                    control(Act::mute, 0, { ctrlX, y, Builder::switchW(mute), kRowH }, mute, "Mute input", true).on = s.muted;
                    y += kRowH + 2.0f;
                    b.notes(kMuteWhy, ctrlX, ctrlW);
                    y += kRowStep - kRowH - 2.0f;
                }
                errors(DeviceSection::input);
            }

            // AUDIO: the type (when there are several), the rate, the buffer, the latency.
            b.heading("AUDIO");
            if (s.types.size() > 1)
            {
                label("Device type");
                control(Act::type, 0, { ctrlX, y, std::min(ctrlW, Builder::pillW(asciiText(s.type), true, false)), kRowH },
                        asciiText(s.type), "Device type", true);
                y += kRowStep;
            }
            if (s.open)
            {
                label("Sample rate");
                const std::string rate = rateLabel(s.rate);
                control(Act::rate, 0, { ctrlX, y, std::min(ctrlW, std::max(130.0f, Builder::pillW(rate, true, false))), kRowH }, rate,
                        "Sample rate", !s.rates.empty());
                y += kRowStep;
                label("Buffer size");
                const std::string buffer = bufferLabel(s.buffer, s.bufferMs);
                control(Act::buffer, 0, { ctrlX, y, std::min(ctrlW, Builder::pillW(buffer, true, false)), kRowH }, buffer, "Buffer size",
                        !s.buffers.empty());
                y += kRowStep;
                label("Latency");
                b.line(Line::Kind::value, latencyText(s), { ctrlX, y, ctrlW, kRowH });
                y += kRowStep;
            }
            else
            {
                b.line(Line::Kind::value, "No audio device is open.", { 0.0f, y, inner, kTextRow });
                y += kTextRow + 8.0f;
            }
            errors(DeviceSection::audio);

            // MIDI (an instrument's): each input with a switch.
            if (s.takesMidi)
            {
                b.heading("MIDI");
                if (s.midi.empty())
                {
                    b.line(Line::Kind::value, "No MIDI inputs", { 0.0f, y, inner, kTextRow });
                    y += kTextRow + 8.0f;
                }
                else
                {
                    label("Inputs");
                    for (std::size_t i = 0; i < s.midi.size(); ++i)
                    {
                        const MidiInputDevice& m = s.midi[i];
                        const std::string name = asciiText(m.name);
                        control(Act::midi, static_cast<int>(i), { ctrlX, y, std::min(ctrlW, Builder::switchW(name)), kRowH }, name,
                                "MIDI input " + m.name + (m.status.empty() ? "" : ": " + m.status), true)
                            .on = m.on;
                        const float step = i + 1 < s.midi.size() ? kFlowStep : kRowStep;
                        if (m.status.empty())
                        {
                            y += step;
                            continue;
                        }
                        // Its status under its name, as the mute's why is under its switch.
                        const float noteX = ctrlX + kSwitchW + 10.0f;
                        y += kRowH + 2.0f;
                        b.notes(asciiText(m.status), noteX, ctrlW - (noteX - ctrlX));
                        y += step - kRowH - 2.0f;
                    }
                }
                errors(DeviceSection::midi);
            }
        }

        // The host's menu beside the pill: the choices, the current one ticked. The choice reaches the devices when it is
        // made, if the card is still open.
        void openMenu(const Control& c, Context& ctx)
        {
            funkgui::HostServices* host = ctx.host;
            if (host == nullptr)
                return;
            constexpr int kNone = 1000;                  // the input's None
            funkgui::MenuRequest request;
            const DeviceSetup& s = ctx.card.setup();
            int current = 0;                             // the choice now, highlighted when the menu opens
            const auto choice = [&](int id, std::string label, bool now) {
                if (now)
                    current = id;
                request.items.push_back(dropdown::choice(id, std::move(label), now));
            };
            const auto items = [&](const std::vector<std::string>& names, const std::string& now) {
                for (std::size_t i = 0; i < names.size(); ++i)
                    choice(static_cast<int>(i) + 1, names[i], names[i] == now);
            };
            switch (c.act)
            {
                case Act::type:   items(s.types, s.type); break;
                case Act::output: items(s.outputs, s.output); break;
                case Act::input:
                    choice(kNone, "None", s.input.empty());
                    if (!s.inputs.empty())
                        request.items.push_back(dropdown::separator());
                    items(s.inputs, s.input);
                    break;
                case Act::rate:
                    for (std::size_t i = 0; i < s.rates.size(); ++i)
                        choice(static_cast<int>(i) + 1, rateLabel(s.rates[i]), std::lround(s.rates[i]) == std::lround(s.rate));
                    break;
                case Act::buffer:
                    for (std::size_t i = 0; i < s.buffers.size(); ++i)
                        choice(static_cast<int>(i) + 1, bufferLabel(s.buffers[i].samples, s.buffers[i].ms),
                               s.buffers[i].samples == s.buffer);
                    break;
                case Act::test:
                case Act::outputPair:
                case Act::inputPair:
                case Act::mute:
                case Act::midi:
                case Act::done:
                case Act::page:
                case Act::item:
                    return;
            }
            dropdown::dropdownMenu(request, c.r, current);
            request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
            const Act act = c.act;
            const DeviceSetup choices = s;               // what the menu lists, whatever changes meanwhile
            SettingsCard* card = &ctx.card;
            host->showMenu(request, [card, act, choices](int chosen) {
                DevicePort* d = card->port().devices();
                if (!card->isOpen() || d == nullptr || chosen <= 0)
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
                    case Act::page:
                    case Act::item:
                        break;
                }
                card->changed();
            });
        }

        void act(const Control& c, Context& ctx)
        {
            DevicePort* d = ctx.port.devices();
            if (d == nullptr)
                return;
            switch (c.act)
            {
                case Act::type:
                case Act::output:
                case Act::input:
                case Act::rate:
                case Act::buffer:     openMenu(c, ctx); return;
                case Act::test:       d->playTestTone(); break;
                case Act::outputPair: d->switchPair(false, c.index, !c.on); break;
                case Act::inputPair:  d->switchPair(true, c.index, !c.on); break;
                case Act::mute:       d->setMuted(!c.on); break;
                case Act::midi:
                    if (const DeviceSetup& s = ctx.card.setup(); c.index >= 0 && c.index < static_cast<int>(s.midi.size()))
                        d->switchMidi(s.midi[static_cast<std::size_t>(c.index)].identifier, !c.on);
                    break;
                case Act::done:
                case Act::page:
                case Act::item:       break;
            }
        }
    }

    const Page kAudioPage{
        "audio",
        "Audio and MIDI",
        "The audio and MIDI devices this app plays through, kept for the next time it opens.",
        [](const Context& ctx) { return ctx.card.hasDevices(); },
        build,
        act,
        nullptr,
    };
}
