// Source/fminus/Circuits.cpp: see Circuits.h.
#include "fminus/Circuits.h"

#include "fminus/Circuit.h"

#include <algorithm>
#include <cstdlib>

namespace playground::circuits
{
    namespace
    {
        std::atomic<uint64_t> gGenerations{ 0 };

        // The line of `notes` that says the configuration is provisional (generate()'s one such note), with its line
        // break; "" when there is none.
        std::string provisionalNote(const std::string& notes)
        {
            constexpr std::string_view kSays = ": note: the self-test chooses this circuit's configuration";
            const std::size_t at = notes.find(kSays);
            if (at == std::string::npos)
                return {};
            const std::size_t from = notes.rfind('\n', at);
            const std::size_t begin = from == std::string::npos ? 0 : from + 1;
            const std::size_t end = notes.find('\n', at);
            return notes.substr(begin, end == std::string::npos ? std::string::npos : end + 1 - begin)
                 + (end == std::string::npos ? "\n" : "");
        }

        // The digits at `at` in `s` as a number, and where they end (`at` itself when there are none).
        int digitsAt(const std::string& s, std::size_t at, std::size_t& end)
        {
            end = at;
            while (end < s.size() && s[end] >= '0' && s[end] <= '9')
                ++end;
            return end > at ? std::atoi(s.substr(at, end - at).c_str()) : 0;
        }
    }

    // ---- the answers ------------------------------------------------------------------------------------------------
    Answers& answers()
    {
        static Answers store;
        return store;
    }

    std::optional<std::string> Answers::find(std::string_view key) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, text] : entries_)
            if (k == key)
                return text;
        return std::nullopt;
    }

    void Answers::store(const std::string& key, const std::string& text)
    {
        if (key.empty())
            return;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; }),
                           entries_.end());
            entries_.emplace_back(key, text);
            while (entries_.size() > kCapacity)
                entries_.erase(entries_.begin());
        }
        landed_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::vector<std::pair<std::string, std::string>> Answers::of(const std::vector<std::string>& keys) const
    {
        std::vector<std::pair<std::string, std::string>> out;
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& key : keys)
        {
            if (std::any_of(out.begin(), out.end(), [&](const auto& o) { return o.first == key; }))
                continue;
            for (const auto& e : entries_)
                if (e.first == key)
                {
                    out.push_back(e);
                    break;
                }
        }
        return out;
    }

    bool Answers::claim(const std::string& key)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (key.empty() || flight_.count(key) != 0
            || std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; }))
            return false;
        flight_.insert(key);
        return true;
    }

    void Answers::release(const std::string& key)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        flight_.erase(key);
    }

    bool Answers::inFlight(std::string_view key) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return flight_.find(key) != flight_.end();
    }

    std::size_t Answers::size() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    void Answers::clearForTests()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        flight_.clear();
    }

    uint64_t generations() noexcept { return gGenerations.load(std::memory_order_relaxed); }

    // ---- one compile's generator --------------------------------------------------------------------------------------
    Generator::Generator(const Sources& files) : files_(files) {}

    Generator::Generator(const Sources& files, const Sources& generated) : files_(files), replay_(true)
    {
        for (const SourceFile& g : generated)
            modules_.push_back(std::make_unique<Module>(Module{ g.name, g.text, {} }));
    }

    Generator::Module* Generator::find(std::string_view name)
    {
        for (const std::unique_ptr<Module>& m : modules_)
            if (m->name == name)
                return m.get();
        return nullptr;
    }

    Generator::Module& Generator::generate(const SourceFile& netlist, bool job)
    {
        fminus::circuit::Options options;
        // The store's answer, when it holds one for this netlist (its key, from a parse and a plan: cheap); one that
        // does not read back is left out, and the provisional configuration plays.
        const std::string key = fminus::circuit::selfTestKey(netlist.text, netlist.name, options);
        if (!key.empty())
            if (const std::optional<std::string> stored = answers().find(key))
                options.chosen = fminus::circuit::Choice::parse(*stored);
        const fminus::circuit::Result r = fminus::circuit::generate(netlist.text, netlist.name, options);
        gGenerations.fetch_add(1, std::memory_order_relaxed);
        errors_ += r.errors;
        warnings_ += r.warnings;
        if (r.errors.empty())
        {
            if (r.provisional)
            {
                notes_ += provisionalNote(r.notes);
                if (job && !r.selfTestKey.empty())
                    jobs_.push_back({ r.selfTestKey, netlist.name, netlist.text });
            }
            if (!r.selfTestKey.empty() && std::find(keys_.begin(), keys_.end(), r.selfTestKey) == keys_.end())
                keys_.push_back(r.selfTestKey);
        }
        modules_.push_back(std::make_unique<Module>(Module{ netlist.name, r.source, r.lines }));
        return *modules_.back();
    }

    const std::string* Generator::module(std::string_view name)
    {
        if (Module* m = find(name))
            return &m->text;
        if (replay_)
            return nullptr;
        const SourceFile* f = findFile(files_, name);
        if (f == nullptr || !isCircuitFile(f->name))
            return nullptr;
        return &generate(*f, true).text;
    }

    void Generator::generateRest()
    {
        if (replay_)
            return;
        for (const SourceFile& f : files_)
            if (isCircuitFile(f.name) && find(f.name) == nullptr)
                generate(f, false);
    }

    Sources Generator::generated() const
    {
        Sources out;
        for (const std::unique_ptr<Module>& m : modules_)
            out.push_back({ m->name, m->text });
        return out;
    }

    std::string Generator::recite(const std::string& messages) const
    {
        if (modules_.empty())
            return messages;
        std::string out;
        std::size_t at = 0;
        while (at < messages.size())
        {
            const std::size_t end = std::min(messages.find('\n', at), messages.size());
            std::string line = messages.substr(at, end - at);
            const bool broken = end < messages.size();
            at = end + 1;
            for (const std::unique_ptr<Module>& m : modules_)
            {
                const std::string generated = m->name + ".fm:";
                const auto from = [&](int number) {
                    return number >= 1 && static_cast<std::size_t>(number) <= m->lines.size()
                               ? m->lines[static_cast<std::size_t>(number) - 1]
                               : 0;
                };
                // "In file included from x.cir.fm:L:" (a generated module includes nothing, but say it right).
                constexpr std::string_view kIncluded = "In file included from ";
                if (line.rfind(std::string(kIncluded) + generated, 0) == 0)
                {
                    std::size_t numberEnd = 0;
                    const std::size_t first = kIncluded.size() + generated.size();
                    const int number = digitsAt(line, first, numberEnd);
                    if (numberEnd > first)
                    {
                        const int n = from(number);
                        line = std::string(kIncluded) + m->name + (n > 0 ? ":" + std::to_string(n) : std::string())
                             + line.substr(numberEnd);
                    }
                    break;
                }
                if (line.rfind(generated, 0) != 0)
                    continue;
                // "x.cir.fm:L:C: error: ..." -> "x.cir:N:1: error: ... (in the module generated from x.cir, line L)"
                std::size_t numberEnd = 0, columnEnd = 0;
                const int number = digitsAt(line, generated.size(), numberEnd);
                if (numberEnd == generated.size() || numberEnd >= line.size() || line[numberEnd] != ':')
                    break;
                digitsAt(line, numberEnd + 1, columnEnd);
                if (columnEnd == numberEnd + 1 || columnEnd >= line.size() || line[columnEnd] != ':')
                    break;
                const int n = from(number);
                line = m->name + (n > 0 ? ":" + std::to_string(n) + ":1" : std::string()) + line.substr(columnEnd)
                     + " (in the module generated from " + m->name + ", line " + std::to_string(number) + ")";
                break;
            }
            out += line;
            if (broken)
                out += '\n';
        }
        return out;
    }
}
