// PROBE name=gui.panel timeout=240
//
// gui.panel: a program's interface from a .gui file (fminus/GuiSpec.h) in the editor's Panel, headless, over a real
// Processor, written to <build>/probe-out/:
//   gui.panel.code.png    the CODE view: the interface under the editor
//   gui.panel.png         the PANEL view: the interface across the left column; .paper.png in PAPER
//   gui.panel.meters.png  its METERS page, audio flowing (meter, VU, light, number, scope, spectrum)
//   gui.panel.small.png   the least size, CODE view, scrolled to its end
//   gui.panel.readouts.png  READOUTS' panel: number displays of Shared's fields and of the main module's, formatted
//   gui.panel.state.png     STATE's picture Panel at its frames' last; .picture.png view_panel's picture with values
//   gui.panel.settle.png    SETTLE's panel: a tone's scope and a field's, after a ramp of the field
//   gui.panel.spectrum.png  SPECTRUM's panel: two spectra of the output (and the effect's of its input), the effect's
//                           input noise, the synth's voice a sine
// and checks:
//   CODE      the program runs with no note; the dials' own items give way to the interface's; every item it shows is
//             inside its area, in the left column, none overlapping another; the area under the problems list
//   VIEW      the toolbar's PANEL cell shows the PANEL view: the tabs, the editor and the problems go, the interface
//             starts under the toolbar and fits, in the area panelViewArea computes (the panel's fit, run_program's
//             line, lays it out there), the chat open and folded; the processor keeps the choice, and its state
//             carries it to another processor; CODE brings the editor back
//   CONTROLS  a drag up on the large knob raises its parameter by a quarter for 50 px, and a double-click returns it to
//             its default; the knob's log taper puts 632 Hz at its middle (the host's text says so); a click on a
//             button of a row of buttons chooses its value, a click on the switch flips it, a click on the menu opens
//             the host's menu of its values (the chosen one ticked) and choosing one sets it; a slider's drag up
//             raises it; a drag on the pad moves both of its parameters; one host gesture each
//   ENVELOPE  the envelope draws its four parameters: its corners in order left to right, the peak at the top, the
//             sustain at its level
//   TABS      the first page shows (the pad), the second's items do not; a click on the second tab shows it (its
//             displays) and hides the first's; accessibility has the tabs as a radio group
//   AUDIO     with audio through the processor the meter reads the output's peak, the light and the number the
//             fields, and the spectrum's strongest point is at the sine's frequency and level (a point either way,
//             3 dB)
//   SCROLL    at the least size the CODE view's interface is taller than its strip: the wheel over its ground
//             scrolls it (its items move up, none drawn outside its area), and over a knob it turns the knob instead.
//             A trackpad's run of small steps (Panel::scrollGui, which the wheel over the ground calls) lays nothing
//             out again (Panel::layouts): every node moves together, by the same whole px, each control and display
//             shown exactly while its node is on a page shown and at least in part in the area, and each where a
//             layout at that scroll puts it (the window a px wider and back; and layoutGui made apart at the scroll
//             rounded: the nodes on the pages shown and the tabs' tabs, at no scroll, a step, a half px, a fraction
//             and the bottom); a step's cost against a layout's, printed
//   PRAGMAS   a program without a .gui file in the PANEL view: its #pragma panel dials from the column's top
//   READOUTS  a program whose Shared's init sets its fields, with no note played: number displays of shared->level
//             (0.750), of an int, shared->notes (3, not 3.000), of shared->speed with unit="rpm" decimals=0 scale=60
//             (1250 rpm, as a knob reads), and a meter of shared->level at three quarters, all whether a voice sounds
//             or not; the main module's int field a whole number too; a readout with a unit (-1200.0 cents) and one of
//             twelve digits each drawn whole inside its box
//   STATE     a picture's state (Panel::showState, view_panel's values and at): a value for a knob's live= field drawn
//             as its mark before the audio thread plays the program (none without it); frames shown at the first until
//             they play, then one a frame to the last (a number's readout, a field scope's points one a frame), a tap and
//             the output's level over each; picturePanel with values: Shared's field, a dial and the output's level drawn
//             as asked (the text says so, and what the displays read), a name that is none listed with the fields, the
//             dial moving the picture and not the user's dial; at's dials drawn as the render left them, and its state
//             refused once the program that plays is another (gui.panel.state.png, gui.panel.state.picture.png)
//   SETTLE    while a steady 220 Hz tone plays, its scope (found from its rising zero crossing between two samples) and a
//             field scope of a constant settle: neither unsettled over a second of frames, and the panel's frames as its
//             items ask them (Cadence.h) at the calm rate, 12 a second; the tone's level moved by a few hundredths of a
//             px of its trace is no move, by px a move; the field's step moves its scope, which settles again once its
//             points are all the new value; after a ramp longer than its 150 points, they are oldest first, the newest
//             last (its ring); number displays of the output's peak and of a field that moves each block: a new
//             readout at each refresh is no motion but the item's draw key, each one recorded (needsDraw), the panel
//             at the calm rate, 12 a second
//   SPECTRUM  a spectrum's points (the source's Spectrum: a real FFT, its tables made once) against the complex FFT a
//             display ran for itself each frame before (oldSpectrum, below), on fixed signals at 44.1, 48 and 96 kHz:
//             within 0.02 dB at every point as drawn (-84 to 0 dB), and within 0.003 dB of the same in double
//             precision (exactSpectrum: what differed was the old one's twiddles by recurrence); in a panel showing two
//             spectra of the output (and the effect's of its input) while audio plays, a block every second frame: one
//             FFT a source for each new block, none on a frame with no block, both displays the same points; once the
//             ring holds only silence, no FFT while blocks still come, and the displays still fall 30 dB a second; an
//             FFT's cost against the old one's, printed
//   HIDDEN    the meters' reader (Panel.h): a panel the host ticks reads them (the output's level shows); four idle
//             calls 0.1 s apart with no tick keep it, twelve let it go (the meters' readers one fewer, and the audio
//             thread measures nothing: the levels read 0 while the sound plays); its next tick takes it again (the
//             level back at the next block); a calm panel ticked 12 times a second with idle calls 10 a second between
//             never lets go; a second panel reads too, lets go hidden, and closed then removes nothing more
#include "Cadence.h"
#include "Probe.h"

#include "editor/DisplayItem.h"
#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <chrono>
#else
#include <time.h>
#endif

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    enum Slot : int { cutoff, resonance, attack, decay, sustain, release, wave, syncSlot, mode, voices, level, pan };

    std::string program()
    {
        const std::string body = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                 "    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.2)]];\n"
                                 "    param float attack [[fminus::range(0.001, 2)]] [[fminus::default(0.1)]];\n"
                                 "    param float decay [[fminus::range(0.001, 2)]] [[fminus::default(0.3)]];\n"
                                 "    param float sustain [[fminus::range(0, 1)]] [[fminus::default(0.6)]];\n"
                                 "    param float release [[fminus::range(0.001, 4)]] [[fminus::default(0.5)]];\n"
                                 "    param int wave [[fminus::range(0, 2)]] [[fminus::default(0)]];\n"
                                 "    param bool sync [[fminus::default(0)]];\n"
                                 "    param int mode [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                 "    param int voices [[fminus::range(1, 16)]] [[fminus::default(4)]];\n"
                                 "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    param float pan [[fminus::range(-1, 1)]] [[fminus::default(0)]];\n"
                                 "    float env;\n    bool gate;\n    int count;\n    float ph;\n    float step;\n";
        const std::string fields = "        env = 0.5f;\n        gate = true;\n        count = 7;\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + body + "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.25f * sinf(6.28318531f * ph);\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                   + fields + "        return true;\n    }\n};\n";
        else
            return "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   + fields + "    }\n};\n";
    }

    const char* const kGui = R"GUI(// The probe's interface.
panel accent=orange {
    row {
        label "PANEL PROBE" size=title
        spacer
        label "EVERY ELEMENT" size=small
    }
    row gap=10 {
        section "FILTER" layout=column {
            knob cutoff size=large taper=log unit="Hz"
            knob resonance style=vintage
        }
        section "ENVELOPE" {
            slider attack
            slider decay
            slider sustain scale=100 unit="%" decimals=0
            slider release
            envelope attack decay sustain release label="AMP"
        }
        box {
            buttons wave "SAW" "SQR" "TRI" horizontal
            switch sync
            menu mode "POLY" "MONO"
        }
    }
    tabs {
        page "MOD" {
            row {
                xy level pan label="LEVEL / PAN"
                divider
                knob voices size=small
            }
        }
        page "METERS" layout=row {
            meter out
            vu out
            led gate label="GATE"
            number count min=0 max=16
            scope env
            spectrum out grow=1
        }
    }
}
)GUI";

    // The editor's files as a session restores them (the editor shows them: draftRevision bumps), then run.
    void load(pg::Processor& p, const pg::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
        juce::MemoryBlock state;
        p.getStateInformation(state);
        p.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        p.editorPort().run(files);
    }

    void settleRun(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }

    bool writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        const bool ok = host.writePng(path.c_str(), 2);
        P.eq(key + ".png_written", ok ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
        return ok;
    }

    bool sameRect(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return std::fabs(a.x - b.x) < 0.5f && std::fabs(a.y - b.y) < 0.5f && std::fabs(a.w - b.w) < 0.5f
            && std::fabs(a.h - b.h) < 0.5f;
    }

    bool inside(const funkgui::Rect& in, const funkgui::Rect& r)
    {
        return r.x >= in.x - 0.5f && r.y >= in.y - 0.5f && r.right() <= in.right() + 0.5f && r.bottom() <= in.bottom() + 0.5f;
    }

    bool overlap(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() - 0.5f && b.x < a.right() - 0.5f && a.y < b.bottom() - 0.5f && b.y < a.bottom() - 0.5f;
    }

    // The interface's items shown, with their places: the controls', the pads', the envelopes', the displays'.
    std::vector<funkgui::Rect> shownCells(const pg::ui::Panel& panel)
    {
        std::vector<funkgui::Rect> out;
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (panel.guiControl(c).shown())
                out.push_back(panel.guiControl(c).cell());
        for (int k = 0; k < 8; ++k)
        {
            const auto& pad = static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, k));
            if (pad.shown())
                out.push_back(pad.pad());
        }
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            if (panel.displayItem(d).shown())
                out.push_back(panel.displayItem(d).cell());
        return out;
    }

    // Every control and display of the interface shows exactly while its node is on a page shown and at least in part
    // in the area (GuiView::place).
    bool shownFollowsArea(const pg::ui::Panel& panel)
    {
        const pg::ui::GuiLayout& L = panel.guiLayout();
        const std::vector<pg::GuiNode>& nodes = panel.panelSpec().gui.nodes;
        bool ok = true;
        for (std::size_t i = 0; i < nodes.size() && i < L.rects.size(); ++i)
        {
            const funkgui::Rect& r = L.rects[i];
            const bool in = L.visible[i] && r.right() > L.area.x && r.x < L.area.right() && r.bottom() > L.area.y
                         && r.y < L.area.bottom();
            const pg::GuiNode& n = nodes[i];
            const bool control = n.kind == pg::GuiKind::knob || n.kind == pg::GuiKind::slider || n.kind == pg::GuiKind::toggle
                              || n.kind == pg::GuiKind::buttons || n.kind == pg::GuiKind::menu;
            if (control && n.slot >= 0 && n.slot < pg::kMaxControls)
                ok = ok && panel.guiControl(n.slot).shown() == in;
            else if (n.kind == pg::GuiKind::display && n.display >= 0 && n.display < pg::kMaxDisplays)
                ok = ok && panel.displayItem(n.display).shown() == in;
        }
        return ok;
    }

    // The interface's places as text: every node's rect and whether it is on a page shown, each control's and
    // display's cell and whether it shows, the pads', envelopes' and tabs' places.
    std::string itemPlaces(const pg::ui::Panel& panel)
    {
        std::string out;
        char line[160];
        const auto rect = [&](const char* what, int k, const funkgui::Rect& r, bool shown) {
            std::snprintf(line, sizeof line, "%s %d: %.2f %.2f %.2f %.2f %d\n", what, k, static_cast<double>(r.x),
                          static_cast<double>(r.y), static_cast<double>(r.w), static_cast<double>(r.h), shown ? 1 : 0);
            out += line;
        };
        const pg::ui::GuiLayout& L = panel.guiLayout();
        rect("area", 0, L.area, true);
        for (std::size_t i = 0; i < L.rects.size(); ++i)
            rect("node", static_cast<int>(i), L.rects[i], L.visible[i]);
        for (int c = 0; c < pg::kMaxControls; ++c)
            rect("control", c, panel.guiControl(c).cell(), panel.guiControl(c).shown());
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            rect("display", d, panel.displayItem(d).cell(), panel.displayItem(d).shown());
        for (int k = 0; k < 8; ++k)
        {
            const auto& pad = static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, k));
            rect("pad", k, pad.pad(), pad.shown());
            const funkgui::Point e = panel.guiItem(1, k).centre();
            rect("envelope", k, { e.x, e.y, 0.0f, 0.0f }, panel.guiItem(1, k).shown());
            const auto& tabs = static_cast<const pg::ui::GuiTabsItem&>(panel.guiItem(2, k));
            for (const funkgui::Rect& t : tabs.tabs())
                rect("tab", k, t, tabs.shown());
        }
        return out;
    }

    float hzOfPoint(int k)
    {
        return 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(pg::ui::DisplayItem::kSpectrumPoints - 1));
    }

    // SPECTRUM's reference: a spectrum display's points as each display computed them for itself every frame before
    // the source's Spectrum (DisplayItem.h), a complex radix-2 FFT of all its samples, its twiddles by recurrence,
    // each bin's magnitude, kept here as it was.
    void oldFft(std::vector<std::complex<float>>& a)
    {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i)
        {
            std::size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
            const std::complex<float> w(std::cos(ang), std::sin(ang));
            for (std::size_t i = 0; i < n; i += len)
            {
                std::complex<float> wk(1.0f, 0.0f);
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * wk;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    wk *= w;
                }
            }
        }
    }

    std::vector<float> oldSpectrum(const float* x, int n, float rate, int points)
    {
        std::vector<std::complex<float>> a(static_cast<std::size_t>(n));
        float windowSum = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n - 1));
            windowSum += w;
            a[static_cast<std::size_t>(i)] = { x[i] * w, 0.0f };
        }
        oldFft(a);
        const float norm = 2.0f / std::max(windowSum, 1.0f);
        const float binHz = rate / static_cast<float>(n);
        std::vector<float> out(static_cast<std::size_t>(points), -120.0f);
        for (int k = 0; k < points; ++k)
        {
            const float hz = 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(points - 1));
            const float half = std::pow(1000.0f, 0.5f / static_cast<float>(points - 1));
            const float lo = hz / half, hi = hz * half;
            int b0 = static_cast<int>(std::floor(lo / binHz)), b1 = static_cast<int>(std::ceil(hi / binHz));
            b0 = std::clamp(b0, 1, n / 2);
            b1 = std::clamp(b1, b0, n / 2);
            float peak = 0.0f;
            for (int b = b0; b <= b1; ++b)
                peak = std::max(peak, std::abs(a[static_cast<std::size_t>(b)]) * norm);
            out[static_cast<std::size_t>(k)] = 20.0f * std::log10(std::max(peak, 1.0e-6f));
        }
        return out;
    }

    // The same points in double precision, every twiddle exact: what both should be.
    std::vector<float> exactSpectrum(const float* x, int n, float rate, int points)
    {
        std::vector<std::complex<double>> a(static_cast<std::size_t>(n));
        double windowSum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (n - 1));
            windowSum += w;
            a[static_cast<std::size_t>(i)] = { x[i] * w, 0.0 };
        }
        for (std::size_t i = 1, j = 0; i < a.size(); ++i)
        {
            std::size_t bit = a.size() >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= a.size(); len <<= 1)
            for (std::size_t i = 0; i < a.size(); i += len)
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const double ang = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(len);
                    const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * std::polar(1.0, ang);
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
        const std::vector<float> bins = oldSpectrum(x, n, rate, points);   // (only its shape: the bins below)
        const float binHz = rate / static_cast<float>(n);
        std::vector<float> out(bins.size(), -120.0f);
        for (int k = 0; k < points; ++k)
        {
            const float hz = 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(points - 1));
            const float half = std::pow(1000.0f, 0.5f / static_cast<float>(points - 1));
            int b0 = static_cast<int>(std::floor(hz / half / binHz)), b1 = static_cast<int>(std::ceil(hz * half / binHz));
            b0 = std::clamp(b0, 1, n / 2);
            b1 = std::clamp(b1, b0, n / 2);
            double peak = 0.0;
            for (int b = b0; b <= b1; ++b)
                peak = std::max(peak, std::abs(a[static_cast<std::size_t>(b)]) * 2.0 / windowSum);
            out[static_cast<std::size_t>(k)] = static_cast<float>(20.0 * std::log10(std::max(peak, 1.0e-6)));
        }
        return out;
    }

    // A point as drawSpectrum draws it: -84 to 0 dB.
    float drawnDb(float db) { return std::clamp(db, -84.0f, 0.0f); }

    // A clock for a printed cost, in seconds: this thread's CPU time (what the work took, however often a busy machine
    // put the thread aside); on Windows, whose C runtime has no clock_gettime, the steady clock's wall time.
    double threadSeconds()
    {
#if defined(_WIN32)
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + 1.0e-9 * static_cast<double>(ts.tv_nsec);
#endif
    }
}

PROBE(gui, panel)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);
    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();

    // ---- CODE ----------------------------------------------------------------------------------------------------------
    load(processor, { { pg::kProgramName, program() }, { "panel.gui", kGui } });
    settleRun(processor, host);
    P.eq("code.compiles", port.status().ok ? 1 : 0, 1);
    if (!port.status().notes.empty() || !port.status().errors.empty())
        std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
    P.eq("code.no_notes", port.status().notes.empty() ? 1 : 0, 1);
    P.eq("code.has_gui", panel.hasGui() ? 1 : 0, 1);
    P.eq("code.dials_give_way", panel.centreOfDial(cutoff).has_value() ? 0 : 1, 1);
    const pg::ui::GuiLayout* L = &panel.guiLayout();
    const funkgui::Rect chat = panel.chatCard();
    P.eq("code.area_left_of_chat", L->area.right() <= chat.x ? 1 : 0, 1);
    P.ge("code.area_under_editor", static_cast<double>(L->area.y), 300.0);
    {
        const std::vector<funkgui::Rect> cells = shownCells(panel);
        bool in = true, apart = true;
        for (std::size_t a = 0; a < cells.size(); ++a)
        {
            in = in && inside(L->area, cells[a]);
            for (std::size_t b = a + 1; b < cells.size(); ++b)
                apart = apart && !overlap(cells[a], cells[b]);
        }
        std::printf("code view: %zu items in %g x %g (content %g x %g)\n", cells.size(), static_cast<double>(L->area.w),
                    static_cast<double>(L->area.h), static_cast<double>(L->contentW), static_cast<double>(L->contentH));
        P.ge("code.items", static_cast<double>(cells.size()), 6.0);   // the rest scrolled out of its strip
        P.eq("code.inside", in || L->maxScrollY > 0.0f ? 1 : 0, 1);
        P.eq("code.apart", apart ? 1 : 0, 1);
    }
    writePng(P, host, "gui.panel.code.png", "code");

    // ---- VIEW ----------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
        L = &panel.guiLayout();
        P.eq("view.panel", panel.panelView() && port.panelView() ? 1 : 0, 1);
        P.le("view.from_toolbar", static_cast<double>(L->area.y), 100.0);
        P.eq("view.fits", L->maxScrollY == 0.0f && L->maxScrollX == 0.0f ? 1 : 0, 1);
        // Where the panel's fit (run_program's line) lays it out: the same area, by panelViewArea's arithmetic.
        {
            const funkgui::Rect a = pg::ui::panelViewArea(static_cast<float>(pg::ui::kDefaultWidth),
                                                          static_cast<float>(pg::ui::kDefaultHeight), port.chatOpen(),
                                                          panel.chatWant());
            P.eq("view.area_as_fit_says", sameRect(a, L->area) ? 1 : 0, 1);
            panel.setChatOpen(false);                // the chat folded to its rail: the area wider
            host.settle();
            const funkgui::Rect folded = pg::ui::panelViewArea(static_cast<float>(pg::ui::kDefaultWidth),
                                                               static_cast<float>(pg::ui::kDefaultHeight), false,
                                                               panel.chatWant());
            P.eq("view.area_as_fit_says_folded", sameRect(folded, panel.guiLayout().area) && folded.w > a.w ? 1 : 0, 1);
            panel.setChatOpen(true);
            host.settle();
            L = &panel.guiLayout();
        }
        bool editorGone = true;                  // no text field in the left column (the chat's message box stays)
        for (const funkgui::A11yItem& a : host.accessibility())
            editorGone = editorGone && !(a.role == funkgui::a11yrole::editableText && a.bounds.x < panel.chatCard().x);
        P.eq("view.editor_gone", editorGone ? 1 : 0, 1);
        const std::vector<funkgui::Rect> cells = shownCells(panel);
        bool in = true, apart = true;
        for (std::size_t a = 0; a < cells.size(); ++a)
        {
            in = in && inside(L->area, cells[a]);
            for (std::size_t b = a + 1; b < cells.size(); ++b)
                apart = apart && !overlap(cells[a], cells[b]);
        }
        P.eq("view.inside", in ? 1 : 0, 1);
        P.eq("view.apart", apart ? 1 : 0, 1);
        P.eq("view.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        writePng(P, host, "gui.panel.png", "view");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            P.eq("view.kept_by_processor", paper.panelView() ? 1 : 0, 1);
            writePng(P, paperHost, "gui.panel.paper.png", "view.paper");
            funkgui::UiPreferences::get().setTheme(0);
        }
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        pg::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("view.in_session", second.editorPort().panelView() ? 1 : 0, 1);
    }

    // ---- CONTROLS ------------------------------------------------------------------------------------------------------
    {
        funkgui::ParamPort& cut = *processor.ports()[cutoff];
        const pg::ui::GuiControlItem& knob = panel.guiControl(cutoff);
        P.eq("controls.knob_large", knob.shown() && knob.cell().w >= 86.0f && knob.cell().h >= 106.0f ? 1 : 0, 1);   // 86 x 106
        const float before = cut.value01();      // 1000 Hz: 0.57 on its log taper
        const funkgui::Point k = knob.centre();
        host.drag(k.x, k.y, k.x, k.y - 50.0f, 10);
        host.settle();
        P.near("controls.knob_drag", static_cast<double>(cut.value01() - before), 0.25, 0.02);
        host.doubleClick(k.x, k.y);
        host.settle();
        P.near("controls.knob_default", static_cast<double>(pg::plainOf(processor.control(cutoff), cut.value01())), 1000.0, 1.0);
        processor.getParameters()[cutoff]->setValueNotifyingHost(0.5f);
        host.settle();
        const std::string middle = processor.getParameters()[cutoff]->getCurrentValueAsText().toStdString();
        std::printf("the cutoff knob's middle: %s\n", middle.c_str());
        P.eq("controls.log_middle_text", middle == "632 Hz" ? 1 : 0, 1);

        // A row of buttons: the third chooses TRI.
        const pg::ui::GuiControlItem& buttons = panel.guiControl(wave);
        const funkgui::Rect b = buttons.cell();
        host.click(b.x + b.w * 5.0f / 6.0f, b.y + 26.0f);
        host.settle();
        P.near("controls.buttons", static_cast<double>(pg::plainOf(processor.control(wave), processor.ports()[wave]->value01())), 2.0, 1e-4);
        P.eq("controls.buttons_host_name", processor.getParameters()[wave]->getCurrentValueAsText() == "TRI" ? 1 : 0, 1);
        const funkgui::Point sw = panel.guiControl(syncSlot).centre();
        host.click(sw.x, sw.y);
        host.settle();
        P.near("controls.switch", static_cast<double>(processor.ports()[syncSlot]->value01()), 1.0, 1e-4);
        const funkgui::Point menu = panel.guiControl(mode).centre();
        host.click(menu.x, menu.y + 8.0f);
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("controls.menu_opens", m != nullptr && m->items.size() == 2 && m->items[0].label == "POLY" && m->items[0].checked ? 1 : 0, 1);
        host.chooseMenuItem("MONO");
        host.settle();
        P.near("controls.menu_chooses", static_cast<double>(processor.ports()[mode]->value01()), 1.0, 1e-4);
        // A slider: a drag up from its cap raises it.
        funkgui::ParamPort& dec = *processor.ports()[decay];
        const pg::ui::GuiControlItem& slider = panel.guiControl(decay);
        const funkgui::Rect tr = slider.track();
        const float y0 = tr.bottom() - tr.h * dec.value01();
        const float d0 = dec.value01();
        host.drag(tr.centreX(), y0, tr.centreX(), y0 - 0.25f * tr.h, 8);
        host.settle();
        P.near("controls.slider_drag", static_cast<double>(dec.value01() - d0), 0.25, 0.03);
        // The pad, on the first page: a drag to its top right corner's quarter.
        const auto& pad = static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0));
        P.eq("controls.pad_shown", pad.shown() ? 1 : 0, 1);
        const funkgui::Rect r = pad.pad();
        host.drag(r.x + 0.5f * r.w, r.y + 0.5f * r.h, r.x + 0.75f * r.w, r.y + 0.25f * r.h, 6);
        host.settle();
        P.near("controls.pad_x", static_cast<double>(processor.ports()[level]->value01()), 0.75, 0.02);
        P.near("controls.pad_y", static_cast<double>(processor.ports()[pan]->value01()), 0.75, 0.02);
    }

    // ---- ENVELOPE ------------------------------------------------------------------------------------------------------
    {
        const auto& env = static_cast<const pg::ui::GuiEnvelopeItem&>(panel.guiItem(1, 0));
        P.eq("envelope.shown", env.shown() ? 1 : 0, 1);
        const std::array<funkgui::Point, 5> p = env.corners();
        bool ordered = true;
        for (std::size_t k = 0; k + 1 < p.size(); ++k)
            ordered = ordered && p[k + 1].x >= p[k].x;
        P.eq("envelope.ordered", ordered ? 1 : 0, 1);
        P.eq("envelope.peak_top", p[1].y < p[2].y && p[0].y == p[4].y ? 1 : 0, 1);
        const float s = processor.control(sustain).def;
        P.near("envelope.sustain_level", static_cast<double>((p[0].y - p[2].y) / (p[0].y - p[1].y)), static_cast<double>(s), 0.02);
    }

    // ---- TABS ----------------------------------------------------------------------------------------------------------
    {
        const auto& tabs = static_cast<const pg::ui::GuiTabsItem&>(panel.guiItem(2, 0));
        P.eq("tabs.two", tabs.shown() && tabs.tabs().size() == 2 ? 1 : 0, 1);
        int displaysShown = 0;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.first_page", displaysShown == 0 && static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
        uint32_t group = 0;
        int buttons = 0;
        const std::vector<funkgui::A11yItem> a11y = host.accessibility();
        for (const funkgui::A11yItem& a : a11y)
            if (a.role == funkgui::A11yRole::radioGroup && a.title == "Pages")
                group = a.id;
        for (const funkgui::A11yItem& a : a11y)
            buttons += group != 0 && a.parent == group && a.role == funkgui::A11yRole::radioButton ? 1 : 0;
        P.eq("tabs.a11y_group", group != 0 && buttons == 2 ? 1 : 0, 1);
        if (tabs.tabs().size() == 2)
        {
            const funkgui::Rect t = tabs.tabs()[1];
            host.click(t.centreX(), t.centreY());
            host.settle();
        }
        displaysShown = 0;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.second_page", displaysShown == 6 && !static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
    }

    // ---- AUDIO ---------------------------------------------------------------------------------------------------------
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;
        processor.prepareToPlay(kRate, kBlock);
        juce::AudioBuffer<float> buffer(2, kBlock);
        double phase = 0.0;
        const auto push = [&](int blocks, bool noteOn) {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer midi;
                if (noteOn && b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float v = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 600.0 / kRate;
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                processor.processBlock(buffer, midi);
            }
        };
        push(1, true);
        for (int k = 0; k < 120; ++k)
        {
            push(1, false);
            host.tick(1);
        }
        const pg::ui::DisplayItem* spectrum = nullptr;
        const pg::ui::DisplayItem* meter = nullptr;
        const pg::ui::DisplayItem* number = nullptr;
        const pg::ui::DisplayItem* light = nullptr;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (!item.shown())
                continue;
            if (item.display().kind == pg::DisplayKind::spectrum)
                spectrum = &item;
            if (item.display().kind == pg::DisplayKind::meter)
                meter = &item;
            if (item.display().kind == pg::DisplayKind::number)
                number = &item;
            if (item.display().kind == pg::DisplayKind::led)
                light = &item;
        }
        const double outDb = kInstrument ? -12.04 : -6.02;
        const float hz = kInstrument ? 440.0f : 600.0f;
        if (meter != nullptr)
            P.near("audio.meter", std::atof(meter->readout().c_str()), outDb, 0.4);
        if (number != nullptr)
            P.eq("audio.number", number->readout() == "7" ? 1 : 0, 1);   // count, an int: a whole number
        if (light != nullptr)
            P.near("audio.light", static_cast<double>(light->level()), 1.0, 0.01);
        P.eq("audio.spectrum_shown", spectrum != nullptr && !spectrum->spectrum().empty() ? 1 : 0, 1);
        if (spectrum != nullptr && !spectrum->spectrum().empty())
        {
            const std::vector<float>& s = spectrum->spectrum();
            const auto top = std::max_element(s.begin(), s.end());
            const int k = static_cast<int>(top - s.begin());
            std::printf("spectrum: strongest %.1f dB at %.0f Hz\n", static_cast<double>(*top), static_cast<double>(hzOfPoint(k)));
            P.in("audio.spectrum_hz", static_cast<double>(hzOfPoint(k)), static_cast<double>(hz) / 1.08, static_cast<double>(hz) * 1.08);
            P.near("audio.spectrum_db", static_cast<double>(*top), outDb, 3.0);
        }
        writePng(P, host, "gui.panel.meters.png", "audio");
    }

    // ---- SCROLL --------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point code = panel.centreOfView(false);
        host.click(code.x, code.y);
        host.settle();
        P.eq("scroll.code_view", !panel.panelView() ? 1 : 0, 1);
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        L = &panel.guiLayout();
        std::printf("least size: the interface's area %g tall, content %g\n", static_cast<double>(L->area.h), static_cast<double>(L->contentH));
        P.eq("scroll.taller", L->maxScrollY > 0.0f ? 1 : 0, 1);
        // Over the ground (the area's right edge, past the items): the interface scrolls.
        const float y0 = panel.guiControl(cutoff).cell().y;
        host.wheel(L->area.right() - 6.0f, L->area.y + 10.0f, -0.5f);
        host.settle();
        L = &panel.guiLayout();
        P.eq("scroll.moved_up", panel.guiControl(cutoff).cell().y < y0 ? 1 : 0, 1);
        // Over a knob it turns the knob.
        funkgui::ParamPort& res = *processor.ports()[resonance];
        const float r0 = res.value01();
        if (panel.guiControl(resonance).shown())
        {
            const funkgui::Point c = panel.guiControl(resonance).centre();
            if (L->area.contains(c))
            {
                const float scrolled = L->maxScrollY;
                host.wheel(c.x, c.y, 0.2f);
                host.settle();
                P.eq("scroll.knob_takes_wheel", res.value01() != r0 && panel.guiLayout().maxScrollY == scrolled ? 1 : 0, 1);
            }
        }
        host.wheel(L->area.right() - 6.0f, L->area.y + 10.0f, -5.0f);
        host.settle();
        writePng(P, host, "gui.panel.small.png", "scroll");

        // A trackpad's run of small steps from the top (Panel::scrollGui, as the wheel over the ground scrolls): the
        // items move, nothing is laid out again.
        panel.scrollGui(0.0f, -1.0e4f);
        host.settle();
        const int layouts0 = panel.layouts();
        const std::vector<funkgui::Rect> top = panel.guiLayout().rects;
        bool together = true, whole = true, follows = shownFollowsArea(panel);
        float moved = 0.0f;
        for (int k = 0; k < 12; ++k)
        {
            panel.scrollGui(0.0f, 19.0f);                // a trackpad's step, 0.037 of the wheel
            host.tick(1);
            const std::vector<funkgui::Rect>& now = panel.guiLayout().rects;
            const float dy = now.empty() || top.empty() ? 0.0f : now[0].y - top[0].y;
            for (std::size_t i = 0; i < now.size() && i < top.size(); ++i)
                together = together && now[i].y - top[i].y == dy && now[i].x == top[i].x && now[i].w == top[i].w
                        && now[i].h == top[i].h;
            whole = whole && dy == std::round(dy);
            follows = follows && shownFollowsArea(panel);
            moved = -dy;
        }
        std::printf("scroll: 12 small steps moved the interface %.0f px\n", static_cast<double>(moved));
        P.eq("scroll.steps_lay_out_nothing", static_cast<int64_t>(panel.layouts() - layouts0), 0);
        P.ge("scroll.steps_moved", static_cast<double>(moved), 100.0);
        P.eq("scroll.steps_move_together", together ? 1 : 0, 1);
        P.eq("scroll.steps_whole_px", whole ? 1 : 0, 1);
        P.eq("scroll.steps_shown_follow", follows ? 1 : 0, 1);
        // Where a layout at that scroll puts each item: the window a px wider and back, laid out twice.
        {
            const std::string scrolled = itemPlaces(panel);
            host.resize(pg::ui::kMinWidth + 1, pg::ui::kMinHeight);
            host.settle();
            host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            host.settle();
            P.ge("scroll.laid_out_again", static_cast<double>(panel.layouts() - layouts0), 2.0);
            const std::string laidOut = itemPlaces(panel);
            if (scrolled != laidOut)
                std::printf("scrolled:\n%s\nlaid out:\n%s\n", scrolled.c_str(), laidOut.c_str());
            P.eq("scroll.as_a_layout", scrolled == laidOut ? 1 : 0, 1);
        }
        // And what a layout at that scroll makes, apart from the panel's (both sides above take the layout at no scroll,
        // moved): layoutGui at the scroll rounded, in the panel's area (its gutter taken), with its pages and the
        // program's dials, every node on a page shown and each tabs node's tabs. At no scroll first (the inputs are the
        // panel's), then a step, a half px, a fraction and the bottom; then back where the steps left it.
        {
            const pg::ui::GuiMeasure gm = [](const std::string& text, const funkgui::TextStyle& st) {
                return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
            };
            const auto same = [](const funkgui::Rect& a, const funkgui::Rect& b) {
                return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
            };
            const auto apart = [&](float scrollY) {
                const pg::ui::GuiLayout& shown = panel.guiLayout();
                const pg::ui::GuiLayout made = pg::ui::layoutGui(panel.panelSpec(), port.controls(), gm, shown.pages,
                                                                 shown.area, 0.0f, std::round(scrollY));
                bool ok = made.rects.size() == shown.rects.size() && made.visible == shown.visible;
                for (std::size_t i = 0; ok && i < made.rects.size(); ++i)
                    ok = !made.visible[i] || same(made.rects[i], shown.rects[i]);
                for (int k = 0; ok && k < 8; ++k)
                {
                    const auto& tabs = static_cast<const pg::ui::GuiTabsItem&>(panel.guiItem(2, k));
                    if (!tabs.shown())
                        continue;
                    const std::vector<funkgui::Rect> want = pg::ui::guiTabRects(
                        panel.panelSpec(), tabs.node(), made.rects[static_cast<std::size_t>(tabs.node())], gm);
                    ok = want.size() == tabs.tabs().size();
                    for (std::size_t t = 0; ok && t < want.size(); ++t)
                        ok = same(want[t], tabs.tabs()[t]);
                }
                if (!ok)
                    std::printf("scroll %.2f: the moved layout differs from one made at that scroll\n",
                                static_cast<double>(scrollY));
                return ok;
            };
            const float bottom = panel.guiLayout().maxScrollY;
            std::printf("scroll: the bottom at %.2f px\n", static_cast<double>(bottom));
            int agree = 0;
            for (const float s : { 0.0f, 19.0f, 57.5f, 120.4f, bottom })
            {
                panel.scrollGui(0.0f, -1.0e4f);
                panel.scrollGui(0.0f, s);
                host.tick(1);
                agree += apart(s) ? 1 : 0;
            }
            P.eq("scroll.as_a_layout_apart", agree, 5);
            panel.scrollGui(0.0f, -1.0e4f);
            panel.scrollGui(0.0f, moved);
            host.tick(1);
        }
        // A scroll step's cost against a layout's (threadSeconds(); printed, as the machine may be busy).
        {
            constexpr int kRuns = 40;
            const double t0 = threadSeconds();
            for (int k = 0; k < kRuns; ++k)
                panel.scrollGui(0.0f, k % 2 == 0 ? 19.0f : -19.0f);
            const double t1 = threadSeconds();
            for (int k = 0; k < kRuns; ++k)
                host.resize(pg::ui::kMinWidth + (k % 2 == 0 ? 1 : 0), pg::ui::kMinHeight);
            const double t2 = threadSeconds();
            std::printf("scroll: a step %.1f us, a resize's layout %.1f us\n", 1.0e6 * (t1 - t0) / kRuns,
                        1.0e6 * (t2 - t1) / kRuns);
        }
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
    }

    // ---- PRAGMAS -------------------------------------------------------------------------------------------------------
    {
        const std::string pragmas = "#pragma panel section \"ONE\" cutoff resonance\n" + program();
        load(processor, { { pg::kProgramName, pragmas } });
        settleRun(processor, host);
        P.eq("pragmas.no_gui", !panel.hasGui() ? 1 : 0, 1);
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
        P.le("pragmas.panel_view_top", static_cast<double>(panel.dialsLayout().top), 100.0);
        P.eq("pragmas.dials_shown", panel.centreOfDial(cutoff).has_value() ? 1 : 0, 1);
        const funkgui::Point code = panel.centreOfView(false);
        host.click(code.x, code.y);
        host.settle();
    }

    // ---- READOUTS ------------------------------------------------------------------------------------------------------
    {
        const std::string shared = "module Shared\n{\n    float level, speed, detune, big;\n    int notes;\n"
                                   "    void init(float rate) { level = 0.75f; notes = 3; speed = 20.8333333f; detune = -1200; big = 123456789012.0f; }\n"
                                   "};\n\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n    int count;\n"
                                       "    void start(float n, float v) { count = 3; }\n"
                                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                     : "module Effect\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n    int count;\n"
                                       "    void process(float* restrict left, float* restrict right, int n) { count = 3; }\n};\n";
        const std::string readoutGui = "row gap=10 {\n"
                                "    section \"SHARED\" {\n"
                                "        number shared->level label=\"LEVEL\"\n"
                                "        number shared->notes label=\"NOTES\"\n"
                                "        number shared->speed label=\"SPEED\" unit=\"rpm\" decimals=0 scale=60\n"
                                "        meter shared->level label=\"METER\"\n"
                                "        number shared->detune label=\"DETUNE\" unit=\"cents\" decimals=1\n"
                                "        number shared->big label=\"BIG\"\n"
                                "    }\n"
                                "    section \"VOICE\" {\n"
                                "        knob gain\n"
                                "        number count label=\"COUNT\"\n"
                                "    }\n"
                                "}\n";
        load(processor, { { pg::kProgramName, shared + main }, { "panel.gui", readoutGui } });
        settleRun(processor, host);
        const pg::CompileStatus st = port.status();
        P.eq("readouts.runs_with_no_note", st.ok && st.notes.empty() ? 1 : 0, 1);
        if (!st.notes.empty())
            std::printf("%s", st.notes.c_str());
        const funkgui::Point view = panel.centreOfView(true);
        host.click(view.x, view.y);
        host.settle();
        processor.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buffer(2, 256);
        for (int k = 0; k < 60; ++k)                     // no note: the synth's voices are silent
        {
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            host.tick(1);
        }
        host.settle();
        std::string level, notes, speed, count, detune;
        float meterLevel = -1.0f;
        // A readout's glyphs whole in its box (looks::number's: the cell less 2 each side, from 18 down, 30 high): as
        // many text quads centred in it as it has characters but spaces.
        const funkgui::PrimList& frame = host.draw();
        const auto inBox = [&](const pg::ui::DisplayItem& item) {
            const funkgui::Rect& r = item.cell();
            const funkgui::Rect box{ r.x + 2.0f, r.y + 18.0f, r.w - 4.0f, 30.0f };
            int glyphs = 0;
            for (const funkgui::Prim& q : frame.prims)
                if (static_cast<int>(q.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::text)
                    && box.contains({ 0.5f * (q.x0 + q.x1), 0.5f * (q.y0 + q.y1) }))
                    ++glyphs;
            const auto chars = std::count_if(item.readout().begin(), item.readout().end(), [](char ch) { return ch != ' '; });
            std::printf("readout %s: %d glyphs in its box of %.0f px, %d characters\n", item.readout().c_str(), glyphs,
                        static_cast<double>(box.w), static_cast<int>(chars));
            return glyphs == static_cast<int>(chars);
        };
        bool detuneFits = false, bigFits = false;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (!item.shown())
                continue;
            const pg::PanelDisplay& x = item.display();
            std::printf("readout %s %s: %s\n", x.source.c_str(), x.label.c_str(), item.readout().c_str());
            if (x.kind == pg::DisplayKind::meter)
                meterLevel = item.level();
            else if (x.label == "LEVEL")
                level = item.readout();
            else if (x.label == "NOTES")
                notes = item.readout();
            else if (x.label == "SPEED")
                speed = item.readout();
            else if (x.label == "COUNT")
                count = item.readout();
            else if (x.label == "DETUNE")
            {
                detune = item.readout();
                detuneFits = inBox(item);
            }
            else if (x.label == "BIG")
                bigFits = inBox(item);
        }
        P.eq("readouts.shared_float", level == "0.750" ? 1 : 0, 1);
        P.eq("readouts.shared_int_whole", notes == "3" ? 1 : 0, 1);
        P.eq("readouts.formatted_as_a_knob", speed == "1250 rpm" ? 1 : 0, 1);
        // A unit's readout has a cell wide enough; a field's huge value is drawn smaller: neither spills out of its box.
        P.eq("readouts.unit_formatted", detune == "-1200.0 cents" ? 1 : 0, 1);
        P.eq("readouts.unit_in_its_box", detuneFits ? 1 : 0, 1);
        P.eq("readouts.huge_in_its_box", bigFits ? 1 : 0, 1);
        P.near("readouts.shared_meter", static_cast<double>(meterLevel), 0.75, 0.01);
        // The main module's int: the effect's process writes 3; the synth's voice sounds none, so it reads 0.
        P.eq("readouts.main_int_whole", count == (kInstrument ? "0" : "3") ? 1 : 0, 1);
        writePng(P, host, "gui.panel.readouts.png", "readouts");
    }

    // ---- STATE ---------------------------------------------------------------------------------------------------------
    {
        const std::string shared = "module Shared\n{\n    float level;\n    int notes;\n"
                                   "    void init(float rate) { level = 0.75f; notes = 3; }\n};\n\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n"
                                       "    void start(float n, float v) {}\n"
                                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                     : "module Effect\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n"
                                       "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const std::string stateGui = "row gap=10 {\n"
                                     "    section \"STATE\" {\n"
                                     "        knob gain live=shared->level\n"
                                     "        number shared->level label=\"LEVEL\"\n"
                                     "        number shared->notes label=\"NOTES\"\n"
                                     "        scope shared->level label=\"TRACE\"\n"
                                     "        meter out label=\"OUT\"\n"
                                     "    }\n"
                                     "}\n";
        load(processor, { { pg::kProgramName, shared + main }, { "panel.gui", stateGui } });
        settleRun(processor, host);
        const pg::PanelSpec spec = port.status().panel;
        int levelTap = -1, traceTap = -1, notesTap = -1, liveTap = -1, levelD = -1, notesD = -1, traceD = -1, outD = -1;
        for (std::size_t d = 0; d < spec.displays.size(); ++d)
        {
            const pg::PanelDisplay& x = spec.displays[d];
            const int i = static_cast<int>(d);
            if (x.hidden)
                liveTap = x.source == "shared->level" ? x.tap : liveTap;   // the knob's live= mark's
            else if (x.source == "shared->level" && x.kind == pg::DisplayKind::number)
                levelTap = x.tap, levelD = i;
            else if (x.source == "shared->level" && x.kind == pg::DisplayKind::scope)
                traceTap = x.tap, traceD = i;
            else if (x.source == "shared->notes")
                notesTap = x.tap, notesD = i;
            else if (x.source == "out")
                outD = i;
        }
        // values alone (view_panel's), before the audio thread has played this program: a live= mark shows no value of
        // its own then, but the one asked for.
        {
            const auto liveMark = [&](const pg::ui::Panel::ShownState* st) {
                pg::ui::Panel early(processor.ports(), port, true);
                if (st != nullptr)
                    early.showState(*st);
                funkgui::HeadlessHost h(early, 0, 1.5f);
                h.settle(120);
                early.setPanelView(true);
                h.settle(120);
                return early.hasGui() ? early.guiControl(0).live01() : -2.0f;
            };
            pg::ui::Panel::ShownState asked;
            asked.taps.emplace_back(liveTap, 0.6f);
            const float none = liveMark(nullptr), drawn = liveMark(&asked);
            std::printf("state, a live= mark before the program plays: %.3f, asked 0.6: %.3f\n", static_cast<double>(none),
                        static_cast<double>(drawn));
            P.eq("state.live_mark_unplayed", liveTap >= 0 && none < 0.0f ? 1 : 0, 1);
            P.eq("state.live_mark_asked", std::fabs(drawn - 0.6f) < 1.0e-4f ? 1 : 0, 1);
        }
        juce::AudioBuffer<float> buffer(2, 256);
        for (int k = 0; k < 8; ++k)                      // its fields published (Shared's init set them)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
        }
        P.eq("state.displays", port.status().ok && levelTap >= 0 && notesTap >= 0 && traceD >= 0 && outD >= 0 ? 1 : 0, 1);
        // A picture's Panel (Panel::showState, view_panel's at): 40 frames, the field rising; held at the first until it
        // plays, then one a frame, the last kept; a tap and the output's level over every frame.
        pg::ui::Panel pic(processor.ports(), port, true);
        pg::ui::Panel::ShownState st;
        for (int k = 0; k < 40; ++k)
        {
            pg::Meters m;
            for (const int t : { levelTap, traceTap })      // each display of the field has a tap of its own
                if (t >= 0)
                    m.taps[t] = 0.1f + 0.02f * static_cast<float>(k);
            m.frame = static_cast<uint32_t>(k + 1);
            st.frames.push_back(m);
        }
        if (notesTap >= 0)
            st.taps.emplace_back(notesTap, 9.0f);
        st.outLevel = 0.5f;
        pic.showState(st);
        funkgui::HeadlessHost picHost(pic, 0, 1.5f);
        picHost.settle(120);
        pic.setPanelView(true);
        picHost.settle(120);
        const auto readout = [&](int d) { return d >= 0 ? pic.displayItem(d).readout() : std::string(); };
        const std::string first = readout(levelD);
        pic.playShown();
        int played = 0;
        for (; played < 200 && !pic.shownDone(); ++played)
            picHost.tick(1);
        picHost.tick(1);
        std::printf("state: LEVEL %s at the first frame, %s after %d frames; NOTES %s, OUT %s, TRACE %d points\n", first.c_str(),
                    readout(levelD).c_str(), played, readout(notesD).c_str(), readout(outD).c_str(),
                    traceD >= 0 ? pic.displayItem(traceD).traceSize() : -1);
        P.eq("state.held_at_first", first == "0.100" ? 1 : 0, 1);
        P.eq("state.played_to_last", played == 39 && readout(levelD) == "0.880" ? 1 : 0, 1);
        P.eq("state.tap_over_frames", readout(notesD) == "9" ? 1 : 0, 1);
        P.eq("state.level_over_frames", readout(outD) == "-6.0" ? 1 : 0, 1);
        P.eq("state.trace_of_frames", traceD >= 0 ? pic.displayItem(traceD).traceSize() : -1, 40);
        writePng(P, picHost, "gui.panel.state.png", "state");
        // picturePanel with values: Shared's field, a dial, the output's level, a name that is none; the dial's value
        // moves the picture.
        pg::PictureState ps;
        const auto value = [](const char* name, float v) {
            pg::PictureValue x;
            x.name = name;
            x.number = v;
            return x;
        };
        ps.values = { value("shared->level", 0.25f), value("gain", 0.9f), value("out", -12.0f), value("zzz", 1.0f) };
        const pg::PanelPicture a = pg::picturePanel(processor.ports(), port, 0, 0, false, "", ps);
        ps.values[1].number = 0.1f;
        const pg::PanelPicture b = pg::picturePanel(processor.ports(), port, 0, 0, false, "", ps);
        std::printf("state, a picture: %s\n", a.about.c_str());
        const auto in = [](const std::string& t, const char* what) { return t.find(what) != std::string::npos; };
        P.eq("state.picture_said", in(a.about, "but shared->level 0.25, gain 0.90, out -12 dBFS (as asked).") && in(a.about, "LEVEL 0.250")
                                       && in(a.about, "NOTES 3") && in(a.about, "OUT -12.0") ? 1 : 0, 1);
        P.eq("state.picture_unknown", in(a.about, "No dial or field of the panel is named zzz (its fields: shared->level, shared->notes).") ? 1 : 0, 1);
        P.eq("state.picture_dial_drawn", !a.png.empty() && !b.png.empty() && a.png != b.png ? 1 : 0, 1);
        P.eq("state.picture_user_dial", processor.getParameters()[0]->getValue() == 0.5f ? 1 : 0, 1);
        // at's state: the dials as the render left them (PictureState::dials) drawn, the user's unmoved; and refused
        // when the program that plays is no longer the one rendered.
        pg::PictureState at;
        at.frames.resize(1);
        at.sources = port.status().sources;
        at.about = "0.50 s into a render";
        at.dials = { { "gain", 0.9f } };
        const pg::PanelPicture high = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        at.dials = { { "gain", 0.1f } };
        const pg::PanelPicture low = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        P.eq("state.at_dials_drawn", !high.png.empty() && !low.png.empty() && high.png != low.png
                                         && high.about.find("the displays and the dials show the state 0.50 s into a render") != std::string::npos
                                         ? 1 : 0, 1);
        P.eq("state.at_user_dial", processor.getParameters()[0]->getValue() == 0.5f ? 1 : 0, 1);
        if (!at.sources.empty())
            at.sources.front().text += "\n// changed\n";
        const pg::PanelPicture other = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        P.eq("state.at_other_program", other.png.empty() && other.why.find("changed while at rendered") != std::string::npos ? 1 : 0, 1);
        if (!a.png.empty())
            if (std::FILE* f = std::fopen(probe::outPath("gui.panel.state.picture.png").c_str(), "wb"))
            {
                std::fwrite(a.png.data(), 1, a.png.size(), f);
                std::fclose(f);
            }
    }

    // ---- SETTLE --------------------------------------------------------------------------------------------------------
    // A steady tone's scope and a field scope of a constant stand still while the audio plays (Panel.h "Unchanged
    // frames": the panel then runs at the calm rate), and move when what they show changes.
    {
        const std::string params = "    param float gain [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                                   "    float hold, wob;\n";
        const std::string wob = "        wob += 0.01f;\n        if (wob > 1) wob = 0;\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n" + params + "    float ph;\n"
                                       "    void start(float n, float v) {}\n"
                                       "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                                       "        for (int i = 0; i < n; ++i)\n        {\n"
                                       "            const float s = gain[i] * sinf(6.28318531f * ph);\n"
                                       "            ph += 220.0f / 48000;\n            if (ph >= 1) ph -= 1;\n"
                                       "            left[i] += s;\n            right[i] += s;\n        }\n"
                                       "        hold = level[n - 1];\n" + wob + "        return true;\n    }\n};\n"
                                     : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                                       "        for (int i = 0; i < n; ++i)\n        {\n"
                                       "            left[i] *= gain[i];\n            right[i] *= gain[i];\n        }\n"
                                       "        hold = level[n - 1];\n" + wob + "    }\n};\n";
        const std::string settleGui = "panel { row { scope out label=\"WAVE\" width=240 height=120; scope hold label=\"HOLD\" width=240 height=120 }\n"
                                      "        row { knob gain; knob level } }\n";
        load(processor, { { pg::kProgramName, main }, { "panel.gui", settleGui } });
        processor.prepareToPlay(48000.0, 256);
        settleRun(processor, host);
        panel.setPanelView(true);
        host.settle();
        P.eq("settle.compiles", port.status().ok && port.status().notes.empty() ? 1 : 0, 1);
        if (!port.status().notes.empty() || !port.status().errors.empty())
            std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
        const pg::ui::DisplayItem* wave = nullptr;
        const pg::ui::DisplayItem* field = nullptr;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (item.shown() && item.display().kind == pg::DisplayKind::scope)
                (item.display().audio() ? wave : field) = &item;
        }
        P.eq("settle.scopes_shown", wave != nullptr && field != nullptr ? 1 : 0, 1);
        // A sixtieth of a second of audio (three blocks of 256): the effect's input a 220 Hz sine at 0.5, the synth's
        // voice its own (its note held from the first block).
        juce::AudioBuffer<float> buffer(2, 256);
        double phase = 0.0;
        bool noted = false;
        const auto sixtieth = [&](int) {
            for (int b = 0; b < 3; ++b)
            {
                juce::MidiBuffer midi;
                if (kInstrument && !noted)
                {
                    midi.addEvent(juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(100)), 0);
                    noted = true;
                }
                for (int i = 0; i < 256; ++i)
                {
                    const float v = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                processor.processBlock(buffer, midi);
            }
        };
        // n frames at 60 Hz with audio; how many of them found the display unsettled.
        const auto unsettled = [&](const pg::ui::DisplayItem* item, int n) {
            int k = 0;
            for (int f = 0; f < n; ++f)
            {
                sixtieth(f);
                host.tick(1);
                k += item != nullptr && !item->settled() ? 1 : 0;
            }
            return k;
        };
        (void)unsettled(nullptr, 180);                   // the tone starts; the field's scope fills (2.5 s)
        constexpr int stillFrames = 60;
        int waveUnsettled = 0, fieldUnsettled = 0;
        for (int f = 0; f < stillFrames; ++f)
        {
            sixtieth(f);
            host.tick(1);
            waveUnsettled += wave != nullptr && !wave->settled() ? 1 : 0;
            fieldUnsettled += field != nullptr && !field->settled() ? 1 : 0;
        }
        std::printf("settle: of %d frames of a steady tone, the scope unsettled in %d, the field scope in %d\n", stillFrames,
                    waveUnsettled, fieldUnsettled);
        P.eq("settle.audio_scope_still", waveUnsettled, 0);
        P.eq("settle.field_scope_still", fieldUnsettled, 0);
        // The panel's frames as the items ask them (probe/Cadence.h): the calm rate, about 12 a second.
        const probe::Cadence calm = probe::runCadence(host, 120, sixtieth, [&] { return panel.moving(); });
        std::printf("settle: a steady tone's panel, %.1f frames a second\n", calm.framesPerSecond());
        P.le("settle.calm_frames_per_second", calm.framesPerSecond(), 12.5);
        // The tone's level moved by a few hundredths of a px of the trace: no move; by px: a move.
        juce::AudioProcessorParameter& gain = *processor.getParameters()[0];
        juce::AudioProcessorParameter& level = *processor.getParameters()[1];
        gain.setValueNotifyingHost(0.501f);
        P.eq("settle.audio_scope_sub_px", unsettled(wave, 30), 0);
        gain.setValueNotifyingHost(0.8f);
        P.ge("settle.audio_scope_moves", unsettled(wave, 6), 1);
        // The field's value steps: its scope moves, then stands still again once its points are all the new value.
        level.setValueNotifyingHost(0.75f);
        P.ge("settle.field_scope_steps", unsettled(field, 6), 1);
        (void)unsettled(field, 180);
        P.eq("settle.field_scope_still_again", unsettled(field, 30), 0);
        // Its ring: a ramp longer than its points, oldest first, the newest last.
        for (int f = 0; f < 200; ++f)
        {
            level.setValueNotifyingHost(0.1f + 0.8f * static_cast<float>(f) / 199.0f);
            sixtieth(f);
            host.tick(1);
        }
        const std::vector<float> points = field != nullptr ? field->points() : std::vector<float>{};
        bool rising = points.size() == static_cast<std::size_t>(pg::ui::DisplayItem::kHistory);
        for (std::size_t k = 1; k < points.size(); ++k)
            rising = rising && points[k] >= points[k - 1];
        P.eq("settle.ring_oldest_first", rising && points.front() < points.back() ? 1 : 0, 1);
        P.near("settle.ring_newest_last", points.empty() ? -9.0 : static_cast<double>(points.back()), 2.0 * 0.9 - 1.0, 0.01);
        writePng(P, host, "gui.panel.settle.png", "settle");

        // Number displays (the output's peak, and a field that moves each block): the readout, made again eight times
        // a second, changes at each refresh. That is no motion but the item's draw key: the frame that made it is
        // recorded (needsDraw), and the panel stays at the calm rate.
        load(processor, { { pg::kProgramName, main },
                          { "panel.gui", "panel { row { number out label=\"PEAK\"; number wob label=\"WOB\" }\n        row { knob gain; knob level } }\n" } });
        settleRun(processor, host);
        noted = false;                                   // the synth's note held again, in the new program
        P.eq("settle.numbers_compile", port.status().ok && port.status().notes.empty() ? 1 : 0, 1);
        if (!port.status().notes.empty() || !port.status().errors.empty())
            std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
        const pg::ui::DisplayItem* number = nullptr;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (item.shown() && item.display().kind == pg::DisplayKind::number && !item.display().audio())
                number = &item;
        }
        P.eq("settle.number_shown", number != nullptr ? 1 : 0, 1);
        (void)unsettled(nullptr, 30);
        host.draw();
        std::string drawnReadout = number != nullptr ? number->readout() : std::string();
        uint64_t key = number != nullptr ? number->drawKey() : 0;
        int changes = 0, keyMissed = 0, frameMissed = 0, numberUnsettled = 0;
        const probe::Cadence numbers = probe::runCadence(host, 240, sixtieth, [&] {
            if (number != nullptr)
            {
                numberUnsettled += number->settled() ? 0 : 1;
                if (number->readout() != drawnReadout)
                {
                    ++changes;
                    keyMissed += number->drawKey() != key ? 0 : 1;
                    frameMissed += panel.needsDraw() ? 0 : 1;
                }
                key = number->drawKey();
            }
            if (panel.needsDraw())                       // as EditorHost records a frame
            {
                host.draw();
                drawnReadout = number != nullptr ? number->readout() : std::string();
            }
            return panel.moving();
        });
        std::printf("settle: number displays, %d readouts in %d frames, %.1f frames a second\n", changes, numbers.frames,
                    numbers.framesPerSecond());
        P.ge("settle.number_readouts", changes, 16);
        P.eq("settle.number_no_motion", numberUnsettled, 0);
        P.eq("settle.number_key_follows", keyMissed, 0);
        P.eq("settle.number_frame_recorded", frameMissed, 0);
        P.le("settle.number_frames_per_second", numbers.framesPerSecond(), 12.5);
    }

    // ---- SPECTRUM ------------------------------------------------------------------------------------------------------
    // A source's spectrum, made once for all its displays when a new block comes (DisplayItem.h's Spectrum), against
    // the FFT each display ran for itself every frame before.
    {
        constexpr int kN = pg::ui::DisplayItem::kScopeSamples;
        constexpr int kPoints = pg::ui::DisplayItem::kSpectrumPoints;
        // Fixed signals of kN samples at a rate: signal s's sample i.
        const char* const names[] = { "sine_1k", "sine_37hz", "two_tones", "noise", "impulse", "sweep", "sine_19k",
                                      "square", "silence" };
        constexpr int kSignals = static_cast<int>(sizeof names / sizeof names[0]);
        const auto fill = [&](int s, double rate, std::vector<float>& x) {
            uint32_t noise = 0x2545F491u;
            constexpr double twoPi = 2.0 * std::numbers::pi;
            for (int i = 0; i < kN; ++i)
            {
                const double t = i / rate;
                noise = noise * 1664525u + 1013904223u;
                const double u = static_cast<double>(noise >> 8) / 8388608.0 - 1.0;
                double v = 0.0;
                switch (s)
                {
                    case 0: v = std::sin(twoPi * 1000.0 * t); break;
                    case 1: v = 0.1 * std::sin(twoPi * 37.3 * t + 0.3); break;
                    case 2: v = 0.5 * std::sin(twoPi * 100.0 * t) + 0.01 * std::sin(twoPi * 5000.0 * t + 1.0); break;
                    case 3: v = 0.1 * u; break;
                    case 4: v = i == kN / 2 ? 1.0 : 0.0; break;
                    case 5: v = 0.5 * std::sin(twoPi * 20.0 * (kN / rate) / std::log(1000.0) * (std::pow(1000.0, i / static_cast<double>(kN)) - 1.0)); break;
                    case 6: v = 0.3 * std::sin(twoPi * 19000.0 * t); break;
                    case 7: v = std::fmod(220.0 * t, 1.0) < 0.5 ? 0.25 : -0.25; break;
                    default: v = 0.0; break;
                }
                x[static_cast<std::size_t>(i)] = static_cast<float>(v);
            }
        };
        std::vector<float> x(static_cast<std::size_t>(kN));
        pg::ui::Spectrum analyser;
        double worstExact = 0.0;
        for (int s = 0; s < kSignals; ++s)
        {
            double most = 0.0, mostUndrawn = 0.0, newExact = 0.0, oldExact = 0.0;
            for (const double rate : { 44100.0, 48000.0, 96000.0 })
            {
                fill(s, rate, x);
                const std::vector<float> before = oldSpectrum(x.data(), kN, static_cast<float>(rate), kPoints);
                const std::vector<float> exact = exactSpectrum(x.data(), kN, static_cast<float>(rate), kPoints);
                analyser.compute(x.data(), static_cast<float>(rate));
                for (int k = 0; k < kPoints; ++k)
                {
                    const float a = before[static_cast<std::size_t>(k)], b = analyser.points()[k];
                    const float e = exact[static_cast<std::size_t>(k)];
                    most = std::max(most, static_cast<double>(std::fabs(drawnDb(a) - drawnDb(b))));
                    mostUndrawn = std::max(mostUndrawn, static_cast<double>(std::fabs(a - b)));
                    newExact = std::max(newExact, static_cast<double>(std::fabs(drawnDb(b) - drawnDb(e))));
                    oldExact = std::max(oldExact, static_cast<double>(std::fabs(drawnDb(a) - drawnDb(e))));
                }
            }
            std::printf("spectrum: %-9s at 44.1, 48 and 96 kHz, the most a point differs as drawn: old and new %.5f dB, new "
                        "and exact %.5f, old and exact %.5f (old and new %.4f under the drawn floor too)\n", names[s], most,
                        newExact, oldExact, mostUndrawn);
            P.le(std::string("spectrum.match_") + names[s], most, 0.02);
            worstExact = std::max(worstExact, newExact);
        }
        P.le("spectrum.exact_db", worstExact, 0.003);
        // An FFT's cost, the old against the new (threadSeconds(); printed, as the machine may be busy).
        {
            fill(3, 48000.0, x);
            constexpr int kRuns = 300;
            float sink = 0.0f;
            const double t0 = threadSeconds();
            for (int r = 0; r < kRuns; ++r)
                sink += oldSpectrum(x.data(), kN, 48000.0f, kPoints)[static_cast<std::size_t>(r % kPoints)];
            const double t1 = threadSeconds();
            for (int r = 0; r < kRuns; ++r)
            {
                analyser.compute(x.data(), 48000.0f);
                sink += analyser.points()[r % kPoints];
            }
            const double t2 = threadSeconds();
            std::printf("spectrum: one FFT and its points, %.1f us before, %.1f us now (%.1fx)%s\n", 1.0e6 * (t1 - t0) / kRuns,
                        1.0e6 * (t2 - t1) / kRuns, (t1 - t0) / std::max(t2 - t1, 1.0e-9), sink == 0.0f ? " " : "");
        }

        // A panel: two spectra of the output (the effect's also one of its input) while the effect's input plays noise
        // and the synth's voice middle C, at a gain.
        const std::string params = "    param float gain [[fminus::range(0, 1)]] [[fminus::default(1)]];\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n" + params + "    float ph;\n    float step;\n"
                                       "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; }\n"
                                       "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                                       "        for (int i = 0; i < n; ++i)\n        {\n"
                                       "            const float s = 0.25f * gain[i] * sinf(6.28318531f * ph);\n"
                                       "            ph += step;\n            if (ph >= 1) ph -= 1;\n"
                                       "            left[i] += s;\n            right[i] += s;\n        }\n"
                                       "        return true;\n    }\n};\n"
                                     : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                                       "        for (int i = 0; i < n; ++i)\n        {\n"
                                       "            left[i] *= gain[i];\n            right[i] *= gain[i];\n        }\n    }\n};\n";
        const std::string spectraGui = kInstrument
            ? "panel { row { spectrum out label=\"OUT A\" width=240 height=120; spectrum out label=\"OUT B\" width=240 height=120 }\n"
              "        row { knob gain } }\n"
            : "panel { row { spectrum out label=\"OUT A\" width=240 height=120; spectrum out label=\"OUT B\" width=240 height=120;"
              " spectrum in label=\"IN\" width=240 height=120 }\n        row { knob gain } }\n";
        load(processor, { { pg::kProgramName, main }, { "panel.gui", spectraGui } });
        processor.prepareToPlay(48000.0, 512);
        settleRun(processor, host);
        panel.setPanelView(true);
        host.settle();
        P.eq("spectrum.compiles", port.status().ok && port.status().notes.empty() ? 1 : 0, 1);
        if (!port.status().notes.empty() || !port.status().errors.empty())
            std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
        std::vector<const pg::ui::DisplayItem*> outs;
        const pg::ui::DisplayItem* ins = nullptr;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (item.shown() && item.display().kind == pg::DisplayKind::spectrum)
            {
                if (item.display().source == "in")
                    ins = &item;
                else
                    outs.push_back(&item);
            }
        }
        P.eq("spectrum.shown", outs.size() == 2 && (kInstrument || ins != nullptr) ? 1 : 0, 1);
        juce::AudioBuffer<float> buffer(2, 512);
        uint32_t noise = 0x9E3779B9u;
        bool noted = false;
        const auto block = [&] {
            juce::MidiBuffer midi;
            if (kInstrument && !noted)
            {
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
                noted = true;
            }
            for (int i = 0; i < 512; ++i)
            {
                noise = noise * 1664525u + 1013904223u;
                const float v = kInstrument ? 0.0f : 0.1f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, v);
            }
            processor.processBlock(buffer, midi);
        };
        for (int f = 0; f < 30; ++f)                     // the noise starts and fills the scopes' rings
        {
            block();
            host.tick(1);
        }
        // A block every second frame: one FFT a source for each, none on the frames between.
        const pg::ui::DisplayFeed& feed = panel.displayFeed();
        const int outBefore = feed.outSpectrum.computed(), inBefore = feed.inSpectrum.computed();
        int blocks = 0, apart = 0;
        for (int f = 0; f < 120; ++f)
        {
            if (f % 2 == 0)
            {
                block();
                ++blocks;
            }
            host.tick(1);
            if (outs.size() == 2 && outs[0]->spectrum() != outs[1]->spectrum())
                ++apart;
        }
        const int outFfts = feed.outSpectrum.computed() - outBefore, inFfts = feed.inSpectrum.computed() - inBefore;
        std::printf("spectrum: %d blocks in 120 frames, %d FFTs of the output for its two displays, %d of the input\n",
                    blocks, outFfts, inFfts);
        P.eq("spectrum.output_ffts_per_block", outFfts, blocks);
        P.eq("spectrum.input_ffts_per_block", inFfts, kInstrument ? 0 : blocks);
        P.eq("spectrum.displays_agree", apart, 0);
        const auto strongest = [](const pg::ui::DisplayItem* item) {
            const std::vector<float>& s = item->spectrum();
            return s.empty() ? -200.0f : *std::max_element(s.begin(), s.end());
        };
        if (!outs.empty() && kInstrument)
            P.near("spectrum.level_db", static_cast<double>(strongest(outs[0])), -12.04, 3.0);   // a sine at 0.25
        else if (!outs.empty())
            P.in("spectrum.level_db", static_cast<double>(strongest(outs[0])), -60.0, -25.0);    // noise at 0.1
        writePng(P, host, "gui.panel.spectrum.png", "spectrum");
        // Silence: once the rings hold nothing else, blocks still come but no FFT runs, and the displays fall 30 dB a
        // second all the same.
        processor.getParameters()[0]->setValueNotifyingHost(0.0f);
        for (int f = 0; f < 12; ++f)                     // the gain glides to 0 (20 ms), then 2048 samples of silence
        {
            block();
            host.tick(1);
        }
        const int silentBefore = feed.outSpectrum.computed();
        const float top = outs.empty() ? -200.0f : strongest(outs[0]);
        for (int f = 0; f < 30; ++f)
        {
            block();
            host.tick(1);
        }
        const float fallen = outs.empty() ? -200.0f : strongest(outs[0]);
        std::printf("spectrum: silence, %d FFTs in 30 frames with a block each; the strongest point %.2f dB, then %.2f\n",
                    feed.outSpectrum.computed() - silentBefore, static_cast<double>(top), static_cast<double>(fallen));
        P.eq("spectrum.silence_no_fft", feed.outSpectrum.computed() - silentBefore, 0);
        P.near("spectrum.falls_db_in_half_a_second", static_cast<double>(top - fallen), 15.0, 0.05);

        // ---- HIDDEN ----------------------------------------------------------------------------------------------------
        // The sound back up; then the host stops ticking the panel (a minimised window) while its idle calls go on.
        processor.getParameters()[0]->setValueNotifyingHost(1.0f);
        for (int f = 0; f < 8; ++f)
        {
            block();
            host.tick(1);
        }
        pg::Meters m;
        port.meters(m);
        const int readers = port.meterReaders();
        P.eq("hidden.reads_while_ticked", panel.readingMeters() && readers >= 1 ? 1 : 0, 1);
        P.ge("hidden.levels_while_ticked", static_cast<double>(m.outPeak[0]), 0.01);
        double t = host.nowSeconds();
        for (int k = 0; k < 4; ++k)
            panel.idle(t += 0.1);
        P.eq("hidden.not_at_once", port.meterReaders(), readers);
        for (int k = 0; k < 8; ++k)
            panel.idle(t += 0.1);
        P.eq("hidden.lets_go", port.meterReaders(), readers - 1);
        P.eq("hidden.panel_says", panel.readingMeters() ? 0 : 1, 1);
        for (int f = 0; f < 4; ++f)
            block();
        port.meters(m);
        P.eq("hidden.nothing_measured", m.outPeak[0] == 0.0f && m.outRms[1] == 0.0f ? 1 : 0, 1);
        host.tick(1);                                    // shown again
        P.eq("hidden.back_at_its_tick", port.meterReaders() == readers && panel.readingMeters() ? 1 : 0, 1);
        block();
        port.meters(m);
        P.ge("hidden.measured_again", static_cast<double>(m.outPeak[0]), 0.01);
        // A calm panel ticked about 12 times a second, its idle calls 10 a second between, never lets go (3 s).
        {
            bool kept = true;
            double nextIdle = host.nowSeconds() + 0.05;
            for (int k = 0; k < 36; ++k)
            {
                host.tick(1, 1.0f / 12.0f);              // a tick, then the clock 1/12 s on (and HeadlessHost's idle call)
                for (; nextIdle < host.nowSeconds(); nextIdle += 0.1)
                {
                    panel.idle(nextIdle);
                    kept = kept && panel.readingMeters();
                }
            }
            P.eq("hidden.calm_keeps_it", kept && port.meterReaders() == readers ? 1 : 0, 1);
        }
        // A second panel, hidden and then closed: its reader goes once, never another's.
        {
            {
                pg::ui::Panel other(processor.ports(), port);
                funkgui::HeadlessHost otherHost(other, 0, 2.0f);
                otherHost.tick(1);
                P.eq("hidden.second_reads", port.meterReaders(), readers + 1);
                double u = otherHost.nowSeconds();
                for (int k = 0; k < 12; ++k)
                    other.idle(u += 0.1);
                P.eq("hidden.second_lets_go", port.meterReaders(), readers);
            }
            P.eq("hidden.second_closed", port.meterReaders(), readers);
        }
    }
    return P.finish();
}
