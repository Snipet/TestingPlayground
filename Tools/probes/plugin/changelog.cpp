// PROBE name=plugin.changelog timeout=60
//
// plugin.changelog: the changelog by release (plugin/Changelog.h), as What's new shows it: the text compiled in, its
// sections, what the card shows for a version seen, never seen, or several skipped, and the links it may open. Nothing
// here reaches the network or a folder of the user's; CHANGELOG.md is read from the source tree (PROBE_SOURCE_DIR).
//   COMPILED  the text compiled in is CHANGELOG.md byte for byte (its CR LF as LF), joined from its parts
//   REAL      the real changelog's sections: Unreleased first, then each release, newest first, each with a version,
//             a date and text; this build's version has its heading (or the build shows Unreleased); 0.1.0's text holds
//             its days and not the preamble, and no section's text starts or ends with a blank line
//   SINCE     since(): never seen (this version's alone), seen this version (its own: the card by hand), one before,
//             several skipped (each, newest first, none at or below seen, none above this version), a downgrade (its
//             own), a seen that is no version (its own); a development build whose version has no heading: Unreleased
//             first, then the releases above seen
//   ODD       a made-up changelog of odd headings: a preamble's text is no section's; `## Version 0.2` (no X.Y.Z), a
//             pre-release's, a `### Version 1.0.0` and `##Version 1.0.0` are text of the section they are in; a release
//             with no date; one with trailing spaces; releases out of order are shown newest first; no Unreleased;
//             CR LF lines; an empty text; a last line with no newline
//   MARKDOWN  the card's text: one version's "Released <date>." then its text, several each under "# Version X" apart
//             by a rule, Unreleased's own line; the title for one, several and Unreleased
//   LINKS     linkAllowed: the repository's and the website's https pages, their origin alone, a path, a query and a
//             fragment; refused: http, another host, a host that only starts the same, a user's part, a port, a
//             space or control character, a backslash, an empty text, another scheme; every link the real changelog
//             holds is allowed
#include "Probe.h"

#include "plugin/Changelog.h"
#include "plugin/FactoryStore.h"
#include "plugin/UpdateCheck.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace pg = playground;
namespace cl = playground::changelog;

namespace
{
    std::string numbers(const std::vector<cl::Version>& v)
    {
        std::string s;
        for (const cl::Version& x : v)
            s += (s.empty() ? "" : ",") + (x.unreleased() ? std::string("U") : x.number);
        return s;
    }

    bool blankEdge(const std::string& t)
    {
        return !t.empty() && (t.front() == '\n' || t.back() == '\n' || t.front() == ' ');
    }
}

PROBE(plugin, changelog)
{
    // ---- COMPILED -----------------------------------------------------------------------------------------------------
    {
        const juce::File file = juce::File(PROBE_SOURCE_DIR).getChildFile("CHANGELOG.md");
        std::string disk = file.loadFileAsString().toStdString();
        disk.erase(std::remove(disk.begin(), disk.end(), '\r'), disk.end());
        P.ge("compiled.disk_bytes", static_cast<double>(disk.size()), 1000);
        P.eq("compiled.same_text", cl::text() == disk ? 1 : 0, 1);
        P.eq("compiled.same_bytes", static_cast<int64_t>(cl::text().size()), static_cast<int64_t>(disk.size()));
    }

    // ---- REAL ---------------------------------------------------------------------------------------------------------
    const std::vector<cl::Version>& all = cl::versions();
    {
        P.ge("real.sections", static_cast<double>(all.size()), 2);
        P.eq("real.unreleased_first", !all.empty() && all.front().unreleased() ? 1 : 0, 1);
        int releases = 0, dated = 0, texts = 0, edges = 0, ordered = 1, unreleased = 0;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const cl::Version& v = all[i];
            unreleased += v.unreleased() ? 1 : 0;
            edges += blankEdge(v.text) ? 1 : 0;
            if (v.unreleased())
                continue;
            ++releases;
            dated += std::regex_match(v.date, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")) ? 1 : 0;
            texts += v.text.size() > 40 ? 1 : 0;
            if (i + 1 < all.size() && !all[i + 1].unreleased() && pg::updates::compareVersions(v.number, all[i + 1].number) <= 0)
                ordered = 0;
        }
        P.ge("real.releases", releases, 1);
        P.eq("real.one_unreleased", unreleased, 1);
        P.eq("real.each_dated", dated, releases);
        P.eq("real.each_has_text", texts, releases);
        P.eq("real.newest_first", ordered, 1);
        P.eq("real.no_blank_edges", edges, 0);
        const auto first = std::find_if(all.begin(), all.end(), [](const cl::Version& v) { return v.number == "0.1.0"; });
        P.eq("real.has_0_1_0", first != all.end() ? 1 : 0, 1);
        if (first != all.end())
        {
            P.eq("real.0_1_0_date", first->date == "2026-11-10" ? 1 : 0, 1);
            P.eq("real.0_1_0_holds_its_days", first->text.rfind("## 2026-11-09", 0) == 0 ? 1 : 0, 1);
            P.eq("real.0_1_0_holds_the_first_day", first->text.find("## 2026-10-15 to 2026-10-28") != std::string::npos ? 1 : 0, 1);
            P.eq("real.0_1_0_no_preamble", first->text.find("# Changelog") == std::string::npos ? 1 : 0, 1);
            P.eq("real.0_1_0_no_unreleased", first->text.find("## Unreleased") == std::string::npos ? 1 : 0, 1);
        }
        P.eq("real.unreleased_no_preamble",
             !all.empty() && all.front().text.find("What changed in the two plugins") == std::string::npos ? 1 : 0, 1);
        // This build: its version's heading, else Unreleased.
        const std::vector<cl::Version> mine = cl::since("", pg::product::kVersion);
        P.eq("real.this_build_shows_one", static_cast<int64_t>(mine.size()), 1);
        const bool headed = std::any_of(all.begin(), all.end(), [](const cl::Version& v) { return v.number == pg::product::kVersion; });
        P.eq("real.this_build_its_own_or_unreleased",
             !mine.empty() && (headed ? mine.front().number == pg::product::kVersion : mine.front().unreleased()) ? 1 : 0, 1);
    }

    // ---- SINCE --------------------------------------------------------------------------------------------------------
    {
        const std::vector<cl::Version> made = cl::parse("# Changelog\n\nPreamble.\n\n## Unreleased\n\n- Coming.\n\n"
                                                        "## Version 0.4.0 (2026-04-01)\n\n- Four.\n\n"
                                                        "## Version 0.3.0 (2026-03-01)\n\n## 2026-02-20\n\n- Three.\n\n"
                                                        "## Version 0.2.0 (2026-02-01)\n\n- Two.\n\n"
                                                        "## Version 0.1.0 (2026-01-01)\n\n- One.\n");
        P.eq("since.parsed", static_cast<int64_t>(made.size()), 5);
        P.eq("since.never_seen", numbers(cl::since(made, "", "0.3.0")) == "0.3.0" ? 1 : 0, 1);
        P.eq("since.seen_this", numbers(cl::since(made, "0.3.0", "0.3.0")) == "0.3.0" ? 1 : 0, 1);
        P.eq("since.one_before", numbers(cl::since(made, "0.2.0", "0.3.0")) == "0.3.0" ? 1 : 0, 1);
        P.eq("since.several_skipped", numbers(cl::since(made, "0.1.0", "0.4.0")) == "0.4.0,0.3.0,0.2.0" ? 1 : 0, 1);
        P.eq("since.none_above_this", numbers(cl::since(made, "0.1.0", "0.3.0")) == "0.3.0,0.2.0" ? 1 : 0, 1);
        P.eq("since.seen_between", numbers(cl::since(made, "0.2.5", "0.4.0")) == "0.4.0,0.3.0" ? 1 : 0, 1);
        P.eq("since.downgrade", numbers(cl::since(made, "0.4.0", "0.2.0")) == "0.2.0" ? 1 : 0, 1);
        P.eq("since.seen_no_version", numbers(cl::since(made, "soon", "0.3.0")) == "0.3.0" ? 1 : 0, 1);
        P.eq("since.dev_never_seen", numbers(cl::since(made, "", "0.5.0")) == "U" ? 1 : 0, 1);
        P.eq("since.dev_seen_before", numbers(cl::since(made, "0.2.0", "0.5.0")) == "U,0.4.0,0.3.0" ? 1 : 0, 1);
        P.eq("since.dev_prerelease", numbers(cl::since(made, "0.3.0", "0.4.0-rc.1")) == "U" ? 1 : 0, 1);
        P.eq("since.text_kept", !made.empty() && made[2].text == "- Four." ? 1 : 0, 1);
        P.eq("since.days_kept", made.size() > 3 && made[3].text == "## 2026-02-20\n\n- Three." ? 1 : 0, 1);
        P.eq("since.nothing_to_show", static_cast<int64_t>(cl::since(cl::parse("# Changelog\n"), "", "0.1.0").size()), 0);
    }

    // ---- ODD ----------------------------------------------------------------------------------------------------------
    {
        const std::string odd = "Preamble before anything.\r\n## Version 0.2 (2026-01-01)\r\nnot a section\r\n"
                                "## Version 1.2.0\r\n- no date\r\n## Version 1.3.0 (2026-05-01)   \r\n"
                                "### Version 1.0.0\r\n##Version 1.0.0\r\n## Version 1.1.0-rc.1 (2026-04-01)\r\n"
                                "## Version 1.4.0 (2026-06-01)\r\n\r\n\r\n- the last line\r\n- with no newline";
        const std::vector<cl::Version> v = cl::parse(odd);
        P.eq("odd.sections", static_cast<int64_t>(v.size()), 3);
        P.eq("odd.no_unreleased", std::none_of(v.begin(), v.end(), [](const cl::Version& x) { return x.unreleased(); }) ? 1 : 0, 1);
        P.eq("odd.order_as_written", numbers(v) == "1.2.0,1.3.0,1.4.0" ? 1 : 0, 1);
        P.eq("odd.no_date", !v.empty() && v[0].date.empty() ? 1 : 0, 1);
        P.eq("odd.trailing_spaces", v.size() > 1 && v[1].date == "2026-05-01" ? 1 : 0, 1);
        P.eq("odd.lesser_headings_are_text",
             v.size() > 1 && v[1].text == "### Version 1.0.0\n##Version 1.0.0\n## Version 1.1.0-rc.1 (2026-04-01)" ? 1 : 0, 1);
        P.eq("odd.crlf_and_last_line", v.size() > 2 && v[2].text == "- the last line\n- with no newline" ? 1 : 0, 1);
        P.eq("odd.newest_first_shown", numbers(cl::since(v, "1.1.0", "1.4.0")) == "1.4.0,1.3.0,1.2.0" ? 1 : 0, 1);
        P.eq("odd.empty", static_cast<int64_t>(cl::parse("").size()), 0);
        P.eq("odd.unreleased_alone", numbers(cl::parse("## Unreleased")) == "U" ? 1 : 0, 1);
    }

    // ---- MARKDOWN -----------------------------------------------------------------------------------------------------
    {
        const std::vector<cl::Version> made = cl::parse("## Unreleased\n\n- Coming.\n\n## Version 0.3.0 (2026-03-01)\n\n"
                                                        "- Three.\n\n## Version 0.2.0 (2026-02-01)\n\n- Two.\n");
        const std::vector<cl::Version> one = cl::since(made, "", "0.3.0");
        P.eq("markdown.one", cl::markdown(one) == "Released 2026-03-01.\n\n- Three." ? 1 : 0, 1);
        P.eq("markdown.title_one", cl::title(one, "0.3.0") == "What's new in 0.3.0" ? 1 : 0, 1);
        const std::vector<cl::Version> two = cl::since(made, "0.1.0", "0.3.0");
        P.eq("markdown.several", cl::markdown(two) == "# Version 0.3.0\n\nReleased 2026-03-01.\n\n- Three.\n\n---\n\n"
                                                      "# Version 0.2.0\n\nReleased 2026-02-01.\n\n- Two." ? 1 : 0, 1);
        P.eq("markdown.title_several", cl::title(two, "0.3.0") == "What's new in 0.3.0" ? 1 : 0, 1);
        const std::vector<cl::Version> dev = cl::since(made, "", "0.4.0");
        P.eq("markdown.unreleased", cl::markdown(dev) == "In this build, not in a release yet.\n\n- Coming." ? 1 : 0, 1);
        P.eq("markdown.title_unreleased", cl::title(dev, "0.4.0") == "What's new in this build" ? 1 : 0, 1);
    }

    // ---- LINKS --------------------------------------------------------------------------------------------------------
    {
        const std::string repo = cl::repository();
        const std::string site = pg::factorystore::kSite;
        P.eq("links.repository_https", repo.rfind("https://", 0) == 0 ? 1 : 0, 1);
        const std::vector<std::string> allowed = { repo, repo + "/pull/31", repo + "/blob/v0.1.0/CHANGELOG.md#top", site,
                                                   site + "/", site + "/download", site + "/blog?x=1" };
        int ok = 0;
        for (const std::string& u : allowed)
            ok += cl::linkAllowed(u) ? 1 : 0;
        P.eq("links.allowed", ok, static_cast<int64_t>(allowed.size()));
        const std::vector<std::string> refused = {
            "http://" + site.substr(8), repo + "Evil/pull/1", site + ".evil.example/x", "https://example.com/",
            "https://user@" + site.substr(8) + "/", site + ":8443/", repo + "/pull/1 2", repo + "/pull/\n1",
            repo + "\\..\\x", "", "javascript:alert(1)", "file:///etc/passwd", "HTTPS://" + site.substr(8) };
        int no = 0;
        for (const std::string& u : refused)
            no += cl::linkAllowed(u) ? 0 : 1;
        P.eq("links.refused", no, static_cast<int64_t>(refused.size()));
        // Every link the real changelog holds (its pull requests' and the like) may open.
        const std::regex link(R"(\]\(([^)\s]+)\))");
        int links = 0, open = 0;
        const std::string& t = cl::text();
        for (auto it = std::sregex_iterator(t.begin(), t.end(), link); it != std::sregex_iterator(); ++it)
        {
            ++links;
            open += cl::linkAllowed((*it)[1].str()) ? 1 : 0;
        }
        P.ge("links.real_count", links, 20);
        P.eq("links.real_all_allowed", open, links);
    }
    return P.finish();
}
