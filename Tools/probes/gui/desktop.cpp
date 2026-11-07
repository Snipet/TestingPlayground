// PROBE name=gui.desktop timeout=180
//
// gui.desktop: Claude Desktop's target as the plugin shows it (the explorer's CLAUDE DESKTOP, editor/Explorer.h; the chat
// header's subtitle and badge; the name card, editor/NameCard.h; EditorPort::desktop, plugin/McpLink.h), headless over a
// real Processor beside a second one, in the probe's own MCP folder, Claude Desktop played by the probe on the first's
// socket, to <build>/probe-out/:
//   gui.desktop.png         the first named "Bass", Claude Desktop's target (chosen), a call acting on it (lit), the
//                           second on its track among the others
//   gui.desktop.paper.png   the same in PAPER
//   gui.desktop.badge.png   the second chosen: the first's badge "Desktop uses Synth 2" (or Effect 2), its row ticked
//   gui.desktop.rail.png    the same with the chat folded: the badge's mark on the rail
//   gui.desktop.name.png    the name card, a name typed
// and checks:
//   ALONE    the section after FOLDER and before LLVM IR: its name ("Synth 1", this synth) and Use with Claude Desktop,
//            ticked (the only one open), no header button
//   OTHERS   a second processor open (its track "Lead"): ALSO OPEN, its row (its name, its track); Use with Claude
//            Desktop no longer ticked; Claude Desktop connected (a request on the socket), the badge "Desktop: none
//            chosen", and the subtitle none
//   CHOOSE   Use with Claude Desktop's click: the processor's choice, the row ticked, no badge, the subtitle "Claude
//            Desktop calls this Synth 1", and the second sees the first as the target; a second click clears it
//   OTHER    the second's row clicked: it is the target, ticked; the badge "Desktop uses Synth 2"; the badge's click
//            makes the first the target again
//   MARK     the second chosen again, the chat folded: no badge in the header, its mark on the rail under the rail's
//            button; the mark's click makes the first the target (the mark gone, the chat still folded)
//   RENAME   the name row's click opens the name card, the field its name (empty: its number's); a name another's
//            number's refused, said in the card, which stays; "Bass" and Return name it (the row "Bass", its detail its
//            number's name; the subtitle); Escape leaves it; an empty name gives its number's back
//   LIVE     a call (listen, 4 s) on the socket: the last call's row lit ("Listen live", now) and the subtitle "Claude
//            Desktop: Listen live" while it runs; then "just now", not lit; its click opens a folded chat
//   A11Y     the section's rows are buttons, Use with Claude Desktop checked; the badge a button
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/Panel.h"
#include "mcp/McpProtocol.h"
#include "plugin/EditorPort.h"
#include "plugin/McpLink.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pg = playground;
using S = pg::ui::ExplorerItem::Section;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    void clickAt(funkgui::HeadlessHost& host, const std::optional<funkgui::Point>& p)
    {
        if (p)
            host.click(p->x, p->y);
        host.tick(2);
        host.settle();
    }

    int find(const pg::ui::ExplorerItem& ex, S s, const std::string& label)
    {
        for (int i = 0; i < ex.rows(s); ++i)
            if (const auto* r = ex.row(s, i); r != nullptr && r->label == label)
                return i;
        return -1;
    }

    // The processors' timers and the Panel's frames, every 20 ms of real time, until `done` or `seconds`.
    bool pump(const std::vector<pg::Processor*>& ps, funkgui::HeadlessHost& host, double seconds, const std::function<bool()>& done)
    {
        const auto from = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
        {
            for (pg::Processor* p : ps)
                p->poll();
            host.tick(1);
            if (done && done())
            {
                host.settle();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        host.settle();
        return done ? done() : true;
    }

    // Types `text` (no commas) as the keys it takes.
    void typeText(funkgui::HeadlessHost& host, const std::string& text)
    {
        std::string spec;
        for (const char ch : text)
            spec += (spec.empty() ? "" : ",") + (ch == ' ' ? std::string("space") : std::string(1, ch));
        host.keys(spec.c_str());
        host.tick(2);
        host.settle();
    }

    // Claude Desktop's server, as the probe plays it: a connection to an instance's socket, a request a line.
    struct Desktop
    {
        int fd = -1;
        std::unique_ptr<pg::mcp::LineReader> reader;
        int next = 1;

        explicit Desktop(const pg::McpLink* link)
        {
            if (link != nullptr)
                fd = pg::mcp::connectTo(link->socketPath());
            if (fd >= 0)
                reader = std::make_unique<pg::mcp::LineReader>(fd);
        }
        ~Desktop()
        {
            if (fd >= 0)
                pg::mcp::closeFd(fd);
        }
        Desktop(const Desktop&) = delete;
        Desktop& operator=(const Desktop&) = delete;

        int send(const char* method, const juce::var& params)
        {
            const int id = next++;
            auto* o = new juce::DynamicObject();
            const juce::var msg(o);
            o->setProperty("id", id);
            o->setProperty("method", method);
            o->setProperty("params", params);
            if (fd >= 0)
                pg::mcp::writeLine(fd, juce::JSON::toString(msg, true).toStdString());
            return id;
        }
        bool reply(int ms)
        {
            std::string line;
            return reader != nullptr && reader->next(line, ms) > 0;
        }
    };

    juce::var listenCall(double seconds)
    {
        auto* args = new juce::DynamicObject();
        args->setProperty("seconds", seconds);
        auto* o = new juce::DynamicObject();
        o->setProperty("name", "listen");
        o->setProperty("arguments", juce::var(args));
        return juce::var(o);
    }
}

PROBE(gui, desktop)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    // A folder of its own for the sockets, short enough for a socket's path (the build's sandbox is too deep).
#if defined(_WIN32)
    const std::string dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "desktop-" + juce::String(getpid()))
                                .getFullPathName()
                                .replaceCharacter('\\', '/')
                                .toStdString();
#else
    const std::string dir = "/tmp/" + std::string(pg::product::kEnvPrefix) + "desktop-" + std::to_string(getpid());
#endif
    setenv((std::string(pg::product::kEnvPrefix) + "MCP_DIR").c_str(), dir.c_str(), 1);
    struct Gone
    {
        std::string dir;
        ~Gone() { juce::File(juce::String::fromUTF8(dir.c_str())).deleteRecursively(); }   // after the processors
    } gone{ dir };
    const std::string kind = kInstrument ? "Synth" : "Effect";
    const std::string first = kind + " 1", second = kind + " 2";

    pg::Processor a;
    pg::EditorPort& port = a.editorPort();
    pg::ui::Panel panel(a.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    a.settle();
    host.tick(4);
    host.settle();
    pg::ui::ExplorerItem& ex = panel.explorer();
    const pg::ui::ExplorerItem::Row none;
    const auto at = [&](int i) -> const pg::ui::ExplorerItem::Row& {
        const auto* r = ex.row(S::desktop, i);
        return r != nullptr ? *r : none;
    };
    P.eq("linked", a.mcpLink() != nullptr && a.mcpLink()->listening() && a.mcpLink()->id() == "1" ? 1 : 0, 1);

    // ---- ALONE ---------------------------------------------------------------------------------------------------------
    pump({ &a }, host, 3.0, [&] { return ex.rows(S::desktop) >= 2; });
    {
        const auto* name = ex.row(S::desktop, 0);
        const auto* use = ex.row(S::desktop, 1);
        P.eq("alone.rows", ex.rows(S::desktop), 2);
        P.eq("alone.name", name != nullptr && name->label == first && name->detail == "this " + std::string(kInstrument ? "synth" : "effect") ? 1 : 0, 1);
        P.eq("alone.use_ticked", use != nullptr && use->label == "Use with Claude Desktop" && use->marked && use->detail == "only one open" ? 1 : 0, 1);
        P.eq("alone.no_button", ex.centreOfAction(S::desktop).has_value() ? 0 : 1, 1);
        const auto folder = ex.centreOfHeader(S::folder), desktop = ex.centreOfHeader(S::desktop), ir = ex.centreOfHeader(S::ir);
        P.eq("alone.after_folder", folder && desktop && ir && folder->y < desktop->y && desktop->y < ir->y ? 1 : 0, 1);
        P.eq("alone.no_badge", panel.desktopBadge().empty() && panel.desktopLine().empty() ? 1 : 0, 1);
    }

    // ---- OTHERS --------------------------------------------------------------------------------------------------------
    pg::Processor b;
    juce::AudioProcessor::TrackProperties track;
    track.name = "Lead";
    b.updateTrackProperties(track);
    Desktop desktop(a.mcpLink());
    desktop.send("info", juce::var(new juce::DynamicObject()));
    const bool answered = desktop.reply(5000);
    const bool others = pump({ &a, &b }, host, 4.0, [&] {
        const int k = find(ex, S::desktop, second);
        return k >= 0 && at(k).detail == "Lead" && !panel.desktopBadge().empty();
    });
    {
        const int also = find(ex, S::desktop, "ALSO OPEN");
        const int other = find(ex, S::desktop, second);
        std::printf("others: rows %d, badge \"%s\"\n", ex.rows(S::desktop), panel.desktopBadge().c_str());
        P.eq("others.listed", others && also >= 2 && at(also).heading && other == also + 1
                                  && at(other).detail == "Lead" && !at(other).marked ? 1 : 0, 1);
        P.eq("others.use_unticked", !at(1).marked && at(1).detail.empty() ? 1 : 0, 1);
        P.eq("others.connected_badge", answered && port.desktop().connected && panel.desktopBadge() == "Desktop: none chosen"
                                           && panel.desktopLine().empty() ? 1 : 0, 1);
    }

    // ---- CHOOSE --------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::desktop, 1));
    pump({ &a, &b }, host, 0.2, {});
    {
        const pg::DesktopState d = port.desktop();
        const bool bSees = pump({ &a, &b }, host, 3.0, [&] { return b.editorPort().desktop().targetName == first; });
        P.eq("choose.chosen", d.chosen && d.target && at(1).marked ? 1 : 0, 1);
        P.eq("choose.no_badge", panel.desktopBadge().empty() && panel.desktopLine() == "Claude Desktop calls this " + first ? 1 : 0, 1);
        P.eq("choose.others_see", bSees ? 1 : 0, 1);
        clickAt(host, ex.centreOfRow(S::desktop, 1));
        P.eq("choose.cleared", !port.desktop().chosen && !at(1).marked && panel.desktopBadge() == "Desktop: none chosen" ? 1 : 0, 1);
    }

    // ---- OTHER ---------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::desktop, find(ex, S::desktop, second)));
    {
        const int other = find(ex, S::desktop, second);
        const bool bSees = pump({ &a, &b }, host, 3.0, [&] { return b.editorPort().desktop().chosen; });
        P.eq("other.chosen", bSees && other >= 0 && at(other).marked
                                 && panel.desktopBadge() == "Desktop uses " + second ? 1 : 0, 1);
        writePng(P, host, "gui.desktop.badge.png");
        const std::optional<funkgui::Point> badge = panel.centreOfDesktopBadge();
        clickAt(host, badge);
        P.eq("other.badge_click", badge.has_value() && port.desktop().chosen && panel.desktopBadge().empty() ? 1 : 0, 1);
    }

    // ---- MARK ----------------------------------------------------------------------------------------------------------
    {
        b.editorPort().chooseDesktop(b.mcpLink()->id(), true);
        const bool elsewhere = pump({ &a, &b }, host, 3.0, [&] { return panel.desktopBadge() == "Desktop uses " + second; });
        panel.setChatOpen(false);
        host.tick(2);
        host.settle();
        const std::optional<funkgui::Point> mark = panel.centreOfDesktopMark();
        P.eq("mark.folded", elsewhere && !panel.chatOpen() && !panel.centreOfDesktopBadge().has_value() && mark.has_value() ? 1 : 0, 1);
        writePng(P, host, "gui.desktop.rail.png");
        clickAt(host, mark);
        P.eq("mark.click", port.desktop().chosen && !panel.chatOpen() && !panel.centreOfDesktopMark().has_value() ? 1 : 0, 1);
        panel.setChatOpen(true);
        host.tick(2);
        host.settle();
    }

    // ---- RENAME --------------------------------------------------------------------------------------------------------
    {
        clickAt(host, ex.centreOfRow(S::desktop, 0));
        pg::ui::NameCard& card = panel.nameCard();
        P.eq("rename.opens", card.isOpen() && card.text().empty() ? 1 : 0, 1);
        typeText(host, second);
        host.keys("return");
        host.tick(2);
        host.settle();
        std::printf("refused: %s\n", card.why().c_str());
        P.eq("rename.refused_stays", card.isOpen() && card.why().find("number") != std::string::npos && port.desktopName() == first ? 1 : 0, 1);
        host.keys("cmd+a");
        typeText(host, "Bass");
        writePng(P, host, "gui.desktop.name.png");
        host.keys("return");
        host.tick(2);
        host.settle();
        pump({ &a, &b }, host, 0.2, {});
        P.eq("rename.named", !card.isOpen() && port.desktopName() == "Bass" && at(0).label == "Bass"
                                 && at(0).detail == first && panel.desktopLine() == "Claude Desktop calls this Bass" ? 1 : 0, 1);
        clickAt(host, ex.centreOfRow(S::desktop, 0));
        P.eq("rename.field_name", card.isOpen() && card.text() == "Bass" ? 1 : 0, 1);
        typeText(host, "x");
        host.keys("escape");
        host.tick(2);
        host.settle();
        P.eq("rename.escape_leaves", !card.isOpen() && port.desktopName() == "Bass" ? 1 : 0, 1);
    }

    // ---- LIVE ----------------------------------------------------------------------------------------------------------
    {
        desktop.send("call", listenCall(4.0));
        const bool lit = pump({ &a, &b }, host, 3.0, [&] {
            const int k = find(ex, S::desktop, "Listen live");
            return k >= 0 && at(k).active;
        });
        const int k = find(ex, S::desktop, "Listen live");
        P.eq("live.lit", lit && k == 2 && at(k).detail == "now" && panel.desktopLine() == "Claude Desktop: Listen live" ? 1 : 0, 1);
        writePng(P, host, "gui.desktop.png");
        {
            // PAPER: another Panel on the processor, in that theme, while the call still runs.
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(a.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            paperHost.tick(4);
            paperHost.settle();
            writePng(P, paperHost, "gui.desktop.paper.png");
            funkgui::UiPreferences::get().setTheme(0);
        }
        host.tick(2);
        const bool done = desktop.reply(10000);
        const bool after = pump({ &a, &b }, host, 3.0, [&] {
            const int j = find(ex, S::desktop, "Listen live");
            return j >= 0 && !at(j).active && at(j).detail == "just now";
        });
        P.eq("live.after", done && after && panel.desktopLine() == "Claude Desktop calls this Bass" ? 1 : 0, 1);
        panel.setChatOpen(false);
        host.tick(2);
        host.settle();
        clickAt(host, ex.centreOfRow(S::desktop, find(ex, S::desktop, "Listen live")));
        P.eq("live.opens_chat", panel.chatOpen() ? 1 : 0, 1);
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        bool use = false, name = false;
        for (const funkgui::A11yItem& it : host.accessibility())
        {
            use = use || (it.title == "Use with Claude Desktop" && it.role == funkgui::A11yRole::button && it.checked);
            name = name || (it.title == "Bass" && it.role == funkgui::A11yRole::button);
        }
        P.eq("a11y.rows", use && name ? 1 : 0, 1);
        b.editorPort().chooseDesktop(b.mcpLink()->id(), true);
        pump({ &a, &b }, host, 3.0, [&] { return !panel.desktopBadge().empty(); });
        bool badge = false;
        for (const funkgui::A11yItem& it : host.accessibility())
            badge = badge || (it.title == "Claude Desktop's target" && it.role == funkgui::A11yRole::button);
        P.eq("a11y.badge", badge ? 1 : 0, 1);
    }

    // An empty name gives it its number's back.
    std::string why;
    P.eq("rename.number_back", port.setDesktopName("", why) && port.desktopName() == first ? 1 : 0, 1);
    return P.finish();
}
