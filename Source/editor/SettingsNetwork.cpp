// Source/editor/SettingsNetwork.cpp: the settings card's Network page (SettingsPages.h; docs/plan/08-updates-and-whats-
// new.md, section 2.4): what of the plugin reaches the website, and its two switches. The update check (EditorPort's
// "The website", plugin/UpdateCheck.h): Check for updates (the offer's same answer), what the last check found, Check
// now. The factory projects (plugin/FactoryStore.h): Get the factory projects (the offer's other answer) and the store's
// state. Each says what a request sends: the plugin's name and version, nothing else. The switches write where the
// answers live (app.json, factory.json, through the port) and the page reads them there each layout; its key follows
// the check's revision and the factory store's words, which move with no input of the card's.
#include "editor/SettingsPages.h"

#include "Product.h"

#include <ctime>
#include <functional>
#include <string>

namespace playground::ui::settings
{
    namespace
    {
        constexpr const char* kSite = "testingplayground.seanfunk.xyz";

        // "today at 14:02", "yesterday at 09:10", "on 2026-11-10 at 14:02": local time.
        std::string whenWords(int64_t ms)
        {
            const std::time_t t = static_cast<std::time_t>(ms / 1000);
            const std::time_t now = std::time(nullptr);
            std::tm at{}, today{};
#if defined(_WIN32)
            localtime_s(&at, &t);
            localtime_s(&today, &now);
#else
            localtime_r(&t, &at);
            localtime_r(&now, &today);
#endif
            char clock[16];
            std::snprintf(clock, sizeof clock, "%02d:%02d", at.tm_hour, at.tm_min);
            std::tm yesterday = today;
            yesterday.tm_mday -= 1;
            (void)std::mktime(&yesterday);
            const auto sameDay = [](const std::tm& a, const std::tm& b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; };
            if (sameDay(at, today))
                return std::string("today at ") + clock;
            if (sameDay(at, yesterday))
                return std::string("yesterday at ") + clock;
            char day[16];
            std::snprintf(day, sizeof day, "%04d-%02d-%02d", at.tm_year + 1900, at.tm_mon + 1, at.tm_mday);
            return std::string("on ") + day + " at " + clock;
        }

        using Check = UpdateState::Check;

        // What the update check says now, and whether it is a failure (drawn as an error line).
        std::string checkWords(const UpdateState& u, bool& error)
        {
            error = false;
            if (u.envOff)
                return "Off on " + std::string(kThisMachine) + " (" + product::kEnvPrefix + "UPDATES=off)";
            switch (u.check)
            {
                case Check::unanswered: return "Off: not asked yet";
                case Check::off: return "Off";
                case Check::checking: return "Checking...";
                case Check::failed: error = true; return "Could not check: " + u.why;
                case Check::idle: break;
            }
            if (u.checkedMs <= 0)
                return "Not checked yet: at the next editor that opens, or Check now";
            const std::string when = "Checked " + whenWords(u.checkedMs);
            if (u.newer)
                return when + ": " + u.latest + " is out (you have " + u.current + ")";
            if (!u.latest.empty() && u.latest == u.skipped && u.latest != u.current)
                return when + ": " + u.latest + " is out, and skipped";
            return when + ": you have the latest";
        }

        void build(Builder& b)
        {
            const UpdateState u = b.ctx.port.updateState();

            b.heading("UPDATES");
            b.label("Updates");
            const std::string check = "Check for updates";
            b.toggle(kCheckUpdates, { b.ctrlX, b.y, Builder::switchW(check), kRowH }, check, "Check for updates", u.updatesOn)
                .enabled = !u.envOff;
            b.y += kRowStep;
            bool error = false;
            const std::string words = checkWords(u, error);
            b.label("Last check");
            b.notes(words, b.ctrlX, b.ctrlW, error ? Line::Kind::error : Line::Kind::value);
            if (error && !u.latest.empty())
                b.notes("The last answer, kept: " + u.latest + (u.newer ? " is out" : ""), b.ctrlX, b.ctrlW, Line::Kind::value);
            b.y += 8.0f;
            const std::string now = "Check now";
            b.button(kCheckNow, { b.ctrlX, b.y, Builder::pillW(now, false, false), kRowH }, now, "Check now: ask the website for the latest version",
                     u.updatesOn && !u.envOff && u.check != Check::checking);
            b.y += kRowStep;
            b.notes(std::string("Sends the plugin's name and version to ") + kSite
                        + ", nothing else. At most once a day, when an editor opens; a new version shows as a pill in the "
                          "top bar, and nothing opens by itself.",
                    b.ctrlX, b.ctrlW);

            b.heading("FACTORY PROJECTS");
            b.label("Projects");
            const std::string factory = "Get the factory projects";
            b.toggle(kFactoryProjects, { b.ctrlX, b.y, Builder::switchW(factory), kRowH }, factory, "Get the factory projects",
                     u.factory == 1)
                .enabled = !u.factoryOff;
            b.y += kRowStep;
            b.label("Now");
            b.notes(u.factoryWords.empty() ? std::string("Reading...") : u.factoryWords, b.ctrlX, b.ctrlW, Line::Kind::value);
            b.y += 8.0f;
            b.notes(std::string("Example projects, kept up to date from ") + kSite
                        + " at most once a day. The same request: the plugin's name and version, nothing else.",
                    b.ctrlX, b.ctrlW);

            b.heading("THE CHAT");
            b.notes("The chat reaches the model provider chosen on the AI page, and only when a message is sent.", 0.0f, b.inner);
        }

        void act(const Control& c, Context& ctx)
        {
            if (c.act != Act::item)
                return;
            const UpdateState u = ctx.port.updateState();
            if (c.index == kCheckUpdates)
                ctx.port.setUpdates(!u.updatesOn);
            else if (c.index == kFactoryProjects)
                ctx.port.setFactoryProjects(u.factory != 1);
            else if (c.index == kCheckNow)
                ctx.port.checkForUpdates();
            ctx.card.changed();
        }

        uint64_t key(const Context& ctx)
        {
            const UpdateState u = ctx.port.updateState();
            uint64_t h = u.revision * 0x100000001B3ull;
            h ^= std::hash<std::string>{}(u.factoryWords) + static_cast<uint64_t>(u.factory) * 0x9E3779B97F4A7C15ull;
            return h;
        }
    }

    const Page kNetworkPage{
        "network",
        "Network",
        "What reaches the website, and when.",
        nullptr,
        build,
        act,
        key,
    };
}
