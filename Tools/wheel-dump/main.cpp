// Tools/wheel-dump/main.cpp: wheel-dump, a developer's tool (never installed, outside `all`: CMakeLists.txt builds it
// with `--target wheel-dump`). It finds a connected Logitech G29 (any of its modes, and any other Logitech joystick or
// game pad), says what plugin/WheelMap.h makes of it, lists its HID elements (type, usage page and usage, logical
// minimum and maximum, report size, the value IOKit holds and its timestamp: 0 until a report carried it, when the 0
// it holds is not the wheel's) with the table's row for each, and then prints, ten times a second, the elements that
// changed (their raw values and what the synth will read) and the MIDI the Standalone would send for them (the control
// changes, and the start key's note). Like the Standalone (plugin/WheelDriver.h), it takes a value only from a report.
// It is how WheelMap.h's table is checked on the device (README, "A racing wheel"):
//
//   wheel-dump              list, then watch (Ctrl-C ends it)
//   wheel-dump --list       list and end
//   wheel-dump --switch     a G29 in compatibility mode (0xC294): send hid-lg4ff's switch to its own mode, as the
//                           Standalone does when its entry is on, and go on with the G29 that comes back
//   wheel-dump --spring     a G29 in its own mode: 900 degrees and the Standalone's autocentre spring while it runs
//                           (off again when it ends: Ctrl-C, SIGTERM or SIGHUP; Ctrl-Z stops it with the spring off,
//                           and fg sets it again; a SIGKILL or a crash leaves it on until the wheel is unplugged)
//
// IOKit's HID manager on the main run loop, opened shared: a game (or the Standalone) may read the wheel meanwhile.
#include "plugin/WheelMap.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>

#include <dispatch/dispatch.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    namespace wheel = playground::wheel;

    struct Options
    {
        bool list = false, toNative = false, spring = false;
    };

    struct Element
    {
        IOHIDElementRef ref = nullptr;
        int64_t value = 0, printed = 0;
        bool changed = false, known = false;
    };

    struct Tool
    {
        Options options;
        IOHIDManagerRef manager = nullptr;
        IOHIDDeviceRef device = nullptr;                     // the one watched (retained)
        bool springOn = false;
        std::map<IOHIDElementRef, Element> elements;
        wheel::Map map;
        std::vector<wheel::Message> ccs;
    };

    Tool tool;

    int intProperty(IOHIDDeviceRef device, CFStringRef key)
    {
        const CFTypeRef v = IOHIDDeviceGetProperty(device, key);
        int out = 0;
        if (v != nullptr && CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &out);
        return out;
    }

    std::string stringProperty(IOHIDDeviceRef device, CFStringRef key)
    {
        const CFTypeRef v = IOHIDDeviceGetProperty(device, key);
        char b[256] = {};
        if (v != nullptr && CFGetTypeID(v) == CFStringGetTypeID())
            CFStringGetCString(static_cast<CFStringRef>(v), b, sizeof b, kCFStringEncodingUTF8);
        return b;
    }

    const char* kindName(wheel::Kind k)
    {
        switch (k)
        {
            case wheel::Kind::g29:    return "a G29 in its own mode: the Standalone reads it";
            case wheel::Kind::compat: return "a G29 in compatibility mode (its switch at PS3, no Logitech software): --switch "
                                             "switches it to its own mode, as the Standalone does when its entry is on";
            case wheel::Kind::ps4:    return "a G29 with its switch at PS4: the Standalone does not read it (set the switch to PS3)";
            case wheel::Kind::none:   return "not a G29 the Standalone knows (another Logitech wheel or a Driving Force?)";
        }
        return "";
    }

    const char* typeName(IOHIDElementType t)
    {
        if (t == kIOHIDElementTypeInput_Misc)
            return "input misc";
        if (t == kIOHIDElementTypeInput_Button)
            return "button";
        if (t == kIOHIDElementTypeInput_Axis)
            return "axis";
        if (t == kIOHIDElementTypeOutput)
            return "output";
        if (t == kIOHIDElementTypeFeature)
            return "feature";
        return "input other";
    }

    std::string usageName(uint32_t page, uint32_t usage)
    {
        static const char* const desktop[] = { "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial", "Wheel", "Hat" };
        char b[64];
        if (page == wheel::kDesktop && usage >= 0x30 && usage <= 0x39)
            return desktop[usage - 0x30];
        if (page == wheel::kButtonPage)
        {
            std::snprintf(b, sizeof b, "button %u", usage);
            return b;
        }
        std::snprintf(b, sizeof b, "page 0x%02x usage 0x%02x", page, usage);
        return b;
    }

    // The table's row for an element, in words ("" when it has none).
    std::string rowOf(uint32_t page, uint32_t usage)
    {
        if (page == wheel::kDesktop)
        {
            for (const wheel::AxisRow& r : wheel::kAxisRows)
                if (r.usage == usage)
                    return std::string(r.name) + (r.restAtMax ? " (its maximum at rest)" : "");
            if (usage == wheel::kHat)
                return "the d-pad (bits 18 to 21)";
            return {};
        }
        if (page != wheel::kButtonPage)
            return {};
        for (const wheel::GearRow& g : wheel::kGears)
            if (g.button == static_cast<int>(usage))
                return g.gear < 0 ? std::string("the shifter's reverse") : "the shifter's gear " + std::to_string(g.gear);
        for (std::size_t i = 0; i < std::size(wheel::kBits); ++i)
            if (wheel::kBits[i].button == static_cast<int>(usage))
                return "bit " + std::to_string(i) + ", " + wheel::kBits[i].name
                     + (wheel::kBits[i].button == wheel::kStartButton ? ", and the start key (note 36)" : "");
        return {};
    }

    bool report(IOHIDDeviceRef device, const wheel::Report& r, const char* what)
    {
        const IOReturn e = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0, r.data(), static_cast<CFIndex>(r.size()));
        std::printf("  sent %s: %02x %02x %02x %02x %02x %02x %02x (%s)\n", what, r[0], r[1], r[2], r[3], r[4], r[5], r[6],
                    e == kIOReturnSuccess ? "ok" : "failed");
        std::fflush(stdout);
        return e == kIOReturnSuccess;
    }

    // 900 degrees, then the spring (none when the range fails), as the Standalone sets them.
    void springOn()
    {
        if (tool.device == nullptr)
            return;
        if (!report(tool.device, wheel::rangeReport(wheel::kRange), "the range, 900 degrees (hid-lg4ff: lg4ff_set_range_g25)"))
            return;
        const auto spring = wheel::springReports(wheel::kSpring);
        tool.springOn = report(tool.device, spring[0], "the spring (hid-lg4ff: lg4ff_set_autocenter_default)")
                     && report(tool.device, spring[1], "activate auto-centre");
    }

    void springOff()
    {
        if (tool.device != nullptr && tool.springOn)
        {
            report(tool.device, wheel::kSpringOff, "the spring off (hid-lg4ff: deactivate auto-centre)");
            tool.springOn = false;
        }
    }

    void list(IOHIDDeviceRef device)
    {
        const int vendor = intProperty(device, CFSTR(kIOHIDVendorIDKey)), product = intProperty(device, CFSTR(kIOHIDProductIDKey));
        const int version = intProperty(device, CFSTR(kIOHIDVersionNumberKey));
        std::printf("%s (%s): vendor 0x%04x, product 0x%04x, version (bcdDevice) 0x%04x\n  %s\n",
                    stringProperty(device, CFSTR(kIOHIDProductKey)).c_str(), stringProperty(device, CFSTR(kIOHIDManufacturerKey)).c_str(),
                    vendor, product, version, kindName(wheel::kindOf(vendor, product, version)));
        const CFArrayRef all = IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone);
        if (all == nullptr)
            return;
        // Each input element's value as IOKit holds it, and its timestamp: 0 (no report yet) and that value is not the
        // wheel's, so it is left out (the seed rule: what the Standalone does too).
        std::printf("  %-11s %-26s %8s %8s %4s %8s %20s  %s\n", "type", "usage", "min", "max", "bits", "value", "timestamp",
                    "WheelMap.h");
        for (CFIndex i = 0, n = CFArrayGetCount(all); i < n; ++i)
        {
            const auto e = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(all, i)));
            const IOHIDElementType t = IOHIDElementGetType(e);
            if (t == kIOHIDElementTypeCollection)
                continue;
            const bool input = t == kIOHIDElementTypeInput_Misc || t == kIOHIDElementTypeInput_Button || t == kIOHIDElementTypeInput_Axis;
            IOHIDValueRef v = nullptr;
            const bool held = input && IOHIDDeviceGetValue(device, e, &v) == kIOReturnSuccess && v != nullptr;
            const long long value = held ? static_cast<long long>(IOHIDValueGetIntegerValue(v)) : 0;
            const unsigned long long stamp = held ? static_cast<unsigned long long>(IOHIDValueGetTimeStamp(v)) : 0;
            char shown[24] = "-", when[24] = "-";
            if (held)
            {
                std::snprintf(shown, sizeof shown, "%lld", value);
                std::snprintf(when, sizeof when, "%llu", stamp);
            }
            const uint32_t page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e);
            std::printf("  %-11s %-26s %8ld %8ld %4u %8s %20s  %s%s\n", typeName(t), usageName(page, usage).c_str(),
                        static_cast<long>(IOHIDElementGetLogicalMin(e)), static_cast<long>(IOHIDElementGetLogicalMax(e)),
                        IOHIDElementGetReportSize(e) * IOHIDElementGetReportCount(e), shown, when, rowOf(page, usage).c_str(),
                        held && stamp == 0 ? " (no report yet: left out)" : "");
            if (input)
            {
                Element& el = tool.elements[e];
                el.ref = e;
                if (held && stamp != 0)
                {
                    el.value = el.printed = value;
                    el.known = true;
                }
            }
        }
        CFRelease(all);
        std::printf("  the buttons' bits (CCs 21 to 24): %s\n", wheel::bitNames().c_str());
    }

    bool take(IOHIDElementRef e, int64_t raw)
    {
        int64_t v = raw, lo = IOHIDElementGetLogicalMin(e), hi = IOHIDElementGetLogicalMax(e);
        wheel::normalise(v, lo, hi, IOHIDElementGetReportSize(e));
        return tool.map.input(IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e), v, lo, hi);
    }

    void start(IOHIDDeviceRef device)
    {
        tool.device = device;
        CFRetain(device);
        tool.elements.clear();
        list(device);
        const int vendor = intProperty(device, CFSTR(kIOHIDVendorIDKey)), product = intProperty(device, CFSTR(kIOHIDProductIDKey));
        const wheel::Kind kind = wheel::kindOf(vendor, product, intProperty(device, CFSTR(kIOHIDVersionNumberKey)));
        if (kind == wheel::Kind::compat && tool.options.toNative)
        {
            std::printf("switching it to its own mode (hid-lg4ff's lg4ff_mode_switch_ext09_g29); it comes back as 0xc24f\n");
            report(device, wheel::kToNative[0], "revert the mode upon a USB reset");
            report(device, wheel::kToNative[1], "switch to G29 mode, with a detach");
            return;
        }
        if (tool.options.list)
        {
            CFRunLoopStop(CFRunLoopGetMain());
            return;
        }
        if (kind == wheel::Kind::g29 && tool.options.spring)
            springOn();
        tool.map.forget();
        for (auto& [ref, el] : tool.elements)
            if (el.known)
                take(ref, el.value);
        tool.ccs.clear();
        tool.map.flush(tool.ccs);
        std::printf("watching (Ctrl-C ends it): the elements that change, ten times a second, and the MIDI\n");
        std::fflush(stdout);
    }

    void onAdded(void*, IOReturn, void*, IOHIDDeviceRef device)
    {
        if (tool.device == nullptr)
            start(device);
        else
            std::printf("another one came; watching the first\n");
    }

    void onRemoved(void*, IOReturn, void*, IOHIDDeviceRef device)
    {
        if (device != tool.device)
            return;
        std::printf("it went (unplugged, or switching its mode)\n");
        CFRelease(tool.device);
        tool.device = nullptr;
        tool.springOn = false;
        tool.elements.clear();
        tool.map.rest(tool.ccs);
    }

    void onValue(void*, IOReturn, void*, IOHIDValueRef value)
    {
        const IOHIDElementRef e = IOHIDValueGetElement(value);
        if (IOHIDElementGetDevice(e) != tool.device)
            return;
        Element& el = tool.elements[e];
        el.ref = e;
        el.value = IOHIDValueGetIntegerValue(value);
        el.changed = true;
        el.known = true;
        take(e, el.value);
    }

    // Ten times a second: what changed since the last time.
    void onTick(CFRunLoopTimerRef, void*)
    {
        if (tool.device == nullptr)
            return;
        bool any = false;
        for (auto& [ref, el] : tool.elements)
        {
            if (!el.changed)
                continue;
            el.changed = false;
            const uint32_t page = IOHIDElementGetUsagePage(ref), usage = IOHIDElementGetUsage(ref);
            std::printf("  %-26s %8lld -> %8lld   %s\n", usageName(page, usage).c_str(), static_cast<long long>(el.printed),
                        static_cast<long long>(el.value), rowOf(page, usage).c_str());
            el.printed = el.value;
            any = true;
        }
        tool.ccs.clear();
        tool.map.flush(tool.ccs);
        if (!any && tool.ccs.empty())
            return;
        const wheel::State& s = tool.map.state();
        std::printf("  synth: steering %+.4f, throttle %.4f, brake %.4f, clutch %.4f, gear %d, buttons 0x%07x\n",
                    static_cast<double>(tool.map.steering()), static_cast<double>(tool.map.pedal(wheel::Axis::throttle)),
                    static_cast<double>(tool.map.pedal(wheel::Axis::brake)), static_cast<double>(tool.map.pedal(wheel::Axis::clutch)),
                    s.gear, s.buttons);
        if (!tool.ccs.empty())
        {
            std::printf("  MIDI:");
            for (const wheel::Message& c : tool.ccs)
                if (c.status == wheel::Message::kCc)
                    std::printf(" CC%u=%u", c.number, c.value);
                else
                    std::printf(" note-%s %u %u", c.status == wheel::Message::kNoteOn ? "on" : "off", c.number, c.value);
            std::printf("\n");
        }
        std::fflush(stdout);
    }

    CFDictionaryRef matching(const char* key1, int value1, const char* key2, int value2, const char* key3 = nullptr, int value3 = 0)
    {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
        const auto put = [d](const char* key, int value) {
            const CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
            const CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
            CFDictionarySetValue(d, k, v);
            CFRelease(k);
            CFRelease(v);
        };
        put(key1, value1);
        put(key2, value2);
        if (key3 != nullptr)
            put(key3, value3);
        return d;
    }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--list") == 0)
            tool.options.list = true;
        else if (std::strcmp(argv[i], "--switch") == 0)
            tool.options.toNative = true;
        else if (std::strcmp(argv[i], "--spring") == 0)
            tool.options.spring = true;
        else
        {
            std::fprintf(stderr, "usage: wheel-dump [--list] [--switch] [--spring]\n");
            return 2;
        }
    }

    tool.manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    const void* each[] = {
        matching(kIOHIDVendorIDKey, wheel::kVendor, kIOHIDProductIDKey, wheel::kG29),
        matching(kIOHIDVendorIDKey, wheel::kVendor, kIOHIDProductIDKey, wheel::kCompat),
        matching(kIOHIDVendorIDKey, wheel::kVendor, kIOHIDProductIDKey, wheel::kG29Ps4),
        matching(kIOHIDVendorIDKey, wheel::kVendor, kIOHIDDeviceUsagePageKey, 0x01, kIOHIDDeviceUsageKey, 0x04),   // a joystick
        matching(kIOHIDVendorIDKey, wheel::kVendor, kIOHIDDeviceUsagePageKey, 0x01, kIOHIDDeviceUsageKey, 0x05),   // a game pad
    };
    const CFArrayRef all = CFArrayCreate(kCFAllocatorDefault, each, static_cast<CFIndex>(std::size(each)), &kCFTypeArrayCallBacks);
    IOHIDManagerSetDeviceMatchingMultiple(tool.manager, all);
    CFRelease(all);
    for (const void* d : each)
        CFRelease(d);
    IOHIDManagerRegisterDeviceMatchingCallback(tool.manager, onAdded, nullptr);
    IOHIDManagerRegisterDeviceRemovalCallback(tool.manager, onRemoved, nullptr);
    IOHIDManagerRegisterInputValueCallback(tool.manager, onValue, nullptr);
    IOHIDManagerScheduleWithRunLoop(tool.manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    const IOReturn opened = IOHIDManagerOpen(tool.manager, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess)
        std::printf("IOHIDManagerOpen: 0x%08x%s\n", static_cast<unsigned>(opened),
                    opened == kIOReturnNotPermitted ? " (not permitted: Privacy & Security, Input Monitoring)" : "");

    CFRunLoopTimerContext context{ 0, nullptr, nullptr, nullptr, nullptr };
    const CFRunLoopTimerRef tick = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0, onTick, &context);
    CFRunLoopAddTimer(CFRunLoopGetMain(), tick, kCFRunLoopDefaultMode);

    // The signals that end or stop it, on the main queue (the run loop's): the spring off first. Ctrl-C ends it here;
    // SIGTERM and SIGHUP then take their default action (the process ends as they say); Ctrl-Z (SIGTSTP) stops it with
    // SIGSTOP (which reaches no handler), and when it goes on (fg) the spring is set again.
    std::vector<dispatch_source_t> sources;
    for (const int sig : { SIGINT, SIGTERM, SIGHUP, SIGTSTP })
    {
        std::signal(sig, SIG_IGN);
        const dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, static_cast<uintptr_t>(sig), 0,
                                                                dispatch_get_main_queue());
        dispatch_source_set_event_handler(source, ^{
            const bool spring = tool.springOn;
            springOff();
            if (sig == SIGINT)
                CFRunLoopStop(CFRunLoopGetMain());
            else if (sig == SIGTSTP)
            {
                std::raise(SIGSTOP);
                if (spring)
                    springOn();
            }
            else
            {
                std::signal(sig, SIG_DFL);
                std::raise(sig);
            }
        });
        dispatch_resume(source);
        sources.push_back(source);
    }

    std::printf("waiting for a Logitech G29 (or any Logitech joystick or game pad)...\n");
    std::fflush(stdout);
    CFRunLoopRun();

    springOff();
    CFRunLoopTimerInvalidate(tick);
    CFRelease(tick);
    if (tool.device != nullptr)
        CFRelease(tool.device);
    IOHIDManagerClose(tool.manager, kIOHIDOptionsTypeNone);
    CFRelease(tool.manager);
    return 0;
}
