// Source/plugin/WheelMap.cpp: see WheelMap.h.
#include "plugin/WheelMap.h"

#include <algorithm>

namespace playground::wheel
{
    namespace
    {
        constexpr uint32_t padBit(Pad p) noexcept
        {
            switch (p)
            {
                case Pad::up:    return 1u;
                case Pad::right: return 2u;
                case Pad::down:  return 4u;
                case Pad::left:  return 8u;
                case Pad::none:  return 0u;
            }
            return 0u;
        }

        Message cc(int number, int value)
        {
            return { Message::kCc, static_cast<uint8_t>(number), static_cast<uint8_t>(value) };
        }

        // What was sent (from) to what is wanted (to), as control changes: an axis's MSB when it differs (or nothing
        // was sent), then its LSB when it differs from what the receiver holds after that (an MSB resets it to 0).
        void diff(const State& from, bool fromValid, const State& to, std::vector<Message>& out)
        {
            using namespace wheelcc;
            for (int a = 0; a < kAxes; ++a)
            {
                const int v = to.v14[static_cast<std::size_t>(a)], s = from.v14[static_cast<std::size_t>(a)];
                const bool msb = !fromValid || (v >> 7) != (s >> 7);
                if (msb)
                    out.push_back(cc(kSteering + a, v >> 7));
                const int held = msb ? 0 : (s & 0x7F);
                if ((v & 0x7F) != held)
                    out.push_back(cc(kSteering + kLsb + a, v & 0x7F));
            }
            if (!fromValid || to.gear != from.gear)
                out.push_back(cc(kGear, to.gear & 0x7F));
            for (int b = 0; b < kButtonCcs; ++b)
            {
                const int shift = b * kButtonBits;
                const auto part = [shift](uint32_t mask) { return static_cast<int>((mask >> shift) & 0x7Fu); };
                if (!fromValid || part(to.buttons) != part(from.buttons))
                    out.push_back(cc(kButtons + b, part(to.buttons)));
            }
        }
    }

    Kind kindOf(int vendor, int product, int version) noexcept
    {
        if (vendor != kVendor)
            return Kind::none;
        if (product == kG29)
            return Kind::g29;
        if (product == kG29Ps4)
            return Kind::ps4;
        if (product == kCompat)
        {
            // lg4ff_main_checklist's first two rows, the G29's (lg4ff_g29_ident_info, lg4ff_g29_ident_info2); any other
            // bcdDevice is another wheel (a DFGT, G27, G25, DFP) or a Driving Force, which hid-lg4ff leaves alone too.
            const int bcd = version & 0xFFFF;
            if ((bcd & 0xFFF8) == 0x1350 || (bcd & 0xFF00) == 0x8900)
                return Kind::compat;
        }
        return Kind::none;
    }

    Report rangeReport(int degrees) noexcept
    {
        const int d = std::clamp(degrees, 40, 900);
        return { 0xf8, 0x81, static_cast<uint8_t>(d & 0xFF), static_cast<uint8_t>((d >> 8) & 0xFF), 0x00, 0x00, 0x00 };
    }

    std::array<Report, 2> springReports(uint16_t magnitude) noexcept
    {
        uint32_t a = 0, b = 0;
        const uint32_t m = magnitude;
        if (m <= 0xAAAAu)
        {
            a = 0x0Cu * m;
            b = 0x80u * m;
        }
        else
        {
            a = 0x0Cu * 0xAAAAu + 0x06u * (m - 0xAAAAu);
            b = 0x80u * 0xAAAAu + 0xFFu * (m - 0xAAAAu);
        }
        a >>= 1;                                             // "Adjust for non-MOMO wheels"
        const auto k = static_cast<uint8_t>(a / 0xAAAAu), clip = static_cast<uint8_t>(b / 0xAAAAu);
        return { { { 0xfe, 0x0d, k, k, clip, 0x00, 0x00 }, { 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } } };
    }

    void normalise(int64_t& value, int64_t& min, int64_t& max, uint32_t reportSize) noexcept
    {
        if (max >= min || reportSize == 0 || reportSize > 32)
            return;
        max = (int64_t{ 1 } << reportSize) - 1;
        min = 0;
        value &= max;
    }

    int Map::axisValue(Axis a, bool restAtMax, int64_t value, int64_t min, int64_t max) noexcept
    {
        using namespace wheelcc;
        if (a == Axis::steering)
        {
            const int64_t span = max - min + 1;
            if (span <= 1)
                return kCentre;
            return static_cast<int>(std::clamp<int64_t>((value - min) * (kMost + 1) / span, 0, kMost));
        }
        const int64_t span = max - min;
        if (span <= 0)
            return 0;
        const int64_t pressed = std::clamp<int64_t>(restAtMax ? max - value : value - min, 0, span);
        const int64_t dead = (span + kDeadband / 2) / kDeadband;   // 1 % of the travel, rounded (8 bits: 3 of 255)
        if (pressed <= dead || span <= dead)
            return 0;
        const int64_t travel = span - dead;
        return static_cast<int>(((pressed - dead) * kMost + travel / 2) / travel);
    }

    uint32_t Map::padBits(int64_t value, int64_t min, int64_t max) noexcept
    {
        constexpr uint32_t up = 1u, right = 2u, down = 4u, left = 8u;
        constexpr uint32_t eight[] = { up, up | right, right, right | down, down, down | left, left, left | up };
        constexpr uint32_t four[] = { up, right, down, left };
        const int64_t n = max - min + 1, i = value - min;
        if (i < 0 || i >= n)
            return 0;
        if (n == 8)
            return eight[static_cast<std::size_t>(i)];
        if (n == 4)
            return four[static_cast<std::size_t>(i)];
        return 0;
    }

    bool Map::input(uint32_t page, uint32_t usage, int64_t value, int64_t min, int64_t max) noexcept
    {
        if (page == kDesktop)
        {
            for (const AxisRow& row : kAxisRows)
                if (row.usage == usage)
                {
                    now_.v14[static_cast<std::size_t>(row.axis)] = axisValue(row.axis, row.restAtMax, value, min, max);
                    return true;
                }
            if (usage == kHat)
            {
                pad_ = padBits(value, min, max);
                derive();
                return true;
            }
            return false;
        }
        if (page != kButtonPage || usage < 1 || usage > static_cast<uint32_t>(kButtonsMax))
            return false;
        const uint64_t bit = uint64_t{ 1 } << (usage - 1);
        held_ = value != 0 ? (held_ | bit) : (held_ & ~bit);
        derive();
        const int button = static_cast<int>(usage);
        return std::any_of(std::begin(kGears), std::end(kGears), [button](const GearRow& g) { return g.button == button; })
            || std::any_of(std::begin(kBits), std::end(kBits), [button](const BitRow& b) { return b.button == button; });
    }

    void Map::derive() noexcept
    {
        const auto down = [this](int button) { return button >= 1 && button <= kButtonsMax && ((held_ >> (button - 1)) & 1u) != 0; };
        now_.gear = 0;
        for (const GearRow& g : kGears)
            if (down(g.button))
            {
                now_.gear = g.gear;
                break;
            }
        uint32_t mask = 0;
        for (std::size_t i = 0; i < std::size(kBits); ++i)
        {
            const BitRow& row = kBits[i];
            if (row.button > 0 ? down(row.button) : (pad_ & padBit(row.pad)) != 0)
                mask |= 1u << i;
        }
        now_.buttons = mask;
    }

    void Map::flush(std::vector<Message>& out)
    {
        State to = now_;
        if (sentValid_)
            for (std::size_t a = 0; a < to.v14.size(); ++a)
            {
                // Jitter: a change within one MSB smaller than kJitter waits (what was sent stands) until it grows,
                // unless it comes to rest.
                const int v = to.v14[a], s = sent_.v14[a], rest = a == 0 ? wheelcc::kCentre : 0;
                if ((v >> 7) == (s >> 7) && v != rest && (v > s ? v - s : s - v) < kJitter)
                    to.v14[a] = s;
            }
        diff(sent_, sentValid_, to, out);
        sent_ = to;
        sentValid_ = true;
        const bool start = ((held_ >> (kStartButton - 1)) & 1u) != 0;   // after the controls: start reads them
        if (offAgain_ && !start && !noteOn_)
            out.push_back({ Message::kNoteOff, kStartNote, 0 });
        offAgain_ = false;
        if (start != noteOn_)
        {
            out.push_back(start ? Message{ Message::kNoteOn, kStartNote, kStartVelocity } : Message{ Message::kNoteOff, kStartNote, 0 });
            noteOn_ = start;
            noteEver_ = noteEver_ || start;
        }
    }

    void Map::resend() noexcept
    {
        sentValid_ = false;
        offAgain_ = noteEver_ && !noteOn_;
    }

    void Map::rest(std::vector<Message>& out)
    {
        if (sentValid_)
            diff(sent_, true, State{}, out);
        if (noteOn_)
            out.push_back({ Message::kNoteOff, kStartNote, 0 });
        noteOn_ = offAgain_ = false;
        now_ = sent_ = State{};
        held_ = 0;
        pad_ = 0;
    }

    std::string bitNames()
    {
        std::string s;
        for (std::size_t i = 0; i < std::size(kBits); ++i)
            s += (i > 0 ? ", " : "") + std::to_string(i) + " " + kBits[i].name;
        return s;
    }

    static_assert(std::size(kBits) <= static_cast<std::size_t>(wheelcc::kButtonCcs * wheelcc::kButtonBits),
                  "the buttons' mask holds at most 28 bits");
}
