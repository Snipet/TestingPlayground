// PROBE name=gui.menus timeout=300
//
// gui.menus: the dropdowns (editor/Dropdown.h): every trigger drawn one way and each menu's rows built one way, headless
// over real Processors, and a picture of each menu as it opens, in both themes: the Panel's frame with its trigger
// pressed and, over it, the menu FunkGui's MenuLook draws for the Panel's request (JUCE's popup menu, shown in a
// component as fg.menulook shows its own), cut to the two, at 2x, to <build>/probe-out/:
//   gui.menus.<menu>.png and gui.menus.<menu>.paper.png, for examples, library, projects (a project's context menu in
//   PROJECTS), projects_tag (PROJECTS' tag filter), projects_sort (PROJECTS' order), model, thinking,
//   thinking_openrouter (the thinking level of a model OpenRouter's list says reasons, against a scripted server), zoom, chats (the chat card's list of the project's
//   chats), provider (the key dialog's Provider pill), settings (the output's select), panel (a .gui file's menu
//   control) and schematic (a netlist's canvas menu); on Linux with no X display, skipped, saying so (noDisplay)
// and checks:
//   TRIGGER  each trigger (Examples, Library, Projects, the zoom, the model, the thinking level, the key dialog's
//            Provider, the output's select, a .gui file's menu control): a combo box to a screen reader, its value its label; its chevron
//            kChevronInset from its right edge; a click opens its menu under it (the anchor its bounds), at least as
//            wide when it holds choices; while the menu is open the trigger's pill has the accent's edge and the one
//            open fill (every trigger's the same), and once it is dismissed neither; Projects' while its card is open;
//            the focus ring (Tab) 2 px outside its pill; the .gui menu at its natural width showing every value whole,
//            and Return, Space and a screen reader's press and showMenu opening its menu as a click does (its value
//            kept); the Schematic view's More: its chevron, its label whole, its menu under it, open and closed
//   ROWS     EXAMPLES an item an example; LIBRARY every file once under its category's header (the library's order),
//            its label starting with its file name (the menu's type-ahead), the files the program has ticked, no
//            disabled row standing for a header; MODEL its two headers, a radio set under each, the current one dotted
//            and highlighted when it opens (selectedId); THINKING, ZOOM, the output's select and the .gui menu a radio
//            set, the current one dotted and highlighted; THINKING on OpenRouter the model's six levels, its default
//            dotted; CHATS its header (the project's name), its chats a radio set,
//            the newest first, the one shown dotted and highlighted, each one's time in the shortcut column, then New
//            chat, Rename and Delete; PROVIDER its header, every provider a radio set, the one shown dotted; the schematic's canvas menu every key in the shortcut column, none left in a label;
//            a choice made from each doing what it did
#include "Probe.h"

#include "../plugin/FakeClaude.h"

#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "editor/SchematicView.h"
#include "editor/SettingsCard.h"
#include "editor/Theme.h"
#include "plugin/ChatStore.h"
#include "plugin/DevicePort.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pg = playground;
namespace dd = playground::ui::dropdown;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    const char* const kThemeName[2] = { "", ".paper" };

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    bool same(const funkgui::Rect& a, const funkgui::Rect& b, float tol = 0.5f)
    {
        return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.w - b.w) <= tol
            && std::abs(a.h - b.h) <= tol;
    }

    uint32_t packed(funkgui::Col c)
    {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16
             | static_cast<uint32_t>(c.a) << 24;
    }

    // ---- what a frame drew --------------------------------------------------------------------------------------------

    // A rounded rectangle as drawn: its rect (the apron taken off), its fill, its edge and the edge's width.
    struct Box
    {
        funkgui::Rect r;
        uint32_t fill = 0, edge = 0;
        float edgeW = 0.0f;
    };

    int kindOf(const funkgui::Prim& p) { return static_cast<int>(p.d2[2] + 0.5f); }

    std::vector<Box> boxes(const funkgui::PrimList& f)
    {
        std::vector<Box> out;
        for (const funkgui::Prim& p : f.prims)
            if (kindOf(p) == static_cast<int>(funkgui::PrimKind::rrect))
            {
                const float pad = 1.5f + p.d2[1];
                out.push_back({ { p.x0 + pad, p.y0 + pad, p.x1 - p.x0 - 2.0f * pad, p.y1 - p.y0 - 2.0f * pad }, p.c0, p.c1, p.d2[0] });
            }
        return out;
    }

    // A box drawn at `r`: its apron may be clipped (a .gui's area clips its items), so its sides within 1.6 px.
    bool drawnAt(const Box& b, const funkgui::Rect& r) { return same(b.r, r, 1.6f) && std::abs(b.r.centreY() - r.centreY()) <= 0.8f; }

    // The last box drawn at `r` (a trigger's pill), if any.
    std::optional<Box> pillAt(const funkgui::PrimList& f, const funkgui::Rect& r)
    {
        std::optional<Box> found;
        for (const Box& b : boxes(f))
            if (drawnAt(b, r))
                found = b;
        return found;
    }

    // The focus ring: a box kRingGap outside `r`, its edge the accent, kRingPx wide, nothing filled. A .gui's area clips
    // what lies outside it (a control at its edge), so the box drawn is within that rect and covers `r` but its edge.
    bool ringAround(const funkgui::PrimList& f, const funkgui::Rect& r, funkgui::Col accent)
    {
        const float g = dd::kRingGap;
        const funkgui::Rect outer{ r.x - g - 0.6f, r.y - g - 0.6f, r.w + 2.0f * g + 1.2f, r.h + 2.0f * g + 1.2f };
        const funkgui::Rect inner{ r.x + g, r.y + g, r.w - 2.0f * g, r.h - 2.0f * g };
        for (const Box& b : boxes(f))
            if (b.edge == packed(accent) && std::abs(b.edgeW - dd::kRingPx) < 0.01f && (b.fill >> 24) == 0
                && b.r.x >= outer.x && b.r.y >= outer.y && b.r.right() <= outer.right() && b.r.bottom() <= outer.bottom()
                && b.r.x <= inner.x && b.r.y <= inner.y && b.r.right() >= inner.right() && b.r.bottom() >= inner.bottom())
                return true;
        return false;
    }

    // A chevron's strokes kChevronInset from the right edge of `r`, inside it.
    bool chevronAt(const funkgui::PrimList& f, const funkgui::Rect& r)
    {
        const float cx = r.right() - dd::kChevronInset, cy = r.centreY();
        int strokes = 0;
        for (const funkgui::Prim& p : f.prims)
            if (kindOf(p) == static_cast<int>(funkgui::PrimKind::segment))
            {
                const float mx = 0.5f * (p.x0 + p.x1), my = 0.5f * (p.y0 + p.y1);
                strokes += std::abs(mx - cx) <= 0.5f * dd::kChevronPx && std::abs(my - cy) <= 0.5f * dd::kChevronPx ? 1 : 0;
            }
        return strokes >= 2;
    }

    // ---- the pictures -------------------------------------------------------------------------------------------------

    // The Panel's frame behind the menu, as a window on a screen with room under it (a menu opens below its trigger
    // when it fits there, as on a display taller than the window).
    struct Backdrop final : juce::Component
    {
        juce::Image frame;
        juce::Rectangle<float> window;
        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff5a5d63));
            g.drawImage(frame, window);
        }
    };

    // Every backdrop and look stays until the probe ends: JUCE deletes a dismissed menu's window later (here at its
    // shutdown), and the looks go before it does (MenuLook.h); the looks are declared last, so they go first.
    struct Pictures
    {
        std::vector<std::unique_ptr<Backdrop>> backdrops;
        std::vector<std::unique_ptr<funkgui::MenuLook>> looks;
    };

    juce::Image toJuce(const funkgui::Image& img)
    {
        juce::Image out(juce::Image::ARGB, std::max(1, img.w), std::max(1, img.h), true);
        juce::Image::BitmapData d(out, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
            {
                const uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x)) * 4u];
                d.setPixelColour(x, y, juce::Colour(p[0], p[1], p[2], p[3]));
            }
        return out;
    }

    // The menu `request` opens, over the Panel's frame as the host draws it now (the trigger pressed), cut to the trigger
    // and the menu with a margin: gui.menus.<name><theme>.png.
    void picture(funkgui::test::Probe& P, Pictures& pics, funkgui::HeadlessHost& host, const pg::ui::Panel& panel,
                 const funkgui::MenuRequest& request, const std::string& name, int theme)
    {
        const std::string file = "gui.menus." + name + kThemeName[theme] + ".png";
        if (const std::string why = probe::noDisplay(); !why.empty())
        {
            probe::skip(P, "png." + name + kThemeName[theme], why);
            return;
        }
        auto backdrop = std::make_unique<Backdrop>();
        backdrop->frame = toJuce(funkgui::rasterise(host.draw(), funkgui::FontService::get().atlas(), 2));
        // Where the backdrop sits "on screen" (it is on no desktop): the display's top left, so that JUCE, which keeps a
        // menu inside the display, has the most room under the trigger.
        const juce::Point<int> at = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.getTopLeft()
                                        : juce::Point<int>{};
        backdrop->window = { 0.0f, 0.0f, static_cast<float>(panel.width()), static_cast<float>(panel.height()) };
        backdrop->setBounds(at.x, at.y, panel.width(), panel.height() + 720);
        auto look = std::make_unique<funkgui::MenuLook>(request.theme);
        juce::PopupMenu menu = funkgui::MenuLook::popupMenuFor(request);
        menu.setLookAndFeel(look.get());
        const funkgui::Rect& a = request.anchor;
        const juce::Rectangle<int> anchor = juce::Rectangle<float>(a.x, a.y, a.w, a.h).toNearestInt();
        auto options = juce::PopupMenu::Options()
                           .withParentComponent(backdrop.get())
                           .withTargetScreenArea(look->targetAreaFor(anchor + at))
                           .withMaximumNumColumns(1);
        if (request.minWidth > 0.0f)                     // as the editor's host gives them (HostServicesJuce)
            options = options.withMinimumWidth(juce::roundToInt(request.minWidth) + 2 * look->shadowPx());
        if (request.selectedId > 0)
            options = options.withInitiallySelectedItem(request.selectedId).withItemThatMustBeVisible(request.selectedId);
        menu.showMenuAsync(options, [](int) {});
        juce::Component* window = nullptr;
        for (int i = backdrop->getNumChildComponents(); --i >= 0;)
            if (backdrop->getChildComponent(i)->getName() == "menu")
                window = backdrop->getChildComponent(i);
        bool written = false;
        if (window != nullptr)
        {
            const juce::Rectangle<int> crop = window->getBounds().getUnion(anchor).expanded(16).getIntersection(backdrop->getLocalBounds());
            const juce::Image shot = backdrop->createComponentSnapshot(crop, true, 2.0f);
            const std::string path = probe::outPath(file);
            const juce::File f(juce::String::fromUTF8(path.c_str()));
            f.deleteFile();
            juce::FileOutputStream out(f);
            juce::PNGImageFormat png;
            written = out.openedOk() && png.writeImageToStream(shot, out);
            if (written)
                std::printf("wrote %s\n", path.c_str());
        }
        juce::PopupMenu::dismissAllActiveMenus();
        P.eq("png." + name + kThemeName[theme], written ? 1 : 0, 1);
        pics.backdrops.push_back(std::move(backdrop));
        pics.looks.push_back(std::move(look));
    }

    // ---- the triggers -------------------------------------------------------------------------------------------------

    // The rows, once: the first theme's pass checks, the second only draws its pictures.
    struct Rows
    {
        funkgui::test::Probe& P;
        bool on = true;
        void eq(const std::string& k, int64_t got, int64_t want)
        {
            if (on)
                P.eq(k, got, want);
        }
        void ge(const std::string& k, double got, double bound)
        {
            if (on)
                P.ge(k, got, bound);
        }
        void near(const std::string& k, double got, double want, double tol)
        {
            if (on)
                P.near(k, got, want, tol);
        }
    };

    // A trigger's accessibility item: the smallest one whose bounds hold `p`.
    std::optional<funkgui::A11yItem> itemAt(const funkgui::HeadlessHost& host, funkgui::Point p)
    {
        std::optional<funkgui::A11yItem> best;
        for (const funkgui::A11yItem& a : host.accessibility())
            if (a.visible && a.bounds.contains(p) && (!best || a.bounds.w * a.bounds.h < best->bounds.w * best->bounds.h))
                best = a;
        return best;
    }

    // What every trigger's check found, for the rows that compare them.
    struct Seen
    {
        std::map<std::string, funkgui::Rect> rects;
        std::map<std::string, uint32_t> openFills;
    };

    // A trigger at `at`: its item, its chevron, its menu's anchor and width, its open and dismissed looks. The menu is
    // left open (pending) for the caller, who reads its rows and answers it; returns it.
    // `box`: the trigger's rect when it is not its accessibility item's (a .gui menu's box, under its label).
    const funkgui::MenuRequest* openTrigger(Rows& P, funkgui::HeadlessHost& host, const std::string& name, funkgui::Point at,
                                            bool choices, Seen& seen, std::optional<funkgui::Rect> box = std::nullopt)
    {
        const std::string k = "trigger." + name + ".";
        host.move(at.x, at.y);
        host.tick(1);
        const std::optional<funkgui::A11yItem> item = itemAt(host, at);
        P.eq(k + "combo_box", item && item->role == funkgui::A11yRole::comboBox && !item->value.empty() ? 1 : 0, 1);
        const funkgui::Rect r = box ? *box : item ? item->bounds : funkgui::Rect{};
        seen.rects[name] = r;
        P.eq(k + "chevron", chevronAt(host.draw(), r) ? 1 : 0, 1);
        host.click(at.x, at.y);
        host.tick(1);
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq(k + "opens", m != nullptr ? 1 : 0, 1);
        if (m == nullptr)
            return nullptr;
        P.eq(k + "under_it", same(m->anchor, r) ? 1 : 0, 1);
        if (choices)
            P.eq(k + "as_wide", m->minWidth >= r.w - 0.5f ? 1 : 0, 1);
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
        const std::optional<Box> open = pillAt(host.draw(), r);
        P.eq(k + "open_edge", open && open->edge == packed(th.accent) && std::abs(open->edgeW - 1.0f) < 0.01f ? 1 : 0, 1);
        if (open)
            seen.openFills[name] = open->fill;
        return m;
    }

    // The menu answered (chosen or dismissed): the trigger's pill no longer open.
    void closedTrigger(Rows& P, funkgui::HeadlessHost& host, const std::string& name, const Seen& seen)
    {
        host.tick(1);
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
        const auto r = seen.rects.find(name);
        const std::optional<Box> pill = r != seen.rects.end() ? pillAt(host.draw(), r->second) : std::nullopt;
        P.eq("trigger." + name + ".closed", r != seen.rects.end() && !(pill && pill->edge == packed(th.accent)) ? 1 : 0, 1);
    }

    // The radio set of a menu's items (no header, no separator): each a radio, one of them checked (`current`), and the
    // request's selectedId on it.
    void radioSet(Rows& P, const std::string& key, const funkgui::MenuRequest& m,
                  const std::vector<funkgui::MenuItem>& items, const std::string& current)
    {
        int radios = 0, checked = 0;
        int currentId = 0;
        for (const funkgui::MenuItem& it : items)
        {
            radios += it.radio ? 1 : 0;
            checked += it.checked ? 1 : 0;
            if (it.checked && it.label.rfind(current, 0) == 0)
                currentId = it.id;
        }
        P.eq(key + ".radio", radios == static_cast<int>(items.size()) && !items.empty() ? 1 : 0, 1);
        P.eq(key + ".one_current", checked == 1 && currentId > 0 ? 1 : 0, 1);
        P.eq(key + ".current_highlighted", currentId > 0 && m.selectedId == currentId ? 1 : 0, 1);
    }

    std::vector<funkgui::MenuItem> choicesOf(const funkgui::MenuRequest& m)
    {
        std::vector<funkgui::MenuItem> out;
        for (const funkgui::MenuItem& it : m.items)
            if (!it.separator && !it.header)
                out.push_back(it);
        return out;
    }

    // Tab (or Shift-Tab) until each trigger's focus ring is drawn; from no focus (Escape first) unless in a card, which
    // Escape would close.
    void focusRings(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::vector<std::string>& names,
                    const Seen& seen, const char* first, const char* again, bool escape = true)
    {
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
        std::map<std::string, bool> found;
        const auto look = [&] {
            const funkgui::PrimList& f = host.draw();
            for (const std::string& n : names)
                if (const auto r = seen.rects.find(n); r != seen.rects.end() && ringAround(f, r->second, th.accent))
                    found[n] = true;
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            if (escape)
            {
                host.keys("escape");
                host.tick(1);
            }
            for (int k = 0; k < 24 && found.size() < names.size(); ++k)
            {
                host.keys(pass == 0 ? first : again);
                host.tick(1);
                look();
            }
        }
        if (escape)
        {
            host.keys("escape");
            host.tick(1);
        }
        for (const std::string& n : names)
            P.eq("trigger." + n + ".focus_ring", found[n] ? 1 : 0, 1);
    }

    // ---- programs and devices -----------------------------------------------------------------------------------------

    // A program with one dial, `wave` (slot 0), and an interface whose menu control sets it.
    std::string waveProgram()
    {
        const std::string param = "    param int wave [[fminus::range(0, 4)]] [[fminus::default(1)]];\n";
        if constexpr (kInstrument)
            return "// Waves: the probe's.\nmodule Voice\n{\n" + param
                   + "    float ph;\n    float step;\n"
                     "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; ph = 0; }\n"
                     "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                     "        for (int i = 0; i < n; ++i)\n        {\n"
                     "            const float s = 0.25f * sinf(6.28318531f * ph);\n"
                     "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n"
                     "        }\n        return true;\n    }\n};\n";
        else
            return "// Waves: the probe's.\nmodule Effect\n{\n" + param
                   + "    void process(float* restrict left, float* restrict right, int n) { }\n};\n";
    }

    const char* const kWaveGui = R"GUI(// The probe's interface.
panel {
    row {
        menu wave "SINE" "SAW" "SQUARE" "TRIANGLE" "NOISE"
        spacer
    }
}
)GUI";

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
        p.settle();
        host.tick(2);
        host.settle();
    }

    // The Standalone's devices, made up: three outputs, a few rates and buffers.
    class FakeDevices final : public pg::DevicePort
    {
    public:
        FakeDevices()
        {
            s_.types = { "CoreAudio" };
            s_.type = "CoreAudio";
            s_.outputs = { "MacBook Pro Speakers", "External Headphones", "Studio Interface" };
            s_.output = "External Headphones";
            s_.outputPairs = { { "Output 1 + 2", true } };
            s_.hasInput = !kInstrument;
            if (s_.hasInput)
            {
                s_.inputs = { "MacBook Pro Microphone", "Studio Interface" };
                s_.input = "Studio Interface";
                s_.inputPairs = { { "Input 1 + 2", true } };
            }
            s_.open = true;
            s_.rates = { 44100.0, 48000.0, 88200.0, 96000.0 };
            s_.rate = 48000.0;
            for (const int b : { 64, 128, 256, 512 })
                s_.buffers.push_back({ b, b * 1000.0 / s_.rate });
            s_.buffer = 256;
            s_.bufferMs = 256 * 1000.0 / s_.rate;
            s_.takesMidi = kInstrument;
            s_.muteApplies = !kInstrument;
        }
        pg::DeviceSetup setup() const override
        {
            pg::DeviceSetup s = s_;
            s.serial = serial_;
            return s;
        }
        uint64_t serial() const override { return serial_; }
        void chooseType(const std::string&) override {}
        void chooseOutput(const std::string& device) override
        {
            s_.output = device;
            ++serial_;
        }
        void chooseInput(const std::string&) override {}
        void chooseRate(double) override {}
        void chooseBuffer(int) override {}
        void switchPair(bool, int, bool) override {}
        void switchMidi(const std::string&, bool) override {}
        void setMuted(bool) override {}
        void playTestTone() override {}

    private:
        pg::DeviceSetup s_;
        uint64_t serial_ = 1;
    };

    std::optional<funkgui::Point> centreTitled(const funkgui::HeadlessHost& host, const std::string& title)
    {
        for (const funkgui::A11yItem& a : host.accessibility())
            if (a.visible && a.title == title)
                return funkgui::Point{ a.bounds.centreX(), a.bounds.centreY() };
        return std::nullopt;
    }
}

PROBE(gui, menus)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File(juce::String(probe::outPath("gui-menus-stores")));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder.getChildFile("projects"));
    pg::chatstore::setDirectoryForTests(folder.getChildFile("keys"));
    Pictures pics;

    // ---- the editor's own: Examples, Library, Projects, the zoom, the model, the thinking level ------------------------
    {
        {
            // A project for PROJECTS' menus, saved into the store (none open): a category and two tags.
            std::string why;
            const bool saved = pg::projectstore::saveVersion("Menu Pad", { { "program.fm", "// Menu Pad: a project for the menus\n" } }, 0, {}, {}, {}, nullptr, &why)
                            && pg::projectstore::setInfo("Menu Pad", { "A pad for the menus' pictures.", "", "Pad", { "warm", "slow" }, {} }, &why);
            P.eq("setup.menu_project", saved ? 1 : 0, 1);
        }
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        processor.settle();
        // CHATS' list: a project with three chats kept on this Mac (today, yesterday: earlier this year but on 1 January,
        // and last year), the second shown. Whatever the time: today's never before midnight, nor past a midnight that
        // comes before the menu opens.
        std::vector<std::string> chatIds;
        {
            pg::SavedVersion saved;
            std::string why;
            port.saveVersion("Glass Bloom", saved, why);
            const std::string owner = processor.chatAgent().chatOwner();
            juce::Time now = juce::Time::getCurrentTime();
            const auto midnight = [](const juce::Time& t) { return juce::Time(t.getYear(), t.getMonth(), t.getDayOfMonth(), 0, 0).toMilliseconds(); };
            if (midnight(juce::Time(now.toMilliseconds() + 30000)) != midnight(now))
            {
                juce::Thread::sleep(31000);                 // midnight within 30 s: the rows read after it
                now = juce::Time::getCurrentTime();
            }
            const int64_t today = std::max(midnight(now), now.toMilliseconds() - 60000);
            const int64_t yesterday = midnight(now) - 12LL * 3600 * 1000;           // noon the day before
            const int64_t lastYear = juce::Time(now.getYear(), 0, 1, 12, 0).toMilliseconds() - 86400000LL * 200;
            const struct
            {
                const char* title;
                int64_t at;
            } made[] = { { "A warm detuned pad with a slow sweep", today },
                         { "Brighter attack, less drive", yesterday },
                         { "Bass ideas", lastYear } };
            for (const auto& c : made)
            {
                pg::chatstore::ChatMeta meta{ pg::chatstore::newId(), c.title, false, c.at, c.at };
                pg::chatstore::Stamp stamp;
                const std::string state = std::string(R"({"version":1,"messages":[],"entries":[{"k":"user","t":")") + c.title + R"("}]})";
                pg::chatstore::saveChat(owner, meta, state, stamp);
                chatIds.push_back(meta.id);
            }
            processor.chatAgent().openChat(chatIds[1]);
        }
        for (int t = 0; t < 2; ++t)
        {
            const bool rows = t == 0;                    // the rows once; the pictures in both themes
            Seen seen;
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, t, 2.0f);
            host.setZoom({ 100, 125, 150, 200 }, 100);
            host.settle();
            Rows R{ P, rows };

            // EXAMPLES: an item an example.
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "examples", panel.centreOfExamples(), true, seen))
            {
                R.eq("rows.examples.items", static_cast<int64_t>(choicesOf(*m).size()), static_cast<int64_t>(std::size(pg::kExamples)));
                picture(P, pics, host, panel, *m, "examples", t);
                host.cancelMenu();
                closedTrigger(R, host, "examples", seen);
            }

            // LIBRARY: every file once, under its category's header, its label from its name; the program's ticked.
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "library", panel.centreOfLibrary(), true, seen))
            {
                std::vector<std::string> headers;
                std::string under;
                int files = 0, misplaced = 0, named = 0, fakeHeaders = 0;
                std::vector<int> ids;
                for (const funkgui::MenuItem& it : m->items)
                {
                    if (it.header)
                    {
                        headers.push_back(it.label);
                        under = it.label;
                        continue;
                    }
                    if (it.separator)
                        continue;
                    if (it.id < 1 || it.id > static_cast<int>(std::size(pg::kLibrary)))
                    {
                        fakeHeaders += !it.enabled ? 1 : 0;
                        continue;
                    }
                    const pg::LibraryFile& f = pg::kLibrary[static_cast<std::size_t>(it.id - 1)];
                    ++files;
                    ids.push_back(it.id);
                    misplaced += pg::ui::upper(f.category) != pg::ui::upper(under) ? 1 : 0;
                    named += it.label.rfind(f.name, 0) == 0 ? 1 : 0;
                }
                std::vector<std::string> categories;
                for (const pg::LibraryFile& f : pg::kLibrary)
                    if (categories.empty() || categories.back() != pg::ui::upper(f.category))
                        categories.push_back(pg::ui::upper(f.category));
                std::vector<std::string> upperHeaders;
                for (const std::string& h : headers)
                    upperHeaders.push_back(pg::ui::upper(h));
                std::sort(ids.begin(), ids.end());
                R.eq("rows.library.every_file_once", files == static_cast<int>(std::size(pg::kLibrary))
                                                         && std::adjacent_find(ids.begin(), ids.end()) == ids.end() ? 1 : 0, 1);
                R.eq("rows.library.headers", upperHeaders == categories ? 1 : 0, 1);
                R.eq("rows.library.under_its_header", misplaced, 0);
                R.eq("rows.library.named_first", named, files);
                R.eq("rows.library.no_fake_headers", fakeHeaders, 0);
                picture(P, pics, host, panel, *m, "library", t);
                host.cancelMenu();
                closedTrigger(R, host, "library", seen);
            }

            // PROJECTS: its card is its list; the trigger open while the card is.
            {
                const funkgui::Point at = panel.centreOfProjects();
                host.move(at.x, at.y);
                host.tick(1);
                const std::optional<funkgui::A11yItem> item = itemAt(host, at);
                R.eq("trigger.projects.combo_box", item && item->role == funkgui::A11yRole::comboBox && !item->value.empty() ? 1 : 0, 1);
                const funkgui::Rect r = item ? item->bounds : funkgui::Rect{};
                seen.rects["projects"] = r;
                R.eq("trigger.projects.chevron", chevronAt(host.draw(), r) ? 1 : 0, 1);
                host.click(at.x, at.y);
                host.settle();
                const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
                const std::optional<Box> open = pillAt(host.draw(), r);
                R.eq("trigger.projects.card_open", panel.projectBrowser().isOpen() ? 1 : 0, 1);
                R.eq("trigger.projects.open_edge", open && open->edge == packed(th.accent) ? 1 : 0, 1);
                if (open)
                    seen.openFills["projects"] = open->fill;
                // PROJECTS' own menus: a project's (a right-click's) and the tag filter's dropdown.
                {
                    pg::ui::ProjectBrowser& b = panel.projectBrowser();
                    const auto W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
                    const funkgui::Point row = b.centreOfRow(pg::ui::ProjectBrowser::List::projects, b.rowOf("Menu Pad"), W, H);
                    funkgui::Mods ctrl;
                    ctrl.ctrl = true;
                    host.click(row.x, row.y, ctrl);
                    host.settle();
                    if (const funkgui::MenuRequest* m = host.pendingMenu())
                    {
                        const auto category = std::find_if(m->items.begin(), m->items.end(), [](const funkgui::MenuItem& i) { return i.label == "Category"; });
                        R.eq("rows.projects.header", !m->items.empty() && m->items[0].header && m->items[0].label == "Menu Pad" ? 1 : 0, 1);
                        R.eq("rows.projects.category_submenu", category != m->items.end() && !category->submenu.empty()
                                                                   && category->submenu.back().label == "New category..." ? 1 : 0, 1);
                        picture(P, pics, host, panel, *m, "projects", t);
                        host.cancelMenu();
                    }
                    else
                        R.eq("rows.projects.menu", 0, 1);
                    host.settle();
                    const funkgui::Rect tag = b.places(W, H).tag;
                    host.click(tag.centreX(), tag.centreY());
                    host.settle();
                    if (const funkgui::MenuRequest* m = host.pendingMenu())
                    {
                        R.eq("rows.projects_tag.any_first", !m->items.empty() && m->items[0].radio && m->items[0].label == "Any tag"
                                                                && m->selectedId == m->items[0].id ? 1 : 0, 1);
                        picture(P, pics, host, panel, *m, "projects_tag", t);
                        host.cancelMenu();
                    }
                    else
                        R.eq("rows.projects_tag.menu", 0, 1);
                    host.settle();
                    // The order: four keys as choices, the current one checked, then Reversed under a separator.
                    const funkgui::Rect sort = b.places(W, H).sort;
                    host.click(sort.centreX(), sort.centreY());
                    host.settle();
                    if (const funkgui::MenuRequest* m = host.pendingMenu())
                    {
                        R.eq("rows.projects_sort.keys", m->items.size() == 6 && m->items[0].radio && m->items[0].label == "Last modified"
                                                            && m->items[3].label == "Name" && m->items[4].separator
                                                            && m->items[5].label == "Reversed" && !m->items[5].radio ? 1 : 0, 1);
                        picture(P, pics, host, panel, *m, "projects_sort", t);
                        host.cancelMenu();
                    }
                    else
                        R.eq("rows.projects_sort.menu", 0, 1);
                    host.settle();
                }
                host.keys("escape");
                host.settle();
                closedTrigger(R, host, "projects", seen);
            }

            // ZOOM: the host's steps, a radio set.
            if (const std::optional<funkgui::Point> zoom = centreTitled(host, "Zoom"))
            {
                if (const funkgui::MenuRequest* m = openTrigger(R, host, "zoom", *zoom, true, seen))
                {
                    radioSet(R, "rows.zoom", *m, choicesOf(*m), "100");
                    picture(P, pics, host, panel, *m, "zoom", t);
                    R.eq("rows.zoom.chooses", host.chooseMenuItem("125%") && host.zoomPercent() == 125 ? 1 : 0, 1);
                    closedTrigger(R, host, "zoom", seen);
                    host.setZoom({ 100, 125, 150, 200 }, 100);
                    host.settle();
                }
            }
            else
                R.eq("trigger.zoom.shown", 0, 1);

            // MODEL: two headers, a radio set under each.
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "model", panel.centreOfModel(), true, seen))
            {
                const pg::ChatState chat = processor.chatAgent().chatState();
                std::vector<std::vector<funkgui::MenuItem>> sets;
                int headers = 0;
                for (const funkgui::MenuItem& it : m->items)
                {
                    if (it.header)
                    {
                        ++headers;
                        sets.emplace_back();
                    }
                    else if (!it.separator && !sets.empty())
                        sets.back().push_back(it);
                }
                R.eq("rows.model.headers", headers, 2);
                std::string builder;
                for (const pg::ChatModel& cm : chat.models)
                    if (cm.id == chat.model)
                        builder = cm.name;
                if (sets.size() == 2)
                {
                    radioSet(R, "rows.model.build", *m, sets[0], builder);
                    int radios = 0, checked = 0;
                    for (const funkgui::MenuItem& it : sets[1])
                    {
                        radios += it.radio ? 1 : 0;
                        checked += it.checked ? 1 : 0;
                    }
                    R.eq("rows.model.plan.radio", radios == static_cast<int>(sets[1].size()) ? 1 : 0, 1);
                    R.eq("rows.model.plan.one_current", checked, 1);
                }
                picture(P, pics, host, panel, *m, "model", t);
                host.cancelMenu();
                closedTrigger(R, host, "model", seen);
            }

            // THINKING: the model's levels, a radio set.
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "thinking", panel.centreOfThinking(), true, seen))
            {
                const std::string effort = processor.chatAgent().chatState().effort;
                std::string name = effort.empty() ? std::string() : effort;
                if (!name.empty())
                    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                radioSet(R, "rows.thinking", *m, choicesOf(*m), name == "Xhigh" ? "X" : name);
                picture(P, pics, host, panel, *m, "thinking", t);
                host.cancelMenu();
                closedTrigger(R, host, "thinking", seen);
            }

            // CHATS: the project's chats under its header, a radio set with each one's time, then three commands.
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "chats", panel.centreOfChats(), true, seen))
            {
                int headers = 0, commands = 0;
                std::string header;
                std::vector<funkgui::MenuItem> list;
                for (const funkgui::MenuItem& it : m->items)
                {
                    if (it.header)
                    {
                        ++headers;
                        header = it.label;
                    }
                    else if (it.radio)
                        list.push_back(it);
                    else if (!it.separator)
                        ++commands;
                }
                R.eq("rows.chats.header", headers == 1 && has(header, "Glass Bloom") ? 1 : 0, 1);
                radioSet(R, "rows.chats", *m, list, "Brighter attack");
                bool newestFirst = list.size() == 3 && list[0].label.rfind("A warm", 0) == 0 && list[2].label == "Bass ideas";
                bool times = !list.empty();
                for (const funkgui::MenuItem& it : list)
                    times = times && !it.shortcut.empty();
                R.eq("rows.chats.newest_first", newestFirst ? 1 : 0, 1);
                R.eq("rows.chats.times", times && has(list.back().shortcut, "-") && has(list.front().shortcut, ":") ? 1 : 0, 1);
                R.eq("rows.chats.commands", commands, 3);
                picture(P, pics, host, panel, *m, "chats", t);
                host.cancelMenu();
                closedTrigger(R, host, "chats", seen);
            }

            // PROVIDER: the key dialog's Provider pill, the providers a radio set under its header.
            {
                const funkgui::Point key = panel.centreOfKey();
                host.click(key.x, key.y);
                host.tick(1);
                if (const funkgui::MenuRequest* m = openTrigger(R, host, "provider", panel.centreOfKeyProvider(), true, seen))
                {
                    int headers = 0;
                    for (const funkgui::MenuItem& it : m->items)
                        headers += it.header ? 1 : 0;
                    R.eq("rows.provider.header", headers, 1);
                    R.eq("rows.provider.every", static_cast<int64_t>(choicesOf(*m).size()), static_cast<int64_t>(std::size(pg::kChatPresets)));
                    radioSet(R, "rows.provider", *m, choicesOf(*m), "Anthropic");
                    picture(P, pics, host, panel, *m, "provider", t);
                    host.cancelMenu();
                    closedTrigger(R, host, "provider", seen);
                }
                host.keys("escape");
                host.tick(1);
            }

            if (rows)
            {
                focusRings(P, host, { "examples", "library", "projects" }, seen, "tab", "tab");
                focusRings(P, host, { "zoom", "model", "thinking" }, seen, "shift+tab", "shift+tab");
                // One open look: every trigger's open fill the same.
                std::size_t fills = 0;
                bool one = true;
                for (const auto& [n, fill] : seen.openFills)
                {
                    ++fills;
                    one = one && fill == seen.openFills.begin()->second;
                }
                P.eq("trigger.one_open_fill", fills >= 6 && one ? 1 : 0, 1);
            }
        }
    }

    // ---- the thinking level on OpenRouter: another provider's model's own levels (a scripted server: no network) --------
    {
        pg::Processor processor;
        processor.settle();
        auto script = std::make_shared<fake::Script>();
        script->gets["/api/v1/models"] = { 200, fake::openAiModels({ { "openai/gpt-probe-5", "GPT-5 via OpenRouter", 1760000000, 400000, true, true,
                                                                       { "xhigh", "high", "medium", "low", "minimal", "none" }, "medium" } },
                                                                     true) };
        pg::ChatAgent& agent = processor.chatAgent();
        agent.setTransport(std::make_unique<fake::Transport>(script));
        std::string why;
        const bool set = pg::chatstore::saveProviderKey("openai", "https://openrouter.ai", fake::fakeOpenRouterKey(), &why)
                      && agent.setProvider({ "openai", "https://openrouter.ai/api/v1", "openai/gpt-probe-5" }, "", why);
        agent.waitIdle();
        P.eq("setup.openrouter_thinking", set && agent.chatState().effort == "medium" ? 1 : 0, 1);
        for (int t = 0; t < 2; ++t)
        {
            Rows R{ P, t == 0 };
            Seen seen;
            pg::ui::Panel panel(processor.ports(), processor.editorPort());
            funkgui::HeadlessHost host(panel, t, 2.0f);
            host.settle();
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "thinking_openrouter", panel.centreOfThinking(), true, seen))
            {
                R.eq("rows.thinking_openrouter.levels", static_cast<int64_t>(choicesOf(*m).size()), 6);
                radioSet(R, "rows.thinking_openrouter", *m, choicesOf(*m), "Medium (the default)");
                picture(P, pics, host, panel, *m, "thinking_openrouter", t);
                host.cancelMenu();
                closedTrigger(R, host, "thinking_openrouter", seen);
            }
        }
        agent.setProvider({}, "", why);           // Anthropic again, for the Processors that follow (chat.json)
        agent.waitIdle();
    }

    // ---- the Settings card's selects --------------------------------------------------------------------------------
    {
        pg::Processor processor;
        processor.settle();
        FakeDevices fake;
        pg::Processor::WindowHooks hooks;
        hooks.devices = &fake;
        processor.setWindowHooks(hooks);
        for (int t = 0; t < 2; ++t)
        {
            const bool rows = t == 0;
            Rows R{ P, rows };
            Seen seen;
            pg::ui::Panel panel(processor.ports(), processor.editorPort());
            funkgui::HeadlessHost host(panel, t, 2.0f);
            host.settle();
            R.eq("settings.opens", panel.openSettings("audio") ? 1 : 0, 1);
            host.settle();
            pg::ui::SettingsCard& card = panel.settingsCard();
            const float W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
            const std::optional<funkgui::Point> output = card.centreOf(pg::ui::SettingsCard::Act::output, 0, W, H);
            if (const funkgui::MenuRequest* m = output ? openTrigger(R, host, "settings", *output, true, seen) : nullptr)
            {
                radioSet(R, "rows.settings", *m, choicesOf(*m), "External Headphones");
                picture(P, pics, host, panel, *m, "settings", t);
                R.eq("rows.settings.chooses", host.chooseMenuItem("Studio Interface") && fake.setup().output == "Studio Interface" ? 1 : 0, 1);
                closedTrigger(R, host, "settings", seen);
                fake.chooseOutput("External Headphones");
                host.settle();
            }
            if (rows)
                focusRings(P, host, { "settings" }, seen, "tab", "tab", false);
            host.keys("escape");
            host.settle();
        }
    }

    // ---- a .gui file's menu control -----------------------------------------------------------------------------------
    {
        pg::Processor processor;
        processor.settle();
        load(processor, { { pg::kProgramName, waveProgram() }, { "panel.gui", kWaveGui } });
        processor.settle();
        for (int t = 0; t < 2; ++t)
        {
            const bool rows = t == 0;
            Rows R{ P, rows };
            Seen seen;
            pg::ui::Panel panel(processor.ports(), processor.editorPort());
            funkgui::HeadlessHost host(panel, t, 2.0f);
            settleRun(processor, host);
            R.eq("panel.has_gui", panel.hasGui() ? 1 : 0, 1);
            const pg::ui::GuiControlItem& control = panel.guiControl(0);
            const funkgui::Rect cell = control.cell();
            const funkgui::Rect box = control.menuBox();
            R.eq("panel.box_under_label", box.y > cell.y && box.bottom() <= cell.bottom() + 0.5f ? 1 : 0, 1);
            const funkgui::Point at{ box.centreX(), box.centreY() };
            // Laid out at its natural width (a spacer beside it), its box shows every value whole: none cut to "...".
            {
                int cut = 0;
                const float room = dd::labelRoom(box, box.x + pg::ui::kGuiMenuInset);
                for (const char* v : { "SINE", "SAW", "SQUARE", "TRIANGLE", "NOISE" })
                    cut += funkgui::text::fits(funkgui::FontService::get().atlas(), v, funkgui::type::kMicro, room) ? 0 : 1;
                R.eq("panel.values_whole", cut, 0);
            }
            if (const funkgui::MenuRequest* m = openTrigger(R, host, "panel", at, true, seen, box))
            {
                radioSet(R, "rows.panel", *m, choicesOf(*m), "SAW");
                picture(P, pics, host, panel, *m, "panel", t);
                R.eq("rows.panel.chooses", host.chooseMenuItem("TRIANGLE") ? 1 : 0, 1);
                host.settle();
                R.near("rows.panel.chose", static_cast<double>(processor.ports()[0]->value01()), 0.75, 1e-4);
                closedTrigger(R, host, "panel", seen);
            }
            if (rows)
            {
                // The click left the focus on it: the next item and back, and the ring is on its box.
                host.keys("tab");
                host.keys("shift+tab");
                host.tick(1);
                const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
                P.eq("trigger.panel.focus_ring", ringAround(host.draw(), box, th.accent) ? 1 : 0, 1);
                // Return and Space open its menu as a click does (every trigger the same), and so does a screen
                // reader's press or showMenu; none of them steps the value.
                const float before = processor.ports()[0]->value01();
                const std::optional<funkgui::A11yItem> item = itemAt(host, at);
                const auto opensMenu = [&](const std::string& how, const std::function<void()>& ask) {
                    ask();
                    host.tick(1);
                    const funkgui::MenuRequest* m = host.pendingMenu();
                    const bool open = m != nullptr && same(m->anchor, box);
                    const std::optional<Box> pill = pillAt(host.draw(), box);
                    P.eq("trigger.panel." + how + "_opens",
                         open && pill && pill->edge == packed(th.accent) && host.cancelMenu() ? 1 : 0, 1);
                    host.tick(1);
                    P.near("trigger.panel." + how + "_keeps_value", static_cast<double>(processor.ports()[0]->value01()),
                           static_cast<double>(before), 1e-6);
                };
                opensMenu("return", [&] { host.keys("return"); });
                opensMenu("space", [&] { host.keys("space"); });
                opensMenu("a11y_press", [&] { if (item) panel.a11yAction(item->id, funkgui::A11yAction::press); });
                opensMenu("a11y_show_menu", [&] { if (item) panel.a11yAction(item->id, funkgui::A11yAction::showMenu); });
            }
        }
    }

    // ---- a netlist's canvas menu: the shortcuts' column --------------------------------------------------------------
    {
        pg::Processor processor;
        processor.settle();
        const pg::LibraryFile* tone = nullptr;
        for (const pg::LibraryFile& f : pg::kLibrary)
            if (std::string(f.name) == "diodetone.cir")
                tone = &f;
        P.eq("schematic.library_has_it", tone != nullptr ? 1 : 0, 1);
        if (tone != nullptr)
        {
            load(processor, { { pg::kProgramName, "#include \"diodetone.cir\"\n" + std::string(pg::kExamples[0].source) },
                              { "diodetone.cir", tone->source } });
            processor.settle();
            for (int t = 0; t < 2; ++t)
            {
                const bool rows = t == 0;
                Rows R{ P, rows };
                pg::ui::Panel panel(processor.ports(), processor.editorPort());
                funkgui::HeadlessHost host(panel, t, 2.0f);
                settleRun(processor, host);
                const funkgui::Point tab = panel.centreOfTab(1);
                host.click(tab.x, tab.y);
                host.tick(1);
                panel.setSchematic(true);
                host.settle();
                const funkgui::Rect canvas = panel.schematic().canvasRect();
                funkgui::Mods popup;
                popup.ctrl = true;
                host.click(canvas.x + 30.0f, canvas.y + 30.0f, popup);
                const funkgui::MenuRequest* m = host.pendingMenu();
                R.eq("rows.schematic.opens", m != nullptr ? 1 : 0, 1);
                if (m != nullptr)
                {
                    int keys = 0, inLabels = 0;
                    for (const funkgui::MenuItem& it : m->items)
                    {
                        keys += !it.shortcut.empty() ? 1 : 0;
                        inLabels += has(it.label, "   ") ? 1 : 0;
                    }
                    R.ge("rows.schematic.shortcuts", static_cast<double>(keys), 6.0);
                    R.eq("rows.schematic.none_in_labels", inLabels, 0);
                    bool selectAll = false;
                    for (const funkgui::MenuItem& it : m->items)
                        selectAll = selectAll || (it.label == "Select all" && it.shortcut == pg::shortcutText("Cmd-A"));
                    R.eq("rows.schematic.select_all_key", selectAll ? 1 : 0, 1);
                    picture(P, pics, host, panel, *m, "schematic", t);
                    R.eq("rows.schematic.chooses", host.chooseMenuItem("Select all")
                                                       && panel.schematic().selection().size() == panel.schematic().drawing().parts.size() ? 1 : 0, 1);
                }
                // The toolbar's More, a trigger too (no screen-reader item: none of the schematic's toolbar is one, the
                // Netlist view being the keyboard's way): its chevron at the one inset, "More" whole, its menu under
                // it, the accent's edge while it is open and not once it is dismissed.
                if (rows)
                {
                    using SV = pg::ui::SchematicView;
                    const funkgui::Rect more = panel.schematic().buttonRect(SV::Button::more);
                    const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(host.themeIndex()));
                    host.move(more.centreX(), more.centreY());
                    host.tick(1);
                    P.eq("trigger.more.chevron", chevronAt(host.draw(), more) ? 1 : 0, 1);
                    P.eq("trigger.more.label_whole", funkgui::text::fits(funkgui::FontService::get().atlas(), "More", pg::ui::chat::kSmall,
                                                                         dd::labelRoom(more, more.x + 8.0f)) ? 1 : 0, 1);
                    host.click(more.centreX(), more.centreY());
                    host.tick(1);
                    const funkgui::MenuRequest* mm = host.pendingMenu();
                    P.eq("trigger.more.under_it", mm != nullptr && same(mm->anchor, more) ? 1 : 0, 1);
                    const std::optional<Box> open = pillAt(host.draw(), more);
                    P.eq("trigger.more.open_edge", open && open->edge == packed(th.accent) && std::abs(open->edgeW - 1.0f) < 0.01f ? 1 : 0, 1);
                    host.cancelMenu();
                    host.tick(1);
                    const std::optional<Box> after = pillAt(host.draw(), more);
                    P.eq("trigger.more.closed", !(after && after->edge == packed(th.accent)) ? 1 : 0, 1);
                }
            }
        }
    }

    pg::projectstore::setDirectoryForTests({});
    pg::chatstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
