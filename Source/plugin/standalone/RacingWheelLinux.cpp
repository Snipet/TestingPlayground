// Source/plugin/standalone/RacingWheelLinux.cpp: RacingWheel.h on Linux. The wheel is the kernel's hid-lg4ff's there,
// read and commanded through evdev and sysfs by plugin/WheelEvdev.h's Hub (which plugin/WheelDriver.h decides for, as
// on macOS); this is its event loop: the nodes' descriptors and an inotify watch of /dev/input (a wheel plugged in or
// out, a udev rule's permissions arriving) on JUCE's message loop (LinuxEventLoop's descriptor callbacks), and the
// driver's three timers as JUCE timers. Message thread only.
#include "plugin/standalone/RacingWheel.h"

#include "plugin/WheelEvdev.h"

#include <juce_events/juce_events.h>

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace playground::standalone
{
    namespace
    {
        // A timer that fires once each time it is started (started again: moved).
        struct Once final : juce::Timer
        {
            std::function<void()> fire;
            void timerCallback() override
            {
                stopTimer();
                if (fire)
                    fire();
            }
        };

        constexpr int kRescanMs = 150;                       // after /dev/input changes: udev's permissions settle
    }

    struct RacingWheel::Impl final : wheel::evdev::System
    {
        Hooks hooks;
        Once timers[3];                                      // wheel::Timer's
        Once rescanSoon;
        int notify = -1;                                     // inotify on /dev/input
        std::unique_ptr<wheel::evdev::Hub> hub;              // last: it goes first, while the rest is here

        Impl(Hooks h, bool on) : hooks(std::move(h))
        {
            for (const wheel::Timer t : { wheel::Timer::flush, wheel::Timer::switched, wheel::Timer::lights })
                timers[static_cast<int>(t)].fire = [this, t] {
                    if (hub != nullptr)
                        hub->driver().fire(t);
                };
            rescanSoon.fire = [this] {
                if (hub != nullptr)
                    hub->rescan();
            };
            hub = std::make_unique<wheel::evdev::Hub>(*this, wheel::evdev::Paths{}, on);
            notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (notify >= 0)
            {
                if (inotify_add_watch(notify, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM) < 0)
                {
                    ::close(notify);
                    notify = -1;
                }
                else
                    juce::LinuxEventLoop::registerFdCallback(notify, [this](int fd) { drained(fd); });
            }
            hub->rescan();
        }

        ~Impl() override
        {
            if (notify >= 0)
            {
                juce::LinuxEventLoop::unregisterFdCallback(notify);
                ::close(notify);
            }
            rescanSoon.stopTimer();
            hub.reset();                                     // the driver quits: the spring and the lights off
            for (Once& t : timers)
                t.stopTimer();
        }

        // /dev/input changed: what inotify says is read away, and the kernel's list looked at again shortly.
        void drained(int fd)
        {
            alignas(inotify_event) char buf[4096];
            while (::read(fd, buf, sizeof buf) > 0)
            {
            }
            rescanSoon.startTimer(kRescanMs);
        }

        // ---- wheel::evdev::System ------------------------------------------------------------------------------------
        void watch(int fd) override
        {
            juce::LinuxEventLoop::registerFdCallback(fd, [this](int f) {
                if (hub != nullptr)
                    hub->readable(f);
            });
        }

        void unwatch(int fd) override { juce::LinuxEventLoop::unregisterFdCallback(fd); }

        void arm(wheel::Timer t, double seconds) override
        {
            timers[static_cast<int>(t)].startTimer(std::max(1, static_cast<int>(std::ceil(seconds * 1000.0))));
        }

        void disarm(wheel::Timer t) override { timers[static_cast<int>(t)].stopTimer(); }

        double now() override
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void send(const std::vector<wheel::Message>& messages) override
        {
            if (hooks.send)
                hooks.send(messages);
        }

        void changed() override
        {
            if (hooks.changed)
                hooks.changed();
        }

        int lights() override { return hooks.lights ? hooks.lights() : -1; }
    };

    RacingWheel::RacingWheel(Hooks hooks, bool on) : impl_(std::make_unique<Impl>(std::move(hooks), on)) {}
    RacingWheel::~RacingWheel() = default;

    bool RacingWheel::present() const noexcept { return impl_->hub->driver().present(); }
    bool RacingWheel::on() const noexcept { return impl_->hub->driver().on(); }
    void RacingWheel::setOn(bool on) { impl_->hub->driver().setOn(on); }
    void RacingWheel::resend() { impl_->hub->driver().resend(); }
    std::string RacingWheel::status() const { return impl_->hub->driver().status(); }
}
