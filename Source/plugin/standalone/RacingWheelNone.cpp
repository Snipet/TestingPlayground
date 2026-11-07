// Source/plugin/standalone/RacingWheelNone.cpp: RacingWheel.h where the Standalone has no racing wheel (Windows: the
// G29 is read over macOS's IOKit, RacingWheel.cpp, and Linux's evdev, RacingWheelLinux.cpp). None is ever present, so
// Devices lists none and sends nothing; the synth still reads a wheel's control changes from any MIDI input
// (dsp/Events.h's wheelcc).
#include "plugin/standalone/RacingWheel.h"

namespace playground::standalone
{
    struct RacingWheel::Impl
    {
        bool on = false;
    };

    RacingWheel::RacingWheel(Hooks, bool on) : impl_(std::make_unique<Impl>())
    {
        impl_->on = on;
    }

    RacingWheel::~RacingWheel() = default;

    bool RacingWheel::present() const noexcept { return false; }
    bool RacingWheel::on() const noexcept { return impl_->on; }
    void RacingWheel::setOn(bool on) { impl_->on = on; }
    void RacingWheel::resend() {}
    std::string RacingWheel::status() const { return {}; }
}
