// Source/plugin/Changelog.cpp: see Changelog.h.
#include "plugin/Changelog.h"

#include "plugin/FactoryStore.h"
#include "plugin/UpdateCheck.h"

#include "ChangelogText.h"   // generated (CMakeLists.txt): the changelog's parts, the repository

#include <algorithm>

namespace playground::changelog
{
    namespace
    {
        std::string_view trimRight(std::string_view s)
        {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        // The heading's version and date when `line` is a release's heading (`## Version X.Y.Z` and, optionally,
        // ` (YYYY-MM-DD)`); false for anything else, a pre-release's version among them.
        bool releaseHeading(std::string_view line, std::string& number, std::string& date)
        {
            constexpr std::string_view kLead = "## Version ";
            line = trimRight(line);
            if (line.substr(0, kLead.size()) != kLead)
                return false;
            std::string_view rest = line.substr(kLead.size());
            const std::size_t space = rest.find(' ');
            const std::string_view v = rest.substr(0, space);
            if (v.find('-') != std::string_view::npos || !updates::validVersion(std::string(v)))
                return false;
            std::string d;
            if (space != std::string_view::npos)
            {
                const std::string_view tail = rest.substr(space + 1);
                if (tail.size() < 2 || tail.front() != '(' || tail.back() != ')')
                    return false;
                d = std::string(tail.substr(1, tail.size() - 2));
                if (d.empty() || d.find_first_of("()") != std::string::npos)
                    return false;
            }
            number = std::string(v);
            date = std::move(d);
            return true;
        }

        bool unreleasedHeading(std::string_view line) { return trimRight(line) == "## Unreleased"; }

        std::string trimmedLines(const std::string& s)
        {
            std::size_t from = 0;
            // Whole blank lines at the start (a line of spaces is blank); the text's own indentation kept.
            for (std::size_t i = 0; i <= s.size(); ++i)
            {
                if (i == s.size() || s[i] == '\n')
                {
                    if (s.find_first_not_of(" \t\r", from) < i)
                        break;
                    from = i + 1;
                }
            }
            if (from >= s.size())
                return {};
            std::size_t to = s.size();
            while (to > from && (s[to - 1] == '\n' || s[to - 1] == ' ' || s[to - 1] == '\t' || s[to - 1] == '\r'))
                --to;
            return s.substr(from, to - from);
        }

        bool sameVersion(const std::string& a, const std::string& b)
        {
            return updates::validVersion(a) && updates::validVersion(b) && updates::compareVersions(a, b) == 0;
        }
    }

    std::vector<Version> parse(std::string_view text)
    {
        std::vector<Version> out;
        bool in = false;                             // a section has begun (the preamble is no section's)
        std::string body;
        const auto close = [&] {
            if (in)
                out.back().text = trimmedLines(body);
            body.clear();
        };
        std::size_t at = 0;
        while (at <= text.size())
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
                end = text.size();
            const std::string_view line = text.substr(at, end - at);
            std::string number, date;
            if (unreleasedHeading(line))
            {
                close();
                out.push_back({});
                in = true;
            }
            else if (releaseHeading(line, number, date))
            {
                close();
                out.push_back({ std::move(number), std::move(date), {} });
                in = true;
            }
            else if (in)
            {
                body.append(trimRight(line));
                body.push_back('\n');
            }
            if (end == text.size())
                break;
            at = end + 1;
        }
        close();
        return out;
    }

    const std::string& text()
    {
        static const std::string kText = [] {
            std::string t;
            t.reserve(generated::kBytes);
            for (std::size_t i = 0; i < generated::kPartCount; ++i)
                t += generated::kParts[i];
            return t;
        }();
        return kText;
    }

    const std::vector<Version>& versions()
    {
        static const std::vector<Version> kVersions = parse(text());
        return kVersions;
    }

    std::vector<Version> since(const std::vector<Version>& all, const std::string& seen, const std::string& current)
    {
        std::vector<Version> out;
        const auto own = std::find_if(all.begin(), all.end(), [&](const Version& v) { return sameVersion(v.number, current); });
        const bool seenBelow = updates::validVersion(seen) && updates::validVersion(current)
                            && updates::compareVersions(seen, current) < 0;
        if (own == all.end())
        {
            // A build whose version has no heading: what is under Unreleased, then the releases above what was seen.
            if (const auto u = std::find_if(all.begin(), all.end(), [](const Version& v) { return v.unreleased(); }); u != all.end())
                out.push_back(*u);
        }
        if (seenBelow)
        {
            for (const Version& v : all)
                if (!v.unreleased() && updates::compareVersions(v.number, seen) > 0 && updates::compareVersions(v.number, current) <= 0)
                    out.push_back(v);
        }
        else if (own != all.end())
            out.push_back(*own);
        // Newest first, whatever the file's order (Unreleased stays on top).
        std::stable_sort(out.begin(), out.end(), [](const Version& a, const Version& b) {
            if (a.unreleased() != b.unreleased())
                return a.unreleased();
            return !a.unreleased() && updates::compareVersions(a.number, b.number) > 0;
        });
        return out;
    }

    std::vector<Version> since(const std::string& seen, const std::string& current) { return since(versions(), seen, current); }

    bool anythingSince(const std::vector<Version>& all, const std::string& seen, const std::string& current)
    {
        // since()'s rule, asked of the numbers alone.
        bool own = false, unreleased = false, between = false;
        const bool seenBelow = updates::validVersion(seen) && updates::validVersion(current)
                            && updates::compareVersions(seen, current) < 0;
        for (const Version& v : all)
        {
            if (v.unreleased())
            {
                unreleased = true;
                continue;
            }
            own = own || sameVersion(v.number, current);
            between = between || (seenBelow && updates::compareVersions(v.number, seen) > 0
                                  && updates::compareVersions(v.number, current) <= 0);
        }
        if (!own && unreleased)
            return true;
        return seenBelow ? between : own;
    }

    bool anythingSince(const std::string& seen, const std::string& current) { return anythingSince(versions(), seen, current); }

    std::string markdown(const std::vector<Version>& shown)
    {
        std::string md;
        const bool several = shown.size() > 1;
        for (const Version& v : shown)
        {
            if (!md.empty())
                md += "\n\n---\n\n";
            if (several)
                md += "# " + (v.unreleased() ? std::string("Unreleased") : "Version " + v.number) + "\n\n";
            if (v.unreleased())
                md += "In this build, not in a release yet.\n\n";
            else if (!v.date.empty())
                md += "Released " + v.date + ".\n\n";
            md += v.text.empty() ? std::string("Nothing written down for it.") : v.text;
        }
        return md;
    }

    std::string title(const std::vector<Version>& shown, const std::string& current)
    {
        if (!shown.empty() && shown.front().unreleased())
            return "What's new in this build";
        if (shown.empty())
            return "What's new";
        return "What's new in " + (shown.front().number.empty() ? current : shown.front().number);
    }

    const char* repository() { return generated::kRepository; }

    bool linkAllowed(std::string_view url)
    {
        for (const char ch : url)
            if (static_cast<unsigned char>(ch) <= 0x20 || ch == 0x7F || ch == '\\')
                return false;
        const auto under = [&](std::string_view origin) {
            if (origin.substr(0, 8) != "https://" || url.substr(0, origin.size()) != origin)
                return false;
            if (url.size() == origin.size())
                return true;
            const char next = url[origin.size()];
            return next == '/' || next == '?' || next == '#';
        };
        if (!under(repository()) && !under(factorystore::kSite))
            return false;
        // An encoded slash or backslash anywhere, any case.
        std::string lower(url);
        for (char& ch : lower)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        if (lower.find("%2f") != std::string::npos || lower.find("%5c") != std::string::npos)
            return false;
        // The path's segments (after "https://host", before ? or #): none . or .., %2e spelt any way.
        const std::size_t host = lower.find('/', 8);
        if (host == std::string::npos)
            return true;
        const std::string path = lower.substr(host, lower.find_first_of("?#", host) - host);
        std::size_t at = 0;
        while (at <= path.size())
        {
            std::size_t end = path.find('/', at);
            if (end == std::string::npos)
                end = path.size();
            std::string seg = path.substr(at, end - at);
            for (std::size_t i; (i = seg.find("%2e")) != std::string::npos;)
                seg.replace(i, 3, ".");
            if (seg == "." || seg == "..")
                return false;
            at = end + 1;
        }
        return true;
    }
}
