// Source/plugin/standalone/RacingWheel.cpp: see RacingWheel.h.
#include "plugin/standalone/RacingWheel.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace playground::standalone
{
    namespace
    {
        constexpr CFTimeInterval kFlushSeconds = 0.004;      // a report's values sent together, at most 250 times a second
        constexpr CFTimeInterval kSwitchSeconds = 8.0;       // the mode switch's re-enumeration, at the most
        constexpr CFTimeInterval kNever = 1.0e10;

        int intProperty(IOHIDDeviceRef device, CFStringRef key)
        {
            const CFTypeRef v = IOHIDDeviceGetProperty(device, key);
            int out = 0;
            if (v != nullptr && CFGetTypeID(v) == CFNumberGetTypeID())
                CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out);
            return out;
        }

        wheel::Kind kindOf(IOHIDDeviceRef device)
        {
            return wheel::kindOf(intProperty(device, CFSTR(kIOHIDVendorIDKey)), intProperty(device, CFSTR(kIOHIDProductIDKey)),
                                 intProperty(device, CFSTR(kIOHIDVersionNumberKey)));
        }

        // A matching dictionary: this vendor and product.
        CFDictionaryRef matching(int vendor, int product)
        {
            CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
            const CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
            const CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
            CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
            CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), p);
            CFRelease(v);
            CFRelease(p);
            return d;
        }

        IOReturn send(IOHIDDeviceRef device, const wheel::Report& r)
        {
            return IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0, r.data(), static_cast<CFIndex>(r.size()));
        }

        std::string hex(IOReturn r)
        {
            char b[16];
            std::snprintf(b, sizeof b, "0x%08x", static_cast<unsigned>(r));
            return b;
        }
    }

    struct RacingWheel::Impl
    {
        struct Device
        {
            IOHIDDeviceRef ref = nullptr;                    // retained
            wheel::Kind kind = wheel::Kind::none;
            bool switchSent = false;                         // a compat one: kToNative sent
        };

        Hooks hooks;
        bool on = false;
        IOHIDManagerRef manager = nullptr;
        IOReturn opened = kIOReturnSuccess;                  // IOHIDManagerOpen's
        std::vector<Device> devices;
        IOHIDDeviceRef reading = nullptr;                    // the G29 set up and read (retained), while on
        wheel::Map map;
        std::vector<wheel::Message> out;
        CFRunLoopTimerRef flushTimer = nullptr, switchTimer = nullptr;
        bool flushArmed = false, switchArmed = false, switchLate = false;
        std::string problem;                                 // the last command that failed, in words

        Impl(Hooks h, bool o) : hooks(std::move(h)), on(o)
        {
            flushTimer = timer(&Impl::onFlush);
            switchTimer = timer(&Impl::onSwitchLate);
            manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
            const void* each[] = { matching(wheel::kVendor, wheel::kG29), matching(wheel::kVendor, wheel::kCompat),
                                   matching(wheel::kVendor, wheel::kG29Ps4) };
            const CFArrayRef all = CFArrayCreate(kCFAllocatorDefault, each, 3, &kCFTypeArrayCallBacks);
            IOHIDManagerSetDeviceMatchingMultiple(manager, all);
            CFRelease(all);
            for (const void* d : each)
                CFRelease(d);
            IOHIDManagerRegisterDeviceMatchingCallback(manager, &Impl::onAdded, this);
            IOHIDManagerRegisterDeviceRemovalCallback(manager, &Impl::onRemoved, this);
            IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
            opened = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);   // shared: a game may open it too
        }

        ~Impl()
        {
            if (reading != nullptr)
            {
                IOHIDManagerRegisterInputValueCallback(manager, nullptr, nullptr);
                send(reading, wheel::kSpringOff);            // the app quits: the wheel turns freely again
                CFRelease(reading);
                reading = nullptr;
            }
            IOHIDManagerRegisterDeviceMatchingCallback(manager, nullptr, nullptr);
            IOHIDManagerRegisterDeviceRemovalCallback(manager, nullptr, nullptr);
            IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
            IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
            CFRelease(manager);
            for (Device& d : devices)
                CFRelease(d.ref);
            for (CFRunLoopTimerRef t : { flushTimer, switchTimer })
            {
                CFRunLoopTimerInvalidate(t);
                CFRelease(t);
            }
        }

        // A timer on the main run loop, in every mode, that fires only when armed (arm), once each time.
        CFRunLoopTimerRef timer(void (*fire)(CFRunLoopTimerRef, void*))
        {
            CFRunLoopTimerContext context{ 0, this, nullptr, nullptr, nullptr };
            CFRunLoopTimerRef t = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kNever, kNever, 0, 0,
                                                       fire, &context);
            CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
            return t;
        }

        static void arm(CFRunLoopTimerRef t, CFTimeInterval after)
        {
            CFRunLoopTimerSetNextFireDate(t, CFAbsoluteTimeGetCurrent() + after);
        }

        const Device* find(wheel::Kind kind) const
        {
            for (const Device& d : devices)
                if (d.kind == kind)
                    return &d;
            return nullptr;
        }

        bool present() const noexcept { return !devices.empty(); }

        std::string status() const
        {
            if (devices.empty())
                return {};
            if (opened == kIOReturnNotPermitted)
                return "macOS keeps it from this app: allow it in Privacy & Security, Input Monitoring";
            if (opened != kIOReturnSuccess)
                return "macOS could not open it (" + hex(opened) + ")";
            if (find(wheel::Kind::g29) != nullptr)
                return on ? problem : std::string();
            if (const Device* c = find(wheel::Kind::compat))
            {
                if (!on)
                    return "in its PS3 mode: switched to its full mode when on";
                if (!problem.empty())
                    return problem;
                if (switchLate)
                    return "it did not switch to its full mode: unplug it and plug it in again";
                return c->switchSent ? "switching to its full mode" : std::string();
            }
            return "set the switch on the wheel to PS3";
        }

        void changed()
        {
            if (hooks.changed)
                hooks.changed();
        }

        void flush()
        {
            out.clear();
            map.flush(out);
            if (!out.empty() && hooks.send)
                hooks.send(out);
        }

        // What the wheel's state says now, as the first flush sends it: every input element's current value.
        void seed(IOHIDDeviceRef device)
        {
            const CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone);
            if (elements == nullptr)
                return;
            for (CFIndex i = 0, n = CFArrayGetCount(elements); i < n; ++i)
            {
                const auto e = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, i)));
                const IOHIDElementType type = IOHIDElementGetType(e);
                if (type != kIOHIDElementTypeInput_Misc && type != kIOHIDElementTypeInput_Button && type != kIOHIDElementTypeInput_Axis)
                    continue;
                IOHIDValueRef value = nullptr;
                if (IOHIDDeviceGetValue(device, e, &value) == kIOReturnSuccess && value != nullptr)
                    take(e, value);
            }
            CFRelease(elements);
        }

        bool take(IOHIDElementRef e, IOHIDValueRef value)
        {
            int64_t v = IOHIDValueGetIntegerValue(value), lo = IOHIDElementGetLogicalMin(e), hi = IOHIDElementGetLogicalMax(e);
            wheel::normalise(v, lo, hi, IOHIDElementGetReportSize(e));
            return map.input(IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e), v, lo, hi);
        }

        // A G29 in its own mode, set up as a game sets it up (hid-lg4ff: lg4ff_set_range_g25, then
        // lg4ff_set_autocenter_default) and read.
        void startReading(IOHIDDeviceRef device)
        {
            problem.clear();
            IOReturn r = send(device, wheel::rangeReport(wheel::kRange));
            for (const wheel::Report& s : wheel::springReports(wheel::kSpring))
                if (r == kIOReturnSuccess)
                    r = send(device, s);
            if (r != kIOReturnSuccess)
                problem = "its range and spring were not set (" + hex(r) + ")";
            reading = device;
            CFRetain(reading);
            map.forget();
            seed(device);
            IOHIDManagerRegisterInputValueCallback(manager, &Impl::onValue, this);
            flush();
        }

        // Reading ends: the spring stops (when the wheel is still there), and what was sent goes back to rest.
        void stopReading(bool stillThere)
        {
            if (reading == nullptr)
                return;
            IOHIDManagerRegisterInputValueCallback(manager, nullptr, nullptr);
            if (stillThere)
                send(reading, wheel::kSpringOff);
            CFRelease(reading);
            reading = nullptr;
            CFRunLoopTimerSetNextFireDate(flushTimer, CFAbsoluteTimeGetCurrent() + kNever);
            flushArmed = false;
            out.clear();
            map.rest(out);
            if (!out.empty() && hooks.send)
                hooks.send(out);
        }

        // hid-lg4ff's lg4ff_switch_compatibility_mode with lg4ff_mode_switch_ext09_g29: the two reports, after which
        // the wheel detaches (the second may fail as it goes) and comes back as 0xC24F.
        void switchToNative(Device& d)
        {
            d.switchSent = true;
            switchLate = false;
            const IOReturn r = send(d.ref, wheel::kToNative[0]);
            if (r != kIOReturnSuccess)
            {
                problem = "it could not be switched to its full mode (" + hex(r) + ")";
                return;
            }
            send(d.ref, wheel::kToNative[1]);
            arm(switchTimer, kSwitchSeconds);
            switchArmed = true;
        }

        // What should be read, switched or told, from what is connected and whether it is on.
        void refresh()
        {
            const Device* g29 = find(wheel::Kind::g29);
            if (on && g29 != nullptr)
            {
                if (reading != g29->ref)
                {
                    stopReading(true);
                    startReading(g29->ref);
                }
            }
            else
                stopReading(g29 != nullptr && reading == g29->ref);
            if (on && g29 == nullptr && opened == kIOReturnSuccess)
                for (Device& d : devices)
                    if (d.kind == wheel::Kind::compat && !d.switchSent)
                    {
                        switchToNative(d);
                        break;
                    }
            if (switchArmed && (g29 != nullptr || find(wheel::Kind::compat) == nullptr || !on))
            {
                CFRunLoopTimerSetNextFireDate(switchTimer, CFAbsoluteTimeGetCurrent() + kNever);
                switchArmed = false;
            }
            changed();
        }

        void setOn(bool o)
        {
            if (o == on)
                return;
            on = o;
            if (on)
            {
                problem.clear();
                switchLate = false;
                for (Device& d : devices)
                    d.switchSent = false;                    // switched on again: try the switch again
            }
            refresh();
        }

        static void onAdded(void* context, IOReturn, void*, IOHIDDeviceRef device)
        {
            auto* self = static_cast<Impl*>(context);
            const wheel::Kind kind = kindOf(device);
            if (kind == wheel::Kind::none)
                return;                                      // another Logitech wheel in compatibility mode
            for (const Device& d : self->devices)
                if (d.ref == device)
                    return;
            CFRetain(device);
            self->devices.push_back({ device, kind, false });
            if (kind == wheel::Kind::g29)
                self->problem.clear();                       // the switch worked
            self->refresh();
        }

        static void onRemoved(void* context, IOReturn, void*, IOHIDDeviceRef device)
        {
            auto* self = static_cast<Impl*>(context);
            const auto it = std::find_if(self->devices.begin(), self->devices.end(), [device](const Device& d) { return d.ref == device; });
            if (it == self->devices.end())
                return;
            if (self->reading == device)
                self->stopReading(false);
            CFRelease(it->ref);
            self->devices.erase(it);
            self->refresh();
        }

        static void onValue(void* context, IOReturn, void*, IOHIDValueRef value)
        {
            auto* self = static_cast<Impl*>(context);
            const IOHIDElementRef e = IOHIDValueGetElement(value);
            if (self->reading == nullptr || IOHIDElementGetDevice(e) != self->reading)
                return;
            if (self->take(e, value) && !self->flushArmed)
            {
                self->flushArmed = true;
                arm(self->flushTimer, kFlushSeconds);
            }
        }

        static void onFlush(CFRunLoopTimerRef, void* info)
        {
            auto* self = static_cast<Impl*>(info);
            self->flushArmed = false;
            if (self->reading != nullptr)
                self->flush();
        }

        static void onSwitchLate(CFRunLoopTimerRef, void* info)
        {
            auto* self = static_cast<Impl*>(info);
            self->switchArmed = false;
            if (self->find(wheel::Kind::g29) == nullptr && self->find(wheel::Kind::compat) != nullptr)
            {
                self->switchLate = true;
                self->changed();
            }
        }
    };

    RacingWheel::RacingWheel(Hooks hooks, bool on) : impl_(std::make_unique<Impl>(std::move(hooks), on)) {}
    RacingWheel::~RacingWheel() = default;

    bool RacingWheel::present() const noexcept { return impl_->present(); }
    bool RacingWheel::on() const noexcept { return impl_->on; }
    void RacingWheel::setOn(bool on) { impl_->setOn(on); }
    std::string RacingWheel::status() const { return impl_->status(); }
}
