// Source/plugin/Changelog.h: CHANGELOG.md by release, as What's new shows it (docs/plan/08-updates-and-whats-new.md,
// section 2.5). JUCE-free.
//
// The changelog is compiled in (CMakeLists.txt writes <build>/generated/common/ChangelogText.h from CHANGELOG.md at
// configure; never docs/changelog/, whose entries a release folds in first): nothing here reaches the network or the
// disk. Its sections:
//
//   ## Unreleased                     what came since the last release (the day sections cut since, and entries)
//   ## Version 0.2.0 (2026-11-20)     a release: everything under it down to the next release's heading (its days)
//
// A line that starts `## Version` but is no release's heading (no X.Y.Z, or a pre-release's) is text of the section
// it is in, as is every other heading (`## 2026-11-09`, `### A change`); the text before `## Unreleased` (the file's
// title and preamble) is no section's. A release's date may be missing (""); scripts/changelog.py check holds the file
// to the full form.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playground::changelog
{
    struct Version
    {
        std::string number;                      // "0.2.0"; "" for Unreleased
        std::string date;                        // "2026-11-20"; "" when its heading gives none (Unreleased too)
        std::string text;                        // under its heading, the blank lines at both ends left out
        bool unreleased() const noexcept { return number.empty(); }
    };

    // A changelog's text read into its sections, in the file's order (Unreleased first when it has one).
    std::vector<Version> parse(std::string_view text);

    // The changelog compiled in: its text, and its sections (parsed once, on first use, from any thread).
    const std::string& text();
    const std::vector<Version>& versions();

    // What What's new shows for a plugin of version `current` whose user last saw `seen` ("": never), newest first:
    //   - every release above `seen` up to `current` (a version skipped shows with the ones after it);
    //   - `current`'s own alone when `seen` is "", not a version, or not below `current` (a first install, the card
    //     opened by hand, a downgrade);
    //   - Unreleased, first, when `current` has no heading (a development build), with the releases above `seen`
    //     below it.
    // Empty only when the changelog has none of these.
    std::vector<Version> since(const std::vector<Version>& all, const std::string& seen, const std::string& current);
    std::vector<Version> since(const std::string& seen, const std::string& current);

    // The card's markdown of `shown`: each version under its own heading when there are several (one's date as a line
    // of its own), in the editor's light markdown (editor/Markdown.h).
    std::string markdown(const std::vector<Version>& shown);
    // The card's title: "What's new in 0.2.0", "What's new in this build" (Unreleased).
    std::string title(const std::vector<Version>& shown, const std::string& current);

    // The addresses a link of the card may open: https:// of the repository (generated: CMakeLists.txt's
    // FP_REPOSITORY_URL) or of the website (factorystore::kSite), the origin alone or a path under it; never another
    // scheme, host, port, a user's part, or a control character or space.
    const char* repository();
    bool linkAllowed(std::string_view url);
}
