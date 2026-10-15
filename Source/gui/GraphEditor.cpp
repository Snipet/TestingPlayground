#include "gui/GraphEditor.h"
#include "embedded/plugin_fonts.h"

#include "visage_graphics/canvas.h"
#include "visage_graphics/color.h"
#include "visage_graphics/font.h"
#include "visage_graphics/gradient.h"
#include "visage_ui/popup_menu.h"
#include "visage_utils/time_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace graph
{
    namespace
    {
        // --- Palette (neutral, professional, restrained accent) -----------------
        constexpr std::uint32_t kBgDeep        = 0xff1d1e21;
        constexpr std::uint32_t kBgPanel       = 0xff242529;
        constexpr std::uint32_t kBgPanelHi     = 0xff2c2e33;
        constexpr std::uint32_t kBgGraph       = 0xff17181b;
        constexpr std::uint32_t kBorder        = 0xff32343a;
        constexpr std::uint32_t kBorderHi      = 0xff42454d;
        constexpr std::uint32_t kGridMinor     = 0x10ffffff;
        constexpr std::uint32_t kGridMajor     = 0x20ffffff;
        constexpr std::uint32_t kLine          = 0xffc8cdd4;   // soft warm-gray line
        constexpr std::uint32_t kLineFillTop   = 0x55c8cdd4;
        constexpr std::uint32_t kLineFillBot   = 0x00c8cdd4;
        constexpr std::uint32_t kPointFill     = 0xffd8dbe1;
        constexpr std::uint32_t kPointBorder   = 0xff17181b;
        constexpr std::uint32_t kPointHover    = 0xffffffff;
        constexpr std::uint32_t kPointSelected = 0xffe8b97a;   // muted amber selection
        constexpr std::uint32_t kAccent        = 0xff8da2b8;   // muted steel — used sparingly
        constexpr std::uint32_t kAccentSoft    = 0x408da2b8;
        constexpr std::uint32_t kTextPri       = 0xffe2e4e8;
        constexpr std::uint32_t kTextSec       = 0xff8b8e95;
        constexpr std::uint32_t kHoverBg       = 0x14ffffff;   // hover overlay on flat surfaces

        constexpr int   kNumSamples     = 256;
        constexpr float kHandleRadius   = 4.0f;
        constexpr float kHitRadius      = 10.0f;
        constexpr float kToolbarHeight  = 30.0f;
        constexpr float kSidePanelWidth = 220.0f;
        constexpr float kSubdivBoxW     = 86.0f;
        constexpr float kSubdivBoxH     = 22.0f;
        constexpr float kRadiusSmall    = 3.0f;
        constexpr float kRadiusMedium   = 4.0f;
        // Interior points are kept this far from x=0 / x=1 so that the
        // dedicated edge points always sort to positions 0 and back().
        constexpr float kInteriorEps    = 1e-3f;

        // Map screen → normalised. y is inverted (1.0 at top).
        inline float normX(float local_x, float w) { return std::clamp(local_x / w, 0.0f, 1.0f); }
        inline float normY(float local_y, float h) { return std::clamp(1.0f - local_y / h, 0.0f, 1.0f); }
    }

    // =====================================================================
    //  Toolbar — flat menu-bar buttons (no pills)
    // =====================================================================
    namespace
    {
        // A single flat menu-bar entry: text only, faint hover background,
        // emits onClick when pressed. Matches the look of system menu bars
        // (Logic / Ableton / Reaper) rather than the rounded action buttons
        // of visage::UiButton.
        class MenuBarButton : public visage::Frame
        {
        public:
            using OnClick = std::function<void(MenuBarButton*)>;

            MenuBarButton(std::string label, const visage::Font& font)
                : label_(std::move(label)), font_(font) {}

            void onClick(OnClick cb) { on_click_ = std::move(cb); }

            void mouseEnter(const visage::MouseEvent&) override { hover_ = true; redraw(); }
            void mouseExit (const visage::MouseEvent&) override
            {
                hover_ = false; pressed_ = false; redraw();
            }
            void mouseDown (const visage::MouseEvent&) override
            {
                pressed_ = true; redraw();
                if (on_click_) on_click_(this);
            }
            void mouseUp   (const visage::MouseEvent&) override { pressed_ = false; redraw(); }

            void setOpen(bool open) { pressed_ = open; redraw(); }

            void draw(visage::Canvas& canvas) override
            {
                if (hover_ || pressed_)
                {
                    canvas.setColor(pressed_ ? kBgPanelHi : kHoverBg);
                    canvas.fill(0.0f, 0.0f, width(), height());
                }
                canvas.setColor(pressed_ || hover_ ? kTextPri : kTextSec);
                canvas.text(label_, font_, visage::Font::kCenter,
                            0.0f, 0.0f, width(), height());
            }

        private:
            std::string label_;
            visage::Font font_;
            bool hover_   = false;
            bool pressed_ = false;
            OnClick on_click_;
        };
    }

    class GraphEditor::Toolbar : public visage::Frame
    {
    public:
        Toolbar(GraphEditor& owner)
            : owner_(owner),
              label_font_(12.0f, resources::fonts::Roboto_Condensed_Regular_ttf),
              title_font_(11.0f, resources::fonts::Roboto_Condensed_Regular_ttf)
        {
            file_btn_ = std::make_unique<MenuBarButton>("File", label_font_);
            edit_btn_ = std::make_unique<MenuBarButton>("Edit", label_font_);
            file_btn_->onClick([this](MenuBarButton* b) { showFileMenu(b); });
            edit_btn_->onClick([this](MenuBarButton* b) { showEditMenu(b); });
            addChild(file_btn_.get());
            addChild(edit_btn_.get());
        }

        void resized() override
        {
            const float pad = 4.0f;
            const float bw  = 44.0f;
            const float bh  = height() - 1.0f;           // leave the 1px divider
            file_btn_->setBounds(pad,      0.0f, bw, bh);
            edit_btn_->setBounds(pad + bw, 0.0f, bw, bh);
        }

        void draw(visage::Canvas& canvas) override
        {
            canvas.setColor(kBgPanel);
            canvas.fill(0.0f, 0.0f, width(), height());
            canvas.setColor(kBorder);
            canvas.fill(0.0f, height() - 1.0f, width(), 1.0f);

            // Right-aligned subdued title — sets the tone for a tool, not a toy.
            canvas.setColor(kTextSec);
            canvas.text("LFO  /  ONESHOT  GRAPH", title_font_, visage::Font::kRight,
                        0.0f, 0.0f, width() - 12.0f, height() - 1.0f);
        }

    private:
        void showFileMenu(MenuBarButton* anchor)
        {
            anchor->setOpen(true);
            visage::PopupMenu m;
            m.addOption(1, "New");
            m.addOption(2, "Save");
            m.addOption(3, "Save As...");
            m.onSelection() = [this, anchor](int id) {
                anchor->setOpen(false);
                if (id == 1) owner_.actionNew();
                else if (id == 2) owner_.actionSave();
                else if (id == 3) owner_.actionSaveAs();
            };
            m.onCancel() = [anchor]() { anchor->setOpen(false); };
            m.show(anchor, { 0.0f, anchor->height() });
        }
        void showEditMenu(MenuBarButton* anchor)
        {
            anchor->setOpen(true);
            visage::PopupMenu m;
            m.addOption(1, "Copy point");
            m.addOption(2, "Cut");
            m.addOption(3, "Paste");
            m.addBreak();
            m.addOption(4, "Delete");
            m.onSelection() = [this, anchor](int id) {
                anchor->setOpen(false);
                switch (id)
                {
                    case 1: owner_.actionCopy(); break;
                    case 2: owner_.actionCut();  break;
                    case 3: owner_.actionPaste(owner_.paste_target_x_); break;
                    case 4: owner_.actionDelete(); break;
                }
            };
            m.onCancel() = [anchor]() { anchor->setOpen(false); };
            m.show(anchor, { 0.0f, anchor->height() });
        }

        GraphEditor& owner_;
        visage::Font label_font_;
        visage::Font title_font_;
        std::unique_ptr<MenuBarButton> file_btn_;
        std::unique_ptr<MenuBarButton> edit_btn_;
    };

    // =====================================================================
    //  Subdivision control (tiny +/- pill in the bottom-right of the graph)
    // =====================================================================
    class GraphEditor::SubdivControl : public visage::Frame
    {
    public:
        SubdivControl(GraphEditor& owner)
            : owner_(owner),
              font_(12.0f, resources::fonts::Roboto_Condensed_Regular_ttf) {}

        void draw(visage::Canvas& canvas) override
        {
            // Flat rectangular surface, not a pill.
            canvas.setColor(0xd9242529);
            canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kRadiusSmall);
            canvas.setColor(kBorder);
            canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), kRadiusSmall, 1.0f);

            // Minus.
            canvas.setColor(minus_hover_ ? kTextPri : kTextSec);
            canvas.fill(7.0f, height() * 0.5f - 0.5f, 8.0f, 1.0f);

            // Label.
            char buf[24];
            std::snprintf(buf, sizeof(buf), "Grid  %d", owner_.subdivisions_);
            canvas.setColor(kTextPri);
            canvas.text(buf, font_, visage::Font::kCenter,
                        0.0f, 0.0f, width(), height());

            // Plus.
            const float px = width() - 15.0f;
            canvas.setColor(plus_hover_ ? kTextPri : kTextSec);
            canvas.fill(px, height() * 0.5f - 0.5f, 8.0f, 1.0f);
            canvas.fill(px + 3.5f, height() * 0.5f - 4.0f, 1.0f, 8.0f);
        }

        void mouseMove(const visage::MouseEvent& e) override
        {
            const bool nm = e.position.x < width() * 0.33f;
            const bool np = e.position.x > width() * 0.66f;
            if (nm != minus_hover_ || np != plus_hover_)
            {
                minus_hover_ = nm;
                plus_hover_  = np;
                redraw();
            }
        }

        void mouseExit(const visage::MouseEvent&) override
        {
            if (minus_hover_ || plus_hover_)
            {
                minus_hover_ = false;
                plus_hover_  = false;
                redraw();
            }
        }

        void mouseDown(const visage::MouseEvent& e) override
        {
            if (e.position.x < width() * 0.33f)
                owner_.subdivisions_ = std::max(1, owner_.subdivisions_ - 1);
            else if (e.position.x > width() * 0.66f)
                owner_.subdivisions_ = std::min(64, owner_.subdivisions_ + 1);
            redraw();
            owner_.redrawCanvas();
        }

    private:
        GraphEditor& owner_;
        visage::Font font_;
        bool minus_hover_ = false;
        bool plus_hover_  = false;
    };

    // =====================================================================
    //  Graph strip — multi-graph switcher at the bottom of the canvas area
    // =====================================================================
    class GraphEditor::GraphStrip : public visage::Frame
    {
    public:
        GraphStrip(GraphEditor& owner)
            : owner_(owner),
              label_font_(9.0f, resources::fonts::Roboto_Condensed_Regular_ttf) {}

        // Rebuilds layout when the graph count changes (no children to
        // manage — tiles are drawn directly).
        void refresh() { redraw(); }

        void draw(visage::Canvas& canvas) override
        {
            canvas.setColor(kBgPanel);
            canvas.fill(0.0f, 0.0f, width(), height());
            canvas.setColor(kBorder);
            canvas.fill(0.0f, 0.0f, width(), 1.0f);     // top divider

            const int n = (int)owner_.graphs_.size();
            for (int i = 0; i < n; ++i)
                drawTile(canvas, tileRect(i), i);
            drawPlusTile(canvas, tileRect(n));
        }

        void mouseMove(const visage::MouseEvent& e) override
        {
            const int idx = tileAt(e.position);
            if (idx != hover_idx_)
            {
                hover_idx_ = idx;
                redraw();
            }
        }
        void mouseExit(const visage::MouseEvent&) override
        {
            if (hover_idx_ != -1) { hover_idx_ = -1; redraw(); }
        }

        void mouseDown(const visage::MouseEvent& e) override
        {
            const int n = (int)owner_.graphs_.size();
            const int idx = tileAt(e.position);

            if (idx < 0) return;
            if (idx == n)
            {
                // "+" tile.
                if (!e.isRightButton())
                    owner_.addGraph();
                return;
            }

            if (e.isRightButton())
            {
                // Right-click → popup with the delete option. The menu IS
                // the confirmation step; choosing it deletes immediately.
                // Disabled if this is the only graph (one must always exist).
                visage::PopupMenu m;
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Delete Graph %d", idx + 1);
                m.addOption(1, buf).enable(n > 1);
                m.onSelection() = [this, idx, n](int id) {
                    if (id == 1 && n > 1)
                        owner_.deleteGraph(idx);
                };
                m.show(this, e.position);
                return;
            }

            // Left-click → switch to that graph.
            if (idx != owner_.active_graph_)
                owner_.switchToGraph(idx);
        }

    private:
        // Geometry.
        static constexpr float kTilePad    = 8.0f;
        static constexpr float kTileGap    = 6.0f;
        static constexpr float kTileWidth  = 64.0f;
        static constexpr float kTileHeight = 36.0f;

        visage::Bounds tileRect(int idx) const
        {
            const float y = (height() - kTileHeight) * 0.5f;
            const float x = kTilePad + idx * (kTileWidth + kTileGap);
            return { x, y, kTileWidth, kTileHeight };
        }

        // Returns: -1 = outside; [0..n-1] = a graph tile; n = the "+" tile.
        int tileAt(visage::Point p) const
        {
            const int n = (int)owner_.graphs_.size();
            for (int i = 0; i <= n; ++i)
            {
                const auto r = tileRect(i);
                if (p.x >= r.x() && p.x <= r.right() &&
                    p.y >= r.y() && p.y <= r.bottom())
                    return i;
            }
            return -1;
        }

        void drawTile(visage::Canvas& canvas, visage::Bounds r, int idx)
        {
            const bool is_active = (idx == owner_.active_graph_);
            const bool is_hover  = (idx == hover_idx_);

            // Background.
            canvas.setColor(is_active ? kBgPanelHi : kBgGraph);
            canvas.roundedRectangle(r.x(), r.y(), r.width(), r.height(),
                                    kRadiusSmall);

            // Border (active = accent; hover = brighter; idle = subtle).
            const std::uint32_t border_col = is_active ? kAccent
                                            : is_hover  ? kBorderHi
                                            :             kBorder;
            canvas.setColor(border_col);
            canvas.roundedRectangleBorder(r.x(), r.y(), r.width(), r.height(),
                                          kRadiusSmall, is_active ? 1.5f : 1.0f);

            // Curve preview — drawn as a stroked path inside an inset region.
            const float inset = 6.0f;
            const float top_inset = 4.0f;
            const float bot_inset = 10.0f;   // leave room for the number label
            const float px = r.x() + inset;
            const float py = r.y() + top_inset;
            const float pw = r.width() - 2.0f * inset;
            const float ph = r.height() - top_inset - bot_inset;

            const auto& pts = owner_.graphs_[idx];
            if (pts.size() >= 2 && pw > 1.0f && ph > 1.0f)
            {
                visage::Path path;
                constexpr int kPreviewSamples = 24;
                for (int s = 0; s < kPreviewSamples; ++s)
                {
                    const float t  = (float)s / (float)(kPreviewSamples - 1);
                    const float y  = sampleCurve(pts, t);
                    const float sx = px + t * pw;
                    const float sy = py + (1.0f - y) * ph;
                    if (s == 0) path.moveTo(sx, sy);
                    else        path.lineTo(sx, sy);
                }
                // visage::Canvas::stroke is templated on a const Path& but
                // internally calls non-const Path::stroke — that's a bug in
                // visage. Build the stroked geometry ourselves and fill it.
                visage::Path stroked = path.stroke(1.25f,
                                                   visage::Path::Join::Round,
                                                   visage::Path::EndCap::Round);
                canvas.setColor(is_active ? kLine : kTextSec);
                canvas.fill(stroked, 0.0f, 0.0f, width(), height());
            }

            // Number label, bottom-centre.
            char num[8];
            std::snprintf(num, sizeof(num), "%d", idx + 1);
            canvas.setColor(is_active ? kTextPri : kTextSec);
            canvas.text(num, label_font_, visage::Font::kCenter,
                        r.x(), r.bottom() - 11.0f, r.width(), 10.0f);
        }

        void drawPlusTile(visage::Canvas& canvas, visage::Bounds r)
        {
            const int n = (int)owner_.graphs_.size();
            const bool is_hover = (hover_idx_ == n);

            canvas.setColor(is_hover ? kBgPanelHi : kBgGraph);
            canvas.roundedRectangle(r.x(), r.y(), r.width(), r.height(),
                                    kRadiusSmall);
            // Dashed look would be nicer but visage rect borders are solid;
            // a softer border distinguishes the "+" tile from real graphs.
            canvas.setColor(is_hover ? kBorderHi : kBorder);
            canvas.roundedRectangleBorder(r.x(), r.y(), r.width(), r.height(),
                                          kRadiusSmall, 1.0f);

            // Plus glyph.
            const float cx = r.x() + r.width() * 0.5f;
            const float cy = r.y() + r.height() * 0.5f - 2.0f;   // pull up so it sits above the implied label area
            const float len = 9.0f;
            canvas.setColor(is_hover ? kTextPri : kTextSec);
            canvas.fill(cx - len * 0.5f, cy - 0.5f, len, 1.0f);
            canvas.fill(cx - 0.5f, cy - len * 0.5f, 1.0f, len);
        }

        GraphEditor& owner_;
        visage::Font label_font_;
        int hover_idx_ = -1;
    };

    // =====================================================================
    //  Points overlay — draws handle dots on top of the GraphLine.
    // =====================================================================
    class GraphEditor::PointsOverlay : public visage::Frame
    {
    public:
        PointsOverlay(GraphEditor& owner) : owner_(owner)
        {
            // Pure decoration — mouse goes through to the Canvas.
            setIgnoresMouseEvents(true, false);
        }

        void draw(visage::Canvas& canvas) override
        {
            const float w = width();
            const float h = height();

            // Midpoint curve indicators (drawn first so endpoint handles
            // sit on top when very close to one another).
            const float mid_r = kHandleRadius * 0.7f;
            for (std::size_t i = 0; i + 1 < owner_.points_.size(); ++i)
            {
                const auto& a = owner_.points_[i];
                const auto& b = owner_.points_[i + 1];
                const float mid_x = 0.5f * (a.x + b.x);
                const float mid_y = a.y + (b.y - a.y) * applyCurve(0.5f, a.curve);
                const float cx = mid_x * w;
                const float cy = (1.0f - mid_y) * h;

                const bool is_hov = ((int)i == owner_.hover_mid_);
                const std::uint32_t fill = is_hov ? kPointHover : kTextSec;

                const float border = mid_r + 1.0f;
                canvas.setColor(kPointBorder);
                canvas.circle(cx - border, cy - border, border * 2.0f);
                canvas.setColor(fill);
                canvas.circle(cx - mid_r, cy - mid_r, mid_r * 2.0f);
            }

            // Regular endpoint handles.
            const int sel = owner_.selected_;
            const int hov = owner_.hover_point_;
            for (std::size_t i = 0; i < owner_.points_.size(); ++i)
            {
                const auto& p = owner_.points_[i];
                const float cx = p.x * w;
                const float cy = (1.0f - p.y) * h;

                const bool is_sel = ((int)i == sel);
                const bool is_hov = ((int)i == hov);
                const std::uint32_t fill = is_sel ? kPointSelected
                                          : is_hov ? kPointHover
                                          :          kPointFill;

                const float border = kHandleRadius + 1.0f;
                canvas.setColor(kPointBorder);
                canvas.circle(cx - border, cy - border, border * 2.0f);
                canvas.setColor(fill);
                canvas.circle(cx - kHandleRadius, cy - kHandleRadius,
                              kHandleRadius * 2.0f);
            }
        }

    private:
        GraphEditor& owner_;
    };

    // =====================================================================
    //  Canvas — the central graph area: grid, GraphLine child, mouse logic
    // =====================================================================
    class GraphEditor::Canvas : public visage::Frame
    {
    public:
        Canvas(GraphEditor& owner) : owner_(owner)
        {
            line_ = std::make_unique<visage::GraphLine>(kNumSamples);
            line_->setFilled(true);
            // Fill from the line downward to the bottom of the graph. In
            // visage sample-space, the bottom of the frame corresponds to
            // a sample value of 1.0 (kTop in the enum is misnamed — see
            // GraphLine::fillLocation, kBottom→0.0 and kTop→1.0).
            line_->setFillCenter(visage::GraphLine::kTop);
            line_->setIgnoresMouseEvents(true, false);
            addChild(line_.get());

            overlay_ = std::make_unique<PointsOverlay>(owner_);
            addChild(overlay_.get());

            subdiv_ = std::make_unique<SubdivControl>(owner_);
            addChild(subdiv_.get());
        }

        visage::GraphLine* graphLine() { return line_.get(); }
        PointsOverlay*     overlay()   { return overlay_.get(); }

        void resized() override
        {
            const float gx = 12.0f;
            const float gy = 12.0f;
            const float gw = width()  - 24.0f;
            const float gh = height() - 24.0f;

            line_->setBounds(gx, gy, gw, gh);
            overlay_->setBounds(gx, gy, gw, gh);

            subdiv_->setBounds(width() - kSubdivBoxW - 18.0f,
                               height() - kSubdivBoxH - 14.0f,
                               kSubdivBoxW, kSubdivBoxH);
        }

        void draw(visage::Canvas& canvas) override
        {
            const float gx = 12.0f, gy = 12.0f;
            const float gw = width() - 24.0f, gh = height() - 24.0f;

            // Outer well.
            canvas.setColor(kBgGraph);
            canvas.roundedRectangle(gx - 2.0f, gy - 2.0f, gw + 4.0f, gh + 4.0f, 6.0f);
            canvas.setColor(kBorder);
            canvas.roundedRectangleBorder(gx - 2.0f, gy - 2.0f, gw + 4.0f, gh + 4.0f, 6.0f, 1.0f);

            // Grid.
            const int n = std::max(1, owner_.subdivisions_);
            for (int i = 1; i < n; ++i)
            {
                const bool major = (i % 4 == 0);
                canvas.setColor(major ? kGridMajor : kGridMinor);
                const float xv = gx + (gw * i) / n;
                canvas.fill(xv, gy, 1.0f, gh);
                const float yv = gy + (gh * i) / n;
                canvas.fill(gx, yv, gw, 1.0f);
            }
            // Centre line.
            canvas.setColor(kGridMajor);
            canvas.fill(gx, gy + gh * 0.5f, gw, 1.0f);
        }

        // ---- Mouse handling ------------------------------------------------
        void mouseMove(const visage::MouseEvent& e) override
        {
            updateHoverAndPasteTarget(e);
        }

        void mouseExit(const visage::MouseEvent&) override
        {
            if (owner_.hover_point_ != -1 || owner_.hover_mid_ != -1)
            {
                owner_.hover_point_ = -1;
                owner_.hover_mid_   = -1;
                overlay_->redraw();
            }
        }

        void mouseDown(const visage::MouseEvent& e) override
        {
            updateHoverAndPasteTarget(e);

            // Right-click on a handle → delete that point (edges excluded).
            if (e.isRightButton())
            {
                tryDeleteAt(owner_.hover_point_);
                return;
            }

            // Double-click (and only double-click — triple+ falls through to
            // normal single-click handling so a quick "add-then-select"
            // gesture doesn't immediately delete the new point) toggles a
            // point at the cursor.
            if (e.repeatClickCount() == 2)
            {
                if (owner_.hover_point_ >= 0)
                    tryDeleteAt(owner_.hover_point_);
                else
                    insertPointAt(e.position.x - graphX(),
                                  e.position.y - graphY());
                return;
            }

            if (owner_.hover_point_ >= 0)
            {
                // Begin dragging a handle.
                owner_.selected_    = owner_.hover_point_;
                drag_mode_          = DragMode::Point;
                drag_index_         = owner_.hover_point_;
                overlay_->redraw();
                return;
            }

            // Midpoint indicator → start a curve drag.
            if (owner_.hover_mid_ >= 0)
            {
                drag_mode_  = DragMode::Curve;
                drag_index_ = owner_.hover_mid_;
                return;
            }

            // Nothing under the cursor — clear selection.
            if (owner_.selected_ != -1)
            {
                owner_.selected_ = -1;
                overlay_->redraw();
            }
        }

        void mouseDrag(const visage::MouseEvent& e) override
        {
            if (drag_mode_ == DragMode::Point && drag_index_ >= 0)
            {
                const float nx_raw = normX(e.position.x - graphX(), graphW());
                const float ny_raw = normY(e.position.y - graphY(), graphH());

                float nx = nx_raw;
                float ny = ny_raw;
                if (e.isAltDown())
                {
                    const float n = (float)std::max(1, owner_.subdivisions_);
                    nx = std::round(nx_raw * n) / n;
                    ny = std::round(ny_raw * n) / n;
                }

                const int last = (int)owner_.points_.size() - 1;
                const bool is_edge = (drag_index_ == 0 || drag_index_ == last);

                if (is_edge)
                {
                    // Edge points are pinned at x=0 / x=1 — only y is editable.
                    nx = (drag_index_ == 0) ? 0.0f : 1.0f;
                }
                else
                {
                    // Stay strictly between the edges so sort order is stable.
                    nx = std::clamp(nx, kInteriorEps, 1.0f - kInteriorEps);

                    // Ctrl/Cmd: don't let this point pass its current neighbours.
                    if (e.isMainModifier())
                    {
                        const float left  = owner_.points_[drag_index_ - 1].x + kInteriorEps;
                        const float right = owner_.points_[drag_index_ + 1].x - kInteriorEps;
                        if (left < right)
                            nx = std::clamp(nx, left, right);
                        else
                            nx = owner_.points_[drag_index_].x;  // no room — freeze x
                    }
                }

                auto& p = owner_.points_[drag_index_];
                p.x = nx;
                p.y = std::clamp(ny, 0.0f, 1.0f);

                // Re-sort & track the moved point's new index.
                Point moved = p;
                owner_.sortPointsByX();
                for (std::size_t i = 0; i < owner_.points_.size(); ++i)
                {
                    if (std::fabs(owner_.points_[i].x - moved.x) < 1e-6f &&
                        std::fabs(owner_.points_[i].y - moved.y) < 1e-6f &&
                        std::fabs(owner_.points_[i].curve - moved.curve) < 1e-6f)
                    {
                        drag_index_      = (int)i;
                        owner_.selected_ = (int)i;
                        break;
                    }
                }
                owner_.rebuildLineSamples();
                redrawAll();
            }
            else if (drag_mode_ == DragMode::Curve && drag_index_ >= 0)
            {
                // Solve for `curve` so the rendered line passes through
                // wherever the user has dragged the midpoint vertically.
                //
                // We use applyCurve(t, curve) from GraphPoint.h, which has
                // two well-behaved branches selected by sign(curve). Both
                // branches feed a non-negative `a = |curve|` into the
                // underlying formula, so no overshoot is possible.
                //
                // Bend-down branch (curve > 0):
                //   f(0.5, a) = 0.5 / (0.5a + 1)^2 = m
                //   ⇒ a = sqrt(2/m) - 2          (valid for m ≤ 0.5)
                //
                // Bend-up branch (curve < 0):
                //   1 - f(0.5, a) = m            (the mirror)
                //   f(0.5, a) = 1 - m
                //   ⇒ a = sqrt(2/(1-m)) - 2      (valid for m ≥ 0.5)
                //
                // Store the sign convention back into Point::curve.
                const int last = (int)owner_.points_.size() - 1;
                if (drag_index_ < 0 || drag_index_ >= last) return;
                const auto& a_pt = owner_.points_[drag_index_];
                const auto& b_pt = owner_.points_[drag_index_ + 1];
                const float dy = b_pt.y - a_pt.y;
                if (std::fabs(dy) < 1e-4f)
                    return;     // horizontal segment — formula can't bend it

                const float target_y_norm = normY(e.position.y - graphY(), graphH());
                float m = (target_y_norm - a_pt.y) / dy;
                m = std::clamp(m, 0.02f, 0.98f);

                float new_curve;
                if (std::fabs(m - 0.5f) < 1e-4f)
                    new_curve = 0.0f;
                else if (m < 0.5f)
                    new_curve =  std::sqrt(2.0f / m)         - 2.0f;
                else
                    new_curve = -(std::sqrt(2.0f / (1.0f - m)) - 2.0f);
                owner_.points_[drag_index_].curve = new_curve;
                owner_.rebuildLineSamples();
                redrawAll();
            }
        }

        void mouseUp(const visage::MouseEvent&) override
        {
            drag_mode_  = DragMode::None;
            drag_index_ = -1;
        }

        bool keyPress(const visage::KeyEvent& e) override
        {
            if (e.keyCode() == visage::KeyCode::Delete ||
                e.keyCode() == visage::KeyCode::Backspace)
            {
                owner_.actionDelete();
                return true;
            }
            return false;
        }

        // The visible graph rectangle inside Canvas's local space.
        float graphX() const { return 12.0f; }
        float graphY() const { return 12.0f; }
        float graphW() const { return width()  - 24.0f; }
        float graphH() const { return height() - 24.0f; }

    private:
        enum class DragMode { None, Point, Curve };

        void updateHoverAndPasteTarget(const visage::MouseEvent& e)
        {
            const int prev_pt  = owner_.hover_point_;
            const int prev_mid = owner_.hover_mid_;
            const float lx = e.position.x - graphX();
            const float ly = e.position.y - graphY();
            owner_.hover_point_ = owner_.pointAtScreen(lx, ly, kHitRadius);
            // Endpoint handles win when both are nearby.
            owner_.hover_mid_   = (owner_.hover_point_ == -1)
                                ? owner_.midpointAtScreen(lx, ly, kHitRadius)
                                : -1;
            owner_.paste_target_x_ = normX(lx, graphW());
            if (prev_pt != owner_.hover_point_ || prev_mid != owner_.hover_mid_)
                overlay_->redraw();
        }

        void redrawAll()
        {
            redraw();
            overlay_->redraw();
        }

        // Insert a new point at the given local coordinates and start
        // dragging it. Coordinates are relative to the graph rectangle's
        // top-left (i.e. caller has already subtracted graphX/Y).
        void insertPointAt(float local_x, float local_y)
        {
            const float nx = std::clamp(normX(local_x, graphW()),
                                        kInteriorEps, 1.0f - kInteriorEps);
            const float ny = normY(local_y, graphH());
            owner_.points_.push_back({ nx, ny, 0.0f });
            owner_.sortPointsByX();

            int idx = -1;
            for (std::size_t i = 0; i < owner_.points_.size(); ++i)
                if (std::fabs(owner_.points_[i].x - nx) < 1e-6f &&
                    std::fabs(owner_.points_[i].y - ny) < 1e-6f)
                    idx = (int)i;

            owner_.selected_    = idx;
            owner_.hover_point_ = idx;
            drag_mode_          = DragMode::Point;
            drag_index_         = idx;
            owner_.rebuildLineSamples();
            redrawAll();
        }

        // Delete a point if it's an interior one. Edge points (x=0, x=1)
        // are pinned and silently ignored.
        void tryDeleteAt(int idx)
        {
            const int last = (int)owner_.points_.size() - 1;
            if (idx <= 0 || idx >= last) return;
            owner_.points_.erase(owner_.points_.begin() + idx);
            owner_.selected_    = -1;
            owner_.hover_point_ = -1;
            owner_.rebuildLineSamples();
            redrawAll();
        }

        GraphEditor& owner_;
        std::unique_ptr<visage::GraphLine> line_;
        std::unique_ptr<PointsOverlay>     overlay_;
        std::unique_ptr<SubdivControl>     subdiv_;

        DragMode drag_mode_  = DragMode::None;
        int      drag_index_ = -1;
    };

    // =====================================================================
    //  Side panel
    // =====================================================================
    namespace
    {
        // Animated pill-style two-position toggle.
        class ModeToggle : public visage::Frame
        {
        public:
            using OnChange = std::function<void(int)>;

            ModeToggle()
                : font_(12.0f, resources::fonts::Roboto_Condensed_Regular_ttf) {}

            void setLabels(std::string a, std::string b)
            {
                label_a_ = std::move(a);
                label_b_ = std::move(b);
                redraw();
            }
            void setIndex(int idx, bool animated = true)
            {
                target_ = std::clamp(idx, 0, 1);
                if (!animated) current_ = (float)target_;
                last_ms_ = visage::time::milliseconds();
                redraw();
            }
            int index() const { return target_; }
            void onChange(OnChange cb) { on_change_ = std::move(cb); }

            void mouseDown(const visage::MouseEvent& e) override
            {
                const int idx = (e.position.x < width() * 0.5f) ? 0 : 1;
                if (idx != target_)
                {
                    setIndex(idx);
                    if (on_change_) on_change_(idx);
                }
            }

            void draw(visage::Canvas& canvas) override
            {
                const long long now = visage::time::milliseconds();
                const float dt_ms = (float)(now - last_ms_);
                last_ms_ = now;
                const float speed = 1.0f / 140.0f;
                const float t = (float)target_;
                if (std::fabs(current_ - t) > 1e-4f)
                {
                    const float dir = (t > current_) ? 1.0f : -1.0f;
                    current_ = std::clamp(current_ + dir * dt_ms * speed, 0.0f, 1.0f);
                    if ((dir > 0 && current_ > t) || (dir < 0 && current_ < t))
                        current_ = t;
                    redraw();
                }

                // Recessed track, not a pill.
                canvas.setColor(kBgGraph);
                canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kRadiusMedium);
                canvas.setColor(kBorder);
                canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), kRadiusMedium, 1.0f);

                // Sliding highlight — neutral panel surface, not an accent stripe.
                const float pad = 2.0f;
                const float half = (width() - 2.0f * pad) * 0.5f;
                const float hx = pad + current_ * half;
                canvas.setColor(kBgPanelHi);
                canvas.roundedRectangle(hx, pad, half, height() - 2.0f * pad,
                                        kRadiusSmall);
                canvas.setColor(kBorderHi);
                canvas.roundedRectangleBorder(hx, pad, half, height() - 2.0f * pad,
                                              kRadiusSmall, 1.0f);

                // Labels.
                const std::uint32_t col_a = (target_ == 0) ? kTextPri : kTextSec;
                const std::uint32_t col_b = (target_ == 1) ? kTextPri : kTextSec;
                canvas.setColor(col_a);
                canvas.text(label_a_, font_, visage::Font::kCenter,
                            0.0f, 0.0f, width() * 0.5f, height());
                canvas.setColor(col_b);
                canvas.text(label_b_, font_, visage::Font::kCenter,
                            width() * 0.5f, 0.0f, width() * 0.5f, height());
            }

        private:
            visage::Font font_;
            std::string label_a_ = "A";
            std::string label_b_ = "B";
            int     target_  = 0;
            float   current_ = 0.0f;
            long long last_ms_ = 0;
            OnChange on_change_;
        };

        // Horizontal click-and-drag numeric row.
        class SettingRow : public visage::Frame
        {
        public:
            using OnChange = std::function<void(float)>;

            SettingRow()
                : font_(12.0f, resources::fonts::Roboto_Condensed_Regular_ttf) {}

            void configure(std::string label, float min, float max, float value,
                           std::string suffix, float sensitivity = 0.5f)
            {
                label_     = std::move(label);
                suffix_    = std::move(suffix);
                min_       = min;
                max_       = max;
                value_     = std::clamp(value, min, max);
                sens_      = sensitivity;
                redraw();
            }
            void setValue(float v) { value_ = std::clamp(v, min_, max_); redraw(); }
            float value() const { return value_; }
            void onChange(OnChange cb) { on_change_ = std::move(cb); }

            void mouseDown(const visage::MouseEvent& e) override
            {
                last_x_ = e.position.x;
            }
            void mouseDrag(const visage::MouseEvent& e) override
            {
                const float dx = e.position.x - last_x_;
                last_x_ = e.position.x;
                const float scale = e.isCtrlDown() ? 0.05f : 1.0f;
                const float range = (max_ - min_);
                value_ = std::clamp(value_ + dx * sens_ * scale * range / std::max(1.0f, width()),
                                    min_, max_);
                if (on_change_) on_change_(value_);
                redraw();
            }
            void mouseEnter(const visage::MouseEvent&) override
            {
                hover_ = true; redraw(); setCursorStyle(visage::MouseCursor::Dragging);
            }
            void mouseExit(const visage::MouseEvent&) override
            {
                hover_ = false; redraw(); setCursorStyle(visage::MouseCursor::Arrow);
            }

            void draw(visage::Canvas& canvas) override
            {
                const float r = kRadiusSmall;
                canvas.setColor(hover_ ? kBgPanelHi : kBgGraph);
                canvas.roundedRectangle(0.0f, 0.0f, width(), height(), r);
                canvas.setColor(hover_ ? kBorderHi : kBorder);
                canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), r, 1.0f);

                // Very subtle value-progress indicator along the bottom edge.
                const float ratio = (value_ - min_) / std::max(1e-6f, max_ - min_);
                canvas.setColor(kAccent);
                canvas.fill(0.0f, height() - 1.0f, width() * ratio, 1.0f);

                canvas.setColor(kTextSec);
                canvas.text(label_, font_, visage::Font::kLeft,
                            10.0f, 0.0f, width() - 20.0f, height());

                char buf[32];
                if (suffix_.empty())
                    std::snprintf(buf, sizeof(buf), "%.2f", value_);
                else
                    std::snprintf(buf, sizeof(buf), "%.2f %s", value_, suffix_.c_str());
                canvas.setColor(kTextPri);
                canvas.text(buf, font_, visage::Font::kRight,
                            10.0f, 0.0f, width() - 20.0f, height());
            }

        private:
            visage::Font font_;
            std::string label_;
            std::string suffix_;
            float min_ = 0.0f, max_ = 1.0f, value_ = 0.0f, sens_ = 0.5f;
            float last_x_ = 0.0f;
            bool  hover_ = false;
            OnChange on_change_;
        };
    }

    class GraphEditor::SidePanel : public visage::Frame
    {
    public:
        SidePanel(GraphEditor& owner)
            : owner_(owner),
              header_font_(12.0f, resources::fonts::Roboto_Condensed_Regular_ttf),
              section_font_(9.0f,  resources::fonts::Roboto_Condensed_Regular_ttf)
        {
            toggle_ = std::make_unique<ModeToggle>();
            toggle_->setLabels("LFO", "OneShot");
            toggle_->setIndex(owner_.mode_ == Mode::LFO ? 0 : 1, false);
            toggle_->onChange([this](int idx) {
                owner_.mode_ = (idx == 0) ? Mode::LFO : Mode::OneShot;
                rate_row_->setVisible(idx == 0);
                redraw();
            });
            addChild(toggle_.get());

            latency_row_   = makeRow("Latency", 0.0f,   500.0f, owner_.latency_ms_, "ms",
                                     [this](float v) { owner_.latency_ms_ = v; });
            rate_row_      = makeRow("Rate",    0.01f,  20.0f,  owner_.rate_hz_,    "Hz",
                                     [this](float v) { owner_.rate_hz_ = v; });
            amp_row_       = makeRow("Amplitude", 0.0f, 1.0f,   owner_.amplitude_,  "",
                                     [this](float v) { owner_.amplitude_ = v; });
            phase_row_     = makeRow("Phase",   0.0f,   1.0f,   owner_.phase_,      "",
                                     [this](float v) { owner_.phase_ = v; });

            rate_row_->setVisible(owner_.mode_ == Mode::LFO);
        }

        void resized() override
        {
            const float pad = 14.0f;
            float y = pad + 18.0f;            // leave room for SETTINGS header
            toggle_->setBounds(pad, y, width() - 2.0f * pad, 28.0f);
            y += 28.0f + 24.0f;                // gap + space for TIMING header
            const float rh = 24.0f;
            latency_row_->setBounds(pad, y, width() - 2.0f * pad, rh); y += rh + 8.0f;
            rate_row_   ->setBounds(pad, y, width() - 2.0f * pad, rh); y += rh + 24.0f; // gap + SHAPE
            amp_row_    ->setBounds(pad, y, width() - 2.0f * pad, rh); y += rh + 8.0f;
            phase_row_  ->setBounds(pad, y, width() - 2.0f * pad, rh);
        }

        void draw(visage::Canvas& canvas) override
        {
            canvas.setColor(kBgPanel);
            canvas.fill(0.0f, 0.0f, width(), height());
            canvas.setColor(kBorder);
            canvas.fill(0.0f, 0.0f, 1.0f, height());     // left divider

            const float pad = 14.0f;
            canvas.setColor(kTextSec);
            canvas.text("SETTINGS", section_font_, visage::Font::kLeft,
                        pad, pad - 2.0f, width(), 16.0f);

            const float toggle_bottom = pad + 18.0f + 28.0f;
            canvas.text("TIMING", section_font_, visage::Font::kLeft,
                        pad, toggle_bottom + 6.0f, width(), 14.0f);

            const float timing_bottom = toggle_bottom + 22.0f + 24.0f + 8.0f + 24.0f;
            canvas.text("SHAPE", section_font_, visage::Font::kLeft,
                        pad, timing_bottom + 6.0f, width(), 14.0f);
        }

    private:
        std::unique_ptr<SettingRow> makeRow(std::string label, float lo, float hi,
                                            float val, std::string suffix,
                                            std::function<void(float)> cb)
        {
            auto row = std::make_unique<SettingRow>();
            row->configure(std::move(label), lo, hi, val, std::move(suffix));
            row->onChange(std::move(cb));
            addChild(row.get());
            return row;
        }

        GraphEditor& owner_;
        visage::Font header_font_;
        visage::Font section_font_;

        std::unique_ptr<ModeToggle>  toggle_;
        std::unique_ptr<SettingRow>  latency_row_;
        std::unique_ptr<SettingRow>  rate_row_;
        std::unique_ptr<SettingRow>  amp_row_;
        std::unique_ptr<SettingRow>  phase_row_;
    };

    // =====================================================================
    //  GraphEditor
    // =====================================================================
    GraphEditor::GraphEditor()
    {
        setAcceptsKeystrokes(true);
        setPalette(&palette_);

        // Override GraphLine theme via the palette so the canvas uses our
        // cyan accent and a vertical fade for the fill.
        palette_.setColor(visage::GraphLine::LineColor, visage::Color(kLine));
        palette_.setColor(visage::GraphLine::LineFillColor,
                          visage::Brush::vertical(visage::Color(kLineFillTop),
                                                  visage::Color(kLineFillBot)));
        palette_.setColor(visage::GraphLine::LineFillColor2,
                          visage::Brush::vertical(visage::Color(kLineFillTop),
                                                  visage::Color(kLineFillBot)));
        palette_.setValue(visage::GraphLine::LineWidth, 2.0f);

        resetToDefault();
        graphs_.push_back(points_);     // seed the first graph

        toolbar_ = std::make_unique<Toolbar>(*this);
        canvas_  = std::make_unique<Canvas>(*this);
        side_    = std::make_unique<SidePanel>(*this);
        strip_   = std::make_unique<GraphStrip>(*this);

        addChild(toolbar_.get());
        addChild(canvas_.get());
        addChild(side_.get());
        addChild(strip_.get());

        rebuildLineSamples();
    }

    GraphEditor::~GraphEditor() = default;

    void GraphEditor::resized()
    {
        constexpr float kStripHeight = 56.0f;

        toolbar_->setBounds(0.0f, 0.0f, width(), kToolbarHeight);

        const float canvas_w = width() - kSidePanelWidth;
        const float canvas_h = height() - kToolbarHeight - kStripHeight;
        canvas_ ->setBounds(0.0f, kToolbarHeight, canvas_w, canvas_h);
        strip_  ->setBounds(0.0f, kToolbarHeight + canvas_h, canvas_w, kStripHeight);
        side_   ->setBounds(canvas_w, kToolbarHeight, kSidePanelWidth,
                            height() - kToolbarHeight);
    }

    void GraphEditor::draw(visage::Canvas& canvas)
    {
        canvas.setColor(kBgDeep);
        canvas.fill(0.0f, 0.0f, width(), height());
    }

    // ---- data ops ----------------------------------------------------------
    void GraphEditor::resetToDefault()
    {
        points_.clear();
        points_.push_back({ 0.0f, 0.5f, 0.0f });
        points_.push_back({ 0.5f, 1.0f, 0.0f });
        points_.push_back({ 1.0f, 0.5f, 0.0f });
        selected_ = -1;
        hover_point_ = -1;
    }

    void GraphEditor::sortPointsByX()
    {
        std::sort(points_.begin(), points_.end(),
                  [](const Point& a, const Point& b) { return a.x < b.x; });
    }

    int GraphEditor::pointAtScreen(float local_x, float local_y, float radius_px) const
    {
        if (!canvas_) return -1;
        const float gw = canvas_->graphW();
        const float gh = canvas_->graphH();
        int best = -1;
        float best_d2 = radius_px * radius_px;
        for (std::size_t i = 0; i < points_.size(); ++i)
        {
            const float cx = points_[i].x * gw;
            const float cy = (1.0f - points_[i].y) * gh;
            const float dx = local_x - cx;
            const float dy = local_y - cy;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
        }
        return best;
    }

    int GraphEditor::midpointAtScreen(float local_x, float local_y, float radius_px) const
    {
        if (points_.size() < 2 || !canvas_) return -1;
        const float gw = canvas_->graphW();
        const float gh = canvas_->graphH();
        int best = -1;
        float best_d2 = radius_px * radius_px;
        for (std::size_t i = 0; i + 1 < points_.size(); ++i)
        {
            const auto& a = points_[i];
            const auto& b = points_[i + 1];
            const float mid_x = 0.5f * (a.x + b.x);
            const float mid_y = a.y + (b.y - a.y) * applyCurve(0.5f, a.curve);
            const float cx = mid_x * gw;
            const float cy = (1.0f - mid_y) * gh;
            const float dx = local_x - cx;
            const float dy = local_y - cy;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
        }
        return best;
    }

    void GraphEditor::rebuildLineSamples()
    {
        // Keep graphs_ in sync so the strip's preview tile reflects the
        // edit immediately.
        commitActive();

        if (!canvas_ || !canvas_->graphLine()) return;
        auto* line = canvas_->graphLine();
        // visage::GraphLine treats sample=0 as the top of the frame and
        // sample=1 as the bottom; our Point::y is the opposite (1 = top),
        // so invert before writing.
        for (int i = 0; i < kNumSamples; ++i)
        {
            const float x = (float)i / (float)(kNumSamples - 1);
            line->set(i, 1.0f - sampleCurve(points_, x));
        }
        if (strip_) strip_->redraw();
    }

    void GraphEditor::redrawCanvas()
    {
        if (canvas_)        canvas_->redraw();
        if (canvas_)        canvas_->overlay()->redraw();
    }

    // --- multi-graph management ---------------------------------------------
    void GraphEditor::commitActive()
    {
        if (active_graph_ < 0 || active_graph_ >= (int)graphs_.size()) return;
        graphs_[active_graph_] = points_;
    }

    void GraphEditor::switchToGraph(int idx)
    {
        if (idx < 0 || idx >= (int)graphs_.size() || idx == active_graph_) return;
        commitActive();
        active_graph_ = idx;
        points_       = graphs_[active_graph_];
        selected_     = -1;
        hover_point_  = -1;
        hover_mid_    = -1;
        rebuildLineSamples();
        redrawCanvas();
        if (strip_) strip_->redraw();
    }

    void GraphEditor::addGraph()
    {
        commitActive();
        // Fresh default curve, identical to GraphEditor's initial state.
        std::vector<Point> fresh;
        fresh.push_back({ 0.0f, 0.5f, 0.0f });
        fresh.push_back({ 0.5f, 1.0f, 0.0f });
        fresh.push_back({ 1.0f, 0.5f, 0.0f });
        graphs_.push_back(std::move(fresh));

        active_graph_ = (int)graphs_.size() - 1;
        points_       = graphs_[active_graph_];
        selected_     = -1;
        hover_point_  = -1;
        hover_mid_    = -1;
        rebuildLineSamples();
        redrawCanvas();
        if (strip_) strip_->redraw();
    }

    void GraphEditor::deleteGraph(int idx)
    {
        if (idx < 0 || idx >= (int)graphs_.size()) return;
        if (graphs_.size() <= 1) return;     // keep at least one graph

        commitActive();
        graphs_.erase(graphs_.begin() + idx);

        if (active_graph_ == idx)
            active_graph_ = std::max(0, idx - 1);
        else if (active_graph_ > idx)
            --active_graph_;

        points_      = graphs_[active_graph_];
        selected_    = -1;
        hover_point_ = -1;
        hover_mid_   = -1;
        rebuildLineSamples();
        redrawCanvas();
        if (strip_) strip_->redraw();
    }

    // ---- toolbar actions ---------------------------------------------------
    void GraphEditor::actionNew()
    {
        resetToDefault();
        rebuildLineSamples();
        redrawCanvas();
    }

    void GraphEditor::actionSave()
    {
        if (!current_path_.empty())
        {
            saveToFile(current_path_);
            return;
        }
        actionSaveAs();
    }

    void GraphEditor::actionSaveAs()
    {
        if (!requestSavePath) return;
        requestSavePath([this](std::string path) {
            if (path.empty()) return;
            if (saveToFile(path))
                current_path_ = std::move(path);
        });
    }

    void GraphEditor::actionCopy()
    {
        if (selected_ < 0 || selected_ >= (int)points_.size()) return;
        clipboard_value_     = points_[selected_].y;
        clipboard_has_value_ = true;

        // Mirror to OS clipboard so the user can paste into a DAW field too.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", clipboard_value_);
        setClipboardText(buf);
    }

    void GraphEditor::actionCut()
    {
        actionCopy();
        actionDelete();
    }

    void GraphEditor::actionPaste(float at_norm_x)
    {
        // Prefer in-process clipboard; fall back to OS clipboard (any float
        // parsable string works).
        float y = 0.5f;
        if (clipboard_has_value_)
            y = clipboard_value_;
        else
        {
            const std::string clip = readClipboardText();
            try { y = std::stof(clip); } catch (...) { y = 0.5f; }
        }
        y = std::clamp(y, 0.0f, 1.0f);
        // Keep pasted points in the interior so the dedicated edge points
        // at x=0 and x=1 are never displaced.
        const float x = std::clamp(at_norm_x, kInteriorEps, 1.0f - kInteriorEps);
        points_.push_back({ x, y, 0.0f });
        sortPointsByX();
        rebuildLineSamples();
        redrawCanvas();
    }

    void GraphEditor::actionDelete()
    {
        const int last = (int)points_.size() - 1;
        // Edge points (x=0, x=1) are pinned — only interior points are deletable.
        if (selected_ <= 0 || selected_ >= last) return;
        points_.erase(points_.begin() + selected_);
        selected_ = -1;
        hover_point_ = -1;
        rebuildLineSamples();
        redrawCanvas();
    }

    bool GraphEditor::saveToFile(const std::string& path) const
    {
        std::ofstream out(path);
        if (!out) return false;
        out << "# TestingPlayground LFO graph\n";
        out << "mode " << (mode_ == Mode::LFO ? "lfo" : "oneshot") << "\n";
        out << "latency_ms " << latency_ms_ << "\n";
        out << "rate_hz "    << rate_hz_    << "\n";
        out << "amplitude "  << amplitude_  << "\n";
        out << "phase "      << phase_      << "\n";
        out << "subdivisions " << subdivisions_ << "\n";
        for (const auto& p : points_)
            out << "point " << p.x << " " << p.y << " " << p.curve << "\n";
        return out.good();
    }

    bool GraphEditor::loadFromFile(const std::string& path)
    {
        std::ifstream in(path);
        if (!in) return false;
        std::vector<Point> loaded;
        std::string tok;
        while (in >> tok)
        {
            if (tok == "point")
            {
                Point p;
                in >> p.x >> p.y >> p.curve;
                loaded.push_back(p);
            }
            else if (tok == "mode")
            {
                std::string m; in >> m;
                mode_ = (m == "lfo") ? Mode::LFO : Mode::OneShot;
            }
            else if (tok == "latency_ms")   in >> latency_ms_;
            else if (tok == "rate_hz")      in >> rate_hz_;
            else if (tok == "amplitude")    in >> amplitude_;
            else if (tok == "phase")        in >> phase_;
            else if (tok == "subdivisions") in >> subdivisions_;
            else                            std::getline(in, tok);  // skip comment / unknown
        }
        if (!loaded.empty())
        {
            points_ = std::move(loaded);
            sortPointsByX();
            // Ensure dedicated edge points exist at x=0 and x=1.
            if (points_.front().x > kInteriorEps)
                points_.insert(points_.begin(), { 0.0f, points_.front().y, 0.0f });
            else
                points_.front().x = 0.0f;
            if (points_.back().x < 1.0f - kInteriorEps)
                points_.push_back({ 1.0f, points_.back().y, 0.0f });
            else
                points_.back().x = 1.0f;
        }
        rebuildLineSamples();
        redrawCanvas();
        return true;
    }
}
