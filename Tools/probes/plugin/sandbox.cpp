// PROBE name=plugin.sandbox timeout=300
//
// plugin.sandbox: the GUI sandbox (Source/sandbox/, mcp/Main.cpp's gui_sandbox) through the product's real MCP server
// over stdio, as Claude Desktop runs it, and the real helper it starts beside itself (<Name>GuiSandbox), in a folder
// of the probe's own (no instance of the user's is reached). PNGs to probe-out/<key>/gui.sandbox*.png.
//   LIST     with no instance open, tools/list has gui_sandbox: titled in words, not read-only, its schema small; a
//            render before an open says to open first
//   BLANK    open blank with dials: the sandbox's number, the dials yours, no note, the report's elements
//   EDIT     an edit naming no dial: GuiSpec's note on its line, the control listed as naming no dial; a dial no
//            control names listed; an old_text that does not occur refused, nothing changed; an overlap and what
//            sticks out of its container reported, text too wide, and a panel taller than the area scrolling
//   RENDER   a PNG of the size asked (1.5 pixels a px) in both themes, which differ (gui.sandbox.png, .paper.png); a
//            dial's value moves its knob: the pictures differ inside its cell and nowhere else (gui.sandbox.knob.png);
//            a large render without detail made smaller to fit the MCP cap (100,000 base64 characters), and saying so
//   TWO      a second sandbox opened (another conversation's, say): laid out at the PANEL view's size, not the last
//            render's; a dial's unit shown only from the .gui's unit=, as in the plugin; an edit naming the first by
//            its number acts on the first (put back in the helper), a render naming the second shows the second as it
//            was; a number no sandbox has refused; apply without a number refused
//   SHADER   a program's folder opened from disk (the .fm, the .gui, the .sc; its dials declared; its fields listed
//            for the displays but those no display can show): the shader drawn by the GPU in its colour, not
//            FunkGui's striped placeholder (gui.sandbox.shader.png)
//   RESTART  the helper killed between calls: the next call starts it again, says so, with the panel as it was (the
//            same picture, at the last render's size)
//   NONE     apply with no instance open gives the .gui's text back, saying how the user can use it, by number (the
//            first sandbox's while another is open); close ends the helper; a sandbox kept comes back after it
//   MATCH    the prompt's example (ChatAgent::promptExample: its program, panel.gui and library files) playing in a
//            processor: the sandbox opened from that instance (its files read through the link, its dials parsed)
//            renders, in the user's theme, what view_panel's picture of it shows, pixel for pixel but the displays and
//            the live= marks (the sandbox's demo values against the processor's silence), within a small bound
//            (gui.sandbox.example.png, .example.view.png); reading the instance leaves the plugin's record of what
//            MCP was told alone (the user's change still starts the next result)
//   APPLY    an edit applied: panel.gui written into the processor's draft, run, its interface the sandbox's; the
//            user's change since the last MCP call said first
//   CIRCUIT  a program holding screamer.cir's module (an Effect's Screamer amp[2], a Voice's one), never calling its
//            set(), with a panel.gui of its dials, playing in a processor: the sandbox opened from that instance lists
//            the same dials, the circuit's (drive, tone, level) by the compiled program's names and defaults, and
//            renders what view_panel's picture shows, pixel for pixel within MATCH's bound (gui.sandbox.circuit.png,
//            .circuit.view.png)
//   MCPB     the extension (<Name>.mcpb) carries the server, the helper and the licences of what the helper holds
//   HANG     a helper that never answers a render (<ENV_PREFIX>SANDBOX_TEST_HANG): the call answered at the deadline
//            (<ENV_PREFIX>SANDBOX_DEADLINE_MS), saying so, the helper gone, and the next call working
//   IDLE     the helper idle past <ENV_PREFIX>SANDBOX_IDLE_MS ended by the server, and back on the next call, quietly
//   EXIT     the server gone while its helper hangs: the helper stopped too
#include "Probe.h"

#include "mcp/McpProtocol.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "sandbox/SandboxProtocol.h"

#include "editor/Panel.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <libproc.h>
#include <spawn.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : props)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string line(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }
    bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

    // A tool's result: its text, its picture (PNG bytes) and whether it is an error.
    struct Result
    {
        std::string text;
        std::string png;
        bool error = true;
        bool answered = false;
    };

    // The server as Claude Desktop runs it, on two pipes; requests one at a time, the processor's message-thread work
    // done while it waits (the link's calls reach the message thread through poll()).
    struct Server
    {
        pid_t pid = -1;
        int in = -1, out = -1;
        std::unique_ptr<pg::mcp::LineReader> reader;
        int next = 1;

        bool start(const std::string& path)
        {
            int toChild[2], fromChild[2];
            if (pipe(toChild) != 0 || pipe(fromChild) != 0)
                return false;
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_adddup2(&fa, toChild[0], 0);
            posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
            posix_spawn_file_actions_addclose(&fa, toChild[1]);
            posix_spawn_file_actions_addclose(&fa, fromChild[0]);
            char* argv[] = { const_cast<char*>(path.c_str()), nullptr };
            const int rc = posix_spawn(&pid, path.c_str(), &fa, nullptr, argv, environ);
            posix_spawn_file_actions_destroy(&fa);
            close(toChild[0]);
            close(fromChild[1]);
            in = toChild[1];
            out = fromChild[0];
            reader = std::make_unique<pg::mcp::LineReader>(out);
            return rc == 0;
        }

        // A request written, its answer not waited for: its id.
        int post(const char* method, const juce::var& params)
        {
            const int id = next++;
            const std::string msg = line(object({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } })) + "\n";
            [[maybe_unused]] const ssize_t n = write(in, msg.data(), msg.size());
            return id;
        }

        juce::var request(const char* method, const juce::var& params, pg::Processor* p = nullptr, int ms = 90000)
        {
            const int id = post(method, params);
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms))
            {
                std::string got;
                const int r = reader->next(got, 10);
                if (r < 0)
                    break;
                if (r > 0)
                {
                    const juce::var v = juce::JSON::parse(juce::String::fromUTF8(got.c_str()));
                    if (v.hasProperty("id") && static_cast<int>(v["id"]) == id)
                        return v;
                }
                if (p != nullptr)
                    p->poll();
            }
            return {};
        }

        Result sandbox(const juce::var& args, pg::Processor* p = nullptr, int ms = 90000)
        {
            const juce::var r = request("tools/call", object({ { "name", "gui_sandbox" }, { "arguments", args } }), p, ms);
            Result got;
            got.answered = r.hasProperty("result");
            got.error = static_cast<bool>(r["result"]["isError"]);
            if (const juce::Array<juce::var>* c = r["result"]["content"].getArray())
                for (const juce::var& b : *c)
                {
                    if (b["type"].toString() == "image")
                    {
                        juce::MemoryOutputStream bytes;
                        juce::Base64::convertFromBase64(bytes, b["data"].toString());
                        got.png.assign(static_cast<const char*>(bytes.getData()), bytes.getDataSize());
                    }
                    else if (b["type"].toString() == "text")
                        got.text += b["text"].toString().toStdString() + "\n";
                }
            return got;
        }

        // The helper processes it started that still run (its children, but those that ended and wait to be reaped).
        std::vector<pid_t> children() const
        {
            pid_t kids[16] = {};
            const int n = proc_listchildpids(pid, kids, sizeof kids);
            std::vector<pid_t> found;
            for (int k = 0; k < n && k < 16; ++k)
                if (kids[k] > 0 && running(kids[k]))
                    found.push_back(kids[k]);
            return found;
        }

        // Whether it has no helper running, within `ms` (a helper ended takes a moment to go).
        bool childless(int ms) const
        {
            const auto t0 = std::chrono::steady_clock::now();
            while (!children().empty())
            {
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return true;
        }

        static bool running(pid_t p)
        {
            proc_bsdinfo info{};
            return proc_pidinfo(p, PROC_PIDTBSDINFO, 0, &info, sizeof info) == static_cast<int>(sizeof info) && info.pbi_status != SZOMB;
        }

        void stop()
        {
            if (in >= 0)
                close(in);
            int status = 0;
            if (pid > 0)
                waitpid(pid, &status, 0);
            if (out >= 0)
                close(out);
        }
    };

    juce::Image decode(const std::string& png)
    {
        return png.empty() ? juce::Image() : juce::ImageFileFormat::loadFrom(png.data(), png.size());
    }

    bool writeBytes(const std::string& path, const std::string& bytes)
    {
        std::ofstream f(path, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::printf("wrote %s\n", path.c_str());
        return static_cast<bool>(f);
    }

    // A rect (logical px in the area) from the report's line of the element named `what` ("knob drive 10,20 60x80").
    bool rectOf(const std::string& report, const std::string& what, float& x, float& y, float& w, float& h)
    {
        const std::size_t at = report.find(what + " ");
        if (at == std::string::npos)
            return false;
        int xi = 0, yi = 0, wi = 0, hi = 0;
        if (std::sscanf(report.c_str() + at + what.size() + 1, "%d,%d %dx%d", &xi, &yi, &wi, &hi) != 4)
            return false;
        x = static_cast<float>(xi);
        y = static_cast<float>(yi);
        w = static_cast<float>(wi);
        h = static_cast<float>(hi);
        return true;
    }

    // Pixels where two pictures of one size differ by more than `tolerance` in a channel, inside and outside a rect
    // (pixels, grown by `grow`), and the largest difference outside it.
    struct Diff
    {
        int inside = 0, outside = 0, largestOutside = 0, total = 0;
        bool sameSize = false;
        std::map<std::pair<int, int>, int> tiles;      // the pixels off outside the masks, by 32-pixel tile
    };

    Diff compare(const juce::Image& a, const juce::Image& b, const std::vector<juce::Rectangle<int>>& masks, int tolerance)
    {
        Diff d;
        d.sameSize = a.isValid() && b.isValid() && a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
        if (!d.sameSize)
            return d;
        const juce::Image::BitmapData pa(a, juce::Image::BitmapData::readOnly), pb(b, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
            {
                const juce::Colour ca = pa.getPixelColour(x, y), cb = pb.getPixelColour(x, y);
                const int diff = std::max({ std::abs(ca.getRed() - cb.getRed()), std::abs(ca.getGreen() - cb.getGreen()),
                                            std::abs(ca.getBlue() - cb.getBlue()) });
                ++d.total;
                const bool masked = std::any_of(masks.begin(), masks.end(), [&](const juce::Rectangle<int>& m) { return m.contains(x, y); });
                if (masked)
                    d.inside += diff > tolerance ? 1 : 0;
                else
                {
                    d.outside += diff > tolerance ? 1 : 0;
                    d.largestOutside = std::max(d.largestOutside, diff);
                    if (diff > tolerance)
                        ++d.tiles[{ x / 32, y / 32 }];
                }
            }
        return d;
    }

    juce::Rectangle<int> pixels(float x, float y, float w, float h, int grow)
    {
        const float k = pg::sandbox::kDpi;
        return juce::Rectangle<int>(static_cast<int>(std::floor(x * k)) - grow, static_cast<int>(std::floor(y * k)) - grow,
                                    static_cast<int>(std::ceil(w * k)) + 2 * grow, static_cast<int>(std::ceil(h * k)) + 2 * grow);
    }

    juce::var dials()
    {
        juce::Array<juce::var> list;
        list.add(object({ { "name", "drive" }, { "range", juce::Array<juce::var>{ 0.0, 24.0 } }, { "default", 6.0 }, { "unit", "dB" } }));
        list.add(object({ { "name", "tone" }, { "range", juce::Array<juce::var>{ 200.0, 8000.0 } }, { "default", 2000.0 }, { "unit", "Hz" } }));
        list.add(object({ { "name", "wave" }, { "type", "int" }, { "options", juce::Array<juce::var>{ "SAW", "SQR", "TRI" } } }));
        list.add(object({ { "name", "spare" } }));
        return list;
    }

    // A program of the product's kind with two dials and two fields (a float a display can show, an array it cannot),
    // for the folder SHADER opens.
    std::string program()
    {
        return kInstrument ? "module Voice\n{\n    param float glow [[fminus::range(0, 1)]] [[fminus::default(0.7)]];\n"
                             "    param float level [[fminus::range(-36, 0)]] [[fminus::default(-12)]];\n    bool held;\n"
                             "    float shine;\n    float ring[4];\n"
                             "    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                             "    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
                           : "module Effect\n{\n    param float glow [[fminus::range(0, 1)]] [[fminus::default(0.7)]];\n"
                             "    param float level [[fminus::range(-36, 0)]] [[fminus::default(-12)]];\n"
                             "    float shine;\n    float ring[4];\n"
                             "    void process(float* restrict left, float* restrict right, int n) { }\n};\n";
    }

    // The sandbox's number an answer names ("Sandbox #3"), 0 for none.
    int numberIn(const std::string& text)
    {
        const std::size_t at = text.find("Sandbox #");
        return at == std::string::npos ? 0 : std::atoi(text.c_str() + at + 9);
    }
}

PROBE(plugin, sandbox)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dir = "/tmp/" + std::string(pg::product::kEnvPrefix) + "sbx-" + std::to_string(getpid());
    setenv((std::string(pg::product::kEnvPrefix) + "MCP_DIR").c_str(), dir.c_str(), 1);
    const char* serverPath = std::getenv((std::string(pg::product::kEnvPrefix) + "MCP_SERVER").c_str());
    const bool built = serverPath != nullptr && juce::File(serverPath).existsAsFile()
                    && juce::File(serverPath).getSiblingFile(juce::String(pg::sandbox::helperName())).existsAsFile();
    P.eq("built", built ? 1 : 0, 1);
    if (!built)
        return P.finish();

    Server s;
    P.eq("server.starts", s.start(serverPath) ? 1 : 0, 1);
    s.request("initialize", object({ { "protocolVersion", "2025-11-25" }, { "capabilities", juce::var(new juce::DynamicObject()) },
                                     { "clientInfo", object({ { "name", "probe" }, { "version", "1" } }) } }));

    // ---- LIST ----------------------------------------------------------------------------------------------------------
    {
        const juce::var list = s.request("tools/list", juce::var(new juce::DynamicObject()))["result"]["tools"];
        juce::var tool;
        if (const juce::Array<juce::var>* a = list.getArray())
            for (const juce::var& t : *a)
                if (t["name"].toString() == "gui_sandbox")
                    tool = t;
        const int bytes = juce::JSON::toString(tool, true).length();
        std::printf("gui_sandbox: %d bytes listed, title \"%s\"\n", bytes, tool["title"].toString().toRawUTF8());
        P.eq("list.listed", tool.isObject() && tool["title"].toString() == "Design a panel" && !static_cast<bool>(tool["annotations"]["readOnlyHint"])
                                && tool["description"].toString().contains("apply only when the user asks") ? 1 : 0, 1);
        P.le("list.bytes", bytes, 2600);
        const Result early = s.sandbox(object({ { "action", "render" } }));
        P.eq("list.open_first", early.error && has(early.text, "open it first") ? 1 : 0, 1);
    }

    // ---- BLANK -----------------------------------------------------------------------------------------------------------
    Result opened = s.sandbox(object({ { "action", "open" }, { "from", "blank" }, { "dials", dials() } }));
    std::printf("open:\n%s", opened.text.c_str());
    const int blank = numberIn(opened.text);
    std::printf("the first sandbox's number: %d\n", blank);
    P.eq("blank.opens", !opened.error && has(opened.text, "Sandbox #" + std::to_string(blank) + ", from a blank panel")
                            && has(opened.text, "yours: drive, tone, wave, spare") && has(opened.text, "No notes") && has(opened.text, "knob drive") ? 1 : 0, 1);
    P.in("blank.number_drawn", blank, 10001, 90000);   // at random: a number from before a restart names none of these

    // ---- EDIT ------------------------------------------------------------------------------------------------------------
    {
        Result r = s.sandbox(object({ { "action", "edit" },
                                      { "edits", juce::Array<juce::var>{ object({ { "old_text", "            knob spare\n" }, { "new_text", "" } }),
                                                                         object({ { "old_text", "knob drive" }, { "new_text", "knob drive size=large\n            knob nothere" } }) } } }));
        std::printf("edit:\n%s", r.text.c_str());
        // knob nothere is on line 6 of the blank panel (its comment line, panel, row, section, then drive's line)
        P.eq("edit.note_line", !r.error && has(r.text, "panel.gui:6:") && has(r.text, "there is no parameter nothere") ? 1 : 0, 1);
        P.eq("edit.nameless", has(r.text, "Controls that name no dial:\n  panel.gui:6:") ? 1 : 0, 1);
        P.eq("edit.missing_dial", has(r.text, "Dials no control names: spare") ? 1 : 0, 1);
        r = s.sandbox(object({ { "action", "edit" }, { "edits", juce::Array<juce::var>{ object({ { "old_text", "knob nowhere" }, { "new_text", "x" } }) } } }));
        P.eq("edit.unmatched", r.error && has(r.text, "occurs 0 times") && has(r.text, "Nothing changed") ? 1 : 0, 1);
        // An overlap, what sticks out of its container, text too wide, and a panel that scrolls, in their own panel.
        const std::string crowded = "row {\n    row maxwidth=100 { knob drive; knob tone }\n    knob wave\n}\n"
                                    "label \"A LABEL FAR TOO LONG FOR ITS ROOM\" maxwidth=80\nspacer height=900\n";
        r = s.sandbox(object({ { "action", "edit" }, { "text", juce::String(crowded) } }));
        std::printf("crowded:\n%s", r.text.c_str());
        P.eq("edit.overlap", has(r.text, "Overlaps:\n  knob tone (line 2) and knob wave (line 3)") ? 1 : 0, 1);
        P.eq("edit.outside", has(r.text, "knob tone (line 2) sticks out of its row (line 2)") ? 1 : 0, 1);
        P.eq("edit.too_wide", has(r.text, "label \"A LABEL FAR TOO LONG FOR ITS ROOM\" (line 5): its text") ? 1 : 0, 1);
        P.eq("edit.scrolls", has(r.text, "taller than the area by") && has(r.text, "so it scrolls") ? 1 : 0, 1);
        // Back to the blank panel's (drive large, wave, tone; spare left over), for the pictures.
        r = s.sandbox(object({ { "action", "open" }, { "from", "blank" }, { "dials", dials() } }));
        P.eq("edit.reopened", !r.error && numberIn(r.text) == blank + 1 ? 1 : 0, 1);
        opened = r;
    }

    // ---- RENDER ----------------------------------------------------------------------------------------------------------
    const auto pictureOf = [&](const juce::var& args) { return s.sandbox(args); };
    juce::Image graphite, paper;
    {
        const Result g = pictureOf(object({ { "action", "render" }, { "width", 520 }, { "height", 260 }, { "theme", "graphite" }, { "detail", "high" } }));
        const Result p = pictureOf(object({ { "action", "render" }, { "width", 520 }, { "height", 260 }, { "theme", "paper" }, { "detail", "high" } }));
        std::printf("render: %s", g.text.c_str());
        graphite = decode(g.png);
        paper = decode(p.png);
        P.eq("render.size", graphite.getWidth() == 780 && graphite.getHeight() == 390 && paper.getWidth() == 780 && paper.getHeight() == 390 ? 1 : 0, 1);
        P.eq("render.gpu", has(g.text, "drawn by the GPU") ? 1 : 0, 1);
        P.eq("render.paper_said", has(p.text, "paper") ? 1 : 0, 1);
        const Diff themes = compare(graphite, paper, {}, 8);
        P.ge("render.themes_differ", themes.outside, themes.total / 2);
        writeBytes(probe::outPath("gui.sandbox.png"), g.png);
        writeBytes(probe::outPath("gui.sandbox.paper.png"), p.png);

        // A dial's value: only its knob's cell changes.
        float x = 0, y = 0, w = 0, h = 0;
        P.eq("render.knob_found", rectOf(opened.text, "knob tone", x, y, w, h) ? 1 : 0, 1);
        const Result moved = pictureOf(object({ { "action", "render" }, { "width", 520 }, { "height", 260 }, { "theme", "graphite" }, { "detail", "high" },
                                                { "values", object({ { "tone", 7000.0 }, { "wave", "TRI" } }) } }));
        writeBytes(probe::outPath("gui.sandbox.knob.png"), moved.png);
        float bx = 0, by = 0, bw = 0, bh = 0;
        rectOf(opened.text, "buttons wave", bx, by, bw, bh);
        const Diff knob = compare(graphite, decode(moved.png), { pixels(x, y, w, h, 2), pixels(bx, by, bw, bh, 2) }, 4);
        std::printf("a value moved: %d pixels differ in its cell (and the buttons'), %d outside (largest %d)\n", knob.inside, knob.outside,
                    knob.largestOutside);
        P.ge("render.value_moves_knob", knob.inside, 50);
        P.eq("render.value_alone", knob.outside, 0);
    }

    // ---- TWO: a second sandbox beside the first, by number ----------------------------------------------------------------
    const int first = numberIn(opened.text);
    {
        // A second sandbox: laid out at the PANEL view's size (not the first's last render's, 520 x 260); its dial's
        // unit shown only as the .gui's unit= says (the plugin takes none from a declaration).
        juce::Array<juce::var> dB;
        dB.add(object({ { "name", "drive" }, { "range", juce::Array<juce::var>{ 0.0, 24.0 } }, { "default", 6.0 }, { "unit", "dB" } }));
        juce::Array<juce::var> plain;
        plain.add(object({ { "name", "drive" }, { "range", juce::Array<juce::var>{ 0.0, 24.0 } }, { "default", 6.0 } }));
        Result r = s.sandbox(object({ { "action", "open" }, { "text", "knob drive\n" }, { "dials", dB } }));
        const int second = numberIn(r.text);
        std::printf("second:\n%s", r.text.c_str());
        P.eq("two.numbered", first > 0 && second == first + 1 ? 1 : 0, 1);
        P.eq("two.area_default", has(r.text, "Layout in the area " + std::to_string(pg::sandbox::kDefaultWidth) + " x "
                                                 + std::to_string(pg::sandbox::kDefaultHeight) + " (the PANEL view") ? 1 : 0, 1);
        const auto picture = [&](int n) {
            return decode(s.sandbox(object({ { "action", "render" }, { "sandbox", n }, { "width", 320 }, { "height", 160 }, { "theme", "graphite" },
                                             { "detail", "high" } })).png);
        };
        const juce::Image withUnit = picture(second);
        s.sandbox(object({ { "action", "edit" }, { "sandbox", second }, { "dials", plain } }));
        const juce::Image without = picture(second);
        s.sandbox(object({ { "action", "edit" }, { "sandbox", second }, { "text", "knob drive unit=\"dB\"\n" } }));
        const juce::Image fromGui = picture(second);
        const Diff given = compare(withUnit, without, {}, 2), written = compare(without, fromGui, {}, 2);
        std::printf("a dial's unit: %d pixels differ given in its list, %d written as unit=\n", given.outside, written.outside);
        P.eq("two.unit_not_from_list", given.sameSize && given.outside == 0 ? 1 : 0, 1);
        P.ge("two.unit_from_gui", written.outside, 20);

        // The first again, by its number: the edit is the first's (its knob tone; the second has none), and the second
        // comes back as it was.
        r = s.sandbox(object({ { "action", "edit" }, { "sandbox", first }, { "edits", juce::Array<juce::var>{ object({ { "old_text", "knob tone" }, { "new_text", "knob tone size=small" } }) } } }));
        std::printf("the first again: %s", r.text.substr(0, r.text.find('\n') + 1).c_str());
        P.eq("two.first_by_number", !r.error && r.text.rfind("Sandbox #" + std::to_string(first) + ": panel.gui now", 0) == 0 ? 1 : 0, 1);
        const Result back = s.sandbox(object({ { "action", "render" }, { "sandbox", second }, { "width", 320 }, { "height", 160 }, { "theme", "graphite" },
                                               { "detail", "high" } }));
        const Diff same = compare(fromGui, decode(back.png), {}, 0);
        P.eq("two.second_as_it_was", has(back.text, "Sandbox #" + std::to_string(second) + ".") && same.sameSize && same.outside == 0 ? 1 : 0, 1);
        r = s.sandbox(object({ { "action", "render" }, { "sandbox", 9 } }));
        P.eq("two.unknown_number", r.error && has(r.text, "There is no sandbox #9 now") ? 1 : 0, 1);
        r = s.sandbox(object({ { "action", "apply" } }));
        P.eq("two.apply_needs_number", r.error && has(r.text, "apply needs sandbox") ? 1 : 0, 1);
    }

    // ---- SHADER ----------------------------------------------------------------------------------------------------------
    int shaderNumber = 0;
    {
        const std::filesystem::path folder = std::filesystem::path(dir) / "program";
        std::filesystem::create_directories(folder);
        std::ofstream(folder / pg::kProgramName) << program();
        std::ofstream(folder / "panel.gui") << "section \"GLOW\" {\n    shader \"glow.sc\" inputs=\"glow\" width=200 height=100\n    knob glow\n    knob level\n}\n";
        std::ofstream(folder / "glow.sc") << "void main()\n{\n    gl_FragColor = vec4(0.9, 0.1, 0.6, 1.0);\n}\n";
        const Result r = s.sandbox(object({ { "action", "open" }, { "from", "file" }, { "path", juce::String(folder.string()) } }));
        std::printf("folder:\n%s", r.text.c_str());
        shaderNumber = numberIn(r.text);
        P.eq("shader.declared", !r.error && has(r.text, "2 from the program's declarations") && has(r.text, "Shaders: 1 of 1 built") ? 1 : 0, 1);
        P.eq("shader.fields", has(r.text, "Fields a display may show: ") && has(r.text, "shine (float)") && !has(r.text, "ring") ? 1 : 0, 1);
        const Result pic = s.sandbox(object({ { "action", "render" }, { "width", 480 }, { "height", 200 }, { "detail", "high" } }));
        writeBytes(probe::outPath("gui.sandbox.shader.png"), pic.png);
        float x = 0, y = 0, w = 0, h = 0;
        const bool found = rectOf(r.text, "shader \"glow.sc\"", x, y, w, h);
        const juce::Image img = decode(pic.png);
        int magenta = 0, placeholder = 0, n = 0;
        if (found && img.isValid())
            for (int py = static_cast<int>((y + 4) * pg::sandbox::kDpi); py < static_cast<int>((y + h - 4) * pg::sandbox::kDpi); ++py)
                for (int px = static_cast<int>((x + 4) * pg::sandbox::kDpi); px < static_cast<int>((x + w - 4) * pg::sandbox::kDpi); ++px)
                {
                    const juce::Colour c = img.getPixelAt(px, py);
                    ++n;
                    magenta += c.getRed() > 200 && c.getGreen() < 60 && c.getBlue() > 120 ? 1 : 0;
                    placeholder += (c.getRed() == 28 && c.getGreen() == 30 && c.getBlue() == 34) || (c.getRed() == 46 && c.getGreen() == 49 && c.getBlue() == 55) ? 1 : 0;
                }
        std::printf("shader cell: %d pixels, %d in its colour, %d the placeholder's\n", n, magenta, placeholder);
        P.eq("shader.gpu", found && has(pic.text, "drawn by the GPU") && n > 0 ? 1 : 0, 1);
        P.ge("shader.drawn", magenta, n * 9 / 10);
        P.eq("shader.no_placeholder", placeholder, 0);
    }

    // ---- RESTART: the helper killed between calls ------------------------------------------------------------------------
    {
        const Result before = s.sandbox(object({ { "action", "render" }, { "width", 480 }, { "height", 200 }, { "detail", "high" } }));
        const std::vector<pid_t> kids = s.children();
        for (const pid_t k : kids)
            kill(k, SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const Result after = s.sandbox(object({ { "action", "render" }, { "detail", "high" } }));   // the last render's size
        const Diff same = compare(decode(before.png), decode(after.png), {}, 2);
        std::printf("restart: %zu helper(s) killed, then: %s", kids.size(), after.text.c_str());
        P.eq("restart.killed", kids.size() == 1 ? 1 : 0, 1);
        P.eq("restart.again", !after.error && has(after.text, "started again, with the panel as it was") && s.children().size() == 1 ? 1 : 0, 1);
        P.eq("restart.same_panel", same.sameSize && same.outside == 0 ? 1 : 0, 1);
    }

    // ---- NONE: apply with no instance; close -----------------------------------------------------------------------------
    {
        const Result r = s.sandbox(object({ { "action", "apply" }, { "sandbox", shaderNumber } }));
        P.eq("none.apply_text", !r.error && has(r.text, "nothing was applied") && has(r.text, "shader \"glow.sc\"") && has(r.text, "paste it") ? 1 : 0, 1);
        const Result older = s.sandbox(object({ { "action", "apply" }, { "sandbox", first } }));
        P.eq("none.apply_by_number", !older.error && has(older.text, "Sandbox #" + std::to_string(first) + "'s panel.gui")
                                         && has(older.text, "knob tone size=small") && !has(older.text, "glow.sc") ? 1 : 0, 1);
        const Result c = s.sandbox(object({ { "action", "close" }, { "sandbox", shaderNumber } }));
        P.eq("none.closed", !c.error && has(c.text, "its helper ended") && s.childless(3000) ? 1 : 0, 1);
        const Result after = s.sandbox(object({ { "action", "edit" }, { "text", "label \"X\"" } }));
        P.eq("none.closed_empty", after.error && has(after.text, "open it first") ? 1 : 0, 1);
        const Result kept = s.sandbox(object({ { "action", "render" }, { "sandbox", first }, { "width", 320 }, { "height", 160 } }));
        P.eq("none.kept_after_close", !kept.error && !kept.png.empty() && has(kept.text, "Sandbox #" + std::to_string(first) + ".") ? 1 : 0, 1);
    }

    // ---- MATCH and APPLY: a processor playing the prompt's example -------------------------------------------------------
    {
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        const pg::Sources example = pg::ChatAgent::promptExample();
        port.setDraft({ example, 0 });
        port.run(example);
        processor.settle();
        P.eq("match.compiled", port.status().ok && !port.panel().gui.empty() ? 1 : 0, 1);
        // view_panel's picture of it (the PANEL view at the editor's default size), and where its displays and live=
        // marks are (a Panel of the probe's own, in the PANEL view).
        const pg::PanelPicture view = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        std::vector<juce::Rectangle<int>> masks;
        int areaW = 0, areaH = 0;
        {
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, 0, pg::sandbox::kDpi);
            host.settle(120);
            panel.setPanelView(true);
            host.settle(120);
            const pg::ui::GuiLayout& gl = panel.guiLayout();
            areaW = static_cast<int>(std::lround(gl.area.w));
            areaH = static_cast<int>(std::lround(gl.area.h));
            const std::vector<pg::GuiNode>& nodes = panel.panelSpec().gui.nodes;
            for (std::size_t i = 0; i < nodes.size() && i < gl.rects.size(); ++i)
                if (nodes[i].kind == pg::GuiKind::display || nodes[i].live >= 0)
                {
                    const funkgui::Rect& r = gl.rects[i];
                    masks.push_back(pixels(r.x - gl.area.x, r.y - gl.area.y, r.w, r.h, 3));
                }
            port.setPanelView(false);
        }
        P.eq("match.area", areaW == pg::sandbox::kDefaultWidth && areaH == pg::sandbox::kDefaultHeight ? 1 : 0, 1);
        // MCP's look at the plugin (the plugin's record of what it was told), then the host moving a dial: the sandbox's
        // read of the instance leaves that record alone, so the change still starts the next result.
        pg::ChatAgent& agent = processor.chatAgent();
        agent.mcpCall("get_status", juce::var(new juce::DynamicObject()));
        processor.getParameters()[0]->setValueNotifyingHost(0.25f);
        processor.poll();
        const Result opened2 = s.sandbox(object({ { "action", "open" }, { "from", "instance" } }), &processor);
        std::printf("from the instance:\n%s", opened2.text.c_str());
        P.eq("match.opened", !opened2.error && has(opened2.text, "from the program of") && has(opened2.text, "from the program's declarations") ? 1 : 0, 1);
        {
            std::string hinted;
            const pg::ChatAgent::McpResult read = agent.mcpCall("read_file", object({ { "name", "panel.gui" } }));
            if (const juce::Array<juce::var>* blocks = read.content.getArray())
                for (const juce::var& b : *blocks)
                    hinted += b["text"].toString().toStdString();
            std::printf("the next MCP result after the sandbox's read: %s\n", hinted.substr(0, hinted.find('\n')).c_str());
            P.eq("peek.cursor_kept", hinted.rfind("(The user changed 1 thing since the last MCP call here", 0) == 0 ? 1 : 0, 1);
        }
        const int exampleNumber = numberIn(opened2.text);
        const std::string theme = funkgui::UiPreferences::get().theme() == 1 ? "paper" : "graphite";
        const Result mine = s.sandbox(object({ { "action", "render" }, { "sandbox", exampleNumber }, { "detail", "high" } }), &processor);   // the user's theme
        P.eq("match.users_theme", has(mine.text, theme + " (the user's theme in the plugin)") ? 1 : 0, 1);
        writeBytes(probe::outPath("gui.sandbox.example.png"), mine.png);
        writeBytes(probe::outPath("gui.sandbox.example.view.png"), view.png);
        const Diff d = compare(decode(view.png), decode(mine.png), masks, 4);
        std::printf("against view_panel (%s; %zu displays and live marks masked): %d of %d pixels differ by more than 4 outside "
                    "them (largest %d), %d inside\n", view.about.c_str(), masks.size(), d.outside, d.total, d.largestOutside, d.inside);
        {
            // Where they differ, for a look: the view's picture dimmed, the pixels off in red, the masks in blue.
            juce::Image a = decode(view.png), b = decode(mine.png);
            if (d.sameSize)
            {
                juce::Image diff(juce::Image::RGB, a.getWidth(), a.getHeight(), true);
                for (int y = 0; y < a.getHeight(); ++y)
                    for (int x = 0; x < a.getWidth(); ++x)
                    {
                        const juce::Colour ca = a.getPixelAt(x, y), cb = b.getPixelAt(x, y);
                        const int off = std::max({ std::abs(ca.getRed() - cb.getRed()), std::abs(ca.getGreen() - cb.getGreen()),
                                                   std::abs(ca.getBlue() - cb.getBlue()) });
                        const bool masked = std::any_of(masks.begin(), masks.end(), [&](const juce::Rectangle<int>& m) { return m.contains(x, y); });
                        diff.setPixelAt(x, y, off > 4 ? juce::Colours::red : masked ? juce::Colours::darkblue : ca.withMultipliedBrightness(0.4f));
                    }
                juce::File out(juce::String(probe::outPath("gui.sandbox.example.diff.png")));
                out.deleteFile();
                juce::FileOutputStream stream(out);
                juce::PNGImageFormat().writeImageToStream(diff, stream);
            }
        }
        for (const auto& [tile, n] : d.tiles)
            if (n >= 16)
                std::printf("  pixels off in the tile at %d,%d (pixels): %d\n", tile.first * 32, tile.second * 32, n);
        P.eq("match.same_size", d.sameSize ? 1 : 0, 1);
        P.le("match.pixels_off", static_cast<double>(d.outside), 0.002 * d.total);
        P.le("match.largest", d.largestOutside, 40);

        // Large and without detail: made smaller to fit the MCP cap.
        {
            const Result big = s.sandbox(object({ { "action", "render" }, { "width", 1700 }, { "height", 1200 } }), &processor);
            const juce::Image bigImage = decode(big.png);
            std::printf("large: %zu bytes, %d x %d: %s", big.png.size(), bigImage.getWidth(), bigImage.getHeight(), big.text.c_str());
            P.le("render.capped_bytes", static_cast<double>(big.png.size()), 75000.0);
            P.eq("render.capped_said", bigImage.isValid() && bigImage.getWidth() < 2550 && has(big.text, "made smaller to fit") ? 1 : 0, 1);
        }

        // APPLY: a change in the sandbox, written into the processor's program and run.
        const Result e = s.sandbox(object({ { "action", "edit" }, { "sandbox", exampleNumber },
                                            { "edits", juce::Array<juce::var>{ object({ { "old_text", "panel " }, { "new_text", "label \"APPLIED HERE\"\npanel " } }) } } }),
                                   &processor);
        P.eq("apply.edited", !e.error ? 1 : 0, 1);
        processor.getParameters()[1]->setValueNotifyingHost(0.75f);   // the host again, another dial (a move of the same
                                                                        // one so soon would join the first, already told)
        processor.poll();
        const Result a = s.sandbox(object({ { "action", "apply" }, { "sandbox", exampleNumber } }), &processor);
        std::printf("apply: %s", a.text.substr(0, 400).c_str());
        processor.settle();
        std::string written;
        for (const pg::SourceFile& f : port.draft().files)
            if (f.name == "panel.gui")
                written = f.text;
        bool labelled = false;
        for (const pg::GuiNode& n : port.panel().gui.nodes)
            labelled = labelled || (n.kind == pg::GuiKind::label && n.text == "APPLIED HERE");
        P.eq("apply.written", !a.error && has(a.text, "wrote panel.gui into its program") && has(written, "label \"APPLIED HERE\"") ? 1 : 0, 1);
        P.eq("apply.runs", port.status().ok && labelled ? 1 : 0, 1);
        P.eq("apply.hint", a.text.rfind("(The user changed 1 thing since the last MCP call here", 0) == 0 ? 1 : 0, 1);
        s.sandbox(object({ { "action", "close" }, { "sandbox", exampleNumber } }), &processor);
    }

    // ---- CIRCUIT: a circuit program's automatic dials, as the sandbox reads them ---------------------------------------
    {
        const pg::LibraryFile* lib = pg::findLibraryFile("screamer.cir");
        const std::string program = kInstrument
            ? "#include \"screamer.cir\"\nmodule Voice\n{\n    param float volume [[fminus::range(0, 1)]] [[fminus::default(0.8)]];\n"
              "    Screamer amp;\n    float phase;\n    bool held;\n\n    void init(float rate)\n    {\n        amp.init(rate);\n    }\n\n"
              "    void start(float note, float velocity)\n    {\n        held = true;\n        amp.clear();\n    }\n\n"
              "    void stop()\n    {\n        held = false;\n    }\n\n"
              "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            phase += 0.01f;\n            if (phase >= 1)\n                phase -= 1;\n"
              "            const float y = volume[i] * amp.tick(phase - 0.5f);\n            left[i] += y;\n            right[i] += y;\n"
              "        }\n        return held;\n    }\n};\n"
            : "#include \"screamer.cir\"\nmodule Effect\n{\n    param float volume [[fminus::range(0, 1)]] [[fminus::default(0.8)]];\n"
              "    Screamer amp[2];\n\n    void init(float rate)\n    {\n        amp[0].init(rate);\n        amp[1].init(rate);\n    }\n\n"
              "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            left[i] = volume[i] * amp[0].tick(left[i]);\n            right[i] = volume[i] * amp[1].tick(right[i]);\n"
              "        }\n    }\n};\n";
        const std::string circuitGui = "panel {\n    row wrap {\n        section \"PEDAL\" {\n            knob drive\n            knob tone\n"
                                "            knob level\n        }\n        section \"OUT\" {\n            knob volume\n        }\n    }\n}\n";
        const pg::Sources files = { { pg::kProgramName, program }, { "screamer.cir", lib != nullptr ? lib->source : "" }, { "panel.gui", circuitGui } };
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        port.setDraft({ files, 0 });
        port.run(files);
        processor.settle();
        std::string names, defaults;
        for (const pg::Control& c : port.controls())
            if (c.used)
            {
                names += (names.empty() ? "" : " ") + std::string(c.name);
                if (std::string(c.name) != "volume")
                    defaults += (defaults.empty() ? "" : ", ") + std::string(c.name) + " " + pg::dialText(c, c.def);
            }
        std::printf("circuit: %s%s; dials %s (%s)\n", port.status().errors.c_str(), port.status().notes.c_str(), names.c_str(), defaults.c_str());
        P.eq("circuit.compiled", port.status().ok && names == "volume drive tone level" && !port.panel().gui.empty() ? 1 : 0, 1);
        const pg::PanelPicture view = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        const Result circuitOpen = s.sandbox(object({ { "action", "open" }, { "from", "instance" } }), &processor);
        std::printf("from the instance:\n%s", circuitOpen.text.c_str());
        P.eq("circuit.same_dials", !circuitOpen.error && has(circuitOpen.text, "Dials: 4, 4 from the program's declarations")
                                       && has(circuitOpen.text, "Circuit dials (the program's circuits' pots and switches, after its params, as the "
                                                           "plugin makes them): " + defaults + ".")
                                       && !has(circuitOpen.text, "no parameter") ? 1 : 0, 1);
        const int number = numberIn(circuitOpen.text);
        const Result mine = s.sandbox(object({ { "action", "render" }, { "sandbox", number }, { "detail", "high" } }), &processor);
        writeBytes(probe::outPath("gui.sandbox.circuit.png"), mine.png);
        writeBytes(probe::outPath("gui.sandbox.circuit.view.png"), view.png);
        const Diff d = compare(decode(view.png), decode(mine.png), {}, 4);
        std::printf("against view_panel: %d of %d pixels differ by more than 4 (largest %d)\n", d.outside, d.total, d.largestOutside);
        for (const auto& [tile, n] : d.tiles)
            if (n >= 16)
                std::printf("  pixels off in the tile at %d,%d (pixels): %d\n", tile.first * 32, tile.second * 32, n);
        P.eq("circuit.match_same_size", d.sameSize ? 1 : 0, 1);
        P.le("circuit.match_pixels_off", static_cast<double>(d.outside), 0.002 * d.total);
        P.le("circuit.match_largest", d.largestOutside, 40);
        s.sandbox(object({ { "action", "close" }, { "sandbox", number } }), &processor);
    }
    s.stop();

    // ---- MCPB: the extension's contents ----------------------------------------------------------------------------------
    {
        const juce::File mcpb = juce::File(serverPath).getSiblingFile(juce::String(pg::product::kName) + ".mcpb");
        juce::ZipFile zip(mcpb);
        std::set<std::string> entries;
        for (int k = 0; k < zip.getNumEntries(); ++k)
            entries.insert(zip.getEntry(k)->filename.toStdString());
        std::string listed;
        for (const std::string& e : entries)
            listed += " " + e;
        std::printf("%s:%s\n", mcpb.getFileName().toRawUTF8(), listed.c_str());
        const std::string server = "server/" + std::string(pg::product::kName) + "MCP", helper = "server/" + pg::sandbox::helperName();
        P.eq("mcpb.programs", static_cast<int>(entries.count(server) + entries.count(helper)), 2);
        int licences = 0;
        for (const char* l : { "JetBrainsMono-LICENSE.txt", "bgfx-LICENSE.txt", "bx-LICENSE.txt", "bimg-LICENSE.txt", "bgfx.cmake-LICENSE.txt" })
            licences += static_cast<int>(entries.count(std::string("licences/") + l));
        P.eq("mcpb.licences", licences, 5);
    }

    // ---- HANG: a helper that never answers a render ----------------------------------------------------------------------
    {
        const std::string hang = std::string(pg::product::kEnvPrefix) + "SANDBOX_TEST_HANG", deadline = std::string(pg::product::kEnvPrefix) + "SANDBOX_DEADLINE_MS",
                          idle = std::string(pg::product::kEnvPrefix) + "SANDBOX_IDLE_MS";
        setenv(hang.c_str(), "render", 1);
        setenv(deadline.c_str(), "3000", 1);
        setenv(idle.c_str(), "1500", 1);
        Server h;
        const bool started = h.start(serverPath);
        unsetenv(hang.c_str());
        unsetenv(deadline.c_str());
        unsetenv(idle.c_str());
        P.eq("hang.starts", started ? 1 : 0, 1);
        h.request("initialize", object({ { "protocolVersion", "2025-11-25" }, { "capabilities", juce::var(new juce::DynamicObject()) } }));
        const Result o = h.sandbox(object({ { "action", "open" }, { "from", "blank" }, { "dials", dials() } }));
        const int hung = numberIn(o.text);
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = h.sandbox(object({ { "action", "render" } }), nullptr, 20000);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("hung: answered after %.1f s: %s", seconds, r.text.c_str());
        P.eq("hang.answered", !o.error && r.answered && r.error && has(r.text, "did not answer in 3 s") ? 1 : 0, 1);
        P.in("hang.at_deadline", seconds, 2.5, 8.0);
        P.eq("hang.helper_gone", h.childless(2000) ? 1 : 0, 1);
        const Result next = h.sandbox(object({ { "action", "edit" }, { "edits", juce::Array<juce::var>{ object({ { "old_text", "knob tone" }, { "new_text", "knob tone size=small" } }) } } }));
        P.eq("hang.next_works", !next.error && hung > 0 && has(next.text, "Sandbox #" + std::to_string(hung)) ? 1 : 0, 1);

        // IDLE: past 1.5 s with no request the server ends its helper; the next call brings it back, saying nothing of it.
        P.eq("idle.running", h.children().size() == 1 ? 1 : 0, 1);
        P.eq("idle.ended", h.childless(6000) ? 1 : 0, 1);
        const Result quiet = h.sandbox(object({ { "action", "edit" }, { "sandbox", hung }, { "edits", juce::Array<juce::var>{ object({ { "old_text", "size=small" }, { "new_text", "size=large" } }) } } }));
        P.eq("idle.back_quietly", !quiet.error && quiet.text.rfind("Sandbox #" + std::to_string(hung) + ": panel.gui now", 0) == 0 && h.children().size() == 1 ? 1 : 0, 1);

        // EXIT: a render that hangs, and the server gone meanwhile: the helper does not outlive it.
        const std::vector<pid_t> before = h.children();
        h.post("tools/call", object({ { "name", "gui_sandbox" }, { "arguments", object({ { "action", "render" } }) } }));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        h.stop();
        bool stopped = before.size() == 1;
        for (int k = 0; k < 100 && stopped && Server::running(before.front()); ++k)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        P.eq("exit.helper_stopped", stopped && !Server::running(before.front()) ? 1 : 0, 1);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return P.finish();
}
