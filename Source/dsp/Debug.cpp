// Source/dsp/Debug.cpp: see Debug.h.
#include "dsp/Debug.h"

#include "dsp/Engine.h"
#include "dsp/SoundView.h"
#include "fminus/Lang.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <span>

namespace playground::measure
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr int kGrid = 16;                          // pass 1's calls (the Engine's grid)
        constexpr double kBig = 1e6;                       // a field past it runs away
        constexpr std::size_t kAfter = 64;                 // pass 2's window goes on past the problem's call
        constexpr long kNear = 32;                         // samples: the events near a problem
        constexpr std::size_t kMaxValues = std::size_t{ 4 } << 20;   // pass 2 keeps at most this many (floats)
        constexpr std::size_t kMaxScanned = 2048;          // fields pass 1 reads after each call
        constexpr std::size_t kMaxTracked = 16;            // instances pass 2 keeps
        constexpr std::size_t kMaxProblems = 3;
        constexpr std::size_t kMaxListed = 56;             // list: lines of fields

        using Field = Patch::Field;
        using Voice = Engine::VoiceInfo;

        // ---- text ----------------------------------------------------------------------------------------------------
        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        std::string num(double v)
        {
            if (std::isnan(v))
                return "nan";
            if (std::isinf(v))
                return v > 0.0 ? "inf" : "-inf";
            return fmt("%.4g", v);
        }

        std::string grouped(std::size_t n)
        {
            std::string s = std::to_string(n), out;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (i > 0 && (s.size() - i) % 3 == 0)
                    out += ',';
                out += s[i];
            }
            return out;
        }

        std::string seconds(double s) { return fmt(s < 10.0 ? "%.3f s" : "%.2f s", s); }

        std::string when(std::size_t at, double rate) { return seconds(static_cast<double>(at) / rate) + " (sample " + grouped(at) + ")"; }

        // A distance in samples, in words: "2 samples", "1.3 ms".
        std::string apart(std::size_t d, double rate)
        {
            if (d < 48)
                return d == 1 ? std::string("1 sample") : std::to_string(d) + " samples";
            return fmt("%.1f ms", static_cast<double>(d) / rate * 1000.0);
        }

        std::string noteName(int n)
        {
            static constexpr const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return std::string(kNames[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
        }

        std::string voiceText(int note) { return note >= 0 ? "the voice of note " + std::to_string(note) + " (" + noteName(note) + ")" : std::string(); }

        std::string joined(const std::vector<std::string>& parts, const char* with = ", ")
        {
            std::string out;
            for (const std::string& p : parts)
                out += (out.empty() ? "" : with) + p;
            return out;
        }

        double median(std::vector<double> v)
        {
            if (v.empty())
                return 0.0;
            const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
            std::nth_element(v.begin(), mid, v.end());
            return *mid;
        }

        bool finite(double v) noexcept { return std::isfinite(v); }

        // A field's value as text: an int's or a bool's whole.
        std::string val(const Patch::Field& f, double v)
        {
            if (f.type != Patch::Field::Type::real && std::isfinite(v))
                return fmt("%.0f", v);
            return num(v);
        }

        // ---- the time limit and the cancel --------------------------------------------------------------------------
        struct Clock
        {
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            double limit = kTraceSeconds;
            const std::function<bool()>* keepGoing = nullptr;
            bool stopped = false;
            unsigned ticks = 0;

            bool ok()
            {
                if (stopped)
                    return false;
                if ((++ticks & 63u) != 0)
                    return true;
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                stopped = s > limit || (keepGoing != nullptr && *keepGoing && !(*keepGoing)());
                return !stopped;
            }
        };

        // ---- the voices a choice puts forward --------------------------------------------------------------------------
        // The instances of a call's voices the choice reads: an effect's one; every voice's (all); the newest that
        // sounds, as the panel's displays read (newest), or of a note.
        void chosen(std::span<const Voice> vs, int voice, std::vector<int>& out)
        {
            out.clear();
            if constexpr (!kInstrument)
            {
                out.push_back(0);
                return;
            }
            if (voice == kTraceAll)
            {
                for (const Voice& v : vs)
                    out.push_back(v.instance);
                return;
            }
            const Voice* best = nullptr;
            for (const Voice& v : vs)
            {
                if (voice >= 0 && v.key != voice)
                    continue;
                if (voice == kTraceNewest && (v.fading || v.ending))
                    continue;
                if (best == nullptr || v.age >= best->age)
                    best = &v;
            }
            if (best != nullptr)
                out.push_back(best->instance);
        }

        // ---- pass 1 ---------------------------------------------------------------------------------------------------
        struct CallRec
        {
            std::size_t at = 0;
            int length = 0;
            uint32_t alerts = 0;
            std::size_t first = 0, count = 0;              // its voices, after it (Pass1::voices)
        };

        struct Transition
        {
            std::size_t call = 0;                          // the call after which it was so (npos: at the start)
            int slot = 0;                                  // the instance, or -1: Shared's
            std::size_t field = 0;                         // an index of the map
        };

        constexpr std::size_t kStart = static_cast<std::size_t>(-1);

        class Pass1 final : public RenderHooks
        {
        public:
            Pass1(const Patch& patch, std::vector<std::size_t> scan, std::vector<std::size_t> watch, int voice, Clock& clock)
                : patch_(patch), map_(patch.fieldMap()), scan_(std::move(scan)), watch_(std::move(watch)), voice_(voice), clock_(clock)
            {
                slots_ = static_cast<std::size_t>(patch.instances()) + 1;
                state_.assign(slots_ * scan_.size(), 0);
                moved.assign(scan_.size(), 0);
                jumpSize.assign(watch_.size(), 0.0);
                jumpAt.assign(watch_.size(), 0);
                last_.assign(slots_ * watch_.size(), 0.0);
                lastCall_.assign(slots_ * watch_.size(), kStart);
            }

            int callLength(std::size_t at) override
            {
                if (!started_)
                {
                    started_ = true;
                    for (int i = 0; i < patch_.instances(); ++i)
                        scanSlot(i, kStart, false);
                    scanSlot(-1, kStart, false);
                }
                return kGrid - static_cast<int>(at % kGrid);
            }

            bool afterCall(const CallInfo& c) override
            {
                CallRec r;
                r.at = c.at;
                r.length = c.length;
                r.alerts = c.alerts;
                r.first = voices.size();
                const std::size_t ci = calls.size();
                if constexpr (kInstrument)
                {
                    c.engine.voices(snap_);
                    voices.insert(voices.end(), snap_.begin(), snap_.end());
                    r.count = snap_.size();
                    // What sounds now, and what sounded before the call (a voice dropped in it).
                    std::set<int> scanned;
                    for (const Voice& v : snap_)
                        if (scanned.insert(v.instance).second)
                            scanSlot(v.instance, ci, true);
                    for (const int i : prev_)
                        if (scanned.insert(i).second)
                            scanSlot(i, ci, false);
                    prev_.clear();
                    for (const Voice& v : snap_)
                        prev_.push_back(v.instance);
                }
                else
                    scanSlot(0, ci, true);
                scanSlot(-1, ci, true);
                calls.push_back(r);
                watchCall(ci, c);
                return clock_.ok();
            }

            std::vector<CallRec> calls;
            std::vector<Voice> voices;
            std::vector<Transition> bad, big;              // the first non-finite, and past 1e6, in order (64 each)
            std::vector<char> moved;                       // per scanned field: not 0 in a voice that sounded
            // The watch, a call each: the value (the mean of every voice's, with all), its range, how many voices
            // had one, and whether one was non-finite.
            struct Watch
            {
                double v = 0.0, lo = 0.0, hi = 0.0;
                uint16_t count = 0;
                bool bad = false;
            };
            std::vector<Watch> watched;                    // calls x watch fields
            std::vector<double> jumpSize;
            std::vector<std::size_t> jumpAt;

        private:
            void scanSlot(int slot, std::size_t ci, bool sounding)
            {
                const auto s = static_cast<std::size_t>(slot < 0 ? static_cast<int>(slots_) - 1 : slot);
                for (std::size_t k = 0; k < scan_.size(); ++k)
                {
                    const Field& f = map_[scan_[k]];
                    if ((f.home == Field::Home::shared) != (slot < 0))
                        continue;
                    const double v = patch_.read(f, slot);
                    uint8_t& st = state_[s * scan_.size() + k];
                    if (!finite(v))
                    {
                        if ((st & 1) == 0)
                        {
                            st |= 1;
                            if (bad.size() < 64)
                                bad.push_back({ ci, slot, scan_[k] });
                        }
                    }
                    else if (f.type == Field::Type::real && std::fabs(v) > kBig && (st & 2) == 0)
                    {
                        st |= 2;
                        if (big.size() < 64)
                            big.push_back({ ci, slot, scan_[k] });
                    }
                    if (sounding && v != 0.0)
                        moved[k] = 1;
                }
            }

            void watchCall(std::size_t ci, const CallInfo& c)
            {
                if (watch_.empty())
                    return;
                chosen(std::span<const Voice>(snap_), voice_, picked_);
                for (std::size_t w = 0; w < watch_.size(); ++w)
                {
                    const Field& f = map_[watch_[w]];
                    Watch out;
                    double sum = 0.0;
                    const auto take = [&](int slot) {
                        const double v = patch_.read(f, slot);
                        if (!finite(v))
                        {
                            out.bad = true;
                            return;
                        }
                        out.lo = out.count == 0 ? v : std::min(out.lo, v);
                        out.hi = out.count == 0 ? v : std::max(out.hi, v);
                        sum += v;
                        ++out.count;
                        const std::size_t at = (slot < 0 ? slots_ - 1 : static_cast<std::size_t>(slot)) * watch_.size() + w;
                        if (lastCall_[at] != kStart && lastCall_[at] + 1 == ci && std::fabs(v - last_[at]) > jumpSize[w])
                        {
                            jumpSize[w] = std::fabs(v - last_[at]);
                            jumpAt[w] = c.at;
                        }
                        last_[at] = v;
                        lastCall_[at] = ci;
                    };
                    if (f.home != Field::Home::instance)
                        take(-1);
                    else
                        for (const int slot : picked_)
                            take(slot);
                    out.v = out.count > 0 ? sum / out.count : 0.0;
                    watched.push_back(out);
                }
            }

            const Patch& patch_;
            const std::vector<Field>& map_;
            std::vector<std::size_t> scan_, watch_;
            int voice_;
            Clock& clock_;
            std::size_t slots_ = 1;
            std::vector<uint8_t> state_;                   // slots x scanned: 1 non-finite seen, 2 past 1e6 seen
            std::vector<double> last_;
            std::vector<std::size_t> lastCall_;
            std::vector<Voice> snap_;
            std::vector<int> prev_, picked_;
            bool started_ = false;
        };

        // ---- pass 2 ---------------------------------------------------------------------------------------------------
        // The same render, in calls of one sample over [w0, w1), keeping the fields of `slots` (an instance, or -1 for
        // Shared's) sample by sample; it stops at w1. Its output is checked against pass 1's.
        class Pass2 final : public RenderHooks
        {
        public:
            struct Pair
            {
                int slot = 0;
                std::size_t field = 0;
            };

            Pass2(const Patch& patch, std::vector<Pair> kept, std::size_t from, std::size_t to, const std::vector<float>& l,
                  const std::vector<float>& r, Clock& clock)
                : pairs(std::move(kept)), w0(from), w1(to), patch_(patch), refL_(l), refR_(r), clock_(clock)
            {
                values.assign((w1 - w0) * pairs.size(), 0.0f);
                alerts.assign(w1 - w0, 0);
                voiceAt.assign(w1 - w0, {});
            }

            int callLength(std::size_t at) override
            {
                if (at < w0)
                    return static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(kGrid) - at % kGrid, w0 - at));
                return 1;
            }

            bool afterCall(const CallInfo& c) override
            {
                for (int i = 0; i < c.length; ++i)
                {
                    const std::size_t at = c.at + static_cast<std::size_t>(i);
                    if (at >= refL_.size() || at >= checkUntil)
                        break;
                    const float a = c.left[i], b = refL_[at], d = c.right[i], e = refR_[at];
                    if (std::memcmp(&a, &b, sizeof a) != 0 || std::memcmp(&d, &e, sizeof d) != 0)
                    {
                        if (!differs)
                            firstDiff = at;
                        differs = true;
                        maxDiff = std::max({ maxDiff, std::fabs(static_cast<double>(a) - b), std::fabs(static_cast<double>(d) - e) });
                    }
                }
                if (c.length > 0 && c.at >= w0 && c.at < w1)
                {
                    const std::size_t s = c.at - w0;
                    alerts[s] |= c.alerts;
                    if constexpr (kInstrument)
                        c.engine.voices(voiceAt[s]);
                    for (std::size_t t = 0; t < pairs.size(); ++t)
                        values[s * pairs.size() + t] = static_cast<float>(patch_.read(patch_.fieldMap()[pairs[t].field], pairs[t].slot));
                    reached = c.at + 1;
                }
                else if (c.length == 0 && c.at >= w0 && c.at < w1)
                    alerts[c.at - w0] |= c.alerts;
                return c.at + static_cast<std::size_t>(c.length) < w1 && clock_.ok();
            }

            double value(std::size_t at, std::size_t t) const { return values[(at - w0) * pairs.size() + t]; }
            bool has(std::size_t at) const { return at >= w0 && at < reached; }

            std::vector<Pair> pairs;
            std::size_t w0, w1, reached = 0;
            // The self-check compares the output before it: pass 1's first call with a NaN, whose voice the Engine drops
            // whole (16 samples), where pass 2 drops it at the sample.
            std::size_t checkUntil = static_cast<std::size_t>(-1);
            std::vector<float> values;
            std::vector<uint32_t> alerts;
            std::vector<std::vector<Voice>> voiceAt;
            bool differs = false;
            std::size_t firstDiff = 0;
            double maxDiff = 0.0;

        private:
            const Patch& patch_;
            const std::vector<float>& refL_;
            const std::vector<float>& refR_;
            Clock& clock_;
        };

        // ---- the lines that write a field (fminus/Lang.h) ----------------------------------------------------------
        class Writers
        {
        public:
            explicit Writers(const Sources& files)
            {
                service_.setOptions({ kInstrument });
                service_.setFiles(files);
                for (const lang::Service::File& f : service_.files())
                {
                    std::vector<lang::Token> code;
                    for (const lang::Token& t : lang::lex(f.text))
                        if (t.kind != lang::Tok::comment && t.kind != lang::Tok::directive)
                            code.push_back(t);
                    tokens_.push_back(std::move(code));
                }
            }

            // The places that assign `module`'s field `name` (or an element or a lane of it), "file:line", at most `most`.
            std::vector<std::string> of(const std::string& module, const std::string& name, std::size_t most) const
            {
                std::vector<std::string> out;
                const lang::Symbol* decl = nullptr;
                for (const lang::Symbol& s : service_.symbols())
                    if (s.kind == lang::Kind::field && s.container == module && s.name == name)
                    {
                        decl = &s;
                        break;
                    }
                if (decl == nullptr)
                    return out;
                const auto text = [](const std::vector<lang::Token>& ts, std::size_t i) {
                    return i < ts.size() && ts[i].kind != lang::Tok::identifier ? ts[i].text : std::string_view();
                };
                for (std::size_t fi = 0; fi < tokens_.size() && out.size() < most; ++fi)
                {
                    const std::vector<lang::Token>& ts = tokens_[fi];
                    for (std::size_t i = 0; i < ts.size() && out.size() < most; ++i)
                    {
                        if (ts[i].kind != lang::Tok::identifier || ts[i].text != name)
                            continue;
                        std::size_t j = i + 1;
                        if (text(ts, j) == "[")                // an element: past its index
                        {
                            int depth = 0;
                            for (; j < ts.size(); ++j)
                            {
                                depth += text(ts, j) == "[" ? 1 : text(ts, j) == "]" ? -1 : 0;
                                if (depth == 0)
                                    break;
                            }
                            ++j;
                        }
                        if (text(ts, j) == "." && j + 1 < ts.size() && ts[j + 1].kind == lang::Tok::identifier)
                            j += 2;                            // a lane
                        const std::string_view op = text(ts, j);
                        const bool assigns = op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%="
                                          || op == "++" || op == "--" || (i > 0 && (text(ts, i - 1) == "++" || text(ts, i - 1) == "--"));
                        if (!assigns)
                            continue;
                        const lang::Pos p{ ts[i].line, ts[i].col };
                        if (static_cast<int>(fi) == decl->file && p == decl->at)
                            continue;                          // the declaration itself
                        const std::optional<lang::Location> d = service_.definition(static_cast<int>(fi), p);
                        if (!d || d->file != decl->file || !(d->at == decl->at))
                            continue;
                        const std::string where = service_.files()[fi].name + ":" + std::to_string(ts[i].line + 1);
                        if (std::find(out.begin(), out.end(), where) == out.end())
                            out.push_back(where);
                    }
                }
                return out;
            }

        private:
            lang::Service service_;
            std::vector<std::vector<lang::Token>> tokens_;
        };

        // ---- the problems in pass 1's output and fields ------------------------------------------------------------
        struct Found
        {
            TraceFind kind = TraceFind::none;
            std::size_t at = 0;                            // its sample, as pass 1 places it
            std::size_t call = 0;                          // pass 1's call that had it (nan, runaway)
            int slot = -2;                                 // its instance (-1 Shared's; -2 not known)
            int note = -1;
            bool fromStart = false;
            std::size_t field = static_cast<std::size_t>(-1);   // the field pass 1 saw (nan, runaway)
            double size = 0.0, usual = 0.0;                // a click's step and its neighbourhood's
            double dc = 0.0;
            bool never = false;                            // silence: it never sounds
        };

        const char* kindWords(TraceFind k)
        {
            switch (k)
            {
                case TraceFind::nan: return "Not a number";
                case TraceFind::click: return "A click";
                case TraceFind::runaway: return "A runaway";
                case TraceFind::silence: return "Silence";
                case TraceFind::dc: return "A DC offset";
                case TraceFind::none:
                case TraceFind::any: return "";
            }
            return "";
        }

        struct Context
        {
            const Patch& patch;
            const std::vector<Field>& map;
            const Pass1& p1;
            const Sound& sound;
            const Timeline& tl;
            double rate;
            int voice;                                     // the request's choice (find's: all unless given)
            std::size_t nanCall;                           // pass 1's first call with a NaN alert's first sample (or none)
        };

        std::span<const Voice> voicesOf(const Pass1& p, std::size_t ci)
        {
            if (ci >= p.calls.size())
                return {};
            return std::span<const Voice>(p.voices.data() + p.calls[ci].first, p.calls[ci].count);
        }

        int keyOf(std::span<const Voice> vs, int slot)
        {
            for (const Voice& v : vs)
                if (v.instance == slot)
                    return v.key;
            return -1;
        }

        // Whether a slot's voice at call ci (or the call before: a voice dropped in it) is one the request asks about.
        bool asked(const Context& x, int slot, std::size_t ci)
        {
            if (!kInstrument || slot < 0 || x.voice == kTraceAll)
                return true;
            std::vector<int> picked;
            for (const std::size_t c : { ci, ci > 0 ? ci - 1 : ci })
            {
                chosen(voicesOf(x.p1, c), x.voice, picked);
                if (std::find(picked.begin(), picked.end(), slot) != picked.end())
                    return true;
            }
            return false;
        }

        // The voice a call dropped (sounding before it, gone after, not ending): its slot and key; else a note-on in it.
        void droppedIn(const Context& x, std::size_t ci, int& slot, int& note)
        {
            slot = -2;
            note = -1;
            if constexpr (!kInstrument)
            {
                slot = 0;
                return;
            }
            const std::span<const Voice> before = ci > 0 ? voicesOf(x.p1, ci - 1) : std::span<const Voice>();
            const std::span<const Voice> after = voicesOf(x.p1, ci);
            for (const Voice& v : before)
                if (!v.ending && std::none_of(after.begin(), after.end(), [&](const Voice& a) { return a.instance == v.instance && a.age == v.age; }))
                {
                    slot = v.instance;
                    note = v.key;
                    return;
                }
            const CallRec& r = x.p1.calls[ci];
            for (const auto& [at, e] : x.tl.events)
                if (e.kind == EventKind::noteOn && at >= r.at && at < r.at + static_cast<std::size_t>(r.length))
                {
                    note = e.note;
                    for (const Voice& v : after)
                        if (v.key == note)
                            slot = v.instance;
                    return;
                }
        }

        Found findNan(const Context& x)
        {
            Found f;
            std::size_t alertCall = kStart;
            for (std::size_t c = 0; c < x.p1.calls.size(); ++c)
                if ((x.p1.calls[c].alerts & Engine::nonFinite) != 0)
                {
                    int slot = 0, note = -1;
                    droppedIn(x, c, slot, note);
                    if (x.voice == kTraceAll || !kInstrument || slot < 0 || asked(x, slot, c))
                    {
                        alertCall = c;
                        break;
                    }
                }
            const Transition* t = nullptr;
            for (const Transition& b : x.p1.bad)
                if (b.call == kStart || asked(x, b.slot, b.call))
                {
                    t = &b;
                    break;
                }
            if (t != nullptr && t->call == kStart)
            {
                f.kind = TraceFind::nan;
                f.fromStart = true;
                f.slot = t->slot;
                f.field = t->field;
                return f;
            }
            if (t == nullptr && alertCall == kStart)
                return f;
            f.kind = TraceFind::nan;
            if (t != nullptr && (alertCall == kStart || t->call <= alertCall))
            {
                f.call = t->call;
                f.slot = t->slot;
                f.field = t->field;
                f.note = kInstrument ? keyOf(voicesOf(x.p1, t->call), t->slot) : -1;
                if (kInstrument && f.note < 0 && t->call > 0)
                    f.note = keyOf(voicesOf(x.p1, t->call - 1), t->slot);
            }
            else
            {
                f.call = alertCall;
                droppedIn(x, alertCall, f.slot, f.note);
            }
            f.at = x.p1.calls[f.call].at;
            return f;
        }

        Found findRunaway(const Context& x)
        {
            Found f;
            std::size_t clipCall = kStart;
            for (std::size_t c = 0; c < x.p1.calls.size() && clipCall == kStart; ++c)
                if ((x.p1.calls[c].alerts & Engine::clipped) != 0)
                    clipCall = c;
            const Transition* t = nullptr;
            for (const Transition& b : x.p1.big)
                if (b.call == kStart || asked(x, b.slot, b.call))
                {
                    t = &b;
                    break;
                }
            if (t == nullptr && clipCall == kStart)
                return f;
            f.kind = TraceFind::runaway;
            if (t != nullptr && t->call == kStart)
            {
                f.fromStart = true;
                f.slot = t->slot;
                f.field = t->field;
                return f;
            }
            if (t != nullptr && (clipCall == kStart || t->call <= clipCall))
            {
                f.call = t->call;
                f.slot = t->slot;
                f.field = t->field;
                f.note = kInstrument ? keyOf(voicesOf(x.p1, t->call), t->slot) : -1;
            }
            else
                f.call = clipCall;
            f.at = x.p1.calls[f.call].at;
            return f;
        }

        // Each 10 ms window's largest step; a click is one far larger than its neighbours' (five windows each side)
        // and a good part of the level there.
        std::vector<Found> findClicks(const Context& x, std::size_t most)
        {
            std::vector<Found> out;
            const std::vector<float>& l = x.sound.l;
            const std::vector<float>& r = x.sound.r;
            const std::size_t n = std::min(l.size(), r.size());
            const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * x.rate));
            std::vector<double> step, peak;
            std::vector<std::size_t> at;
            for (std::size_t a = 0; a < n; a += w)
            {
                double s = 0.0, p = 0.0;
                std::size_t where = a;
                for (std::size_t i = a; i < std::min(n, a + w); ++i)
                {
                    p = std::max({ p, static_cast<double>(std::fabs(l[i])), static_cast<double>(std::fabs(r[i])) });
                    if (i == 0)
                        continue;
                    const double d = std::max(std::fabs(static_cast<double>(l[i]) - l[i - 1]), std::fabs(static_cast<double>(r[i]) - r[i - 1]));
                    if (d > s)
                    {
                        s = d;
                        where = i;
                    }
                }
                step.push_back(s);
                peak.push_back(p);
                at.push_back(where);
            }
            std::vector<Found> all;
            for (std::size_t k = 0; k < step.size(); ++k)
            {
                std::vector<double> around;
                double local = 0.0;
                for (std::size_t j = k >= 5 ? k - 5 : 0; j < std::min(step.size(), k + 6); ++j)
                {
                    local = std::max(local, peak[j]);
                    if (j != k)
                        around.push_back(step[j]);
                }
                const double usual = median(around);
                if (step[k] > 0.05 && step[k] > 3.0 * usual && step[k] > 0.35 * local)
                {
                    Found f;
                    f.kind = TraceFind::click;
                    f.at = at[k];
                    f.size = step[k];
                    f.usual = usual;
                    all.push_back(f);
                }
            }
            std::stable_sort(all.begin(), all.end(), [](const Found& a, const Found& b) {
                return a.size / std::max(a.usual, 1e-9) > b.size / std::max(b.usual, 1e-9);
            });
            for (const Found& f : all)
                if (out.size() < most)
                    out.push_back(f);
            return out;
        }

        // The windows (10 ms) in which a key is held (an instrument's) or the input plays (an effect's).
        std::vector<char> heldWindows(const Context& x, std::size_t w, std::size_t count, int input, bool ownInput)
        {
            std::vector<char> held(count, 0);
            if constexpr (kInstrument)
            {
                std::array<int, 128> down{};
                int downCount = 0;
                std::size_t e = 0;
                for (std::size_t k = 0; k < count; ++k)
                {
                    const std::size_t end = (k + 1) * w;
                    bool any = downCount > 0;
                    for (; e < x.tl.events.size() && x.tl.events[e].first < end; ++e)
                    {
                        const Event& ev = x.tl.events[e].second;
                        if (ev.kind == EventKind::noteOn)
                        {
                            downCount += down[ev.note] == 0 ? 1 : 0;
                            ++down[ev.note];
                            any = true;
                        }
                        else if (ev.kind == EventKind::noteOff && down[ev.note] > 0)
                        {
                            down[ev.note] = 0;
                            --downCount;
                        }
                        else if (ev.kind == EventKind::allNotesOff)
                        {
                            down.fill(0);
                            downCount = 0;
                        }
                    }
                    held[k] = any && downCount > 0 ? 1 : 0;
                }
            }
            else
            {
                const auto in = static_cast<ViewInput>(input);
                if (ownInput || (in != ViewInput::silence && in != ViewInput::impulse))
                    for (std::size_t k = 0; k < count; ++k)
                        held[k] = (k + 1) * w <= x.tl.heldUntil ? 1 : 0;
            }
            return held;
        }

        Found findSilence(const Context& x, int input, bool ownInput)
        {
            Found f;
            const std::vector<float>& l = x.sound.l;
            const std::vector<float>& r = x.sound.r;
            const std::size_t n = std::min(l.size(), r.size());
            const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * x.rate));
            const std::size_t count = n / w;
            const std::vector<char> held = heldWindows(x, w, count, input, ownInput);
            std::vector<double> rms(count, 0.0);
            for (std::size_t k = 0; k < count; ++k)
            {
                double e = 0.0;
                for (std::size_t i = k * w; i < (k + 1) * w; ++i)
                    e += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
                rms[k] = std::sqrt(e / static_cast<double>(w));
            }
            bool anyHeld = false, anySound = false;
            for (std::size_t k = 0; k < count; ++k)
                if (held[k] != 0)
                {
                    anyHeld = true;
                    anySound = anySound || rms[k] > 1e-6;
                }
            if (anyHeld && !anySound)
            {
                f.kind = TraceFind::silence;
                f.never = true;
                for (std::size_t k = 0; k < count; ++k)
                    if (held[k] != 0)
                    {
                        f.at = k * w;
                        break;
                    }
                if constexpr (kInstrument)
                    f.at = std::max(f.at, x.tl.firstNote);
                return f;
            }
            for (std::size_t k = 1; k < count; ++k)
                if (held[k] != 0 && held[k - 1] != 0 && rms[k] < 1e-6 && rms[k - 1] > 1e-3)
                {
                    f.kind = TraceFind::silence;
                    std::size_t last = (k - 1) * w;
                    for (std::size_t i = (k - 1) * w; i < (k + 1) * w; ++i)
                        if (std::fabs(l[i]) > 1e-6f || std::fabs(r[i]) > 1e-6f)
                            last = i;
                    f.at = last + 1;
                    return f;
                }
            return f;
        }

        Found findDc(const Context& x)
        {
            Found f;
            const std::vector<float>& l = x.sound.l;
            const std::vector<float>& r = x.sound.r;
            const std::size_t n = std::min(l.size(), r.size());
            const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.05 * x.rate));
            int run = 0, sign = 0;
            std::size_t from = 0;
            double sum = 0.0;
            for (std::size_t a = 0; a + w <= n; a += w)
            {
                double m = 0.0, e = 0.0;
                for (std::size_t i = a; i < a + w; ++i)
                {
                    const double v = 0.5 * (static_cast<double>(l[i]) + r[i]);
                    m += v;
                    e += v * v;
                }
                m /= static_cast<double>(w);
                const double rms = std::sqrt(e / static_cast<double>(w));
                const int s = m > 0.0 ? 1 : -1;
                if (rms > 1e-4 && std::fabs(m) > std::max(0.01, 0.2 * rms) && (run == 0 || s == sign))
                {
                    if (run == 0)
                    {
                        from = a;
                        sum = 0.0;
                    }
                    sign = s;
                    sum += m;
                    if (++run >= 4)
                    {
                        f.kind = TraceFind::dc;
                        f.at = from;
                        f.dc = sum / run;
                        // the rest of the run
                        for (std::size_t b = a + w; b + w <= n; b += w)
                        {
                            double mb = 0.0;
                            for (std::size_t i = b; i < b + w; ++i)
                                mb += 0.5 * (static_cast<double>(l[i]) + r[i]);
                            mb /= static_cast<double>(w);
                            if ((mb > 0.0 ? 1 : -1) != sign || std::fabs(mb) < 0.01)
                                break;
                            sum += mb;
                            ++run;
                        }
                        f.dc = sum / run;
                        return f;
                    }
                }
                else
                    run = 0;
            }
            return f;
        }

        // ---- a problem traced in pass 2 ----------------------------------------------------------------------------
        struct Culprit : TraceCulprit
        {
            const Field* field = nullptr;
        };

        struct Traced
        {
            TraceProblem problem;
            std::vector<std::string> lines;
            bool differs = false;
            std::size_t firstDiff = 0;
            double maxDiff = 0.0;
            std::size_t w0 = 0, w1 = 0;
        };

        // The slots pass 2 keeps for a window: the problem's first, then those that sounded in it (an effect's one).
        std::vector<int> slotsFor(const Context& x, std::size_t w0, std::size_t w1, int first)
        {
            std::vector<int> out;
            if constexpr (!kInstrument)
                out.push_back(0);
            else
            {
                if (first >= 0)
                    out.push_back(first);
                for (std::size_t c = 0; c < x.p1.calls.size() && out.size() < kMaxTracked; ++c)
                {
                    const CallRec& r = x.p1.calls[c];
                    if (r.at + static_cast<std::size_t>(r.length) + kGrid < w0 || r.at > w1)
                        continue;
                    for (const Voice& v : voicesOf(x.p1, c))
                        if (std::find(out.begin(), out.end(), v.instance) == out.end() && out.size() < kMaxTracked)
                            out.push_back(v.instance);
                }
            }
            return out;
        }

        std::vector<Pass2::Pair> pairsFor(const Context& x, const std::vector<std::size_t>& scan, const std::vector<int>& slots)
        {
            std::vector<Pass2::Pair> pairs;
            for (const int s : slots)
                for (const std::size_t k : scan)
                    if (x.map[k].home == Field::Home::instance)
                        pairs.push_back({ s, k });
            for (const std::size_t k : scan)
                if (x.map[k].home == Field::Home::shared)
                    pairs.push_back({ -1, k });
            return pairs;
        }

        // The voices' own events in pass 2's window within kNear of `at`, and the request's (the timeline's), in words.
        std::vector<std::string> nearEvents(const Context& x, const Pass2& p, std::size_t at)
        {
            struct Ev
            {
                std::size_t at;
                std::string what;
            };
            std::vector<Ev> evs;
            const auto lo = at > static_cast<std::size_t>(kNear) ? at - static_cast<std::size_t>(kNear) : 0;
            const std::size_t hi = at + static_cast<std::size_t>(kNear);
            for (const Mark& m : x.tl.marks)
                if (m.at >= lo && m.at <= hi)
                    evs.push_back({ m.at, m.what });
            if constexpr (kInstrument)
                for (std::size_t s = std::max(lo, p.w0 + 1); s <= hi && p.has(s); ++s)
                {
                    const std::vector<Voice>& before = p.voiceAt[s - 1 - p.w0];
                    const std::vector<Voice>& now = p.voiceAt[s - p.w0];
                    for (const Voice& v : before)
                    {
                        const auto it = std::find_if(now.begin(), now.end(), [&](const Voice& a) { return a.instance == v.instance && a.age == v.age; });
                        if (it == now.end())
                            evs.push_back({ s, (p.alerts[s - p.w0] & Engine::nonFinite) != 0 && !v.ending
                                                   ? "the Engine drops the voice of note " + std::to_string(v.key) + ", not a number"
                                               : v.fading ? "the stolen voice of note " + std::to_string(v.key) + " is gone"
                                                          : "the voice of note " + std::to_string(v.key) + " ends, its process returned false" });
                        else if (it->fading && !v.fading)
                            evs.push_back({ s, "the voice of note " + std::to_string(v.key) + " is stolen, a 2 ms fade" });
                    }
                }
            std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
            std::vector<std::string> out;
            for (const Ev& e : evs)
            {
                const std::string d = e.at == at ? "at it" : apart(e.at > at ? e.at - at : at - e.at, x.rate) + (e.at > at ? " after" : " before");
                out.push_back(e.what + " (" + d + ")");
                if (out.size() >= 4)
                    break;
            }
            return out;
        }

        Traced tracePass2(const Context& x, Session& session, const ViewRequest& vr, const Found& f, const std::vector<std::size_t>& scan,
                          const Writers& writers, bool detail, Clock& clock)
        {
            Traced out;
            TraceProblem& pr = out.problem;
            pr.kind = f.kind;
            pr.at = f.at;
            pr.note = f.note;
            pr.fromStart = f.fromStart;
            const std::size_t length = x.sound.l.size();
            const std::size_t most = detail ? 6 : 3, show = detail ? 8 : 4;

            std::size_t w1 = 0;
            if (f.kind == TraceFind::nan || f.kind == TraceFind::runaway)
                w1 = f.call < x.p1.calls.size() ? x.p1.calls[f.call].at + static_cast<std::size_t>(x.p1.calls[f.call].length) : f.at + kGrid;
            else
                w1 = f.at + 1;
            w1 = std::min(length, w1 + kAfter);
            std::size_t w0 = w1 > kTraceWindow ? w1 - kTraceWindow : 0;
            const std::vector<int> slots = slotsFor(x, w0, w1, f.slot >= 0 ? f.slot : -2);
            std::vector<Pass2::Pair> pairs = pairsFor(x, scan, slots);
            if (!pairs.empty() && (w1 - w0) * pairs.size() > kMaxValues)
                w0 = w1 - std::max<std::size_t>(64, kMaxValues / pairs.size());
            if (f.fromStart || w1 <= w0)
            {
                // Bad from the start: no window to look in, only the field pass 1 saw.
                if (f.field < x.map.size())
                {
                    TraceCulprit c;
                    c.path = x.map[f.field].path;
                    c.writes = writers.of(x.map[f.field].module, x.map[f.field].name, most);
                    pr.culprits.push_back(c);
                    pr.verdict = c.path + (f.kind == TraceFind::nan ? " is not a number from the start" : " is past 1e6 from the start")
                               + ": its module's init, or a default, makes it so"
                               + (c.writes.empty() ? std::string(".") : " (it is written at " + joined(c.writes) + ").");
                }
                out.lines.push_back(std::string(kindWords(f.kind)) + " from the start (before the first call), in "
                                    + (f.slot >= 0 && kInstrument ? "the voices' instances" : std::string("the program's state")) + ".");
                if (!pr.culprits.empty())
                    out.lines.push_back("  " + pr.culprits[0].path + ": " + num(x.patch.read(x.map[f.field], f.slot)) + " already before the first call.");
                out.lines.push_back("  Verdict: " + pr.verdict);
                return out;
            }
            Pass2 p(x.patch, pairs, w0, w1, x.sound.l, x.sound.r, clock);
            p.checkUntil = x.nanCall;
            std::vector<Culprit> cs;
            out.w0 = w0;
            out.w1 = w1;
            renderSound(session, vr, &p);
            out.differs = p.differs;
            out.firstDiff = p.firstDiff;
            out.maxDiff = p.maxDiff;

            // The exact sample, and each pair's first bad (or big) sample in the window (kStart: none in it, or so from
            // its start already).
            const std::size_t T = p.pairs.size();
            const auto badAt = [&](std::size_t t, bool big) {
                for (std::size_t s = p.w0; p.has(s); ++s)
                {
                    const double v = p.value(s, t);
                    if (big ? (finite(v) && std::fabs(v) > kBig && x.map[p.pairs[t].field].type == Field::Type::real) : !finite(v))
                        return s == p.w0 ? kStart : s;
                }
                return kStart;
            };
            std::size_t S = f.at;
            std::vector<std::pair<std::size_t, std::size_t>> firsts;   // (sample, pair)
            std::vector<std::size_t> firstsAt;
            if (f.kind == TraceFind::nan || f.kind == TraceFind::runaway)
            {
                const bool big = f.kind == TraceFind::runaway;
                for (std::size_t t = 0; t < T; ++t)
                {
                    if (const std::size_t s = badAt(t, big); s != kStart)
                        firsts.emplace_back(s, t);
                }
                std::stable_sort(firsts.begin(), firsts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                const uint32_t flag = big ? Engine::clipped : Engine::nonFinite;
                std::size_t alertAt = kStart;
                for (std::size_t s = p.w0; p.has(s); ++s)
                    if ((p.alerts[s - p.w0] & flag) != 0)
                    {
                        alertAt = s;
                        break;
                    }
                S = std::min(firsts.empty() ? kStart : firsts.front().first, alertAt);
                for (const auto& q : firsts)
                    firstsAt.push_back(q.first);
                if (S == kStart)
                    S = f.at;
                // The voice of the first to go bad: as it was then, or the sample before (a voice dropped then).
                if (kInstrument && !firsts.empty() && p.pairs[firsts.front().second].slot >= 0)
                {
                    const std::size_t s = firsts.front().first;
                    const int slot = p.pairs[firsts.front().second].slot;
                    int key = p.has(s) ? keyOf(p.voiceAt[s - p.w0], slot) : -1;
                    if (key < 0 && s > p.w0)
                        key = keyOf(p.voiceAt[s - 1 - p.w0], slot);
                    if (key >= 0)
                        pr.note = key;
                }
            }
            pr.at = S;

            const auto values = [&](std::size_t t, TraceCulprit& c) {
                for (std::size_t s = c.at > p.w0 + show ? c.at - show : p.w0; s < c.at; ++s)
                    if (p.has(s))
                        c.before.push_back(p.value(s, t));
                for (std::size_t s = c.at; s < c.at + show && p.has(s); ++s)
                    c.after.push_back(p.value(s, t));
            };
            const auto culprit = [&](std::size_t t, std::size_t at) {
                Culprit c;
                const Field& fd = x.map[p.pairs[t].field];
                c.field = &fd;
                c.path = fd.path;
                if (kInstrument && p.pairs[t].slot >= 0 && pr.note >= 0 && p.has(at))
                {
                    const int key = keyOf(p.voiceAt[at - p.w0], p.pairs[t].slot);
                    if (key >= 0 && key != pr.note)
                        c.path += " (in the voice of note " + std::to_string(key) + ")";
                }
                c.at = at;
                values(t, c);
                c.writes = writers.of(fd.module, fd.name, most);
                return c;
            };

            if (f.kind == TraceFind::nan || f.kind == TraceFind::runaway)
            {
                // The first to go bad: those of the problem's voice and Shared's first, at most `most`.
                int voiceSlot = -2;
                if (!firsts.empty())
                    voiceSlot = p.pairs[firsts.front().second].slot;
                else if (f.slot >= -1)
                    voiceSlot = f.slot;
                for (const auto& [s, t] : firsts)
                    if (cs.size() < most && (p.pairs[t].slot == voiceSlot || p.pairs[t].slot < 0 || voiceSlot == -2))
                        cs.push_back(culprit(t, s));
                // What came before it: the culprit's module's other fields one sample before (a held module's), or a
                // field of that voice far from its usual size then (within 1e6, and not gone bad itself).
                std::string precursor;
                const auto sounding = [&](std::size_t at, int slot) {
                    if (!kInstrument || slot < 0)
                        return true;
                    const std::vector<Voice>& vs = p.voiceAt[at - p.w0];
                    return std::any_of(vs.begin(), vs.end(), [&](const Voice& v) { return v.instance == slot; });
                };
                const auto wentBad = [&](std::size_t t) {
                    return std::any_of(firsts.begin(), firsts.end(), [&](const auto& q) { return q.second == t; });
                };
                if (!firsts.empty() && S > p.w0 && p.has(S - 1))
                {
                    const std::size_t t0 = firsts.front().second;
                    const std::string& path = x.map[p.pairs[t0].field].path;
                    const std::size_t cut = std::max(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1,
                                                     path.rfind("->") == std::string::npos ? 0 : path.rfind("->") + 2);
                    if (cut > 0)
                    {
                        const std::string prefix = path.substr(0, cut);
                        std::vector<std::string> fields;
                        for (const bool reals : { true, false })
                            for (std::size_t t = 0; t < T && fields.size() < 4; ++t)
                            {
                                const Field& fd = x.map[p.pairs[t].field];
                                if (t == t0 || p.pairs[t].slot != p.pairs[t0].slot || fd.path.compare(0, prefix.size(), prefix) != 0
                                    || fd.path.find_first_of(".[", prefix.size()) != std::string::npos
                                    || (fd.type == Field::Type::real) != reals || !finite(p.value(S - 1, t)))
                                    continue;
                                fields.push_back(fd.path.substr(prefix.size()) + " " + val(fd, p.value(S - 1, t)));
                            }
                        if (!fields.empty())
                            precursor = "its module's fields were " + joined(fields);
                    }
                }
                if (precursor.empty() && S > p.w0 + 8 && p.has(S - 1))
                {
                    double best = 10.0;
                    for (std::size_t t = 0; t < T; ++t)
                    {
                        if ((p.pairs[t].slot != voiceSlot && p.pairs[t].slot >= 0) || x.map[p.pairs[t].field].type != Field::Type::real || wentBad(t))
                            continue;
                        const double v = p.value(S - 1, t);
                        if (!finite(v) || std::fabs(v) < 1e-3 || std::fabs(v) > kBig)
                            continue;
                        std::vector<double> mags;
                        for (std::size_t at = p.w0; at + 1 < S; ++at)
                            if (finite(p.value(at, t)) && sounding(at, p.pairs[t].slot))
                                mags.push_back(std::fabs(p.value(at, t)));
                        if (mags.size() < 8)
                            continue;
                        const double usual = median(mags);
                        if (const double ratio = std::fabs(v) / (usual + 1e-12); ratio > best)
                        {
                            best = ratio;
                            precursor = x.map[p.pairs[t].field].path + " was " + num(v) + " (usually about " + num(usual) + ")";
                        }
                    }
                }
                if (f.kind == TraceFind::nan)
                {
                    if (cs.empty())
                        pr.verdict = "the output went non-finite, but no field went bad: the NaN lives in a local (a value computed "
                                     "and used within one call: a division by zero, sqrtf or logf of a negative number, an "
                                     "exponent too large). Watching expressions comes in a later wave; until then guard the "
                                     "suspect division or call (try_edits tests a guard)"
                                   + (precursor.empty() ? std::string(".") : "; one sample before, " + precursor + ".");
                    else
                        pr.verdict = cs[0].path + " is the first value to go bad"
                                   + (precursor.empty() ? std::string() : "; one sample before, " + precursor)
                                   + (cs[0].writes.empty() ? std::string(".") : ": look at what it is computed from at "
                                                                                           + joined(cs[0].writes) + ".");
                }
                else if (cs.empty())
                    pr.verdict = "the output passes +-4 (+12 dBFS, the guard clips it) and no field passes 1e6: a gain too "
                                 "large, rather than a state that grows.";
                else
                    pr.verdict = cs[0].path + " passes 1e6 first: a feedback whose gain is 1 or more, or a filter "
                                 "outside its stable range (a cutoff at or above the Nyquist frequency, a resonance past its "
                                 "limit)" + (cs[0].writes.empty() ? std::string(".") : "; it is written at "
                                                                                                  + joined(cs[0].writes) + ".");
            }
            else
            {
                // Steps: those at least ten times the field's usual step there; floats first.
                struct Cand
                {
                    std::size_t t;
                    double step, usual, score;
                    bool real;
                };
                std::vector<Cand> cands;
                if (S > p.w0 && p.has(S))
                    for (std::size_t t = 0; t < T; ++t)
                    {
                        const double a = p.value(S - 1, t), b = p.value(S, t);
                        if (!finite(a) || !finite(b) || a == b)
                            continue;
                        std::vector<double> steps;
                        double top = 0.0;
                        for (std::size_t s = p.w0 + 1; s + 1 < S; ++s)
                        {
                            const double u = p.value(s, t), v = p.value(s - 1, t);
                            if (finite(u) && finite(v))
                            {
                                steps.push_back(std::fabs(u - v));
                                top = std::max(top, std::fabs(u));
                            }
                        }
                        const double step = std::fabs(b - a), usual = median(steps);
                        if (step < 10.0 * usual)
                            continue;
                        const bool real = x.map[p.pairs[t].field].type == Field::Type::real;
                        cands.push_back({ t, step, usual, step / std::max({ usual, 1e-6 * std::max(top, std::fabs(b)), 1e-12 }), real });
                    }
                std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                    return a.real != b.real ? a.real : a.score > b.score;
                });
                // The voice is the strongest culprit's; its fields and Shared's then.
                int voiceSlot = cands.empty() ? -2 : p.pairs[cands.front().t].slot;
                if (kInstrument && voiceSlot >= 0 && pr.note < 0 && p.has(S))
                    pr.note = keyOf(p.voiceAt[S - p.w0], voiceSlot);
                if (kInstrument && voiceSlot >= 0 && pr.note < 0 && S > p.w0)
                    pr.note = keyOf(p.voiceAt[S - 1 - p.w0], voiceSlot);
                for (const Cand& c : cands)
                    if (cs.size() < most && (p.pairs[c.t].slot == voiceSlot || p.pairs[c.t].slot < 0))
                    {
                        Culprit k = culprit(c.t, S);
                        k.step = c.step;
                        k.usual = c.usual;
                        cs.push_back(std::move(k));
                    }
                const std::vector<std::string> near = nearEvents(x, p, S);
                const bool ended = std::any_of(near.begin(), near.end(), [](const std::string& e) { return e.find("ends, its process") != std::string::npos; });
                if (f.kind == TraceFind::click && ended && (cs.empty() || !cs[0].field || cs[0].field->type != Field::Type::real))
                    pr.verdict = "a voice stops at once there: its process returned false while it still sounded; let it fade "
                                 "to silence (a release) before it returns false.";
                else if (f.kind == TraceFind::click)
                    pr.verdict = cs.empty()
                                     ? "no field jumps with it: the step comes from the output's own path (a signal switched "
                                       "at once, a sum that changes in one sample, a buffer read past its end)."
                                     : cs[0].path + " jumps " + num(cs[0].step) + " where it usually moves "
                                           + num(cs[0].usual) + " a sample"
                                           + (cs[0].writes.empty() ? std::string() : " (" + joined(cs[0].writes) + ")")
                                           + ": a phase or a level that jumps clicks; keep it continuous, or ramp it over a few ms.";
                else if (f.kind == TraceFind::silence)
                    pr.verdict = ended ? "the voice ends while its key is held: its process returned false (it said it was silent for good)."
                               : cs.empty() ? "no field steps with it: what feeds the output stops (a gain, a gate, a "
                                                       "buffer, a condition that holds)."
                                                     : cs[0].path + " steps to " + num(cs[0].after.empty() ? 0.0 : cs[0].after[0])
                                                           + " there" + (cs[0].writes.empty() ? std::string() : " (" + joined(cs[0].writes) + ")") + ".";
                else
                    pr.verdict = "a mean offset of " + num(f.dc) + " (" + fmt("%.1f dBFS", 20.0 * std::log10(std::max(std::fabs(f.dc), 1e-12)))
                               + "): a DC blocker removes it (a one-pole high-pass at about 10 Hz, onepole.fm), or find what "
                                 "adds it (a shaper that is not symmetric, a constant added)"
                               + (cs.empty() ? std::string(".") : "; it starts as " + cs[0].path + " steps (" + joined(cs[0].writes) + ").");
                pr.near = near;
            }
            if (pr.near.empty())
                pr.near = nearEvents(x, p, S);

            // The words.
            std::string head = std::string(kindWords(f.kind)) + " at " + when(S, x.rate);
            if (kInstrument && pr.note >= 0)
            {
                head += ", in " + voiceText(pr.note);
                const Mark* on = nullptr;
                for (const Mark& m : x.tl.marks)
                    if (m.on && m.note == pr.note && m.at <= S)
                        on = &m;
                if (on != nullptr)
                    head += ", " + (on->at == S ? std::string("at its note-on") : apart(S - on->at, x.rate) + " after its note-on");
            }
            if (f.kind == TraceFind::runaway)
                head += !firstsAt.empty() && firstsAt.front() <= S ? std::string(" (a field passes 1e6)") : " (the output passes +-4, which the guard clips)";
            if (f.kind == TraceFind::click)
                head += fmt(": a step of %.3f", f.size) + fmt(" (the steps around it: %.3f)", f.usual);
            if (f.kind == TraceFind::silence && f.never)
                head = std::string("Silence: nothing sounds") + (kInstrument ? " while the notes are held" : " while the input plays")
                     + " (from " + when(S, x.rate) + ")";
            head += pr.near.empty() ? "; " + nearestMark(x.tl, S, x.rate) + "." : "; within 32 samples: " + joined(pr.near, "; ") + ".";
            out.lines.push_back(head);
            for (const Culprit& c : cs)
            {
                std::string line = "  " + c.path + ": ";
                if (f.kind == TraceFind::nan)
                    line += "non-finite from sample " + grouped(c.at);
                else if (f.kind == TraceFind::runaway)
                    line += "past 1e6 from sample " + grouped(c.at);
                else
                    line += "steps " + val(*c.field, c.step) + " (usually " + val(*c.field, c.usual) + ")";
                std::vector<std::string> b, a;
                for (const double v : c.before)
                    b.push_back(val(*c.field, v));
                for (const double v : c.after)
                    a.push_back(val(*c.field, v));
                line += "; " + joined(b, " ") + " | " + joined(a, " ");
                line += c.writes.empty() ? std::string("; no line found that writes it") : "; written at " + joined(c.writes);
                out.lines.push_back(line);
            }
            if (f.kind == TraceFind::silence && f.never)
            {
                std::vector<std::string> still;
                for (std::size_t k = 0; k < scan.size() && still.size() < 4; ++k)
                    if (x.p1.moved[k] == 0 && x.map[scan[k]].home == Field::Home::instance)
                        still.push_back(x.map[scan[k]].path);
                pr.verdict = still.empty() ? "every field moves, but nothing reaches the output: check what process writes into left and right."
                                           : "fields that never left 0: " + joined(still) + " (what sets them is not reached, or "
                                                                                           "sets another field).";
            }
            out.lines.push_back("  Verdict: " + pr.verdict);
            pr.culprits.assign(cs.begin(), cs.end());
            return out;
        }

        // ---- watch ------------------------------------------------------------------------------------------------------
        std::string sparkline(const Pass1& p, std::size_t w, std::size_t watchCount)
        {
            static constexpr const char* kBars[8] = { "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                                      "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };
            const std::size_t calls = p.calls.size();
            if (calls == 0)
                return {};
            constexpr std::size_t kParts = 32;
            std::vector<double> mean(kParts, 0.0);
            std::vector<int> count(kParts, 0);
            std::vector<char> bad(kParts, 0);
            for (std::size_t c = 0; c < calls; ++c)
            {
                const std::size_t k = std::min(kParts - 1, c * kParts / calls);
                const Pass1::Watch& v = p.watched[c * watchCount + w];
                bad[k] = static_cast<char>(bad[k] != 0 || v.bad);
                if (v.count > 0)
                {
                    mean[k] += v.v;
                    ++count[k];
                }
            }
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (std::size_t k = 0; k < kParts; ++k)
                if (count[k] > 0)
                {
                    mean[k] /= count[k];
                    lo = std::min(lo, mean[k]);
                    hi = std::max(hi, mean[k]);
                }
            std::string out;
            for (std::size_t k = 0; k < kParts; ++k)
            {
                if (bad[k] != 0)
                    out += "!";
                else if (count[k] == 0)
                    out += "\xc2\xb7";
                else
                    out += kBars[hi > lo ? std::clamp(static_cast<int>((mean[k] - lo) / (hi - lo) * 7.0 + 0.5), 0, 7) : 0];
            }
            return out;
        }

        std::vector<std::string> watchLines(const Context& x, const std::vector<std::size_t>& watch, const Pass1& p)
        {
            std::vector<std::string> out;
            const std::size_t W = watch.size();
            for (std::size_t w = 0; w < W; ++w)
            {
                double lo = 0.0, hi = 0.0, sum = 0.0, first = 0.0, last = 0.0;
                std::size_t n = 0;
                long badFrom = -1;
                for (std::size_t c = 0; c < p.calls.size(); ++c)
                {
                    const Pass1::Watch& v = p.watched[c * W + w];
                    if (v.bad && badFrom < 0)
                        badFrom = static_cast<long>(p.calls[c].at);
                    if (v.count == 0)
                        continue;
                    lo = n == 0 ? v.lo : std::min(lo, v.lo);
                    hi = n == 0 ? v.hi : std::max(hi, v.hi);
                    if (n == 0)
                        first = v.v;
                    last = v.v;
                    sum += v.v;
                    ++n;
                }
                const Field& f = x.map[watch[w]];
                std::string line = f.path + ": ";
                if (n == 0 && badFrom < 0)
                {
                    line += kInstrument ? "no value (no voice of that choice sounded)" : "no value";
                    out.push_back(line);
                    continue;
                }
                line += "min " + val(f, lo) + ", max " + val(f, hi) + ", mean " + num(n > 0 ? sum / static_cast<double>(n) : 0.0) + ", first "
                      + val(f, first) + ", last " + val(f, last);
                line += badFrom < 0 ? std::string("; never non-finite") : "; non-finite from " + seconds(static_cast<double>(badFrom) / x.rate);
                if (p.jumpSize[w] > 0.0)
                    line += "; largest jump " + num(p.jumpSize[w]) + " at " + seconds(static_cast<double>(p.jumpAt[w]) / x.rate);
                line += "; " + sparkline(p, w, W);
                out.push_back(line);
            }
            return out;
        }

        // ---- list -------------------------------------------------------------------------------------------------------
        const char* typeWord(Field::Type t) { return t == Field::Type::real ? "float" : t == Field::Type::integer ? "int" : "bool"; }

        std::vector<std::string> listLines(const Patch& patch)
        {
            const std::vector<Field>& map = patch.fieldMap();
            std::vector<std::string> out;
            // An array's elements as one: "osc[0..7].phase float".
            std::size_t shown = 0;
            for (std::size_t i = 0; i < map.size();)
            {
                const auto pattern = [](const std::string& p, std::vector<int>& idx) {
                    std::string s;
                    idx.clear();
                    for (std::size_t k = 0; k < p.size(); ++k)
                    {
                        if (p[k] == '[')
                        {
                            const std::size_t close = p.find(']', k);
                            idx.push_back(std::atoi(p.c_str() + k + 1));
                            s += "[]";
                            k = close == std::string::npos ? p.size() : close;
                            continue;
                        }
                        s += p[k];
                    }
                    return s;
                };
                std::vector<int> idx, next;
                const std::string pat = pattern(map[i].path, idx);
                std::size_t j = i + 1;
                int lo = idx.empty() ? 0 : idx.back(), hi = lo;
                while (j < map.size() && !idx.empty() && map[j].type == map[i].type && map[j].home == map[i].home
                       && pattern(map[j].path, next) == pat && next.size() == idx.size()
                       && std::equal(next.begin(), next.end() - 1, idx.begin()) && next.back() == hi + 1)
                {
                    hi = next.back();
                    ++j;
                }
                std::string path = map[i].path;
                if (j > i + 1)
                {
                    // The last index's range in place of its number.
                    const std::size_t at = path.rfind('[');
                    const std::size_t close = path.find(']', at);
                    path = path.substr(0, at) + "[" + std::to_string(lo) + ".." + std::to_string(hi) + "]" + path.substr(close + 1);
                }
                if (shown < kMaxListed)
                    out.push_back(path + " " + typeWord(map[i].type));
                ++shown;
                i = j;
            }
            if (shown > kMaxListed)
                out.push_back("and " + std::to_string(shown - kMaxListed) + " more (a glob such as filter.* watches a part)");
            const std::vector<std::string>& left = patch.unmapped();
            for (std::size_t k = 0; k < left.size() && k < 3; ++k)
                out.push_back("left out: " + left[k]);
            if (left.size() > 3)
                out.push_back("left out: " + std::to_string(left.size() - 3) + " more");
            return out;
        }
    }

    bool tracePathMatches(const std::string& pattern, const std::string& path)
    {
        // A module's path names its fields.
        if (pattern.find_first_of("*?") == std::string::npos)
            return path == pattern || (path.size() > pattern.size() && path.compare(0, pattern.size(), pattern) == 0
                                       && (path[pattern.size()] == '.' || path[pattern.size()] == '[' || path[pattern.size()] == '-'));
        // The glob, by backtracking on the last '*'.
        std::size_t p = 0, s = 0, star = std::string::npos, mark = 0;
        while (s < path.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == path[s]))
            {
                ++p;
                ++s;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = s;
            }
            else if (star != std::string::npos)
            {
                p = star + 1;
                s = ++mark;
            }
            else
                return false;
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
    }

    TraceResult trace(const Sources& files, const TraceRequest& q)
    {
        TraceResult out;
        Session session(files, q.render.rate);
        if (!session.compiled())
        {
            out.error = "It did not compile, so nothing was traced:\n" + session.errors();
            return out;
        }
        const Patch* patch = session.patch();
        if (patch == nullptr)
        {
            out.error = "There is no program to trace.";
            return out;
        }
        const std::vector<Field>& map = patch->fieldMap();
        const double rate = session.rate();
        if (q.list)
        {
            out.lines.push_back("The program's fields (" + std::to_string(map.size()) + "; trace's fields takes these paths, or globs "
                                "such as filter.*):");
            for (std::string& l : listLines(*patch))
                out.lines.push_back(std::move(l));
            return out;
        }

        // What to read: pass 1 scans every field (but Host's, which the program only reads), and watches those named.
        std::vector<std::size_t> scan, watch;
        for (std::size_t k = 0; k < map.size() && scan.size() < kMaxScanned; ++k)
            if (map[k].home != Field::Home::host)
                scan.push_back(k);
        std::vector<std::string> unmatched;
        std::size_t more = 0;
        for (std::size_t i = 0; i < q.fields.size() && i < kMaxTracePatterns; ++i)
        {
            bool any = false;
            for (std::size_t k = 0; k < map.size(); ++k)
                if (tracePathMatches(q.fields[i], map[k].path))
                {
                    any = true;
                    if (std::find(watch.begin(), watch.end(), k) != watch.end())
                        continue;
                    if (watch.size() < kMaxTraceWatched)
                        watch.push_back(k);
                    else
                        ++more;
                }
            if (!any)
                unmatched.push_back(q.fields[i]);
        }
        const TraceFind find = q.find == TraceFind::none && q.fields.empty() ? TraceFind::any : q.find;
        for (const std::size_t k : watch)
            out.watched.push_back(map[k].path);

        Clock clock;
        clock.limit = q.seconds > 0.0 ? q.seconds : kTraceSeconds;
        clock.keepGoing = &q.keepGoing;
        ViewRequest vr;
        vr.render = q.render;
        vr.input = static_cast<ViewInput>(std::clamp(q.input, 0, static_cast<int>(ViewInput::silence)));
        const int watchVoice = q.voiceGiven ? q.voice : kTraceNewest;
        Pass1 p1(*patch, scan, watch, watchVoice, clock);
        const Sound sound = renderSound(session, vr, &p1);
        out.stopped = clock.stopped;
        const Timeline tl = session.timeline(vr.render, sound.l.size());
        std::size_t nanCall = static_cast<std::size_t>(-1);
        for (const CallRec& c : p1.calls)
            if ((c.alerts & Engine::nonFinite) != 0)
            {
                nanCall = c.at;
                break;
            }
        const Context x{ *patch, map, p1, sound, tl, rate, q.voiceGiven ? q.voice : kTraceAll, nanCall };
        out.lines.push_back("Rendered: " + sound.title + (sound.dials.empty() || sound.dials == "none" ? std::string() : "; dials " + sound.dials)
                            + "; in calls of 16 samples.");
        if (clock.stopped)
            out.lines.push_back("(Stopped after " + seconds(static_cast<double>(p1.calls.empty() ? 0 : p1.calls.back().at) / rate)
                                + " of it: " + (q.keepGoing && !q.keepGoing() ? "cancelled" : "the time limit") + ".)");

        Writers writers(files);
        std::vector<Traced> traced;
        if (find != TraceFind::none)
        {
            std::vector<Found> found;
            const bool any = find == TraceFind::any;
            if (any || find == TraceFind::nan)
                if (Found f = findNan(x); f.kind != TraceFind::none)
                    found.push_back(f);
            if (any || find == TraceFind::runaway)
                if (Found f = findRunaway(x); f.kind != TraceFind::none)
                    found.push_back(f);
            // A NaN or a runaway explains the steps and the silence that follow it (the guard zeroes, the voice goes).
            std::size_t explained = static_cast<std::size_t>(-1);
            for (const Found& f : found)
                explained = std::min(explained, f.at);
            const auto unexplained = [&](const Found& f) { return !any || explained == static_cast<std::size_t>(-1) || f.at + 1 < explained; };
            if (any || find == TraceFind::click)
                for (const Found& f : findClicks(x, any ? 1 : kMaxProblems))
                    if (unexplained(f))
                        found.push_back(f);
            if (any || find == TraceFind::silence)
                if (Found f = findSilence(x, vr.render.signal >= 0 ? vr.render.signal : q.input, q.render.input != nullptr);
                    f.kind != TraceFind::none && unexplained(f))
                    found.push_back(f);
            if (any || find == TraceFind::dc)
                if (Found f = findDc(x); f.kind != TraceFind::none && unexplained(f))
                    found.push_back(f);
            std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.at < b.at; });
            if (found.size() > kMaxProblems)
                found.resize(kMaxProblems);
            for (const Found& f : found)
            {
                if (clock.stopped)
                    break;
                traced.push_back(tracePass2(x, session, vr, f, scan, writers, q.detail, clock));
            }
            for (Traced& t : traced)
            {
                for (std::string& l : t.lines)
                    out.lines.push_back(std::move(l));
                out.problems.push_back(std::move(t.problem));
            }
            if (traced.empty())
            {
                // Nothing: what was checked, and one window in calls of 1 sample for the self-check.
                std::vector<std::string> checked;
                std::size_t voices = 0;
                {
                    std::set<int> seen;
                    for (const Voice& v : p1.voices)
                        seen.insert(v.instance);
                    voices = kInstrument ? seen.size() : 1;
                }
                if (any || find == TraceFind::nan)
                    checked.push_back("not a number (the output guard, and " + std::to_string(scan.size()) + (scan.size() == 1 ? " field" : " fields")
                                      + (kInstrument ? " of " + std::to_string(voices) + (voices == 1 ? " voice" : " voices") : std::string())
                                      + " after each call)");
                if (any || find == TraceFind::runaway)
                    checked.push_back("a runaway (the guard's clip, a field past 1e6)");
                if (any || find == TraceFind::click)
                    checked.push_back("clicks (no step stands out from those around it)");
                if (any || find == TraceFind::silence)
                    checked.push_back(kInstrument ? "silence while a key is held" : "silence while the input plays");
                if (any || find == TraceFind::dc)
                    checked.push_back("a DC offset held 200 ms");
                out.lines.push_back("Nothing found; checked: " + joined(checked, "; ") + ", over " + seconds(static_cast<double>(sound.l.size()) / rate) + ".");
            }
            // The self-check looks at the start too (a voice's first samples, an effect's), unless a problem's window did.
            const std::size_t start = std::min(kInstrument ? tl.firstNote : std::size_t{ 0 }, sound.l.empty() ? 0 : sound.l.size() - 1);
            const bool covered = std::any_of(traced.begin(), traced.end(), [&](const Traced& t) { return t.w0 <= start && start < t.w1; });
            if (!covered && !clock.stopped && !sound.l.empty() && start < nanCall)
            {
                const std::size_t w1 = std::min(sound.l.size(), start + kTraceWindow);
                Pass2 p(*patch, {}, start, w1, sound.l, sound.r, clock);
                p.checkUntil = nanCall;
                renderSound(session, vr, &p);
                Traced t;
                t.differs = p.differs;
                t.firstDiff = p.firstDiff;
                t.maxDiff = p.maxDiff;
                t.w0 = start;
                t.w1 = w1;
                traced.push_back(std::move(t));
            }
        }

        // The self-check.
        if (!traced.empty())
        {
            const Traced* first = nullptr;
            for (const Traced& t : traced)
                if (t.differs && (first == nullptr || t.firstDiff < first->firstDiff))
                    first = &t;
            if (first != nullptr)
            {
                out.splitDiffers = true;
                out.splitAt = first->firstDiff;
                double most = 0.0;
                for (const Traced& t : traced)
                    most = std::max(most, t.maxDiff);
                out.lines.push_back("Self-check: in calls of 1 sample (from " + seconds(static_cast<double>(first->w0) / rate)
                                    + ") the output differs from calls of 16 from " + when(first->firstDiff, rate) + " (by up to "
                                    + num(most) + "): the program's output depends on how its calls are split, so it keeps "
                                    "some state per call, not per sample (a host may split its blocks anywhere): a finding "
                                    "of its own.");
            }
            else
            {
                std::size_t lo = traced.front().w0, hi = traced.front().w1;
                for (const Traced& t : traced)
                {
                    lo = std::min(lo, t.w0);
                    hi = std::max(hi, t.w1);
                }
                if (hi > lo)
                    out.lines.push_back("Self-check: a second pass in calls of 1 sample (from " + seconds(static_cast<double>(lo) / rate)
                                        + ") gave the same bits as calls of 16.");
            }
        }

        // The watch.
        if (!watch.empty() || !unmatched.empty())
        {
            if (!watch.empty())
            {
                const std::string whose = !kInstrument ? std::string()
                                        : watchVoice == kTraceAll ? " (every voice pooled)"
                                        : watchVoice >= 0 ? " (" + voiceText(watchVoice) + ")"
                                                          : " (the newest voice that sounds)";
                out.lines.push_back("Fields after each call" + whose + ":");
                for (std::string& l : watchLines(x, watch, p1))
                    out.lines.push_back(std::move(l));
            }
            if (more > 0)
                out.lines.push_back("(and " + std::to_string(more) + " more matched: name them more narrowly)");
            for (const std::string& u : unmatched)
                out.lines.push_back("No field matches " + u + " (list: true names them).");
        }

        // Raw values.
        const std::size_t points = static_cast<std::size_t>(std::clamp(q.points, 0, static_cast<int>(kMaxTracePoints)));
        if (points > 0 && !watch.empty() && !clock.stopped)
        {
            const std::size_t W = watch.size();
            if (q.around >= 0.0)
            {
                const auto centre = static_cast<std::size_t>(std::llround(q.around * rate));
                const std::size_t w0 = std::min(sound.l.size(), centre > points / 2 ? centre - points / 2 : 0);
                const std::size_t w1 = std::min(sound.l.size(), w0 + points);
                if (w1 > w0)
                {
                    const std::vector<int> slots = slotsFor(x, w0, w1, -2);
                    std::vector<Pass2::Pair> pairs;
                    for (const int s : slots)
                        for (const std::size_t k : watch)
                            if (map[k].home == Field::Home::instance)
                                pairs.push_back({ s, k });
                    for (const std::size_t k : watch)
                        if (map[k].home != Field::Home::instance)
                            pairs.push_back({ -1, k });
                    Pass2 p(*patch, pairs, w0, w1, sound.l, sound.r, clock);
                    renderSound(session, vr, &p);
                    std::vector<int> picked;
                    for (std::size_t w = 0; w < W; ++w)
                    {
                        std::vector<std::string> vals;
                        for (std::size_t s = w0; s < w1 && p.has(s); ++s)
                        {
                            chosen(kInstrument ? std::span<const Voice>(p.voiceAt[s - w0]) : std::span<const Voice>(), watchVoice, picked);
                            std::string v = "-";
                            for (std::size_t t = 0; t < p.pairs.size(); ++t)
                                if (p.pairs[t].field == watch[w]
                                    && (p.pairs[t].slot < 0 || (!picked.empty() && p.pairs[t].slot == picked.front())))
                                {
                                    v = num(p.value(s, t));
                                    break;
                                }
                            vals.push_back(v);
                        }
                        out.lines.push_back("  " + map[watch[w]].path + " from " + when(w0, rate) + ", a sample each: " + joined(vals, " "));
                    }
                }
            }
            else if (!p1.calls.empty())
            {
                const std::size_t calls = p1.calls.size();
                for (std::size_t w = 0; w < W; ++w)
                {
                    std::vector<std::string> vals;
                    for (std::size_t k = 0; k < points; ++k)
                    {
                        const std::size_t c = points > 1 ? k * (calls - 1) / (points - 1) : 0;
                        const Pass1::Watch& v = p1.watched[c * W + w];
                        vals.push_back(v.bad ? std::string("nan") : v.count == 0 ? std::string("-") : num(v.v));
                    }
                    const double step = points > 1 ? static_cast<double>(p1.calls.back().at) / rate / static_cast<double>(points - 1) : 0.0;
                    out.lines.push_back("  " + map[watch[w]].path + " every " + seconds(step) + " from 0: " + joined(vals, " "));
                }
            }
        }
        return out;
    }
}
