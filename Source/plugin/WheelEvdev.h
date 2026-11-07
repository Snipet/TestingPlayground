// Source/plugin/WheelEvdev.h: the Standalone's racing wheel on Linux, apart from the event loop. There the kernel's
// hid-lg4ff drives a Logitech G29 (drivers/hid/hid-lg4ff.c): it switches a compatibility-mode wheel to its own mode by
// itself (unless loaded with lg4ff_no_autoswitch), gives its values as an input device's events (evdev,
// /dev/input/event<n>), and takes its commands through its files in sysfs and through force feedback. So the Hub, a
// wheel::Transport (plugin/WheelDriver.h decides as on macOS), reads and commands the wheel that way:
//
//   finding it    <sys>/class/input/event<n>/device/id/{vendor,product,version} (hex), told apart by WheelMap.h's
//                 kindOf; the node <dev>/input/event<n>; the HID device's folder <sys>/class/input/event<n>/device/device
//   its values    the node's events (struct input_event), read non-blocking whenever it can be read, never grabbed (a
//                 game may read it too): hid-input's codes made WheelMap.h's HID usages again (ABS_X, Y, Z, RX, RY and RZ
//                 the Generic Desktop's 0x30 to 0x35 with the ranges EVIOCGABS gives, HAT0X and HAT0Y the hat, 0 to 7
//                 and 8 the centre; a joystick's buttons, BTN_TRIGGER and on and BTN_TRIGGER_HAPPY and on, buttons 1 to
//                 16 and 17 on); each event's time is its value's timestamp, so the seed rule holds (a value is taken
//                 only from an event); SYN_DROPPED (the node's buffer overran) drops what follows until SYN_REPORT, then
//                 reads the state again (EVIOCGABS, EVIOCGKEY)
//   its range     hid-lg4ff's sysfs file `range` (degrees): read first, written only when it differs (it is root's, and
//                 the kernel sets the wheel's 900 degrees itself when it binds it)
//   its spring    force feedback's FF_AUTOCENTER written to the node (an EV_FF event; 0 stops it): hid-lg4ff's
//                 lg4ff_set_autocenter_default, the reports WheelMap.h's springReports are
//   its lights    hid-lg4ff's LED class devices, <hid>/leds/<name>::RPM1 to ::RPM5, `brightness` 1 or 0: bit i is RPM<i+1>
//   the switch    hid-lg4ff's sysfs file `alternate_modes`, "native" written (only where the kernel did not switch it)
//   unplugged     a read that fails (ENODEV), or the kernel no longer listing it (rescan): the driver told
//
// A file it may not write (range, the lights, alternate_modes are root's unless a udev rule gives them to the user: the
// README's "A racing wheel") says so on the card (describe), and the rest still works; a node it may not open at all
// lists the wheel with the card saying why.
//
// The System is the event loop's part (descriptors watched, timers, the messages out: standalone/RacingWheelLinux.cpp
// over JUCE's, or a probe's), and the device's own calls (open, EVIOCGABS, EVIOCGKEY), which a probe makes up: plugin.linux
// drives a Hub over a made-up sysfs tree with nodes of its own. Message thread only (or a probe's). Linux only.
#pragma once

#if defined(__linux__)
#include "plugin/WheelDriver.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace playground::wheel::evdev
{
    // Where the kernel's files are: /sys and /dev, or a probe's tree.
    struct Paths
    {
        std::string sys = "/sys";
        std::string dev = "/dev";
    };

    // A wheel the kernel lists.
    struct Found
    {
        std::string event;                   // "event7"
        std::string input;                   // its input device's name ("input27": a wheel plugged in again is another)
        std::string node;                    // <dev>/input/event7
        std::string hid;                     // <sys>/class/input/event7/device/device: range, alternate_modes, leds/
        int vendor = 0, product = 0, version = 0;
        Kind kind = Kind::none;
    };
    // Every G29 (any of its kinds), in the order of their event numbers.
    std::vector<Found> scan(const Paths& paths);

    // hid-input's mapping the other way round: a key code's button (1-based; 0 none), an absolute axis's usage on the
    // Generic Desktop page (0 none), and the hat's position from HAT0X and HAT0Y (-1, 0, 1 each): 0 up, 1 up-right, ...,
    // 7 up-left, 8 the centre.
    int buttonOf(uint16_t code) noexcept;
    uint32_t usageOf(uint16_t absCode) noexcept;
    int hatOf(int x, int y) noexcept;

    // An event as the kernel's struct input_event carries it, its time in microseconds.
    struct Event
    {
        uint64_t micros = 0;
        uint16_t type = 0, code = 0;
        int32_t value = 0;
    };
    inline constexpr uint16_t kSyn = 0x00, kKey = 0x01, kAbs = 0x03, kFf = 0x15;            // EV_*
    inline constexpr uint16_t kSynReport = 0, kSynDropped = 3;                              // SYN_*
    inline constexpr uint16_t kHatX = 0x10, kHatY = 0x11;                                   // ABS_HAT0X, ABS_HAT0Y
    inline constexpr uint16_t kAutocenter = 0x61;                                           // FF_AUTOCENTER

    class System
    {
    public:
        virtual ~System() = default;
        virtual void watch(int fd) = 0;      // Hub::readable(fd) whenever it can be read (or has hung up)
        virtual void unwatch(int fd) = 0;
        virtual void arm(Timer t, double seconds) = 0;
        virtual void disarm(Timer t) = 0;
        virtual double now() = 0;            // seconds, steady
        virtual void send(const std::vector<Message>& messages) = 0;
        virtual void changed() = 0;
        virtual int lights() = 0;            // Transport::lights
        // The node opened non-blocking, for reading and writing (else reading only): its descriptor, or -1 and errno.
        virtual int open(const std::string& node);
        // An absolute axis's value and range (EVIOCGABS); false when the device does not say.
        virtual bool abs(int fd, uint16_t code, int32_t& value, int32_t& min, int32_t& max);
        // The keys down (EVIOCGKEY), a bit a code; false when the device does not say.
        virtual bool keys(int fd, std::vector<uint8_t>& bits);
    };

    class Hub final : public Transport
    {
    public:
        // The System is the caller's, alive until after the Hub. The constructor reads nothing: rescan() does.
        Hub(System& system, Paths paths, bool on);
        ~Hub() override;                     // the driver quits (the spring off, the lights off), every node closed
        Hub(const Hub&) = delete;
        Hub& operator=(const Hub&) = delete;

        Driver& driver() noexcept { return driver_; }
        // The wheels the kernel lists now: new ones opened and told to the driver, gone ones taken away, one that could
        // not be opened tried again (a udev rule's permissions arrive after the node).
        void rescan();
        // Its node can be read (or hung up): its events, or it gone.
        void readable(int fd);

        // ---- Transport -------------------------------------------------------------------------------------------
        int report(Device d, const Report& r) override;      // none here: the kernel's driver takes the commands
        int setRange(Device d, int degrees) override;
        int setSpring(Device d, uint16_t magnitude) override;
        int setLights(Device d, int mask) override;
        int toNative(Device d) override;
        std::string describe(int error) override;
        std::vector<Value> values(Device d) override;
        void listen(Device d) override;
        void arm(Timer t, double seconds) override { sys_.arm(t, seconds); }
        void disarm(Timer t) override { sys_.disarm(t); }
        double now() override { return sys_.now(); }
        void send(const std::vector<Message>& messages) override { sys_.send(messages); }
        void changed() override { sys_.changed(); }
        int lights() override { return sys_.lights(); }

    private:
        struct Dev
        {
            Found found;
            int fd = -1;
            int error = 0;                                   // why it is not open (errno), 0 when it is
            std::map<uint32_t, Value> values;                // by page << 16 | usage: what events carried
            std::map<uint16_t, std::pair<int32_t, int32_t>> ranges;   // an axis's minimum and maximum
            int hatX = 0, hatY = 0;
            bool dropping = false;                           // SYN_DROPPED: until the next SYN_REPORT
        };

        Dev* find(Device d) const noexcept;
        void openNode(Dev& d);
        void closeNode(Dev& d);
        void apply(Dev& d, const Event& e);
        void resync(Dev& d, uint64_t micros);
        void take(Dev& d, const Value& v);
        void remove(Dev* d);

        System& sys_;
        Paths paths_;
        std::vector<std::unique_ptr<Dev>> devs_;
        Device listening_ = nullptr;
        Driver driver_;
    };
}
#endif
