// Source/editor/UpdateCards.h: the website's two cards (docs/plan/08-updates-and-whats-new.md, sections 2.1 and 2.4).
//
// OfferCard: "Use the website?", made the first time an editor opens for a user who has answered neither question
// (UpdateState::offer), never in a picture's Panel. Two lines, each ticked at first: the factory projects,
// kept up to date; a note when a new version is out. Allow stores a yes for each ticked line and a no for each unticked
// one; Not now stores "later" for the update check and leaves the factory projects unasked, and the offer is not made
// again by itself (EditorPort::answerOffer). Both can be changed on the Settings card's Network page. It stops nothing:
// it is not modal. The panel plays and edits behind it, takes its keys and its other clicks; only a press inside its box
// is its own. While a modal card is open it is not drawn (never two cards at once). It closes itself when another editor
// answered (the Panel's sync).
//
// UpdateCard: opened only by a click on the top bar's pill ("0.2.0 is out"), never by itself. "<Name> 0.2.0 is out", the
// version this is, the release's one-line summary, then Download (the site's fixed download page in the browser:
// EditorPort::openDownloadPage), Skip this version (EditorPort::skipUpdate: the pill goes), Later (or Escape, or a press
// outside the card). Modal while open, as the other cards are.
#pragma once

#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    class OfferCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FF60000u;   // its accessibility ids, past every item's (and the other cards')

        explicit OfferCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open();                                     // both lines ticked
        void close();

        struct Places
        {
            funkgui::Rect box, factory, updates, allow, notNow;
        };
        Places places(float width, float height) const;
        bool contains(funkgui::Point p, float width, float height) const { return open_ && places(width, height).box.contains(p); }

        void pointerDown(funkgui::Point p, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }

        std::string takeNotice();                        // what it did, once, for the status line
        bool factoryTicked() const noexcept { return factory_; }
        bool updatesTicked() const noexcept { return updates_; }

    private:
        EditorPort& port_;
        bool open_ = false;
        bool factory_ = true, updates_ = true;
        std::string notice_;
        uint32_t revision_ = 0;
    };

    class UpdateCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FF50000u;

        explicit UpdateCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open(const UpdateState& state);   // of the version the state names
        void close();

        struct Places
        {
            funkgui::Rect box, download, skip, later;
            std::vector<std::string> summary;            // its lines, wrapped
        };
        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height);
        void key(const funkgui::KeyEvent& e, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }

        std::string takeNotice();
        const std::string& version() const noexcept { return version_; }

    private:
        void download();
        void skip();

        EditorPort& port_;
        bool open_ = false;
        std::string version_, current_, date_, summary_;
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
