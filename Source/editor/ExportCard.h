// Source/editor/ExportCard.h: EXPORT, a card over the panel (from PROJECTS' Export...): a project's info, what it is to
// someone else, and the two ways out of this Mac (EditorPort::exportProject, plugin/ProjectExport.h):
//
//   DESCRIPTION  a few sentences (one field, wrapped)
//   AUTHOR       a name                 TAGS  words, commas between ("warm, analog, slow attack")
//   CATEGORY     one of this kind's (a synth's: Bass, Lead, Pad, ...; an effect's: Reverb, Delay, ...); a second click
//                on the chosen one clears it
//   GENRES       any of them
//   VERSIONS     every version the project keeps, or the newest only
//   Export source (.zip)       the code, a README and project.json, to read and edit anywhere
//   Export package (.fmpack)   everything in one compressed file, for another playground's PROJECTS, Import...
//   Done         or Escape: the info kept with the project (when it changed), the card closed
// An export keeps the info first, then asks the Panel for a save chooser (takeWant); the Panel writes the file and says
// how it went (exported). Tab moves between the fields; Return to the next. While it is open it takes every pointer
// event and key.
#pragma once

#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/LineEdit.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground::ui
{
    class ExportCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFD0000u;   // its accessibility ids, past every item's
        static constexpr int kDescriptionLength = 600, kAuthorLength = 60, kTagsLength = 200;
        static constexpr std::array<const char*, 12> kInstrumentCategories{ "Bass", "Lead", "Pad", "Keys", "Pluck", "Bell",
                                                                            "Brass", "Strings", "Arp", "Drums", "FX", "Other" };
        static constexpr std::array<const char*, 12> kEffectCategories{ "Reverb", "Delay", "Modulation", "Filter", "Distortion",
                                                                        "Dynamics", "EQ", "Pitch", "Stereo", "Utility",
                                                                        "Creative", "Other" };
        static constexpr std::array<const char*, 16> kGenres{ "Ambient", "Cinematic", "Electronic", "House", "Techno", "Trance",
                                                              "Drum & Bass", "Dubstep", "Hip-Hop", "Trap", "Pop", "Rock",
                                                              "Synthwave", "Lo-fi", "Jazz", "Experimental" };

        enum class Field : uint8_t { description, author, tags };
        enum class Want : uint8_t { none, source, package };

        explicit ExportCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open(const std::string& project);           // its info and versions read
        void close();                                    // the info kept when it changed

        struct Places
        {
            funkgui::Rect box, description, author, tags, versions, done, source, package;
            std::vector<funkgui::Rect> categories, genres;
            int descriptionLines = 4;
        };
        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }

        // An export asked for (the info kept first): the Panel asks where, writes it, and calls exported().
        Want takeWant();
        void exported(bool ok, const std::string& line);  // the line under the fields ("Exported to ~/Desktop/...")
        std::string takeNotice();                         // for the status line, once

        const std::string& project() const noexcept { return project_; }
        bool allVersions() const noexcept { return allVersions_; }
        ProjectInfo info() const;                         // as the fields have it now
        Field focus() const noexcept { return focus_; }
        const std::string& why() const noexcept { return why_; }
        static std::vector<std::string> categories();     // this kind's

    private:
        funkgui::text::LineEdit& field(Field f);
        const funkgui::text::LineEdit& field(Field f) const;
        void focusOn(Field f);
        bool keepInfo();                                  // setProjectInfo when it changed; false, with why_, on failure
        void ask(Want w);

        EditorPort& port_;
        bool open_ = false;
        std::string project_;
        int versions_ = 0, latest_ = 0, presets_ = 0;
        ProjectInfo saved_;                               // as it was read or last kept
        funkgui::text::LineEdit description_, author_, tags_;
        std::string category_;
        std::vector<std::string> genres_;
        bool allVersions_ = true;
        Field focus_ = Field::description;
        Want want_ = Want::none;
        std::string why_;
        bool failed_ = false;                             // why_ says something went wrong
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
