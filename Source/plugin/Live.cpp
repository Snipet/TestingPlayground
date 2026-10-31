// Source/plugin/Live.cpp: the live tools (ChatAgent.h's liveTool). listen: what plays now in the user's song, over a
// few seconds (the meters, the scopes and the blocks' costs as the audio thread publishes them, Engine.h; the host's
// bypass and blocks; Live.h's LiveRead). get_status: the state now, and with `since` only what changed after a token
// (the Processor's journal, Journal.h; the files as diffs against their text at that token). MCP's own record of the
// files, by token (mcpMarks_: one server may serve several conversations, each with its own tokens), and the line its
// results start with when something changed since the last MCP call.
//
// Both are live from MCP (ChatAgent::mcpLive): they run beside a running call, on the link's threads, and keep
// CLAUDE.md's contract: ChatHost's lock-free reads and locked getters, mcpMarks_ under liveMutex_, never the program,
// the editor, the chat's history or busy_. listen waits between its reads (about 25 a second) and stops when its call
// is cancelled (mcpCancel, which keeps a cancel that comes before it starts; a turn's: the stop button); while it
// listens the audio thread measures the levels and the scopes, as for an open editor (chatListening), until it
// returns, however it returns.
#include "plugin/ChatAgent.h"

#include "plugin/ClaudeApi.h"

#include "dsp/Measure.h"
#include "dsp/TextDiff.h"
#include "fminus/CirDoc.h"

#include "Product.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <thread>

namespace playground
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr int kFft = Engine::kScopeLength;              // a spectrum's frame: the scope's whole ring
        constexpr double kPollSeconds = 0.04;                   // 25 reads a second
        constexpr double kSilentDb = -120.0;

        juce::var prop(const char* type, const char* description)
        {
            return claude::object({ { "type", type }, { "description", description } });
        }

        juce::var definition(const char* name, const std::string& description, juce::var properties)
        {
            return claude::object({ { "name", name },
                                    { "description", juce::String::fromUTF8(description.c_str()) },
                                    { "eager_input_streaming", true },
                                    { "input_schema", claude::object({ { "type", "object" }, { "properties", properties } }) } });
        }

        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        double db(double lin) { return lin > 1.0e-12 ? 20.0 * std::log10(lin) : -240.0; }
        std::string dbfs(double d) { return d <= kSilentDb ? std::string("silent") : fmt("%.1f dBFS", d); }

        double pct(std::vector<double> v, double q)
        {
            if (v.empty())
                return 0.0;
            std::sort(v.begin(), v.end());
            return v[static_cast<std::size_t>(std::lround(q * static_cast<double>(v.size() - 1)))];
        }

        [[maybe_unused]] std::string noteName(int n)   // an instrument's
        {
            static constexpr const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return std::string(kNames[n % 12]) + std::to_string(n / 12 - 1);    // 60: C4, 48: C3 (the README's)
        }

        std::string khz(double rate)
        {
            const double k = rate / 1000.0;
            return std::fabs(k - std::round(k)) < 0.05 ? fmt("%.0f kHz", k) : fmt("%.1f kHz", k);
        }

        const char* kindName(DisplayKind k)
        {
            switch (k)
            {
                case DisplayKind::meter:    return "meter";
                case DisplayKind::vu:       return "vu";
                case DisplayKind::led:      return "led";
                case DisplayKind::number:   return "number";
                case DisplayKind::scope:    return "scope";
                case DisplayKind::spectrum: return "spectrum";
            }
            return "display";
        }

        std::string flagWords(uint8_t f)
        {
            std::string out;
            const auto add = [&](const char* w) { out += std::string(out.empty() ? "" : ", ") + w; };
            if ((f & BlockStat::swapping) != 0)
                add("a program swap fading");
            if ((f & BlockStat::moving) != 0)
                add("a dial moving");
            if ((f & BlockStat::notes) != 0)
                add("a note-on");
            if ((f & BlockStat::alert) != 0)
                add("the output guard acting");
            add((f & BlockStat::frozen) != 0 ? "the frozen copy playing" : "the program's own code (not the frozen copy)");
            return out;
        }

        // A spectrum, Welch-averaged: the power of Hann-windowed frames of kFft samples, summed.
        struct Spectrum
        {
            std::vector<double> power;
            int frames = 0;

            void add(const std::vector<float>& x)
            {
                if (x.size() < static_cast<std::size_t>(kFft))
                    return;
                std::vector<std::complex<double>> a(static_cast<std::size_t>(kFft));
                for (int i = 0; i < kFft; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kFft);
                    a[static_cast<std::size_t>(i)] = { w * static_cast<double>(x[static_cast<std::size_t>(i)]), 0.0 };
                }
                measure::fft(a);
                power.resize(static_cast<std::size_t>(kFft / 2 + 1), 0.0);
                for (std::size_t k = 0; k < power.size(); ++k)
                    power[k] += std::norm(a[k]);
                ++frames;
            }

            // The power in [lo, hi) Hz, the bins' parts in it counted (so a band narrower than a bin has its share).
            double band(double lo, double hi, double binHz) const
            {
                double sum = 0.0;
                for (std::size_t k = 1; k < power.size(); ++k)
                {
                    const double a = (static_cast<double>(k) - 0.5) * binHz, b = (static_cast<double>(k) + 0.5) * binHz;
                    const double overlap = std::min(b, hi) - std::max(a, lo);
                    if (overlap > 0.0)
                        sum += power[k] * overlap / binHz;
                }
                return sum;
            }
        };

        struct Bands
        {
            std::vector<double> hz, dbUnder;      // centre, dB under the loudest band (-99 at most)
        };

        Bands bandsOf(const Spectrum& s, double rate, bool thirds)
        {
            Bands out;
            if (s.frames == 0)
                return out;
            const double binHz = rate / kFft;
            const double half = std::pow(2.0, thirds ? 1.0 / 6.0 : 0.5);
            std::vector<double> p;
            for (int k = thirds ? -17 : 0; k <= (thirds ? 13 : 9); ++k)
            {
                const double fc = thirds ? 1000.0 * std::pow(2.0, k / 3.0) : 31.25 * std::pow(2.0, k);
                if (fc * half > rate / 2.0 && fc > rate / 2.0)
                    break;
                out.hz.push_back(fc);
                p.push_back(s.band(fc / half, fc * half, binHz));
            }
            const double most = p.empty() ? 0.0 : *std::max_element(p.begin(), p.end());
            for (const double v : p)
                out.dbUnder.push_back(most > 0.0 && v > 0.0 ? std::max(-99.0, 10.0 * std::log10(v / most)) : -99.0);
            return out;
        }

        std::string hzText(double f) { return f >= 1000.0 ? fmt(f >= 10000.0 ? "%.1f kHz" : "%.2f kHz", f / 1000.0) : fmt("%.0f Hz", f); }

        // The centroid and the strongest peaks (at most 3, within 60 dB of the strongest), 20 Hz to 20 kHz.
        void peaksOf(const Spectrum& s, double rate, double& centroid, std::vector<measure::Peak>& peaks)
        {
            centroid = 0.0;
            peaks.clear();
            if (s.frames == 0)
                return;
            const double binHz = rate / kFft;
            const auto lo = static_cast<std::size_t>(std::ceil(20.0 / binHz));
            const std::size_t hi = std::min(s.power.size() - 1, static_cast<std::size_t>(20000.0 / binHz));
            double num = 0.0, den = 0.0;
            for (std::size_t k = lo; k <= hi; ++k)
            {
                num += static_cast<double>(k) * binHz * s.power[k];
                den += s.power[k];
            }
            centroid = den > 0.0 ? num / den : 0.0;
            std::vector<std::pair<double, std::size_t>> local;
            for (std::size_t k = std::max<std::size_t>(lo, 1); k < hi; ++k)
                if (s.power[k] > s.power[k - 1] && s.power[k] >= s.power[k + 1] && s.power[k] > 0.0)
                    local.emplace_back(s.power[k], k);
            std::sort(local.begin(), local.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < local.size() && peaks.size() < 3; ++i)
            {
                const std::size_t k = local[i].second;
                const double a = 10.0 * std::log10(std::max(s.power[k - 1], 1e-30)), b = 10.0 * std::log10(s.power[k]),
                             c = 10.0 * std::log10(std::max(s.power[k + 1], 1e-30));
                const double d = a - 2.0 * b + c;
                const double shift = d < 0.0 ? std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
                const double rel = 10.0 * std::log10(local[i].first / local[0].first);
                if (rel < -60.0)
                    break;
                peaks.push_back({ (static_cast<double>(k) + shift) * binHz, rel });
            }
        }

        // A level's numbers over the window: the RMS's percentiles and the peak (dBFS).
        struct Level
        {
            std::vector<double> rms;              // dBFS a read
            double peak = 0.0;                    // linear, the most
            void add(double rmsLin, double peakLin)
            {
                rms.push_back(db(rmsLin));
                peak = std::max(peak, peakLin);
            }
            bool any() const { return !rms.empty(); }
        };

        std::string levelText(const Level& l)
        {
            if (!l.any())
                return "not measured";
            const double p50 = pct(l.rms, 0.5);
            if (p50 <= kSilentDb && db(l.peak) <= kSilentDb)
                return "silent";
            return "RMS " + dbfs(p50) + " (p10 " + dbfs(pct(l.rms, 0.1)) + ", p90 " + dbfs(pct(l.rms, 0.9)) + "), peak "
                 + dbfs(db(l.peak));
        }

        juce::var levelVar(const Level& l)
        {
            return claude::object({ { "rms_p10_dbfs", pct(l.rms, 0.1) }, { "rms_p50_dbfs", pct(l.rms, 0.5) },
                                    { "rms_p90_dbfs", pct(l.rms, 0.9) }, { "peak_dbfs", db(l.peak) } });
        }

        std::string dbWhole(double d) { return d > -0.5 ? std::string("0") : fmt("%.0f", d); }   // never "-0"

        std::string bandsText(const Bands& b)
        {
            std::string out;
            for (const double d : b.dbUnder)
                out += (out.empty() ? "" : " ") + dbWhole(d);
            return out;
        }

        juce::var bandsVar(const Bands& b)
        {
            juce::Array<juce::var> a;
            for (std::size_t i = 0; i < b.hz.size(); ++i)
                a.add(claude::object({ { "hz", b.hz[i] }, { "db", b.dbUnder[i] } }));
            return a;
        }

        // A display's value over the window.
        struct Seen
        {
            double now = 0.0, lo = 0.0, hi = 0.0;
            int count = 0;
            void add(double v)
            {
                now = v;
                lo = count == 0 ? v : std::min(lo, v);
                hi = count == 0 ? v : std::max(hi, v);
                ++count;
            }
        };

        std::string num(double v) { return fmt(std::fabs(v) >= 100.0 || v == std::round(v) ? "%.0f" : "%.3g", v); }
    }

    // ---- the tools' definitions --------------------------------------------------------------------------------------
    juce::var ChatAgent::statusDefinition()
    {
        return definition(
            "get_status",
            "Read the plugin's state now: the files, what plays, the last compile, the dials, the CPU load and the output "
            "guard's alerts. It ends with a token: with since set to it, only what changed after (dials, code as a diff, "
            "runs, presets, bypass, the host).\n\n"
            "files: none (the default without since), changed (with since: each file as a diff against its text at "
            "that token), all (each whole). A token of another instance, or of before a reload, gets the whole "
            "state. Your other MCP results start with a line when something changed.",
            claude::object({ { "since", prop("string", "A token from an earlier call") },
                             { "files", claude::object({ { "type", "string" },
                                                         { "enum", juce::Array<juce::var>{ "none", "changed", "all" } } }) } }));
    }

    juce::var ChatAgent::listenDefinition()
    {
        const std::string lead =
            kInstrument ? "Listen live to what the synth plays now, for a few seconds: notes, voices, level, spectrum, "
                          "displays, tempo, CPU, alerts. Not reproducible."
                        : "Listen live to what the effect does now, for a few seconds: levels in and out, gain, spectrum, "
                          "displays, tempo, bypass, CPU, alerts. Not reproducible.";
        const std::string more =
            std::string("\n\nUse it first when the user says what they hear now. 0 blocks: the host is not calling the "
                        "plugin (a track disabled or frozen, the audio stopped, the host's bypass).")
            + (kInstrument ? " No note-ons while the host plays: no MIDI comes; note-ons and no voices: the program." : "")
            + " Levels are RMS p10, p50, p90 and the peak (dBFS), bands dB under the loudest. seconds 0 reads once.";
        return definition("listen", lead + more,
                          claude::object({ { "seconds", prop("number", "0 to 10, default 2") },
                                           { "detail", prop("boolean", "Per channel; third octaves") },
                                           { "format", claude::object({ { "type", "string" },
                                                                        { "enum", juce::Array<juce::var>{ "text", "json" } } }) } }));
    }

    bool ChatAgent::mcpLiveWaits(const std::string& name)
    {
        return name == "listen";                                 // seconds of reads (Live.cpp)
    }

    // ---- the dispatch ------------------------------------------------------------------------------------------------
    juce::var ChatAgent::liveTool(const juce::var& use, const LiveCall& caller)
    {
        const std::string name = use.getProperty("name", "").toString().toStdString();
        if (name != "listen" && name != "get_status")
            return runTool(use);                                 // another live call (capture's status and stop)
        const juce::var input = use.getProperty("input", juce::var());
        bool isError = false;
        std::string result;
        if (name == "get_status")
            result = statusText(input, caller.mcp, isError);
        else if (caller.mcp)
        {
            // Its cancels (mcpCancel keeps each, even one that came before it started).
            const uint64_t call = caller.call;
            result = listenText(input, [this, call] {
                if (call == 0)
                    return false;
                const std::lock_guard<std::mutex> lock(liveMutex_);
                return mcpStopped_.count(call) != 0;
            }, isError);
            if (call != 0)
            {
                const std::lock_guard<std::mutex> lock(liveMutex_);
                mcpStopped_.erase(call);
            }
        }
        else
        {
            setActivity("Listening");
            result = listenText(input, [this] { return cancel_.load(); }, isError);
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, "listened to what plays");
        }
        juce::var block = claude::object({ { "type", "tool_result" },
                                           { "tool_use_id", use.getProperty("id", "") },
                                           { "content", juce::String::fromUTF8(result.c_str()) } });
        if (isError)
            block.getDynamicObject()->setProperty("is_error", true);
        return block;
    }

    // ---- the hint -------------------------------------------------------------------------------------------------------
    // One cursor for the instance: Claude Desktop's calls carry no conversation, so the line says "the last MCP call"
    // (get_status's token is each conversation's own).
    std::string ChatAgent::mcpHint()
    {
        Changes ch;
        {
            const std::lock_guard<std::mutex> lock(liveMutex_);
            ch = host_.chatChanges(mcpLook_, false);
            mcpLook_ = ch.token;
        }
        if (!ch.known || (ch.changes == 0 && ch.alerts == 0))
            return {};
        std::string what;
        if (ch.changes > 0)
            what = "The user changed " + std::to_string(ch.changes) + (ch.changes == 1 ? " thing" : " things");
        if (ch.alerts > 0)
            what += ch.changes > 0 ? " and the output guard acted" : "The output guard acted";
        return "(" + what + " since the last MCP call here: get_status with since shows " + (ch.changes + ch.alerts == 1 ? "it" : "them")
             + ".)";
    }

    // ---- get_status ----------------------------------------------------------------------------------------------------
    std::string ChatAgent::statusText(const juce::var& input, bool mcp, bool& isError)
    {
        // peek, the MCP server's own read (the GUI sandbox's, mcp/Main.cpp's instanceFiles; no schema names it): the
        // files alone, and nothing kept of the look: no token, no mark, mcpHint's cursor where it was, so that what the
        // user changed still starts Claude's next result.
        if (mcp && static_cast<bool>(input.getProperty("peek", false)))
        {
            std::string out = "The files:\n";
            for (const SourceFile& f : host_.chatDraft().files)
                out += "<file name=\"" + f.name + "\">\n" + f.text + (f.text.empty() || f.text.back() == '\n' ? "" : "\n") + "</file>\n";
            return out;
        }
        const juce::var sinceVar = input.getProperty("since", juce::var());
        const std::string since = sinceVar.isString() ? sinceVar.toString().trim().toStdString() : std::string();
        std::string files = input.getProperty("files", "").toString().toStdString();
        if (files != "none" && files != "changed" && files != "all")
            files = since.empty() ? "none" : "changed";
        // The files first, then the token: what a token keeps of them is never newer than it (an edit between the two
        // is listed after the token, and shows in the next diff too).
        const Draft d = host_.chatDraft();
        Changes ch;
        std::shared_ptr<const std::vector<SourceFile>> then;     // the files at `since` (MCP's mark of it)
        if (mcp)
        {
            const std::lock_guard<std::mutex> lock(liveMutex_);
            ch = host_.chatChanges(since, true);
            mcpLook_ = ch.token;
            for (const McpMark& m : mcpMarks_)
                if (ch.known && m.token == since)
                    then = m.files;
            std::shared_ptr<const std::vector<SourceFile>> now = !mcpMarks_.empty() && *mcpMarks_.back().files == d.files
                                                                     ? mcpMarks_.back().files
                                                                     : std::make_shared<const std::vector<SourceFile>>(d.files);
            mcpMarks_.push_back({ ch.token, std::move(now) });
            while (mcpMarks_.size() > kMcpMarks)
                mcpMarks_.pop_front();
        }
        else
            ch = host_.chatChanges(since, true);

        std::string out;
        if (!since.empty() && ch.known)
        {
            if (ch.lines.empty())
                out = "Nothing changed since your last look.\n";
            else
            {
                out = "What changed since your last look, oldest first:\n";
                for (const std::string& l : ch.lines)
                    out += "- " + l + "\n";
            }
            if (!ch.complete)
                out += "(Older changes than these were dropped: the plugin keeps the last " + std::to_string(256)
                     + ". get_status without since gives the whole state.)\n";
        }
        else
        {
            if (!since.empty())
                out = "(That token is not this instance's, or is from before the plugin was opened again: here is the whole "
                      "state.)\n";
            out += toolText("get_status", input, isError);
        }

        // The files' text, as asked.
        if (files == "all")
        {
            out += "The files:\n";
            for (const SourceFile& f : d.files)
            {
                out += "<file name=\"" + f.name + "\">\n" + f.text + (f.text.empty() || f.text.back() == '\n' ? "" : "\n") + "</file>\n";
                if (!mcp)
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    markSeen(f.name, f.text);
                }
            }
        }
        else if (files == "changed" && mcp)
        {
            // Each file against its text at the token `since` (that conversation's own: another one's looks move
            // nothing), the edits made through MCP since among them (Claude Desktop's calls name no conversation).
            if (since.empty())
                out += "(files \"changed\" goes with since: read_file shows a file, files \"all\" all of them.)\n";
            else if (ch.known && !then)
                out += "(The files at that token are no longer kept, only the newest " + std::to_string(kMcpMarks)
                     + " tokens' are: read_file shows a file, files \"all\" all of them.)\n";
            else if (then)
            {
                std::map<std::string, const SourceFile*> was;
                for (const SourceFile& f : *then)
                    was[f.name] = &f;
                std::string part;
                int budget = 60;
                for (const SourceFile& f : d.files)
                {
                    const auto it = was.find(f.name);
                    if (it == was.end())
                    {
                        part += f.name + ": new since then, " + std::to_string(textdiff::lines(f.text).size())
                              + " lines (read_file shows it).\n";
                        continue;
                    }
                    // a netlist's drawing (its *@ lines, which Claude leaves alone) is no diff: its netlist's lines are
                    const bool netlist = isCircuitFile(f.name);
                    const std::string before = netlist ? cir::stripLayout(it->second->text) : it->second->text;
                    const std::string now = netlist ? cir::stripLayout(f.text) : f.text;
                    const bool redrawn = it->second->text != f.text;
                    was.erase(it);
                    if (before == now)
                    {
                        if (redrawn)
                            part += f.name + ": only its drawing (its *@ lines) changed.\n";
                        continue;
                    }
                    if (budget <= 0)
                    {
                        part += f.name + " changed too: read_file shows it.\n";
                        continue;
                    }
                    const textdiff::Unified u = textdiff::unified(before, now, 2, budget);
                    part += f.name + " (+" + std::to_string(u.added) + " -" + std::to_string(u.removed) + "):\n" + u.text;
                    budget -= u.lines;
                    if (u.cut)
                        part += "(cut here: read_file shows the whole file)\n";
                }
                for (const auto& [name, f] : was)
                    part += name + " is gone (closed or renamed).\n";
                out += part.empty() ? "The files are as they were at that token.\n"
                                    : "The files against their text at that token (edits made through MCP since are in it too):\n" + part;
            }
        }
        out += "Token: " + ch.token + " (get_status with since \"" + ch.token + "\" shows only what changes after this).\n";
        return out;
    }

    // ---- listen --------------------------------------------------------------------------------------------------------
    std::string ChatAgent::listenText(const juce::var& input, const std::function<bool()>& stopped, bool& isError)
    {
        isError = false;
        double seconds = 2.0;
        if (const juce::var v = input.getProperty("seconds", juce::var()); v.isDouble() || v.isInt() || v.isInt64())
            if (const double s = static_cast<double>(v); std::isfinite(s))
                seconds = std::clamp(s, 0.0, 10.0);
        const bool detail = static_cast<bool>(input.getProperty("detail", false));
        const bool json = input.getProperty("format", "").toString() == "json";

        // A reader while it listens (the audio thread measures the levels and the scopes only for readers).
        struct Listening
        {
            ChatHost& host;
            explicit Listening(ChatHost& h) : host(h) { host.chatListening(true); }
            ~Listening() { host.chatListening(false); }
            Listening(const Listening&) = delete;
            Listening& operator=(const Listening&) = delete;
        } listening(host_);

        using Clock = std::chrono::steady_clock;
        const auto rest = [&](double s) {                       // a pause, cut short by a cancel
            const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
            while (Clock::now() < until && !stopped())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };

        // The levels come from the next block measured: wait for the scope to hold samples measured since this reader
        // came (kFft of them), 0.3 s at most (a host that does not call: nothing comes).
        LiveRead r;
        for (int tries = 0; tries < 8 && !r.metered; ++tries)   // the seqlock is busy only while a block publishes
            host_.chatLive(r);
        r.blocksFrom = r.blocksTo;
        const uint64_t samplesAtAdd = r.meters.samples;
        const auto warm = Clock::now();
        while (!stopped() && Clock::now() - warm < std::chrono::milliseconds(300))
        {
            if (r.metered && r.meters.samples >= samplesAtAdd + static_cast<uint64_t>(kFft))
                break;
            rest(0.01);
            host_.chatLive(r);
            r.blocksFrom = r.blocksTo;
        }

        // The window. A read's levels count once a block was measured since this reader came, and once a block (not
        // the same one twice); the counters (note-ons, steals, dropped voices, the host's blocks) are differences from
        // the first read the meters gave.
        Level outL, inL, outCh[2], inCh[2];
        std::vector<double> gains;
        Spectrum outS, inS;
        std::vector<Seen> shown;
        std::vector<double> shares;
        BlockStat worst{};
        int mostVoices = 0, mostReleased = 0, mostPedal = 0, mostFading = 0;
        std::bitset<128> notes;
        int velLo = 128, velHi = -1;
        Seen mod, bend, after;
        bool pedalNow = false, pedalWas = false, bypassNow = r.bypassed, bypassWas = false;
        uint32_t alerts = 0;
        uint64_t lastFft = 0, counted = 0;
        LiveRead first = r, last = r;
        const auto start = Clock::now();
        const double window = seconds;
        for (;;)
        {
            host_.chatLive(r);
            r.blocksFrom = r.blocksTo;
            for (const BlockStat& b : r.blocks)
            {
                shares.push_back(static_cast<double>(b.share));
                if (b.share >= worst.share)
                    worst = b;
                mostVoices = std::max(mostVoices, static_cast<int>(b.voices));
            }
            alerts |= r.alerts;
            bypassNow = r.bypassed;
            bypassWas = bypassWas || r.bypassed;
            if (r.metered)
            {
                const Meters& m = r.meters;
                if (!first.metered)
                    first = r;                                   // the counters' start
                const auto both = [](const float* x) {
                    return std::sqrt(0.5 * (static_cast<double>(x[0]) * x[0] + static_cast<double>(x[1]) * x[1]));
                };
                if (m.samples > samplesAtAdd && m.samples != counted)
                {
                    counted = m.samples;
                    outL.add(both(m.outRms), std::max(m.outPeak[0], m.outPeak[1]));
                    for (int c = 0; c < 2; ++c)
                        outCh[c].add(m.outRms[c], m.outPeak[c]);
                    if constexpr (!kInstrument)
                    {
                        const double in = both(m.inRms);
                        inL.add(in, std::max(m.inPeak[0], m.inPeak[1]));
                        for (int c = 0; c < 2; ++c)
                            inCh[c].add(m.inRms[c], m.inPeak[c]);
                        if (in > 0.001)                         // above -60 dBFS
                            gains.push_back(db(both(m.outRms)) - db(in));
                    }
                }
                if (m.samples >= samplesAtAdd + static_cast<uint64_t>(kFft) && m.samples >= lastFft + static_cast<uint64_t>(kFft / 2))
                {
                    outS.add(r.out);
                    if constexpr (!kInstrument)
                        inS.add(r.in);
                    lastFft = m.samples;
                }
                if (m.program == r.program)
                {
                    shown.resize(r.displays.size());
                    for (std::size_t i = 0; i < r.displays.size(); ++i)
                        if (const PanelDisplay& d = r.displays[i]; !d.audio() && !d.hidden && d.tap >= 0 && d.tap < kMaxDisplays)
                            shown[i].add(static_cast<double>(m.taps[d.tap]));
                }
                if constexpr (kInstrument)
                {
                    if (last.metered && m.noteOns != last.meters.noteOns && m.lastNote >= 0)
                    {
                        notes.set(static_cast<std::size_t>(m.lastNote));
                        const int v = static_cast<int>(std::lround(m.lastVelocity * 127.0f));
                        velLo = std::min(velLo, v);
                        velHi = std::max(velHi, v);
                    }
                    for (std::size_t k = 0; k < 128; ++k)
                        if ((m.heldKeys[k / 32] & (1u << (k % 32))) != 0)
                            notes.set(k);
                    mod.add(static_cast<double>(m.host.modwheel));
                    bend.add(static_cast<double>(m.host.pitchbend));
                    after.add(static_cast<double>(m.host.aftertouch));
                    pedalNow = m.host.sustain;
                    pedalWas = pedalWas || m.host.sustain;
                    mostVoices = std::max(mostVoices, static_cast<int>(m.voices));
                    mostReleased = std::max(mostReleased, static_cast<int>(m.released));
                    mostPedal = std::max(mostPedal, static_cast<int>(m.pedalHeld));
                    mostFading = std::max(mostFading, static_cast<int>(m.fading));
                }
                last = r;
            }
            const double went = std::chrono::duration<double>(Clock::now() - start).count();
            if (went >= window || stopped())
                break;
            rest(std::min(kPollSeconds, window - went));
        }
        const double took = std::chrono::duration<double>(Clock::now() - start).count();
        const bool cut = stopped() && took + 0.02 < window;
        const uint64_t blocks = last.hostBlocks >= first.hostBlocks ? last.hostBlocks - first.hostBlocks : 0;
        const Meters& m0 = first.meters;
        const Meters& m1 = last.meters;
        const double rate = static_cast<double>(last.rate > 0.0f ? last.rate : r.rate);
        const uint32_t noteOns = m1.noteOns - m0.noteOns, steals = m1.steals - m0.steals, dropped = m1.dropped - m0.dropped;
        const bool playsDraft = cir::sameProgram(host_.chatRunning(), host_.chatDraft().files);   // *@ lines aside

        Bands outB = bandsOf(outS, rate, false), inB = bandsOf(inS, rate, false);
        double outCentroid = 0.0, inCentroid = 0.0;
        std::vector<measure::Peak> outPeaks, inPeaks;
        peaksOf(outS, rate, outCentroid, outPeaks);
        peaksOf(inS, rate, inCentroid, inPeaks);
        const bool outSilent = !outL.any() || (pct(outL.rms, 0.9) <= kSilentDb && db(outL.peak) <= kSilentDb);
        const bool inSilent = !inL.any() || (pct(inL.rms, 0.9) <= kSilentDb && db(inL.peak) <= kSilentDb);
        const HostBlock& h = m1.host;
        const double qpb = h.beatUnit > 0 ? h.beatsPerBar * 4.0 / h.beatUnit : 4.0;
        const int bar = static_cast<int>(std::floor(static_cast<double>(h.barStart) / std::max(qpb, 1e-9))) + 1;
        const double beatInBar = (static_cast<double>(h.beat) - static_cast<double>(h.barStart)) / (4.0 / std::max(h.beatUnit, 1)) + 1.0;

        if (json)
        {
            auto* o = new juce::DynamicObject();
            const juce::var root(o);
            o->setProperty("live", true);
            o->setProperty("note", "live numbers: they cannot be reproduced; measure_output renders offline");
            o->setProperty("seconds", took);
            o->setProperty("stopped", cut);
            o->setProperty("host", claude::object({ { "known", m1.hostKnown }, { "bpm", static_cast<double>(h.bpm) },
                                                    { "playing", h.playing }, { "bar", bar }, { "beat", beatInBar },
                                                    { "beats_per_bar", h.beatsPerBar }, { "beat_unit", h.beatUnit },
                                                    { "rate", rate }, { "largest_block", last.largestBlock },
                                                    { "bypassed", bypassNow }, { "bypassed_in_window", bypassWas },
                                                    { "blocks", static_cast<juce::int64>(blocks) } }));
            auto* lv = new juce::DynamicObject();
            const juce::var levels(lv);
            if (outL.any())
                lv->setProperty("out", levelVar(outL));
            if (!kInstrument && inL.any())
            {
                lv->setProperty("in", levelVar(inL));
                if (!gains.empty())
                    lv->setProperty("gain_db", claude::object({ { "p10", pct(gains, 0.1) }, { "p50", pct(gains, 0.5) }, { "p90", pct(gains, 0.9) } }));
            }
            if (detail)
                for (int c = 0; c < 2; ++c)
                {
                    lv->setProperty(c == 0 ? "out_left" : "out_right", levelVar(outCh[c]));
                    if (!kInstrument)
                        lv->setProperty(c == 0 ? "in_left" : "in_right", levelVar(inCh[c]));
                }
            o->setProperty("levels", levels);
            const auto spectrumVar = [&](const Spectrum& s, const Bands& b, double centroid, const std::vector<measure::Peak>& peaks) {
                juce::Array<juce::var> ps;
                for (const measure::Peak& p : peaks)
                    ps.add(claude::object({ { "hz", p.hz }, { "db", p.db } }));
                juce::var v = claude::object({ { "octave_bands", bandsVar(b) }, { "centroid_hz", centroid }, { "peaks", ps } });
                if (detail)
                    v.getDynamicObject()->setProperty("third_octave_bands", bandsVar(bandsOf(s, rate, true)));
                return v;
            };
            auto* sp = new juce::DynamicObject();
            const juce::var spectra(sp);
            if (outS.frames > 0 && !outSilent)
                sp->setProperty("out", spectrumVar(outS, outB, outCentroid, outPeaks));
            if (!kInstrument && inS.frames > 0 && !inSilent)
                sp->setProperty("in", spectrumVar(inS, inB, inCentroid, inPeaks));
            o->setProperty("spectrum", spectra);
            juce::Array<juce::var> ds;
            for (std::size_t i = 0; i < shown.size() && i < last.displays.size(); ++i)
                if (shown[i].count > 0)
                    ds.add(claude::object({ { "name", juce::String(last.displays[i].name) }, { "kind", kindName(last.displays[i].kind) },
                                            { "now", shown[i].now }, { "min", shown[i].lo }, { "max", shown[i].hi } }));
            o->setProperty("displays", ds);
            if constexpr (kInstrument)
            {
                juce::Array<juce::var> heldNow;
                for (std::size_t k = 0; k < 128; ++k)
                    if ((m1.heldKeys[k / 32] & (1u << (k % 32))) != 0)
                        heldNow.add(static_cast<int>(k));
                int lo = -1, hi = -1;
                for (int k = 0; k < 128; ++k)
                    if (notes.test(static_cast<std::size_t>(k)))
                    {
                        lo = lo < 0 ? k : lo;
                        hi = k;
                    }
                o->setProperty("midi", claude::object({ { "note_ons", static_cast<int>(noteOns) }, { "lowest", lo }, { "highest", hi },
                                                        { "velocity_min", velHi >= 0 ? velLo : -1 }, { "velocity_max", velHi },
                                                        { "held", heldNow }, { "modwheel", mod.now }, { "pitchbend", bend.now },
                                                        { "aftertouch", after.now }, { "pedal", pedalNow } }));
                o->setProperty("voices", claude::object({ { "most", mostVoices }, { "of", Patch::kVoices }, { "released", mostReleased },
                                                          { "pedal_held", mostPedal }, { "fading", mostFading },
                                                          { "steals", static_cast<int>(steals) }, { "dropped", static_cast<int>(dropped) } }));
            }
            if (!shares.empty())
                o->setProperty("cpu_percent", claude::object({ { "p50", 100.0 * pct(shares, 0.5) }, { "p95", 100.0 * pct(shares, 0.95) },
                                                               { "max", 100.0 * pct(shares, 1.0) }, { "worst_samples", static_cast<int>(worst.samples) },
                                                               { "worst_voices", static_cast<int>(worst.voices) },
                                                               { "worst_context", juce::String(flagWords(worst.flags)) } }));
            juce::Array<juce::var> al;
            if ((alerts & Engine::nonFinite) != 0)
                al.add("NaN or inf replaced by 0");
            if ((alerts & Engine::clipped) != 0)
                al.add("clipped beyond +-4");
            o->setProperty("alerts", al);
            o->setProperty("plays_the_editors_files", playsDraft);
            return juce::JSON::toString(root, true, 3).toStdString() + "\n";
        }

        std::string out = std::string("Live: ") + fmt("%.1f", took) + " s of what plays now in the user's song"
                        + (cut ? " (stopped early)" : "") + ". These numbers are live and cannot be reproduced "
                          "(measure_output renders offline).\n";
        // The host.
        out += "Host: ";
        if (m1.hostKnown)
            out += fmt("%.1f BPM", static_cast<double>(h.bpm)) + ", " + (h.playing ? "playing" : "stopped") + " at bar "
                 + std::to_string(bar) + fmt(", beat %.1f", beatInBar) + ", " + std::to_string(h.beatsPerBar) + "/"
                 + std::to_string(h.beatUnit);
        else
            out += "no transport from the host (the Standalone, or a host that says nothing): the plugin's own clock at 120 BPM";
        out += "; " + khz(rate) + ", blocks up to " + std::to_string(last.largestBlock) + " samples; ";
        out += bypassNow ? (kInstrument ? "bypassed (silent)" : "bypassed (the input passes)")
                         : (bypassWas ? "bypassed for part of it" : "not bypassed");
        out += "; " + std::to_string(blocks) + " blocks";
        if (blocks == 0)
            out += ": the host is not calling the plugin (a track disabled or frozen, the audio stopped, or a bypass on the "
                   "host's side)";
        out += ".\n";
        // The levels.
        if (blocks == 0 && !outL.any())
            out += "Levels: no blocks came, so nothing was measured.\n";
        else
        {
            if constexpr (!kInstrument)
                out += "Input: " + levelText(inL) + ".\n";
            out += "Output: " + levelText(outL) + (bypassNow ? " (the program's, unheard while bypassed)" : "") + ".\n";
            if (detail)
                for (int c = 0; c < 2; ++c)
                    out += std::string(c == 0 ? "  left: " : "  right: ") + (kInstrument ? "" : "in " + levelText(inCh[c]) + "; out ")
                         + levelText(outCh[c]) + "\n";
            if constexpr (!kInstrument)
            {
                if (!gains.empty())
                    out += "Gain in to out: " + fmt("%+.1f dB", pct(gains, 0.5)) + " (p10 " + fmt("%+.1f", pct(gains, 0.1)) + ", p90 "
                         + fmt("%+.1f", pct(gains, 0.9)) + "), while the input was above -60 dBFS.\n";
                else if (inL.any())
                    out += "Gain in to out: the input stayed under -60 dBFS.\n";
            }
            // The spectrum.
            const auto spectrumLine = [&](const char* what, const Spectrum& s, const Bands& b, double centroid,
                                          const std::vector<measure::Peak>& peaks) {
                std::string l = std::string(what) + " octave bands 31 Hz to 16 kHz, dB under the loudest: " + bandsText(b)
                              + "; centroid " + hzText(centroid);
                if (!peaks.empty())
                {
                    l += "; peaks";
                    for (std::size_t i = 0; i < peaks.size(); ++i)
                        l += (i == 0 ? " " : ", ") + hzText(peaks[i].hz)
                           + (i == 0 ? "" : " (" + dbWhole(peaks[i].db) + " dB)");
                }
                l += ".\n";
                if (detail)
                    l += std::string("  third-octave bands 20 Hz to 20 kHz: ") + bandsText(bandsOf(s, rate, true)) + "\n";
                return l;
            };
            if (!kInstrument && inS.frames > 0 && !inSilent)
                out += spectrumLine("Input spectrum:", inS, inB, inCentroid, inPeaks);
            if (outS.frames > 0 && !outSilent)
                out += spectrumLine(kInstrument ? "Spectrum:" : "Output spectrum:", outS, outB, outCentroid, outPeaks);
            else if (outS.frames > 0)
                out += "Spectrum: the output is silent.\n";
        }
        // The panel's displays.
        {
            std::string ds;
            for (std::size_t i = 0; i < shown.size() && i < last.displays.size(); ++i)
                if (shown[i].count > 0)
                {
                    const PanelDisplay& d = last.displays[i];
                    ds += (ds.empty() ? "" : "; ") + d.name + " (" + kindName(d.kind) + ") now " + num(shown[i].now)
                        + (shown[i].lo == shown[i].hi ? std::string() : ", " + num(shown[i].lo) + " to " + num(shown[i].hi));
                }
            if (!ds.empty())
                out += "Displays: " + ds + ".\n";
        }
        // The notes and the voices.
        if constexpr (kInstrument)
        {
            int lo = -1, hi = -1;
            for (int k = 0; k < 128; ++k)
                if (notes.test(static_cast<std::size_t>(k)))
                {
                    lo = lo < 0 ? k : lo;
                    hi = k;
                }
            std::string held;
            for (std::size_t k = 0; k < 128; ++k)
                if ((m1.heldKeys[k / 32] & (1u << (k % 32))) != 0)
                    held += (held.empty() ? "" : " ") + noteName(static_cast<int>(k));
            out += "MIDI: " + std::to_string(noteOns) + " note-on" + (noteOns == 1 ? "" : "s");
            if (lo >= 0)
                out += ", notes " + noteName(lo) + (hi != lo ? " to " + noteName(hi) : std::string());
            if (velHi >= 0)
                out += ", velocity " + std::to_string(velLo) + (velHi != velLo ? " to " + std::to_string(velHi) : std::string());
            out += "; held now: " + (held.empty() ? std::string("none") : held);
            const auto wheel = [&](const char* name, const Seen& s, const char* f) {
                std::string w = std::string(name) + " " + fmt(f, s.now);
                if (s.count > 0 && s.lo != s.hi)
                    w += " (" + fmt(f, s.lo) + " to " + fmt(f, s.hi) + ")";
                return w;
            };
            out += "; " + wheel("mod wheel", mod, "%.2f") + ", " + wheel("pitch bend", bend, "%+.2f") + ", "
                 + wheel("aftertouch", after, "%.2f") + ", pedal " + (pedalNow ? "down" : (pedalWas ? "up (down for part of it)" : "up"));
            if (noteOns == 0 && held.empty() && mostVoices == 0 && blocks > 0 && m1.hostKnown && h.playing)
                out += ". No note-ons while the host plays: no MIDI arrives (is the track armed, its input routed here?)";
            out += ".\n";
            out += "Voices: at most " + std::to_string(mostVoices) + " at once (of " + std::to_string(Patch::kVoices) + "), "
                 + std::to_string(mostReleased) + " released, " + std::to_string(mostPedal) + " held by the pedal, "
                 + std::to_string(mostFading) + " fading out; " + std::to_string(steals) + " stolen";
            if (dropped > 0)
            {
                const double ago = m1.samples >= m1.droppedAt ? static_cast<double>(m1.samples - m1.droppedAt) / rate : 0.0;
                out += "; " + std::to_string(dropped) + " dropped for going non-finite (NaN or inf; the last: "
                     + (m1.droppedKey >= 0 ? noteName(m1.droppedKey) : std::string("a note")) + fmt(", %.1f s ago)", ago);
            }
            else
                out += "; none dropped";
            if (noteOns > 0 && mostVoices == 0)
                out += ". Note-ons and no voice sounding: the program does not play its notes";
            out += ".\n";
        }
        // The CPU.
        if (!shares.empty())
            out += "CPU per block: p50 " + fmt("%.1f", 100.0 * pct(shares, 0.5)) + " %, p95 " + fmt("%.1f", 100.0 * pct(shares, 0.95))
                 + " %, max " + fmt("%.1f", 100.0 * pct(shares, 1.0)) + " % of the block's own time; the worst: "
                 + std::to_string(worst.samples) + " samples" + (kInstrument ? ", " + std::to_string(worst.voices) + " voices" : std::string())
                 + ", " + flagWords(worst.flags) + ".\n";
        // The alerts, and what plays.
        out += "Alerts: ";
        if (alerts == 0 && dropped == 0)
            out += "none";
        else
        {
            std::string a;
            if ((alerts & Engine::nonFinite) != 0)
                a += "NaN or inf samples replaced by 0";
            if ((alerts & Engine::clipped) != 0)
                a += std::string(a.empty() ? "" : "; ") + "samples beyond +-4 (+12 dBFS) clipped";
            out += a.empty() ? std::string("a voice dropped (above)") : a + " (the output guard)";
        }
        out += ".\n";
        out += playsDraft ? "What plays is the editor's files.\n"
                          : "What plays is an earlier version of the files: they were edited since the last run.\n";
        return out;
    }
}
