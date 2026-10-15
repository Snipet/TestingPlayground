// Source/editor/ParamModel.h: one parameter as FunkGui's ValueModel, what a Dial or a RuleSlider renders and writes.
// The track is the host-normalised value itself (0..1), so a drag, the wheel and the keys move the parameter in the
// same skewed space the host automates; the text is the plain value (ParamSpec's mapping) formatted with
// funkgui::fmt (U+2212 minus, no locale). Writes go through the widget's GestureController to the port, which is a
// funkgui::JuceParamPort owned by the processor (begin/set/end gestures, so hosts record automation).
// Message thread only. view() runs only when key() changes (the port's value bits).
#pragma once

#include "dsp/ParamSpec.h"

#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/ValueModel.h>

#include <cstdint>

namespace testingplayground::ui
{
    class ParamModel final : public funkgui::ValueModel
    {
    public:
        ParamModel(const ParamSpec& spec, funkgui::ParamPort& port);

        ParamModel(const ParamModel&) = delete;
        ParamModel& operator=(const ParamModel&) = delete;

        uint64_t key() const override;
        void view(funkgui::ValueView& v) const override;
        funkgui::ParamPort* port() override { return &port_; }
        float host01FromTrack(float t) const override;
        float defaultHost01() const override { return port_.default01(); }

        const ParamSpec& spec() const noexcept { return spec_; }

        // The plain value as the panel prints it: value and unit apart ("−6.0" "DB"), and a spoken form.
        static void format(const ParamSpec& spec, float plain, funkgui::ValueText& out);

    private:
        const ParamSpec& spec_;
        funkgui::ParamPort& port_;
    };
}
