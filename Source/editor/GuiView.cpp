// Source/editor/GuiView.cpp: see GuiView.h.
#include "editor/GuiView.h"

#include "editor/DisplayItem.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <array>
#include <cstdlib>
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
        : items_(items), displays_(std::move(displays))
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
            controls_[static_cast<std::size_t>(c)] =
                add(std::make_unique<GuiControlItem>(*ports[static_cast<std::size_t>(c)], idFor(), hooks.menuTheme));
        for (int k = 0; k < kPool; ++k)
            pads_.push_back(add(std::make_unique<GuiXyItem>(idFor())));
        for (int k = 0; k < kPool; ++k)
            envelopes_.push_back(add(std::make_unique<GuiEnvelopeItem>(idFor())));
        for (int k = 0; k < kKeyboards; ++k)
            keyboards_.push_back(add(std::make_unique<GuiKeyboardItem>(idFor(), hooks.playNote, hooks.playWheel)));
        for (int k = 0; k < kShaders; ++k)
            shaders_.push_back(add(std::make_unique<GuiShaderItem>(idFor())));
        shaderNode_.assign(shaders_.size(), -1);
    }

    GuiControlItem& GuiView::control(int slot)
    {
        return static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(controls_[static_cast<std::size_t>(slot)])]);
    }

    int GuiView::controlItem(int slot) const
    {
        return slot >= 0 && slot < static_cast<int>(controls_.size()) ? controls_[static_cast<std::size_t>(slot)] : -1;
    }

    bool GuiView::isControl(int item) const
    {
        return item >= 0 && std::find(controls_.begin(), controls_.end(), item) != controls_.end();
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
        pages_.resize(spec.gui.nodes.size(), 0);
        gui_ = layoutGui(spec, controls, measureText, pages_, area, scrollX_, scrollY_);
        if (gui_.maxScrollY > 0.0f)                      // a gutter for the scroll bar
        {
            area.w -= kScrollGutter;
            gui_ = layoutGui(spec, controls, measureText, pages_, area, scrollX_, scrollY_);
        }
        pages_ = gui_.pages;
        scrollX_ = std::clamp(scrollX_, 0.0f, gui_.maxScrollX);
        scrollY_ = std::clamp(scrollY_, 0.0f, gui_.maxScrollY);
        place(spec);
    }

    void GuiView::clear(const PanelSpec& spec)
    {
        gui_ = GuiLayout{};
        place(spec);
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

    bool GuiView::scrollBy(float dx, float dy)
    {
        const float x = std::clamp(scrollX_ + dx, 0.0f, gui_.maxScrollX), y = std::clamp(scrollY_ + dy, 0.0f, gui_.maxScrollY);
        if (x == scrollX_ && y == scrollY_)
            return false;
        scrollX_ = x;
        scrollY_ = y;
        return true;
    }

    void GuiView::releaseKeys()
    {
        for (const int i : keyboards_)
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(i)]).release();
    }

    void GuiView::place(const PanelSpec& spec)
    {
        mine_.assign(items_.size(), false);
        const bool gui = !spec.gui.empty();
        const std::vector<GuiNode>& nodes = spec.gui.nodes;
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
                static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(shaders_[shade++])])
                    .place(n, gui_.rects[i], area, seen(i), known ? shaderIds_[k] : 0u, n.file,
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
                item.place(static_cast<int>(i), gui_.rects[i], guiTabRects(spec, static_cast<int>(i), gui_.rects[i], measureText),
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
            static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(shaders_[shade])]).place(GuiNode{}, {}, area, false, 0u, "", "", false);
        }
        for (const std::vector<int>* pool : { &keyboards_, &shaders_, &pads_, &envelopes_, &tabs_ })
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
            item.setRect(has ? gui_.rects[static_cast<std::size_t>(i)] : funkgui::Rect{});
            item.setArea(area);
            mine_[static_cast<std::size_t>(idx)] = true;
        }
    }

    // A shader's program goes to FunkGui's registry (process-wide, keyed by its bytes' hash: the same shader in two
    // instances is one program) the first time a panel shows it; a program is never removed, since another instance
    // may draw it too. One that was not built says why from the compile's notes; one FunkGui refuses (Metal's own
    // compiler, say), from FunkGui.
    void GuiView::registerShaders(const PanelSpec& spec, const Sources& files, const std::string& notes)
    {
        const std::size_t n = spec.shaders.size();
        shaderIds_.assign(n, 0u);
        shaderWhy_.assign(n, std::string());
        shaderMoves_.assign(n, false);
        for (std::size_t k = 0; k < n; ++k)
        {
            const PanelShader& sh = spec.shaders[k];
            const auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& x) { return x.name == sh.file; });
            if (f != files.end())
                shaderMoves_[k] = f->text.find("u_time") != std::string::npos || f->text.find("u_viewSize") != std::string::npos;
            if (sh.binary.empty() || sh.id == 0)
            {
                std::string why = f == files.end() ? "there is no file " + sh.file + " among the program's" : std::string("not built");
                std::istringstream in(notes);
                std::string line;
                while (std::getline(in, line))
                    if (line.rfind(sh.file + ":", 0) == 0)
                        if (const std::size_t at = line.find("note: "); at != std::string::npos)
                        {
                            why = line.substr(at + 6);
                            break;
                        }
                shaderWhy_[k] = why;
                continue;
            }
            std::string why;
            if (funkgui::shaders::has(sh.id)
                || funkgui::shaders::set(sh.id, sh.binary.data(), static_cast<uint32_t>(sh.binary.size()), why))
                shaderIds_[k] = sh.id;
            else
                shaderWhy_[k] = why.empty() ? std::string("the GPU refused it") : why;
        }
    }

    void GuiView::tick(const PanelSpec& spec, const Meters& shown, bool live)
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
            const bool now = live && t >= 0 && t < kMaxDisplays;
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
            std::array<float, 8> v{};
            for (std::size_t j = 0; j < v.size(); ++j)
            {
                const std::string& in = n.inputs[j];
                if (in == "in")
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
        const funkgui::Rect& a = gui_.area;
        if (gui_.maxScrollY > 0.0f)
        {
            const float h = std::max(24.0f, a.h * a.h / (a.h + gui_.maxScrollY));
            const float y = a.y + (a.h - h) * scrollY_ / gui_.maxScrollY;
            c.rrect(a.right() + kScrollGutter - 4.0f, y, 3.0f, h, 1.5f, th.ink32);
        }
        if (gui_.maxScrollX > 0.0f)
        {
            const float w = std::max(24.0f, a.w * a.w / (a.w + gui_.maxScrollX));
            const float x = a.x + (a.w - w) * scrollX_ / gui_.maxScrollX;
            c.rrect(x, a.bottom() - 4.0f, w, 3.0f, 1.5f, th.ink32);
        }
    }

    void updateDisplays(GuiView::Items& items, const std::vector<int>& displays, const Meters& shown,
                        const std::function<void(bool input, float* out, int n)>& scope, std::vector<float>& in,
                        std::vector<float>& out, float dt)
    {
        bool readIn = false, readOut = false;
        for (const int i : displays)
        {
            bool input = false;
            if (static_cast<const DisplayItem&>(*items[static_cast<std::size_t>(i)]).wantsScope(input))
                (input ? readIn : readOut) = true;
        }
        if (readIn || readOut)
            in.resize(DisplayItem::kScopeSamples), out.resize(DisplayItem::kScopeSamples);
        if (readIn)
            scope(true, in.data(), DisplayItem::kScopeSamples);
        if (readOut)
            scope(false, out.data(), DisplayItem::kScopeSamples);
        for (const int i : displays)
        {
            auto& item = static_cast<DisplayItem&>(*items[static_cast<std::size_t>(i)]);
            bool input = false;
            const bool wants = item.wantsScope(input);
            item.update(shown, wants ? (input ? in.data() : out.data()) : nullptr, dt);
        }
    }
}
