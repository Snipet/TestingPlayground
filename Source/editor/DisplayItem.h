// Source/editor/DisplayItem.h: one of a program's displays on the panel (fminus/PanelSpec.h: meter, vu, led, number,
// scope), drawn by Looks.h from what the Engine published (Engine::Meters, Engine::scope), which the Panel hands it
// each frame. It moves as hardware does:
//   meter   the audio's peaks in dBFS, -48 to +6 (the Engine's peaks fall 20 dB a second), or a field's value across
//           its range; the highest of the last second held, then falling
//   vu      VU ballistics (about 300 ms to settle) on the audio's RMS, 0 VU at -18 dBFS; or on a field's value
//   led     the audio's clip light (a peak at 0 dBFS lights it for 0.4 s); or a field's value as its brightness
//   number  the audio's peak in dBFS, or the field's value, eight times a second
//   scope   the audio's last 21 ms from a rising zero crossing; or the field's last 2.5 seconds
//   spectrum (a .gui file's) the audio's last 2048 samples through a Hann window and an FFT, in dB against a full-
//           scale sine, at kSpectrumPoints frequencies from 20 Hz to 20 kHz spaced evenly in octaves (each the
//           strongest bin in its share of them); a peak shows at once and falls 30 dB a second
// It is a readout: no input, no Tab stop; accessibility reads its label and value. settled() while nothing it shows
// moves, so a silent panel lets the host rest.
#pragma once

#include "editor/DialLayout.h"
#include "editor/Items.h"

#include "dsp/Engine.h"
#include "fminus/PanelSpec.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground::ui
{
    class DisplayItem final : public Item
    {
    public:
        static constexpr int kScopeSamples = 2048;    // what the Panel reads of a scope's audio each frame
        static constexpr int kHistory = 150;          // a field scope's points (2.5 s at 60 frames a second)
        static constexpr int kSpectrumPoints = 96;

        explicit DisplayItem(uint32_t id) : id_(id) {}

        // The display it shows (null: none, not shown) and its cell (DialLayout's place).
        void setDisplay(const PanelDisplay* d);
        void setCell(const ItemPlace& p) { cell_ = { p.x, p.top, p.w, p.h }; }
        const PanelDisplay& display() const noexcept { return display_; }
        bool wantsScope(bool& input) const noexcept;  // an audio scope: which of the Engine's rings it reads

        // A frame: the meters, the scope's samples (kScopeSamples, oldest first, when wantsScope), dt in seconds.
        void update(const Meters& m, const float* scope, float dt);

        // A cell of any size (a .gui file's layout).
        void setRect(const funkgui::Rect& r) { cell_ = r; }
        const funkgui::Rect& cell() const noexcept { return cell_; }
        // The part of the panel it may draw in and take input in (a .gui panel's area; everywhere when unset).
        void setArea(const funkgui::Rect& r) { area_ = r; }
        // A spectrum's points, dB (kSpectrumPoints, from 20 Hz up), for the probes.
        const std::vector<float>& spectrum() const noexcept { return spectrum_; }

        // For probes: what it shows now (a meter's first level and a VU's needle 0..1, an LED's brightness), and its
        // readout.
        float level() const noexcept;
        const std::string& readout() const noexcept { return readout_; }
        int traceSize() const noexcept { return static_cast<int>(display_.audio() ? trace_.size() : history_.size()); }

        // Item
        void tick(float, bool, bool, funkgui::Point) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const override;
        bool contains(funkgui::Point p) const override { return shown_ && cell_.contains(p) && (area_.w <= 0.0f || area_.contains(p)); }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !moving_; }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_; }
        bool focusable() const override { return false; }

    private:
        float fieldPosition(float v) const noexcept;  // a field's value across its range, 0..1

        uint32_t id_;
        PanelDisplay display_;
        bool shown_ = false;
        funkgui::Rect cell_;
        // What it shows, moving.
        std::array<float, 2> level_{}, hold_{}, holdFor_{};   // a meter's bars, 0..1
        float needle_ = 0.0f;                         // a VU's, 0..1
        float glow_ = 0.0f;                           // an LED's
        float readoutIn_ = 0.0f;                      // seconds to the number's next refresh
        std::string readout_;
        std::vector<float> trace_;                    // a scope's points, -1..1
        std::vector<float> history_;                  // a field scope's values, oldest first
        std::vector<float> spectrum_;                 // a spectrum's points, dB
        funkgui::Rect area_;
        bool moving_ = false;
        uint32_t lastFrame_ = 0;
    };
}
