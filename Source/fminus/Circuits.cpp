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
            || std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; })
            || std::any_of(outcomes_.begin(), outcomes_.end(),
                           [&](const auto& o) { return o.first == key && !o.second.answered; }))
            return false;
        flight_.insert(key);
        return true;
    }

    void Answers::erase(std::string_view key)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; }),
                       entries_.end());
    }

    void Answers::report(const std::string& key, Outcome outcome)
    {
        if (key.empty())
            return;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            outcomes_.erase(std::remove_if(outcomes_.begin(), outcomes_.end(), [&](const auto& o) { return o.first == key; }),
                            outcomes_.end());
            outcomes_.emplace_back(key, std::move(outcome));
            while (outcomes_.size() > kCapacity)
                outcomes_.erase(outcomes_.begin());
        }
        reported_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::optional<Answers::Outcome> Answers::outcome(std::string_view key) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, o] : outcomes_)
            if (k == key)
                return o;
        return std::nullopt;
    }

    bool Answers::gaveUp(std::string_view key) const
    {
        const std::optional<Outcome> o = outcome(key);
        return o && !o->answered;
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
        outcomes_.clear();
        flight_.clear();
    }

    bool restore(const std::string& key, const std::string& text)
    {
        if (key.size() != 16 || text.size() > 2000)
            return false;
        const std::optional<fminus::circuit::Choice> c = fminus::circuit::Choice::parse(text);
        if (!c || c->key != key)
            return false;
        answers().store(key, text);
        return true;
    }

    std::vector<std::string> keysOf(const Sources& files)
    {
        std::vector<std::string> out;
        for (const SourceFile& f : files)
            if (isCircuitFile(f.name))
            {
                std::string key = fminus::circuit::selfTestKey(f.text, f.name, {});
                if (!key.empty() && std::find(out.begin(), out.end(), key) == out.end())
                    out.push_back(std::move(key));
            }
        return out;
    }

    std::string chosenNote(std::string_view file, std::string_view text, std::string_view key)
    {
        const std::optional<fminus::circuit::Choice> c = fminus::circuit::Choice::parse(text);
        if (!c)
            return {};
        std::string words = std::to_string(c->oversample) + "x";
        if (c->adaa)
            words += ", ADAA";
        words += c->solver.guess == fminus::circuit::Solver::Guess::Omega ? ", omega(" : ", newton(";
        words += std::to_string(c->solver.steps) + ")";
        if (c->method == fminus::circuit::Method::Bdf2)
            words += ", BDF2";
        std::string out = std::string(file) + ": note: the self-test chose this circuit's configuration: " + words + "\n";
        if (!key.empty())
            if (const std::optional<Answers::Outcome> o = answers().outcome(key); o && o->answered)
                out += o->warnings;
        return out;
    }

    std::string gaveUpNote(std::string_view provisional, const Answers::Outcome& outcome)
    {
        // "pedal.cir: note: the self-test chooses this circuit's configuration; provisional: 8x IIR, omega(2)"
        constexpr std::string_view kSays = ": note: the self-test chooses this circuit's configuration; provisional: ";
        const std::size_t at = provisional.find(kSays);
        if (at == std::string_view::npos)
            return std::string(provisional);
        std::string_view configuration = provisional.substr(at + kSays.size());
        while (!configuration.empty() && (configuration.back() == '\n' || configuration.back() == '\r'))
            configuration.remove_suffix(1);
        const std::string why = outcome.why.empty() ? std::string() : " (" + outcome.why + ")";
        return std::string(provisional.substr(0, at)) + ": note: the self-test found no configuration for this circuit"
             + why + ", so the provisional one plays on: " + std::string(configuration) + "\n" + outcome.warnings;
    }

    std::string withOutcomes(const std::string& notes, const std::vector<Job>& jobs)
    {
        std::string out = notes;
        for (const Job& job : jobs)
        {
            const std::optional<Answers::Outcome> o = answers().outcome(job.key);
            if (!o || o->answered)
                continue;
            // the job's provisional note, in its file's words: what a compile of it said
            const std::string says = job.file + ": note: the self-test chooses this circuit's configuration; provisional: ";
            const std::size_t at = out.find(says);
            if (at != std::string::npos && (at == 0 || out[at - 1] == '\n'))
            {
                const std::size_t end = out.find('\n', at);
                const std::size_t stop = end == std::string::npos ? out.size() : end + 1;
                out.replace(at, stop - at, gaveUpNote(std::string_view(out).substr(at, stop - at), *o));
                continue;
            }
            const std::string gave = job.file + ": note: the self-test found no configuration for this circuit";
            if (out.find(gave) != std::string::npos)
                continue;                                    // said already (the compile read the outcome)
            if (!out.empty() && out.back() != '\n')
                out += '\n';
            out += gaveUpNote(o->provisional, *o);
        }
        return out;
    }

    Answers::Outcome outcomeOf(const Job& job, bool answered, const std::string& warnings, const std::string& notes,
                               const std::string& errors)
    {
        Answers::Outcome o;
        o.answered = answered;
        const fminus::circuit::Result own = fminus::circuit::generate(job.netlist, job.file);
        o.provisional = provisionalNote(own.notes);
        // the self-test's own warnings: its warnings less the netlist's
        std::size_t at = 0;
        while (at < warnings.size())
        {
            const std::size_t end = std::min(warnings.find('\n', at), warnings.size());
            const std::string line = warnings.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && own.warnings.find(line) == std::string::npos)
                o.warnings += line + "\n";
        }
        if (answered)
            return o;
        // why: the last note that says "no answer" ("x.cir: note: self-test: no candidate under ...; no answer"), else
        // the first error's words
        auto words = [](std::string line, std::string_view after) {
            const std::size_t from = line.find(after);
            line = from == std::string::npos ? line : line.substr(from + after.size());
            constexpr std::string_view kSelfTest = "self-test: ";
            if (line.rfind(kSelfTest, 0) == 0)
                line = line.substr(kSelfTest.size());
            constexpr std::string_view kNone = "; no answer";
            if (line.size() >= kNone.size() && line.compare(line.size() - kNone.size(), kNone.size(), kNone) == 0)
                line.resize(line.size() - kNone.size());
            return line;
        };
        const std::size_t last = notes.rfind("no answer");
        if (last != std::string::npos)
        {
            const std::size_t from = notes.rfind('\n', last);
            const std::size_t begin = from == std::string::npos ? 0 : from + 1;
            const std::size_t end = notes.find('\n', last);
            o.why = words(notes.substr(begin, (end == std::string::npos ? notes.size() : end) - begin), ": note: ");
        }
        else if (!errors.empty())
            o.why = words(errors.substr(0, errors.find('\n')), ": error: ");
        else
            o.why = "it gave no answer";
        return o;
    }

    uint64_t generations() noexcept { return gGenerations.load(std::memory_order_relaxed); }

    // ---- one compile's generator --------------------------------------------------------------------------------------
    Generator::Generator(const Sources& files, const CircuitPins* pins) : files_(files), pins_(pins) {}

    Generator::Generator(const Sources& files, const Sources& generated) : files_(files), replay_(true)
    {
        for (const SourceFile& g : generated)
            modules_.push_back(std::make_unique<Module>(Module{ g.name, g.text, {}, {} }));
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
        // The store's answer, when it holds one for this netlist (its key, from a parse and a plan: cheap), or the pin's
        // when the pins name its key (an empty one: none); one that does not read back is left out, and the
        // provisional configuration plays.
        const std::string key = fminus::circuit::selfTestKey(netlist.text, netlist.name, options);
        std::optional<std::string> answer;
        if (!key.empty())
        {
            const auto pin = pins_ == nullptr ? CircuitPins::const_iterator{}
                                              : std::find_if(pins_->begin(), pins_->end(), [&](const CircuitPin& p) { return p.key == key; });
            if (pins_ != nullptr && pin != pins_->end())
            {
                if (!pin->answer.empty())
                    answer = pin->answer;
            }
            else
                answer = answers().find(key);
        }
        if (answer)
            options.chosen = fminus::circuit::Choice::parse(*answer);
        fminus::circuit::Result r = fminus::circuit::generate(netlist.text, netlist.name, options);
        gGenerations.fetch_add(1, std::memory_order_relaxed);
        if (options.chosen && (!r.errors.empty() || r.provisional))
        {
            // An answer its netlist cannot take (a damaged session's or project's): generate() refused it, or ignored
            // it (its note says why) and plays the provisional configuration. Generated without it; the store's is
            // erased (a pin's is a recording's: kept), so the self-test answers the key again.
            fminus::circuit::Options without;
            const fminus::circuit::Result again = fminus::circuit::generate(netlist.text, netlist.name, without);
            gGenerations.fetch_add(1, std::memory_order_relaxed);
            if (again.errors.empty())
            {
                if (pins_ == nullptr || std::none_of(pins_->begin(), pins_->end(), [&](const CircuitPin& p) { return p.key == key; }))
                    answers().erase(key);
                r = again;
                options.chosen.reset();
            }
        }
        errors_ += r.errors;
        warnings_ += r.warnings;
        if (r.errors.empty())
        {
            if (r.provisional)
            {
                // A key whose test gave no answer is not tested again: why, in place of the provisional note, and no
                // job (Answers::Outcome).
                const std::optional<Answers::Outcome> o = r.selfTestKey.empty() ? std::nullopt : answers().outcome(r.selfTestKey);
                if (o && !o->answered)
                    notes_ += gaveUpNote(provisionalNote(r.notes), *o);
                else
                {
                    notes_ += provisionalNote(r.notes);
                    if (job && !r.selfTestKey.empty())
                        jobs_.push_back({ r.selfTestKey, netlist.name, netlist.text });
                }
            }
            if (!r.selfTestKey.empty() && std::find(keys_.begin(), keys_.end(), r.selfTestKey) == keys_.end())
            {
                keys_.push_back(r.selfTestKey);
                used_.push_back({ r.selfTestKey, options.chosen ? *answer : std::string() });
            }
        }
        modules_.push_back(std::make_unique<Module>(Module{ netlist.name, r.source, r.lines, Shape{ r.module, r.pots, r.switches } }));
        return *modules_.back();
    }

    const Generator::Shape* Generator::shape(std::string_view name) const
    {
        for (const std::unique_ptr<Module>& m : modules_)
            if (m->name == name && !m->text.empty())
                return &m->shape;
        return nullptr;
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
