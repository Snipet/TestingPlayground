// Source/editor/GuiView.cpp: see GuiView.h.
#include "editor/GuiView.h"

#include "editor/DisplayItem.h"
#include "editor/ShaderFont.h"

#include "fminus/Shaders.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <utility>

namespace playground::ui
{
    namespace
    {
        float measureText(const std::string& text, const funkgui::TextStyle& st)
        {
            return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
        }
    }

    GuiView::GuiView(Items& items, std::span<funkgui::ParamPort* const> ports, std::vector<int> displays,
                     const std::function<uint32_t()>& idFor, Hooks hooks)
        : items_(items), displays_(std::move(displays)), dialMenu_(hooks.dialMenu)
    {
        const auto add = [this](std::unique_ptr<Item> item) {
            items_.push_back(std::move(item));
            return static_cast<int>(items_.size()) - 1;
        };
        // Its tabs first (their frames under the rest), a control a slot, and pools of pads, envelopes, keyboards and
        // shaders.
        for (int k = 0; k < kPool; ++k)
            tabs_.push_back(add(std::make_unique<GuiTabsItem>(idFor(), hooks.choosePage)));
        controls_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
        {
            controls_[static_cast<std::size_t>(c)] = add(std::make_unique<GuiControlItem>(
                *ports[static_cast<std::size_t>(c)], idFor(), hooks.menuTheme, hooks.menuOpen, hooks.menuHost));
            if (hooks.dialMenu)
                control(c).setMenu([menu = hooks.dialMenu, c](funkgui::Point at) { menu(c, at); });
        }
        for (int k = 0; k < kPool; ++k)
            pads_.push_back(add(std::make_unique<GuiXyItem>(idFor())));
        for (int k = 0; k < kPool; ++k)
            envelopes_.push_back(add(std::make_unique<GuiEnvelopeItem>(idFor())));
        for (int k = 0; k < kKeyboards; ++k)
            keyboards_.push_back(add(std::make_unique<GuiKeyboardItem>(idFor(), hooks.playNote, hooks.playWheel)));
        for (int k = 0; k < kShaders; ++k)
        {
            const uint32_t own = idFor(), x = idFor(), y = idFor(), down = idFor();
            shaders_.push_back(add(std::make_unique<GuiShaderItem>(std::array<uint32_t, 4>{ own, x, y, down })));
        }
        shaderNode_.assign(shaders_.size(), -1);
        // The pointer's items last: hit after every other item.
        for (int k = 0; k < kPointers; ++k)
        {
            const uint32_t x = idFor(), y = idFor(), down = idFor();
            pointers_.push_back(add(std::make_unique<GuiPointerItem>(std::array<uint32_t, 3>{ x, y, down })));
        }
    }

    void GuiView::setMidi(int slot, bool mapped, bool learning, std::string spec)
    {
        if (controlItem(slot) >= 0)
            control(slot).setMidi(mapped, learning, std::move(spec));
    }

    GuiControlItem& GuiView::control(int slot)
    {
        return static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(controls_[static_cast<std::size_t>(slot)])]);
    }

    std::array<GuiValue*, 5> GuiView::pointerValues(const PanelPointer& p)
    {
        std::array<GuiValue*, 5> out{};
        const int slots[] = { p.x, p.y, p.down, p.wheel, p.hover };
        for (std::size_t k = 0; k < out.size(); ++k)
            if (slots[k] >= 0 && slots[k] < kMaxControls && controlItem(slots[k]) >= 0)
                out[k] = &control(slots[k]).value();
        return out;
    }

    int GuiView::controlItem(int slot) const
    {
        return slot >= 0 && slot < static_cast<int>(controls_.size()) ? controls_[static_cast<std::size_t>(slot)] : -1;
    }

    bool GuiView::isControl(int item) const
    {
        if (item < 0)
            return false;
        if (std::find(controls_.begin(), controls_.end(), item) != controls_.end())
            return true;
        if (std::find(pointers_.begin(), pointers_.end(), item) != pointers_.end())
            return static_cast<const GuiPointerItem&>(*items_[static_cast<std::size_t>(item)]).sink().wheels();
        if (std::find(shaders_.begin(), shaders_.end(), item) != shaders_.end())
            return static_cast<const GuiShaderItem&>(*items_[static_cast<std::size_t>(item)]).sink().wheels();
        return false;
    }

    void GuiView::setControls(const Controls& cs, const PanelSpec& spec)
    {
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = controlItem(c); i >= 0)
                control(c).value().set(cs[static_cast<std::size_t>(c)], spec.labels[static_cast<std::size_t>(c)],
                                       spec.options[static_cast<std::size_t>(c)]);
    }

    void GuiView::layout(const PanelSpec& spec, const Controls& controls, funkgui::Rect area)
    {
        if (spec.gui.empty())
        {
            clear(spec);
            return;
        }
        // Laid out once at no scroll (the content's places): the scroll only moves what is shown (scrolled()).
        pages_.resize(spec.gui.nodes.size(), 0);
        content_ = layoutGui(spec, controls, measureText, pages_, area, 0.0f, 0.0f);
        if (content_.maxScrollY > 0.0f)                  // a gutter for the scroll bar
        {
            area.w -= kScrollGutter;
            content_ = layoutGui(spec, controls, measureText, pages_, area, 0.0f, 0.0f);
        }
        pages_ = content_.pages;
        contentTabs_.assign(spec.gui.nodes.size(), {});
        for (std::size_t i = 0; i < spec.gui.nodes.size(); ++i)
            if (spec.gui.nodes[i].kind == GuiKind::tabs)
                contentTabs_[i] = guiTabRects(spec, static_cast<int>(i), content_.rects[i], measureText);
        scrollX_ = std::clamp(scrollX_, 0.0f, content_.maxScrollX);
        scrollY_ = std::clamp(scrollY_, 0.0f, content_.maxScrollY);
        scrolled();
        place(spec);
    }

    void GuiView::clear(const PanelSpec& spec)
    {
        content_ = gui_ = GuiLayout{};
        contentTabs_.clear();
        shownTabs_.clear();
        place(spec);
    }

    void GuiView::scrolled()
    {
        shownX_ = std::round(scrollX_);
        shownY_ = std::round(scrollY_);
        gui_ = content_;
        for (funkgui::Rect& r : gui_.rects)
        {
            r.x -= shownX_;
            r.y -= shownY_;
        }
        shownTabs_ = contentTabs_;
        for (std::vector<funkgui::Rect>& tabs : shownTabs_)
            for (funkgui::Rect& r : tabs)
            {
                r.x -= shownX_;
                r.y -= shownY_;
            }
    }

    bool GuiView::choosePage(const PanelSpec& spec, int node, int page)
    {
        if (node < 0 || static_cast<std::size_t>(node) >= spec.gui.nodes.size())
            return false;
        pages_.resize(spec.gui.nodes.size(), 0);
        if (pages_[static_cast<std::size_t>(node)] == page)
            return false;
        pages_[static_cast<std::size_t>(node)] = page;
        return true;
    }

    bool GuiView::scrollBy(const PanelSpec& spec, float dx, float dy)
    {
        const float x = std::clamp(scrollX_ + dx, 0.0f, content_.maxScrollX), y = std::clamp(scrollY_ + dy, 0.0f, content_.maxScrollY);
        if (x == scrollX_ && y == scrollY_)
            return false;
        scrollX_ = x;
        scrollY_ = y;
        if (std::round(x) != shownX_ || std::round(y) != shownY_)
        {
            scrolled();
            place(spec);                                 // each item where the layout put it, moved (and shown or not)
        }
        return true;
    }

    void GuiView::releaseKeys()
    {
        for (const int i : keyboards_)
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(i)]).release();
        for (const int i : pointers_)
            static_cast<GuiPointerItem&>(*items_[static_cast<std::size_t>(i)]).sink().release();
        for (const int i : shaders_)
            static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(i)]).sink().release();
    }

    void GuiView::place(const PanelSpec& spec)
    {
        mine_.assign(items_.size(), false);
        // An interface with no layout of its nodes (cleared: hidden under the code) is placed as none.
        const bool gui = !spec.gui.empty() && gui_.rects.size() == spec.gui.nodes.size()
                      && gui_.visible.size() == spec.gui.nodes.size() && gui_.pages.size() == spec.gui.nodes.size();
        const std::vector<GuiNode> none;
        const std::vector<GuiNode>& nodes = gui ? spec.gui.nodes : none;
        const funkgui::Rect area = gui_.area;
        // A node on the pages shown and, at least in part, in the area.
        const auto seen = [&](std::size_t i) {
            if (!gui || i >= gui_.rects.size() || !gui_.visible[i])
                return false;
            const funkgui::Rect& r = gui_.rects[i];
            return r.right() > area.x && r.x < area.right() && r.bottom() > area.y && r.y < area.bottom();
        };
        std::vector<int> nodeOfSlot(static_cast<std::size_t>(kMaxControls), -1), nodeOfDisplay(static_cast<std::size_t>(kMaxDisplays), -1);
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const GuiNode& n = nodes[i];
            const bool isControl = n.kind == GuiKind::knob || n.kind == GuiKind::slider || n.kind == GuiKind::toggle
                                || n.kind == GuiKind::buttons || n.kind == GuiKind::menu;
            if (isControl && n.slot >= 0 && n.slot < kMaxControls)
                nodeOfSlot[static_cast<std::size_t>(n.slot)] = static_cast<int>(i);
            if (n.kind == GuiKind::display && n.display >= 0 && n.display < kMaxDisplays)
                nodeOfDisplay[static_cast<std::size_t>(n.display)] = static_cast<int>(i);
        }
        for (int c = 0; c < kMaxControls; ++c)
        {
            if (controlItem(c) < 0)
                continue;
            const int i = nodeOfSlot[static_cast<std::size_t>(c)];
            if (i >= 0)
                control(c).place(nodes[static_cast<std::size_t>(i)], gui_.rects[static_cast<std::size_t>(i)], area,
                                 seen(static_cast<std::size_t>(i)), nodes[static_cast<std::size_t>(i)].color);
            else
                control(c).place(GuiNode{}, {}, area, false, "");
            mine_[static_cast<std::size_t>(controlItem(c))] = true;
        }
        std::size_t pad = 0, env = 0, tab = 0, keys = 0, shade = 0;
        for (std::size_t i = 0; i < nodes.size() && gui; ++i)
        {
            const GuiNode& n = nodes[i];
            if (n.kind == GuiKind::shader && shade < shaders_.size())
            {
                const std::size_t k = n.shader >= 0 ? static_cast<std::size_t>(n.shader) : spec.shaders.size();
                const bool known = k < spec.shaders.size() && k < shaderIds_.size();
                shaderNode_[shade] = static_cast<int>(i);
                auto& item = static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(shaders_[shade++])]);
                item.sink().bind(n.pointer, pointerValues(n.pointer), dialMenu_);
                item.setReadsPointer(std::any_of(n.inputs.begin(), n.inputs.end(), [](const std::string& in) { return pointerInput(in) >= 0; }));
                item.place(n, gui_.rects[i], area, seen(i), known ? shaderIds_[k] : 0u, n.file,
                           known ? shaderWhy_[k] : std::string("not built"), known && shaderMoves_[k]);
            }
            else if (n.kind == GuiKind::keyboard && keys < keyboards_.size())
                static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(keyboards_[keys++])])
                    .place(n, gui_.rects[i], area, seen(i), n.color);
            else if (n.kind == GuiKind::xy && pad < pads_.size() && n.slot >= 0 && n.slot2 >= 0)
            {
                auto& item = static_cast<GuiXyItem&>(*items_[static_cast<std::size_t>(pads_[pad++])]);
                item.bind(&control(n.slot).value(), &control(n.slot2).value());
                item.place(n, gui_.rects[i], area, seen(i), n.color);
            }
            else if (n.kind == GuiKind::envelope && env < envelopes_.size())
            {
                auto& item = static_cast<GuiEnvelopeItem&>(*items_[static_cast<std::size_t>(envelopes_[env++])]);
                std::array<GuiValue*, 4> adsr{};
                for (std::size_t k = 0; k < 4; ++k)
                    adsr[k] = n.env[k] >= 0 ? &control(n.env[k]).value() : nullptr;
                item.bind(adsr);
                item.place(n, gui_.rects[i], area, seen(i), n.color);
            }
            else if (n.kind == GuiKind::tabs && tab < tabs_.size())
            {
                auto& item = static_cast<GuiTabsItem&>(*items_[static_cast<std::size_t>(tabs_[tab++])]);
                std::vector<std::string> titles;
                for (const int child : n.children)
                    titles.push_back(nodes[static_cast<std::size_t>(child)].text);
                item.place(static_cast<int>(i), gui_.rects[i], i < shownTabs_.size() ? shownTabs_[i] : std::vector<funkgui::Rect>(),
                           std::move(titles), gui_.pages[i], area, seen(i), n.color, n.style);
            }
        }
        for (; pad < pads_.size(); ++pad)
            static_cast<GuiXyItem&>(*items_[static_cast<std::size_t>(pads_[pad])]).place(GuiNode{}, {}, area, false, "");
        for (; env < envelopes_.size(); ++env)
            static_cast<GuiEnvelopeItem&>(*items_[static_cast<std::size_t>(envelopes_[env])]).place(GuiNode{}, {}, area, false, "");
        for (; tab < tabs_.size(); ++tab)
            static_cast<GuiTabsItem&>(*items_[static_cast<std::size_t>(tabs_[tab])]).place(-1, {}, {}, {}, 0, area, false, "", {});
        for (; keys < keyboards_.size(); ++keys)
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(keyboards_[keys])]).place(GuiNode{}, {}, area, false, "");
        for (; shade < shaders_.size(); ++shade)
        {
            shaderNode_[shade] = -1;
            auto& item = static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(shaders_[shade])]);
            item.place(GuiNode{}, {}, area, false, 0u, "", "", false);
            item.sink().bind({}, {}, {});
            item.setReadsPointer(false);
        }
        // The pointer's items: the innermost element's first (a box's inside a box's), so it is hit first; a display
        // under one passes presses by.
        std::vector<bool> underPointer(nodes.size(), false);
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const int parent = nodes[i].parent;
            underPointer[i] = (nodes[i].pointer.any() && nodes[i].kind != GuiKind::shader)
                           || (parent >= 0 && static_cast<std::size_t>(parent) < i && underPointer[static_cast<std::size_t>(parent)]);
        }
        std::size_t point = 0;
        for (std::size_t i = nodes.size(); i-- > 0 && gui;)
        {
            const GuiNode& n = nodes[i];
            if (!n.pointer.any() || n.kind == GuiKind::shader || point >= pointers_.size())
                continue;
            auto& item = static_cast<GuiPointerItem&>(*items_[static_cast<std::size_t>(pointers_[point++])]);
            item.sink().bind(n.pointer, pointerValues(n.pointer), dialMenu_);
            item.place(n.kind, gui_.rects[i], area, seen(i));
        }
        for (; point < pointers_.size(); ++point)
        {
            auto& item = static_cast<GuiPointerItem&>(*items_[static_cast<std::size_t>(pointers_[point])]);
            item.place(GuiKind::area, {}, area, false);
            item.sink().bind({}, {}, {});
        }
        for (const std::vector<int>* pool : { &keyboards_, &shaders_, &pads_, &envelopes_, &tabs_, &pointers_ })
            for (const int i : *pool)
                mine_[static_cast<std::size_t>(i)] = true;
        if (!gui)
            return;
        // The displays: where the interface puts them, while it shows them.
        for (int d = 0; d < kMaxDisplays && d < static_cast<int>(displays_.size()); ++d)
        {
            const int idx = displays_[static_cast<std::size_t>(d)];
            auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(idx)]);
            const int i = nodeOfDisplay[static_cast<std::size_t>(d)];
            const bool has = d < static_cast<int>(spec.displays.size()) && i >= 0;
            item.setDisplay(has && seen(static_cast<std::size_t>(i)) ? &spec.displays[static_cast<std::size_t>(d)] : nullptr);
            item.setPassThrough(has && underPointer[static_cast<std::size_t>(i)]);
            item.setRect(has ? gui_.rects[static_cast<std::size_t>(i)] : funkgui::Rect{});
            item.setArea(area);
            mine_[static_cast<std::size_t>(idx)] = true;
        }
    }

    // A shader's program goes to FunkGui's registry (process-wide, keyed by its bytes' hash: the same shader in two
    // instances is one program) the first time a panel shows it, with the text font bound to it (ShaderFont.h); a
    // program is never removed, since another instance may draw it too. One that was not built says why from the
    // compile's notes: the note on its own file, else the first on a .sh file of the program's it includes, with that
    // file's name and line ("ink.sh:5: the shader does not compile: ..."); one FunkGui refuses (Metal's own compiler,
    // say), from FunkGui.
    void GuiView::registerShaders(const PanelSpec& spec, const Sources& files, const std::string& notes)
    {
        const std::size_t n = spec.shaders.size();
        shaderIds_.assign(n, 0u);
        shaderWhy_.assign(n, std::string());
        shaderMoves_.assign(n, false);
        const Sources headers = shaderHeaders(files);
        for (std::size_t k = 0; k < n; ++k)
        {
            const PanelShader& sh = spec.shaders[k];
            const auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& x) { return x.name == sh.file; });
            if (f != files.end())
                shaderMoves_[k] = shaderAnimates(sh.file, f->text, headers);
            if (sh.binary.empty() || sh.id == 0)
            {
                std::string why = f == files.end() ? "there is no file " + sh.file + " among the program's" : std::string("not built");
                std::vector<std::string> included;
                if (f != files.end())
                {
                    std::string unused;
                    included = shaderIncludes(sh.file, f->text, headers, unused);
                }
                std::string own, fromInclude;
                std::istringstream in(notes);
                std::string line;
                while (own.empty() && std::getline(in, line))
                {
                    const std::size_t at = line.find("note: ");
                    if (at == std::string::npos)
                        continue;
                    if (line.rfind(sh.file + ":", 0) == 0)
                        own = line.substr(at + 6);
                    else if (fromInclude.empty())
                        for (const std::string& name : included)
                            if (line.rfind(name + ":", 0) == 0)
                            {
                                const int lineAt = std::max(1, std::atoi(line.c_str() + name.size() + 1));
                                fromInclude = name + ":" + std::to_string(lineAt) + ": " + line.substr(at + 6);
                                break;
                            }
                }
                shaderWhy_[k] = !own.empty() ? own : !fromInclude.empty() ? fromInclude : why;
                continue;
            }
            std::string why;
            if (funkgui::shaders::has(sh.id)
                || funkgui::shaders::set(sh.id, sh.binary.data(), static_cast<uint32_t>(sh.binary.size()), why))
            {
                shaderIds_[k] = sh.id;
                bindShaderFont(sh.id);                   // funk/text.sh's font (ShaderFont.h)
            }
            else
                shaderWhy_[k] = why.empty() ? std::string("the GPU refused it") : why;
        }
    }

    void GuiView::tick(const PanelSpec& spec, const Meters& shown, bool live, uint32_t liveTaps)
    {
        // The controls' live= fields: where the program has each parameter now (none until it plays this program).
        for (const int i : controls_)
        {
            if (i < 0)
                continue;
            auto& item = static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(i)]);
            const int d = item.liveDisplay();
            if (d < 0 || d >= static_cast<int>(spec.displays.size()))
                continue;
            const int t = spec.displays[static_cast<std::size_t>(d)].tap;
            const bool now = t >= 0 && t < kMaxDisplays && (live || ((liveTaps >> t) & 1u) != 0);
            item.setLive(now ? shown.taps[t] : std::numeric_limits<float>::quiet_NaN());
        }
        // The shaders' inputs: a parameter's 0..1, a field's value, the audio's level.
        for (std::size_t k = 0; k < shaders_.size(); ++k)
        {
            auto& item = static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(shaders_[k])]);
            const int node = shaderNode_[k];
            if (!item.shown() || node < 0 || static_cast<std::size_t>(node) >= spec.gui.nodes.size())
                continue;
            const GuiNode& n = spec.gui.nodes[static_cast<std::size_t>(node)];
            std::array<float, kMaxShaderInputs> v{};
            for (std::size_t j = 0; j < v.size(); ++j)
            {
                const std::string& in = n.inputs[j];
                if (const int w = pointerInput(in); w >= 0)
                {
                    // The pointer over this shader, or a picture's (showPointer).
                    const float asked = shownPointer_[static_cast<std::size_t>(w)];
                    v[j] = std::isfinite(asked) ? asked : item.sink().inputs()[static_cast<std::size_t>(w)];
                }
                else if (in == "in")
                    v[j] = std::max(shown.inPeak[0], shown.inPeak[1]);
                else if (in == "out")
                    v[j] = std::max(shown.outPeak[0], shown.outPeak[1]);
                else if (in.rfind("param:", 0) == 0)
                {
                    const int slot = std::atoi(in.c_str() + 6);
                    if (slot >= 0 && slot < kMaxControls && controlItem(slot) >= 0)
                        v[j] = control(slot).value().value01();
                }
                else if (in.rfind("field:", 0) == 0)
                {
                    const int d = std::atoi(in.c_str() + 6);
                    const int t = d >= 0 && d < static_cast<int>(spec.displays.size()) ? spec.displays[static_cast<std::size_t>(d)].tap : -1;
                    v[j] = t >= 0 && t < kMaxDisplays ? shown.taps[t] : 0.0f;
                }
            }
            item.setInputs(v);
        }
    }

    funkgui::Theme GuiView::theme(const funkgui::Theme& th, const PanelSpec& spec) const
    {
        return spec.gui.empty() ? th : guiTheme(th, spec.gui);
    }

    void GuiView::drawGround(funkgui::Canvas& c, const funkgui::Theme& gth, const PanelSpec& spec, funkgui::Tag tag) const
    {
        if (spec.gui.empty())
            return;
        const funkgui::Canvas::Scope s(c, tag, false);
        const funkgui::Canvas::ClipScope clip(c, gui_.area);
        drawGuiPanel(c, gth, spec, gui_);
    }

    void GuiView::drawScrollBars(funkgui::Canvas& c, const funkgui::Theme& th) const
    {
        scroller(false).draw(c, barArea(false), th);
        scroller(true).draw(c, barArea(true), th);
    }

    Scroll GuiView::scroller(bool horizontal) const noexcept
    {
        Scroll s = horizontal ? barX_ : barY_;
        const funkgui::Rect& a = gui_.area;
        s.horizontal = horizontal;
        s.offset = horizontal ? scrollX_ : scrollY_;
        s.view = horizontal ? a.w : a.h;
        s.content = s.view + (horizontal ? gui_.maxScrollX : gui_.maxScrollY);
        s.row = kRow;
        return s;
    }

    // The vertical bar in the gutter at the area's right, the horizontal one along its bottom.
    funkgui::Rect GuiView::barArea(bool horizontal) const noexcept
    {
        const funkgui::Rect& a = gui_.area;
        return horizontal ? a : funkgui::Rect{ a.x, a.y, a.w + kScrollGutter, a.h };
    }

    bool GuiView::pressBar(const PanelSpec& spec, funkgui::Point p)
    {
        for (const bool horizontal : { false, true })
            if (Scroll s = scroller(horizontal); s.press(p, barArea(horizontal)))
            {
                (horizontal ? barX_ : barY_) = s;
                scrollBy(spec, horizontal ? s.offset - scrollX_ : 0.0f, horizontal ? 0.0f : s.offset - scrollY_);
                return true;
            }
        return false;
    }

    bool GuiView::dragBar(const PanelSpec& spec, funkgui::Point p)
    {
        for (const bool horizontal : { false, true })
            if (Scroll s = scroller(horizontal); s.dragging())
            {
                s.drag(p, barArea(horizontal));
                (horizontal ? barX_ : barY_) = s;
                return scrollBy(spec, horizontal ? s.offset - scrollX_ : 0.0f, horizontal ? 0.0f : s.offset - scrollY_);
            }
        return false;
    }

    void GuiView::releaseBar() noexcept
    {
        barX_.release();
        barY_.release();
    }

    void updateDisplays(GuiView::Items& items, const std::vector<int>& displays, const Meters& shown,
                        const std::function<void(bool input, float* out, int n)>& scope, DisplayFeed& feed, float dt)
    {
        // Each source's samples read once, and its spectrum made once (when its samples changed), for all its displays.
        bool readIn = false, readOut = false, analyseIn = false, analyseOut = false;
        for (const int i : displays)
        {
            const auto& item = static_cast<const DisplayItem&>(*items[static_cast<std::size_t>(i)]);
            bool input = false;
            if (!item.wantsScope(input))
                continue;
            (input ? readIn : readOut) = true;
            if (item.display().kind == DisplayKind::spectrum)
                (input ? analyseIn : analyseOut) = true;
        }
        if (readIn || readOut)
            feed.in.resize(DisplayItem::kScopeSamples), feed.out.resize(DisplayItem::kScopeSamples);
        if (readIn)
            scope(true, feed.in.data(), DisplayItem::kScopeSamples);
        if (readOut)
            scope(false, feed.out.data(), DisplayItem::kScopeSamples);
        const float rate = shown.rate > 0.0f ? shown.rate : 48000.0f;
        if (analyseIn)
            feed.inSpectrum.update(feed.in.data(), rate);
        if (analyseOut)
            feed.outSpectrum.update(feed.out.data(), rate);
        for (const int i : displays)
        {
            auto& item = static_cast<DisplayItem&>(*items[static_cast<std::size_t>(i)]);
            bool input = false;
            const bool wants = item.wantsScope(input);
            const Spectrum& spectrum = input ? feed.inSpectrum : feed.outSpectrum;
            item.update(shown, wants ? (input ? feed.in.data() : feed.out.data()) : nullptr,
                        wants && item.display().kind == DisplayKind::spectrum ? spectrum.points() : nullptr, dt);
        }
    }
}
