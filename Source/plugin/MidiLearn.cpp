// Source/plugin/MidiLearn.cpp: MIDI learn for the dials (MidiLearn.h).
#include "plugin/MidiLearn.h"

#include "dsp/Events.h"

#include <algorithm>
#include <cstring>

namespace playground::midilearn
{
    namespace
    {
        constexpr uint32_t kPacked = 1u << 16;           // a packed control: kPacked | channel << 8 | cc

        constexpr bool wheelControl(int cc) noexcept
        {
            using namespace wheelcc;
            return (cc >= kSteering && cc < kButtons + kButtonCcs) || (cc >= kSteering + kLsb && cc < kSteering + kLsb + kAxes);
        }

        // The controls a mapping takes: its number, and its LSB's when it has 14 bits.
        bool overlaps(const Mapping& a, const Mapping& b) noexcept
        {
            if (a.channel != b.channel)
                return false;
            const auto takes = [](const Mapping& m, int cc) { return m.cc == cc || (m.fine && m.cc + kLsb == cc); };
            return takes(b, a.cc) || (a.fine && takes(b, a.cc + kLsb));
        }

        bool valid(const Mapping& m) noexcept
        {
            if (m.dial.empty() || m.dial.size() >= sizeof(Control{}.name) || m.channel < 1 || m.channel > kChannels
                || m.cc < 0 || m.cc >= kFirstMode || refused(m.cc))
                return false;
            return !m.fine || (m.cc < kPairs && !refused(m.cc + kLsb));
        }
    }

    bool refused(int cc) noexcept { return cc < 0 || cc >= kFirstMode || wheelControl(cc); }

    std::string refusal(int cc)
    {
        const std::string n = "CC " + std::to_string(cc);
        if (cc < 0 || cc >= kControls)
            return n + " is no control";
        if (cc >= kFirstMode)
        {
            static constexpr const char* kModes[] = { "All Sound Off", "Reset All Controllers", "Local Control", "All Notes Off",
                                                      "Omni Off",      "Omni On",               "Mono On",       "Poly On" };
            return n + " is a channel mode message (" + kModes[cc - kFirstMode] + "), not a control";
        }
        if (!wheelControl(cc))
            return {};
        using namespace wheelcc;
        static constexpr const char* kAxisNames[] = { "steering", "throttle", "brake", "clutch" };
        const char* what = cc >= kSteering + kLsb  ? kAxisNames[cc - kSteering - kLsb]
                         : cc < kSteering + kAxes ? kAxisNames[cc - kSteering]
                         : cc == kGear            ? "gear"
                                                  : "buttons";
        return n + " is the racing wheel's " + what + " (a wheel's controls, CC 16 to 24 and 48 to 51, are not learned)";
    }

    std::string describe(const Mapping& m)
    {
        const std::string cc = m.fine ? "CC " + std::to_string(m.cc) + " and " + std::to_string(m.cc + kLsb) + " (14-bit)"
                                      : "CC " + std::to_string(m.cc);
        return cc + " on channel " + std::to_string(m.channel);
    }

    std::string mark(const Mapping& m)
    {
        return "CC " + std::to_string(m.cc) + (m.fine ? "+" + std::to_string(m.cc + kLsb) : std::string());
    }

    std::vector<Mapping> tidy(std::vector<Mapping> in)
    {
        std::vector<Mapping> kept;
        for (auto it = in.rbegin(); it != in.rend() && kept.size() < static_cast<std::size_t>(kMaxControls); ++it)
        {
            if (!valid(*it))
                continue;
            if (std::any_of(kept.begin(), kept.end(), [&](const Mapping& k) { return k.dial == it->dial || overlaps(k, *it); }))
                continue;                                // a later one has that dial or that control
            kept.push_back(*it);
        }
        std::reverse(kept.begin(), kept.end());
        return kept;
    }

    Learn::Learn()
    {
        lastMs_.fill(0.0);
    }

    // ---- the audio thread --------------------------------------------------------------------------------------------
    void Learn::controlChange(int channel, int cc, int value) noexcept
    {
        if (channel < 0 || channel >= kChannels || cc < 0 || cc >= kControls)
            return;
        value = std::clamp(value, 0, 127);
        const int i = index(channel, cc);
        if (armed_.load(std::memory_order_acquire))
        {
            const uint32_t packed = kPacked | static_cast<uint32_t>(channel) << 8 | static_cast<uint32_t>(cc);
            if (refused(cc))
                refusedCc_.store(packed, std::memory_order_release);
            else
            {
                seen_[static_cast<std::size_t>(i / 64)].fetch_or(uint64_t{ 1 } << static_cast<unsigned>(i % 64), std::memory_order_release);
                uint32_t none = 0;
                (void)first_.compare_exchange_strong(none, packed, std::memory_order_acq_rel);
            }
        }
        const uint16_t route = routes_[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
        if (route == 0)
            return;
        const int slot = (route & 0xff) - 1;
        const int kind = route >> 8;
        float v = 0.0f;
        if (kind == kSeven)
            v = static_cast<float>(value) / 127.0f;
        else
        {
            const int pair = channel * kPairs + (kind == kMsb ? cc : cc - kLsb);
            if (pair < 0 || pair >= kChannels * kPairs)
                return;
            auto& msb = msb_[static_cast<std::size_t>(pair)];
            auto& lsb = lsb_[static_cast<std::size_t>(pair)];
            if (kind == kMsb)
            {
                msb = static_cast<uint8_t>(value);
                lsb = 0;                                 // an MSB resets its LSB, as MIDI says
            }
            else
                lsb = static_cast<uint8_t>(value);
            v = static_cast<float>(msb << 7 | lsb) / 16383.0f;
        }
        if (slot < 0 || slot >= kMaxControls)
            return;
        values_[static_cast<std::size_t>(slot)].store(v, std::memory_order_relaxed);
        fresh_.fetch_or(slotBit(slot), std::memory_order_release);
    }

    // ---- under the mutex --------------------------------------------------------------------------------------------
    void Learn::changed()
    {
        revision_.fetch_add(1, std::memory_order_acq_rel);
    }

    void Learn::updateListening()
    {
        bool any = armed_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; !any && i < routes_.size(); ++i)
            any = routes_[i].load(std::memory_order_relaxed) != 0;
        listening_.store(any, std::memory_order_release);
    }

    void Learn::rebuild()
    {
        std::array<uint16_t, kChannels * kControls> want{};
        for (const Mapping& m : mappings_)
        {
            const auto at = std::find(names_.begin(), names_.end(), m.dial);
            if (at == names_.end())
                continue;                                // waits for a program with that dial
            const auto slot = static_cast<uint16_t>(at - names_.begin() + 1);
            const int ch = m.channel - 1;
            if (m.fine)
            {
                want[static_cast<std::size_t>(index(ch, m.cc))] = static_cast<uint16_t>(slot | kMsb << 8);
                want[static_cast<std::size_t>(index(ch, m.cc + kLsb))] = static_cast<uint16_t>(slot | kLsbKind << 8);
            }
            else
                want[static_cast<std::size_t>(index(ch, m.cc))] = slot;
        }
        for (std::size_t i = 0; i < want.size(); ++i)
            if (routes_[i].load(std::memory_order_relaxed) != want[i])
                routes_[i].store(want[i], std::memory_order_release);
        updateListening();
    }

    void Learn::bind(const Controls& controls)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& k = controls[static_cast<std::size_t>(c)];
            names_[static_cast<std::size_t>(c)] = k.used ? std::string(k.name, strnlen(k.name, sizeof k.name)) : std::string();
        }
        if (learning_ >= 0)
        {
            const auto at = std::find(names_.begin(), names_.end(), learningDial_);
            if (at == names_.end())
            {
                armed_.store(false, std::memory_order_release);   // the program lost the dial that was learning
                learning_ = -1;
                learningDial_.clear();
                note_.clear();
            }
            else
                learning_ = static_cast<int>(at - names_.begin());
        }
        endAll_ |= touching_;                            // a slot may mean another dial now
        fresh_.store(0, std::memory_order_release);      // ... and a value waiting is the old one's
        rebuild();
        changed();
    }

    void Learn::setMappings(std::vector<Mapping> mappings)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        mappings_ = tidy(std::move(mappings));
        armed_.store(false, std::memory_order_release);
        learning_ = -1;
        learningDial_.clear();
        note_.clear();
        endAll_ |= touching_;
        fresh_.store(0, std::memory_order_release);
        rebuild();
        changed();
    }

    std::vector<Mapping> Learn::mappings() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return mappings_;
    }

    bool Learn::arm(int slot)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (slot < 0 || slot >= kMaxControls || names_[static_cast<std::size_t>(slot)].empty())
            return false;
        armed_.store(false, std::memory_order_release);
        first_.store(0, std::memory_order_release);
        refusedCc_.store(0, std::memory_order_release);
        for (auto& s : seen_)
            s.store(0, std::memory_order_release);
        learning_ = slot;
        learningDial_ = names_[static_cast<std::size_t>(slot)];
        note_.clear();
        firstAtMs_ = -1.0;
        armed_.store(true, std::memory_order_release);
        updateListening();
        changed();
        return true;
    }

    void Learn::cancel()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        armed_.store(false, std::memory_order_release);
        updateListening();
        if (learning_ < 0 && note_.empty())
            return;
        learning_ = -1;
        learningDial_.clear();
        note_.clear();
        changed();
    }

    bool Learn::forget(int slot)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (slot < 0 || slot >= kMaxControls || names_[static_cast<std::size_t>(slot)].empty())
            return false;
        const std::string& dial = names_[static_cast<std::size_t>(slot)];
        const auto gone = std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& m) { return m.dial == dial; });
        if (gone == mappings_.end())
            return false;
        mappings_.erase(gone, mappings_.end());
        rebuild();
        changed();
        return true;
    }

    int Learn::forgetAll()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const int n = static_cast<int>(mappings_.size());
        if (n == 0)
            return 0;
        mappings_.clear();
        rebuild();
        changed();
        return n;
    }

    View Learn::view() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        View v;
        for (const Mapping& m : mappings_)
        {
            const auto at = std::find(names_.begin(), names_.end(), m.dial);
            if (at == names_.end())
            {
                v.waiting.push_back(m);
                continue;
            }
            const auto slot = static_cast<std::size_t>(at - names_.begin());
            v.marks[slot] = mark(m);
            v.words[slot] = describe(m);
        }
        v.learning = learning_;
        v.learningDial = learningDial_;
        v.note = note_;
        v.revision = revision_.load(std::memory_order_acquire);
        return v;
    }

    uint64_t Learn::touching() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return touching_;
    }

    // ---- the message thread -----------------------------------------------------------------------------------------
    void Learn::tick(double nowMs, Tick& out)
    {
        out = Tick{};
        const std::lock_guard<std::mutex> lock(mutex_);
        if (learning_ >= 0)
        {
            if (const uint32_t r = refusedCc_.exchange(0, std::memory_order_acq_rel); r != 0)
            {
                out.refused = refusal(static_cast<int>(r & 0xffu));
                if (out.refused != note_)
                {
                    note_ = out.refused;
                    changed();
                }
            }
            if (const uint32_t f = first_.load(std::memory_order_acquire); f != 0)
            {
                if (firstAtMs_ < 0.0)
                    firstAtMs_ = nowMs;
                const int ch = static_cast<int>((f >> 8) & 0xffu), cc = static_cast<int>(f & 0xffu);
                const auto seen = [&](int n) {
                    const int i = index(ch, n);
                    return (seen_[static_cast<std::size_t>(i / 64)].load(std::memory_order_acquire) >> (i % 64) & 1u) != 0;
                };
                const bool lsbCame = cc < kPairs && !refused(cc + kLsb) && seen(cc + kLsb);
                const bool msbCame = cc >= kLsb && cc < kLsb + kPairs && !refused(cc - kLsb) && seen(cc - kLsb);
                if (!(cc < kPairs && !lsbCame && nowMs - firstAtMs_ < kPairMs))   // else a control 0 to 31 waits
                {
                    Mapping m{ learningDial_, ch + 1, msbCame ? cc - kLsb : cc, lsbCame || msbCame };
                    armed_.store(false, std::memory_order_release);
                    mappings_.push_back(m);
                    mappings_ = tidy(std::move(mappings_));
                    learning_ = -1;
                    learningDial_.clear();
                    note_.clear();
                    rebuild();
                    changed();
                    out.learned = true;
                    out.mapping = std::move(m);
                }
            }
        }
        // The touches: those that ended (their last control change kTouchMs ago, or a bind's), then the moves.
        const uint64_t fresh = fresh_.exchange(0, std::memory_order_acq_rel);
        uint64_t ended = touching_ & endAll_;
        endAll_ = 0;
        for (int c = 0; c < kMaxControls; ++c)
            if ((touching_ & slotBit(c)) != 0 && (fresh & slotBit(c)) == 0 && nowMs - lastMs_[static_cast<std::size_t>(c)] >= kTouchMs)
                ended |= slotBit(c);
        touching_ &= ~ended;
        out.ended = ended;
        for (int c = 0; c < kMaxControls; ++c)
            if ((fresh & slotBit(c)) != 0 && !names_[static_cast<std::size_t>(c)].empty())
            {
                out.moves.push_back({ c, values_[static_cast<std::size_t>(c)].load(std::memory_order_relaxed) });
                lastMs_[static_cast<std::size_t>(c)] = nowMs;
                touching_ |= slotBit(c);
            }
    }
}
