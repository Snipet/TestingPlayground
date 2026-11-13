// Source/editor/ExportCard.h: EXPORT, a card over the panel (from PROJECTS' Export...): a project's info, what it is to
// someone else, and the two ways out of this Mac (EditorPort::exportProject, plugin/ProjectExport.h):
//
//   DESCRIPTION  a few sentences (one field, wrapped)
//   AUTHOR       a name                 TAGS  words, commas between ("warm, analog, slow attack")
//   CATEGORY     one of this kind's (a synth's: Bass, Lead, Pad, ...; an effect's: Reverb, Delay, ...), or the project's
//                own when it is another (one typed in PROJECTS); a second click on the chosen one clears it
//   GENRES       any of them
//   VERSIONS     every version the project keeps, or the newest only
//   Export source (.zip)       the code, a README and project.json, to read and edit anywhere
//   Export package (.fmpack)   everything in one compressed file, for another playground's PROJECTS, Import...
//   Done         or Escape: the info kept with the project (when it changed), the card closed
// An export keeps the info first, then asks the Panel for a save chooser (takeWant); the Panel writes the file and says
// how it went (exported). Tab moves between the fields; Return to the next. While it is open it takes every pointer
// event and key. A factory project's (isFactoryName) is read-only: its info shown as it is, no field or pill taking an
// edit, nothing kept (the store refuses its info), its exports as they are; a copy saved as the user's own changes it.
// It reads the store's cache alone (EditorPort::projectPolled): a project the cache lacks shows "Reading the project...",
// its fields taking nothing, until the store's Reader has read it (poll()).
//
// Several projects (open() with more than one, PROJECTS' Export... or its context menu's with several chosen): no info
// fields (they are one project's), the count and the first names ("and 8 more"), the versions' switch and the two
// exports, each one .zip, the bundle, holding every project's own export of that format (ProjectExport.h).
// Every export runs off the message thread (EditorPort::startExport): while one runs the card shows how far it has
// come and Done is Cancel (takeCancel); the Panel calls progress() and exported() as the job moves and ends. An export
// never replaces a file: one there already is refused in the refusal's words, the card asking for another name.
// A press outside the card closes it as Done does (the Panel then drops the press); an export under way goes on, its
// outcome in the status line.
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

namespace playground::ui
{
    class ExportCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFD0000u;   // its accessibility ids, past every item's
        static constexpr const char* kReading = "Reading the project...";   // its line while reading()
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
        void open(const std::string& project);           // its info and versions read (the store's cache)
        void open(const std::vector<std::string>& projects);   // one: open(project); more: the bulk card
        void close();                                    // the info kept when it changed
        // While open, each frame: once the store's Reader has read a file (EditorPort::projectsRead), the info again from
        // the cache: a project it lacked when the card opened is filled in, one another process changed shows as it is
        // now, unless the fields have been edited since (they stay as typed). Whether it changed.
        bool poll();
        bool reading() const noexcept { return reading_; }   // its info not read yet: the fields take nothing

        struct Places
        {
            funkgui::Rect box, description, author, tags, versions, done, source, package;
            funkgui::Rect names;                         // the bulk card's: the projects' names
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
        // The export job (EditorPort::startExport) under way: started() as it starts, progress() as projects are done.
        void started(const std::string& line);
        void progress(int done, int total);
        bool running() const noexcept { return running_; }
        int doneCount() const noexcept { return done_; }
        bool takeCancel();                                // Cancel pressed while it runs (the Panel cancels the job)
        std::string takeNotice();                         // for the status line, once

        const std::string& project() const noexcept { return project_; }   // the first of several
        const std::vector<std::string>& projects() const noexcept { return projects_; }
        bool bulk() const noexcept { return projects_.size() > 1; }
        static std::string countText(int n) { return std::to_string(n) + (n == 1 ? " project" : " projects"); }
        bool readOnly() const noexcept { return readOnly_; }   // a factory project's: its info as it is
        bool allVersions() const noexcept { return allVersions_; }
        ProjectInfo info() const;                         // as the fields have it now
        Field focus() const noexcept { return focus_; }
        const std::string& why() const noexcept { return why_; }
        static std::vector<std::string> categories();     // this kind's
        std::vector<std::string> choices() const;         // its pills: this kind's, and the project's own when another

    private:
        funkgui::text::LineEdit& field(Field f);
        const funkgui::text::LineEdit& field(Field f) const;
        void focusOn(Field f);
        void read();                                      // its info and counts, from the store's cache
        bool locked() const noexcept { return readOnly_ || reading_ || bulk(); }   // no field takes an edit
        bool keepInfo();                                  // setProjectInfo when it changed; false, with why_, on failure
        void ask(Want w);

        EditorPort& port_;
        bool open_ = false;
        std::string project_;
        std::vector<std::string> projects_;               // every project it exports (one, or the bulk card's)
        int64_t bytes_ = 0;                               // the bulk card's: their files' sizes, in all
        bool running_ = false;                            // an export under way
        int done_ = 0, total_ = 0;                        // how far it has come
        bool cancel_ = false;                             // takeCancel
        bool readOnly_ = false;
        bool reading_ = false;                            // its file not read yet (the store's Reader reads it)
        uint64_t readSeen_ = 0;                           // EditorPort::projectsRead when it read the cache
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
