// Source/plugin/WheelEvdev.cpp: see WheelEvdev.h.
#include "plugin/WheelEvdev.h"

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace playground::wheel::evdev
{
    namespace
    {
        // A small file's text (sysfs's), its trailing white space trimmed; nothing when it cannot be read.
        bool readSmall(const std::string& path, std::string& out)
        {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            char buf[256];
            const ssize_t n = ::read(fd, buf, sizeof buf - 1);
            ::close(fd);
            if (n < 0)
                return false;
            out.assign(buf, static_cast<std::size_t>(n));
            while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t' || out.back() == '\r'))
                out.pop_back();
            return true;
        }

        // `text` written to a sysfs file: 0, else errno.
        int writeSmall(const std::string& path, const std::string& text)
        {
            const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
            if (fd < 0)
                return errno;
            const ssize_t n = ::write(fd, text.data(), text.size());
            const int e = n < 0 ? errno : n != static_cast<ssize_t>(text.size()) ? EIO : 0;
            ::close(fd);
            return e;
        }

        int hexFile(const std::string& path)
        {
            std::string t;
            if (!readSmall(path, t) || t.empty())
                return -1;
            char* end = nullptr;
            const long v = std::strtol(t.c_str(), &end, 16);
            return end != nullptr && *end == '\0' && v >= 0 && v <= 0xFFFF ? static_cast<int>(v) : -1;
        }

        int eventNumber(const std::string& name) { return std::atoi(name.c_str() + 5); }   // "event<n>"

        constexpr uint32_t key(uint32_t page, uint32_t usage) { return page << 16 | usage; }
    }

    // ---- finding and mapping ------------------------------------------------------------------------------------------
    std::vector<Found> scan(const Paths& paths)
    {
        std::vector<Found> out;
        const std::string cls = paths.sys + "/class/input";
        DIR* dir = ::opendir(cls.c_str());
        if (dir == nullptr)
            return out;
        while (const dirent* e = ::readdir(dir))
        {
            const std::string name = e->d_name;
            if (name.size() < 6 || name.compare(0, 5, "event") != 0
                || !std::all_of(name.begin() + 5, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            const std::string device = cls + "/" + name + "/device";
            Found f;
            f.vendor = hexFile(device + "/id/vendor");
            f.product = hexFile(device + "/id/product");
            f.version = hexFile(device + "/id/version");
            f.kind = kindOf(f.vendor, f.product, f.version);
            if (f.kind == Kind::none)
                continue;
            f.event = name;
            char link[256] = {};
            if (const ssize_t n = ::readlink(device.c_str(), link, sizeof link - 1); n > 0)
            {
                const std::string l(link, static_cast<std::size_t>(n));
                f.input = l.substr(l.rfind('/') + 1);
            }
            f.node = paths.dev + "/input/" + name;
            f.hid = device + "/device";
            out.push_back(std::move(f));
        }
        ::closedir(dir);
        std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return eventNumber(a.event) < eventNumber(b.event); });
        return out;
    }

    int buttonOf(uint16_t code) noexcept
    {
        // hid-input: a joystick's buttons 1 to 16 from BTN_JOYSTICK (0x120), a gamepad's from BTN_GAMEPAD (0x130),
        // another's from BTN_MISC (0x100); buttons 17 on from BTN_TRIGGER_HAPPY (0x2c0), forty of them.
        if (code >= BTN_JOYSTICK && code < BTN_JOYSTICK + 16)
            return code - BTN_JOYSTICK + 1;
        if (code >= BTN_GAMEPAD && code < BTN_GAMEPAD + 16)
            return code - BTN_GAMEPAD + 1;
        if (code >= BTN_MISC && code < BTN_MISC + 16)
            return code - BTN_MISC + 1;
        if (code >= BTN_TRIGGER_HAPPY && code < BTN_TRIGGER_HAPPY + 40)
            return code - BTN_TRIGGER_HAPPY + 17;
        return 0;
    }

    uint32_t usageOf(uint16_t absCode) noexcept
    {
        // hid-input: the Generic Desktop's X (0x30) to Rz (0x35) are ABS_X to ABS_RZ, in order.
        return absCode <= ABS_RZ ? 0x30u + absCode : 0u;
    }

    int hatOf(int x, int y) noexcept
    {
        static constexpr int positions[3][3] = { { 7, 0, 1 }, { 6, 8, 2 }, { 5, 4, 3 } };   // [y + 1][x + 1]
        return positions[std::clamp(y, -1, 1) + 1][std::clamp(x, -1, 1) + 1];
    }

    static_assert(kSyn == EV_SYN && kKey == EV_KEY && kAbs == EV_ABS && kFf == EV_FF);
    static_assert(kSynReport == SYN_REPORT && kSynDropped == SYN_DROPPED && kHatX == ABS_HAT0X && kHatY == ABS_HAT0Y);
    static_assert(kAutocenter == FF_AUTOCENTER);

    // ---- the device's own calls -----------------------------------------------------------------------------------------
    int System::open(const std::string& node)
    {
        int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS))
            fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        return fd;
    }

    bool System::abs(int fd, uint16_t code, int32_t& value, int32_t& min, int32_t& max)
    {
        input_absinfo info{};
        if (::ioctl(fd, EVIOCGABS(code), &info) != 0)
            return false;
        value = info.value;
        min = info.minimum;
        max = info.maximum;
        return true;
    }

    bool System::keys(int fd, std::vector<uint8_t>& bits)
    {
        bits.assign(KEY_MAX / 8 + 1, 0);
        return ::ioctl(fd, EVIOCGKEY(bits.size()), bits.data()) >= 0;
    }

    // ---- the hub ------------------------------------------------------------------------------------------------------------
    Hub::Hub(System& system, Paths paths, bool on) : sys_(system), paths_(std::move(paths)), driver_(*this, on) {}

    Hub::~Hub()
    {
        driver_.quit();                                      // the spring and the lights off, while the nodes are open
        for (const auto& d : devs_)
            closeNode(*d);
    }

    Hub::Dev* Hub::find(Device d) const noexcept
    {
        for (const auto& p : devs_)
            if (p.get() == d)
                return p.get();
        return nullptr;
    }

    void Hub::openNode(Dev& d)
    {
        d.fd = sys_.open(d.found.node);
        if (d.fd < 0)
        {
            d.error = errno != 0 ? errno : EIO;
            return;
        }
        d.error = 0;
        d.ranges.clear();
        for (uint16_t code = ABS_X; code <= ABS_RZ; ++code)
        {
            int32_t v = 0, lo = 0, hi = 0;
            if (sys_.abs(d.fd, code, v, lo, hi))
                d.ranges[code] = { lo, hi };
        }
        sys_.watch(d.fd);
    }

    void Hub::closeNode(Dev& d)
    {
        if (d.fd < 0)
            return;
        sys_.unwatch(d.fd);
        ::close(d.fd);
        d.fd = -1;
    }

    void Hub::remove(Dev* d)
    {
        driver_.removed(d);                                  // before it goes: the driver may still name it
        if (listening_ == d)
            listening_ = nullptr;
        closeNode(*d);
        devs_.erase(std::remove_if(devs_.begin(), devs_.end(), [d](const auto& p) { return p.get() == d; }), devs_.end());
    }

    void Hub::rescan()
    {
        const std::vector<Found> found = scan(paths_);
        const auto same = [](const Found& a, const Found& b) { return a.event == b.event && a.input == b.input && a.kind == b.kind; };
        // Gone (or plugged in again as another input device, or come back as another kind under the same node).
        std::vector<Dev*> gone;
        for (const auto& p : devs_)
            if (std::none_of(found.begin(), found.end(), [&](const Found& f) { return same(f, p->found); }))
                gone.push_back(p.get());
        for (Dev* d : gone)
            remove(d);
        // One that could not be opened: tried again, and told to the driver as come anew once it opens (set up, read).
        for (const auto& p : devs_)
            if (p->fd < 0)
            {
                openNode(*p);
                if (p->fd >= 0)
                {
                    driver_.removed(p.get());
                    driver_.opened(0);
                    driver_.added(p.get(), p->found.kind);
                }
            }
        // New.
        for (const Found& f : found)
        {
            if (std::any_of(devs_.begin(), devs_.end(), [&](const auto& p) { return same(f, p->found); }))
                continue;
            auto d = std::make_unique<Dev>();
            d->found = f;
            openNode(*d);
            // A node not made yet (udev's) waits for the next scan; one this account may not open is listed, said why.
            if (d->fd < 0 && (d->error == ENOENT || d->error == ENODEV || d->error == ENXIO))
                continue;
            Dev* raw = d.get();
            devs_.push_back(std::move(d));
            driver_.opened(raw->fd >= 0 ? 0 : raw->error == EACCES || raw->error == EPERM ? Driver::kNotPermitted : raw->error);
            driver_.added(raw, raw->found.kind);
        }
    }

    void Hub::readable(int fd)
    {
        Dev* d = nullptr;
        for (const auto& p : devs_)
            if (p->fd == fd && fd >= 0)
                d = p.get();
        if (d == nullptr)
            return;
        input_event buf[64];
        for (;;)
        {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (n <= 0)
            {
                remove(d);                                   // ENODEV: unplugged (end of file: a probe's node closed)
                return;
            }
            const std::size_t count = static_cast<std::size_t>(n) / sizeof(input_event);
            for (std::size_t i = 0; i < count; ++i)
            {
                Event e;
                e.micros = static_cast<uint64_t>(buf[i].input_event_sec) * 1000000u + static_cast<uint64_t>(buf[i].input_event_usec);
                e.type = buf[i].type;
                e.code = buf[i].code;
                e.value = buf[i].value;
                apply(*d, e);
                if (find(d) == nullptr)
                    return;
            }
        }
    }

    void Hub::apply(Dev& d, const Event& e)
    {
        if (e.type == kSyn)
        {
            if (e.code == kSynDropped)
                d.dropping = true;
            else if (e.code == kSynReport && d.dropping)
            {
                d.dropping = false;
                resync(d, e.micros);
            }
            return;
        }
        if (d.dropping)
            return;
        const uint64_t at = std::max<uint64_t>(e.micros, 1);
        if (e.type == kKey)
        {
            if (const int b = buttonOf(e.code); b > 0)
                take(d, { kButtonPage, static_cast<uint32_t>(b), e.value != 0 ? 1 : 0, 0, 1, 1, at });
        }
        else if (e.type == kAbs)
        {
            if (e.code == kHatX || e.code == kHatY)
            {
                (e.code == kHatX ? d.hatX : d.hatY) = e.value;
                take(d, { kDesktop, kHat, hatOf(d.hatX, d.hatY), 0, 7, 4, at });
            }
            else if (const uint32_t usage = usageOf(e.code); usage != 0)
            {
                const auto r = d.ranges.find(e.code);
                const int64_t lo = r != d.ranges.end() ? r->second.first : 0;
                const int64_t hi = r != d.ranges.end() ? r->second.second : 255;
                take(d, { kDesktop, usage, e.value, lo, hi, 16, at });
            }
        }
    }

    // After SYN_DROPPED: the state as the kernel holds it now, each value as if an event had carried it.
    void Hub::resync(Dev& d, uint64_t micros)
    {
        const uint64_t at = std::max<uint64_t>(micros, 1);
        for (const auto& [code, range] : d.ranges)
        {
            int32_t v = 0, lo = 0, hi = 0;
            if (sys_.abs(d.fd, code, v, lo, hi))
                take(d, { kDesktop, usageOf(code), v, lo, hi, 16, at });
        }
        int32_t x = 0, y = 0, lo = 0, hi = 0;
        if (sys_.abs(d.fd, kHatX, x, lo, hi) && sys_.abs(d.fd, kHatY, y, lo, hi))
        {
            d.hatX = x;
            d.hatY = y;
            take(d, { kDesktop, kHat, hatOf(x, y), 0, 7, 4, at });
        }
        // A button is down when any code that names it is (a joystick's, a gamepad's or another's: buttonOf's ranges).
        std::vector<uint8_t> bits;
        if (sys_.keys(d.fd, bits))
        {
            std::map<int, bool> down;
            for (std::size_t code = 0; code < bits.size() * 8; ++code)
                if (const int b = buttonOf(static_cast<uint16_t>(code)); b > 0)
                    down[b] = down[b] || ((bits[code / 8] >> (code % 8)) & 1) != 0;
            for (const auto& [b, isDown] : down)
                if (isDown || d.values.count(key(kButtonPage, static_cast<uint32_t>(b))) != 0)
                    take(d, { kButtonPage, static_cast<uint32_t>(b), isDown ? 1 : 0, 0, 1, 1, at });
        }
    }

    void Hub::take(Dev& d, const Value& v)
    {
        d.values[key(v.page, v.usage)] = v;
        if (listening_ == &d)
            driver_.value(&d, v);
    }

    // ---- Transport ------------------------------------------------------------------------------------------------------------
    int Hub::report(Device, const Report&) { return ENOTSUP; }

    int Hub::setRange(Device dv, int degrees)
    {
        const Dev* d = find(dv);
        if (d == nullptr)
            return ENODEV;
        const std::string path = d->found.hid + "/range";
        std::string now;
        if (!readSmall(path, now))
            return errno == ENOENT ? ENOENT : errno != 0 ? errno : EIO;
        if (now == std::to_string(degrees))
            return 0;                                        // as the kernel set it: nothing to write
        return writeSmall(path, std::to_string(degrees));
    }

    int Hub::setSpring(Device dv, uint16_t magnitude)
    {
        const Dev* d = find(dv);
        if (d == nullptr)
            return ENODEV;
        if (d->fd < 0)
            return d->error != 0 ? d->error : EBADF;
        input_event e{};
        e.type = EV_FF;
        e.code = FF_AUTOCENTER;
        e.value = magnitude;
        const ssize_t n = ::write(d->fd, &e, sizeof e);
        return n == static_cast<ssize_t>(sizeof e) ? 0 : n < 0 ? errno : EIO;
    }

    int Hub::setLights(Device dv, int mask)
    {
        const Dev* d = find(dv);
        if (d == nullptr)
            return ENODEV;
        const std::string leds = d->found.hid + "/leds";
        DIR* dir = ::opendir(leds.c_str());
        if (dir == nullptr)
            return errno == ENOENT ? ENOENT : errno != 0 ? errno : EIO;
        std::vector<std::pair<int, std::string>> found;      // RPM<n>'s n, its folder
        while (const dirent* e = ::readdir(dir))
        {
            const std::string name = e->d_name;
            const std::size_t at = name.rfind("::RPM");
            if (at == std::string::npos || name.size() != at + 6 || name.back() < '1' || name.back() > '5')
                continue;
            found.emplace_back(name.back() - '0', leds + "/" + name);
        }
        ::closedir(dir);
        if (found.empty())
            return ENOENT;
        std::sort(found.begin(), found.end());
        int first = 0;
        for (const auto& [n, folder] : found)
            if (const int e = writeSmall(folder + "/brightness", ((mask >> (n - 1)) & 1) != 0 ? "1" : "0"); e != 0 && first == 0)
                first = e;
        return first;
    }

    int Hub::toNative(Device dv)
    {
        const Dev* d = find(dv);
        return d == nullptr ? ENODEV : writeSmall(d->found.hid + "/alternate_modes", "native");
    }

    std::string Hub::describe(int error)
    {
        switch (error)
        {
            case EACCES:
            case EPERM:   return "it is root's: see the README for a udev rule";
            case ENOENT:  return "the kernel's hid-lg4ff does not drive it";
            case EBADF:   return "its device is open for reading only";
            case ENODEV:  return "it is gone";
            case ENOTSUP: return "not here";
            default:
            {
                std::string s = std::strerror(error);
                if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z')
                    s[0] = static_cast<char>(s[0] - 'A' + 'a');
                return s;
            }
        }
    }

    std::vector<Value> Hub::values(Device dv)
    {
        std::vector<Value> out;
        if (const Dev* d = find(dv))
            for (const auto& [k, v] : d->values)
                out.push_back(v);
        return out;
    }

    void Hub::listen(Device dv) { listening_ = find(dv) != nullptr ? dv : nullptr; }
}
#endif
