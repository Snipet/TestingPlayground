// Source/editor/EnvelopeView.h: a picture of the ADSR, drawn from the current ATTACK, DECAY, SUSTAIN and RELEASE
// (read from their ports, so host automation shows as it happens). It is the panel's example of a custom view: plain
// drawing on the funkgui::Canvas, no input.
//
// The shape is the envelope of one note held for a while: a straight rise, a straight fall to the sustain level, the
// hold, a straight fall to 0 (Source/dsp/Adsr.h). Times are drawn on a square-root scale, the same for the three
// segments, so 1 ms still shows and 2 s does not crowd the rest out: a segment is kSegment of the width at the longest
// time a table row allows, and the hold takes what is left. The segment of the control under the hand (or in focus)
// is drawn in ink100; the rest in ink70 over an ink16 fill. Its primitives carry the tag kEnvelopeTag, for the probes.
// A host's NaN on one of the four parameters leaves the shape as it was (ParamModel.h's shownValue01: the last finite
// value seen), as the dials do.
//
// Message thread only; the ports belong to the processor and outlive the view.
#pragma once

#include "dsp/Params.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/params/ParamPort.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace funkgui
{
    class Canvas;
    struct Theme;
}

namespace testingplaygroundsynth::ui
{
    // "ENVELOPE": the next product tag after Panel.h's kHeaderTextTag (firstProduct).
    inline constexpr funkgui::Tag kEnvelopeTag = static_cast<funkgui::Tag>(funkgui::tags::firstProduct + 1);

    class EnvelopeView
    {
    public:
        static constexpr float kSegment = 0.28f;               // a segment's width at the longest time, of the plot's

        // The view over the ports of the rows "attack", "decay", "sustain" and "release" (table order), in `plot`;
        // nullopt when the table lacks one of them.
        static std::optional<EnvelopeView> make(std::span<funkgui::ParamPort* const> ports, funkgui::Rect plot);

        // `highlight`: the table index of the parameter whose segment to draw in ink100, or -1.
        void draw(funkgui::Canvas&, const funkgui::Theme&, int highlight) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, uint32_t id) const;

        const funkgui::Rect& plot() const noexcept { return plot_; }
        bool shows(std::size_t param) const noexcept;          // one of the four rows

        // The shape's five corners at the current values, left to right (logical px): the start, the attack's peak,
        // the decay's end, the release's start, the end.
        std::array<funkgui::Point, 5> corners() const;

    private:
        enum Seg : std::size_t { attack, decay, sustain, release };

        float plain(Seg s) const;

        std::array<std::size_t, 4> param_{};
        std::array<funkgui::ParamPort*, 4> port_{};
        mutable std::array<float, 4> last01_{};                // the last finite value01 of each (shownValue01)
        funkgui::Rect plot_{};
        float maxMs_ = 1.0f;                                   // the longest time a row allows
    };
}
