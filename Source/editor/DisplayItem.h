// Source/editor/DisplayItem.h: one of a program's displays on the panel (fminus/PanelSpec.h: meter, vu, led, number,
// scope), drawn by Looks.h from what the Engine published (Meters.h, Engine::scope), which the Panel hands it
// each frame. It moves as hardware does:
//   meter   the audio's peaks in dBFS, -48 to +6 (the Engine's peaks fall 20 dB a second), or a field's value across
//           its range; the highest of the last second held, then falling
//   vu      VU ballistics (about 300 ms to settle) on the audio's RMS, 0 VU at -18 dBFS; or on a field's value
//   led     the audio's clip light (a peak at 0 dBFS lights it for 0.4 s); or a field's value as its brightness
//   number  the audio's peak in dBFS, or the field's value, eight times a second (an int's or a bool's a whole number;
//           with unit=, decimals= or scale=, as a knob's value reads)
//   scope   the audio's last 21 ms from a rising zero crossing (found between two samples, so a steady tone's trace
//           stands still); or the field's last 2.5 seconds
//   spectrum (a .gui file's) its source's Spectrum (below: the audio's last 2048 samples through a Hann window and
//           an FFT, made once for every display of that source); a peak shows at once and falls 30 dB a second
// It is a readout: no input, no Tab stop; accessibility reads its label and value. settled() once nothing it shows has
// moved for a moment (Stirred: the audio's blocks may come a frame or two apart), so a silent or steady panel lets the
// host rest: an audio scope moves when a point of its trace would move by more than half a px (the trace drawn is the
// last one that did), a field scope when the field's value changes or its points differ (a flat line shifted along is
// the same line). The readout's text (a meter's, a number's) is no motion but its drawKey(): each new one, eight a
// second at most, asks for one frame, so a number that keeps changing leaves the panel at the calm rate.
#pragma once

#include "editor/DialLayout.h"
#include "editor/Items.h"

#include "dsp/Meters.h"
#include "fminus/PanelSpec.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
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

        // A frame: the meters, the scope's samples (kScopeSamples, oldest first, when wantsScope), a spectrum's points
        // (its source's Spectrum::points(), kSpectrumPoints; null for any other display), dt in seconds.
        void update(const Meters& m, const float* scope, const float* spectrum, float dt);

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
        // Its readout made again at the next update (a picture's last frame: what it shows then, not 1/8 s before).
        void readoutNow() noexcept { readoutIn_ = 0.0f; }
        int traceSize() const noexcept { return display_.audio() ? static_cast<int>(trace_.size()) : historyCount_; }
        // A scope's points as draw() takes them: the audio's trace, or the field's values, oldest first.
        std::vector<float> points() const
        {
            return display_.audio() ? trace_ : std::vector<float>(history(), history() + historyCount_);
        }

        // Item
        void tick(float dt, bool, bool, funkgui::Point) override { stir_.tick(dt, moving_); }   // after update()
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const override;
        bool contains(funkgui::Point p) const override { return shown_ && cell_.contains(p) && (area_.w <= 0.0f || area_.contains(p)); }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !shown_ || !stir_.moving(); }
        uint64_t drawKey() const override                // a meter's and a number's readout (no motion: one frame each)
        {
            const bool drawn = display_.kind == DisplayKind::meter || display_.kind == DisplayKind::number;
            return drawn ? DrawKey{}.mix(readout_).h : 0;
        }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_; }
        bool focusable() const override { return false; }

    private:
        float fieldPosition(float v) const noexcept;  // a field's value across its range, 0..1
        void pushHistory(float v) noexcept;
        const float* history() const noexcept         // a field scope's values, oldest first (historyCount_)
        {
            return history_.data() + (historyAt_ + kHistory - historyCount_) % kHistory;
        }

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
        std::vector<float> trace_;                    // a scope's points, -1..1, as last moved
        // A field scope's values: a ring written twice over (each value at i and i + kHistory), so the newest
        // historyCount_ are always in a row, oldest first (history()); historySame_ of the newest are equal.
        std::array<float, 2 * kHistory> history_{};
        int historyAt_ = 0, historyCount_ = 0, historySame_ = 0;
        std::vector<float> spectrum_;                 // a spectrum's points, dB
        funkgui::Rect area_;
        bool moving_ = false;                         // at the last update()
        Stirred stir_;                                // moving: kept a moment, as blocks come a frame or two apart
        uint32_t lastFrame_ = 0;
    };

    // A source's spectrum (the input's or the output's), which every spectrum display of that source shows: its last
    // DisplayItem::kScopeSamples samples through a Hann window and a real FFT, in dB against a full-scale sine, at
    // DisplayItem::kSpectrumPoints frequencies from 20 Hz to 20 kHz spaced evenly in octaves, each the strongest bin in
    // its share of them (the bins' powers compared, one logarithm a point). update() computes it only when the samples
    // or the rate differ from the last it took: the Engine writes the scope's ring once a block, so a frame with no new
    // block costs no FFT, and a frame with one costs one however many displays show the source. Its buffers are sized
    // once (nothing allocated a frame); the window and the twiddles are tables made once for the process, and each
    // point's bins are worked out again only when the rate changes.
    class Spectrum
    {
    public:
        Spectrum();

        // The spectrum of `x` (kScopeSamples, oldest first) at `rate`; false when they are those of the last call (the
        // points as they were).
        bool update(const float* x, float rate);
        // The same, whatever the last call took.
        void compute(const float* x, float rate);
        const float* points() const noexcept { return points_.data(); }   // kSpectrumPoints, dB
        int computed() const noexcept { return computed_; }              // FFTs run, for the probes

    private:
        void binsFor(float rate);

        std::vector<float> last_;                     // the samples it last took
        bool taken_ = false;
        float rate_ = 0.0f;                           // the rate bins_ are for
        std::vector<float> re_, im_;                  // the complex FFT's halves
        std::vector<float> power_;                    // each bin's power, 0 to kScopeSamples / 2
        std::array<int16_t, 2 * DisplayItem::kSpectrumPoints> bins_{};   // each point's first and last bin
        std::array<float, DisplayItem::kSpectrumPoints> points_{};
        int computed_ = 0;
    };

    // What updateDisplays (GuiView.h) keeps between frames: each source's samples, read once a frame for every display
    // of it, and its spectrum.
    struct DisplayFeed
    {
        std::vector<float> in, out;
        Spectrum inSpectrum, outSpectrum;
    };

    // The fields a panel's displays read of a frame (Meters::taps), for the panel of `program` whose displays index
    // tap table `table` (PanelSpec::tapTable): the frame's when it is of both; the last such frame's while it is of the
    // program but of another table (a panel's edit's table, read by the audio thread before the panel has the edit's
    // status: its displays hold their values); else none, every field 0 (another program's, or the panel's own new
    // table not read yet: the fields at those indices are others'). show() puts them in `m` and says whether they are
    // the panel's own (the frame's or held).
    struct TapsShown
    {
        bool show(Meters& m, uint64_t program, uint64_t table) noexcept;

        float taps[kMaxDisplays] = {};                // the last frame of `program` and `table`
        uint64_t program = 0, table = 0;
        bool held = false;                            // taps holds one
    };
}
