// Source/sandbox/View.h: the GUI sandbox's panel: a .gui file's interface as the plugin's PANEL view shows it, with no
// plugin. A funkgui::Panel of the area's size holding the editor's own items (editor/GuiView.h: the controls, pads,
// envelopes, keyboards, tabs and shaders of GuiItems.h, and the displays of DisplayItem.h, made in the Panel's order),
// laid out, fed and drawn by the editor's code over the program's card's colour, as Panel.cpp draws them; the dials
// are mock ports (their values set before it is drawn), the displays read demo meters (MockMeters below), and a
// HeadlessHost records its frame, which editor/Picture.h draws (on the GPU, else SoftRaster).
#pragma once

#include "editor/GuiView.h"
#include "editor/Items.h"

#include "dsp/Controls.h"
#include "dsp/Meters.h"
#include "dsp/Sources.h"
#include "fminus/PanelSpec.h"

#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/ParamPort.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace playground::sandbox
{
    // A dial's port: its value 0..1, set by the sandbox (a render's values), and its default.
    class MockPort final : public funkgui::ParamPort
    {
    public:
        explicit MockPort(int slot) : id_("p" + std::to_string(slot + 1)) {}

        void set(float value01, float default01, int steps)
        {
            value_ = value01;
            default_ = default01;
            steps_ = steps;
        }

        float value01() const override { return value_; }
        float default01() const override { return default_; }
        int numSteps() const override { return steps_; }
        void beginGesture() override {}
        void setValue01(float v) override { value_ = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        void endGesture() override {}
        const char* id() const override { return id_.c_str(); }
        void* native() const override { return nullptr; }

    private:
        std::string id_;
        float value_ = 0.0f, default_ = 0.0f;
        int steps_ = 0;
    };

    class View final : public funkgui::Panel
    {
    public:
        // `controls` as readGuiSpec left them (its tapers and units), `values01` a dial a slot (0..1), `files` the
        // program's (the shaders' sources), `notes` readGuiSpec's (why a shader was not built). `page`: a tabs element's
        // page to show by its title ("" for the first pages); `shownPage` says which was, `allPages` the pages there are
        // ("OSC / FILTER; A / B").
        View(const PanelSpec& spec, const Controls& controls, const std::array<float, kMaxControls>& values01,
             const Sources& files, const std::string& notes, int width, int height, const std::string& page,
             bool instrument);

        const ui::GuiLayout& places() const noexcept { return view_.places(); }
        // The picture's pixels that are the area (the frame may be a px wider or taller: see width()).
        int pictureWidth() const noexcept;
        int pictureHeight() const noexcept;
        const std::string& shownPage() const noexcept { return shownPage_; }
        const std::string& allPages() const noexcept { return allPages_; }

        // funkgui::Panel
        void attach(funkgui::HostServices& host) override { host_ = &host; }
        // The frame: the area, an even number of px across and down, so that its pixels are whole at kDpi (1.5) and the
        // picture is the plugin's to the pixel (a frame of 853 px would be drawn 1280 pixels wide, a scale a hair over
        // 1.5, the far side a third of a pixel off); picture*() is the part that is the area.
        int width() const override { return width_ + width_ % 2; }
        int height() const override { return height_ + height_ % 2; }
        void tick(float dt) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& theme) override;
        bool wantsFullRate() const override;
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override { view_.releaseKeys(); }

    private:
        void demoMeters();                       // the meters for this frame (MockMeters)

        const PanelSpec& spec_;
        const Controls& controls_;
        bool instrument_;
        int width_, height_;
        std::vector<std::unique_ptr<MockPort>> ports_;
        std::vector<funkgui::ParamPort*> portList_;
        ui::GuiView::Items items_;
        std::vector<int> displays_;
        ui::GuiView view_;
        Meters meters_{};
        std::vector<float> scopeIn_, scopeOut_;
        std::string shownPage_, allPages_;
        bool dirty_ = true;                      // until the first frame (the displays take their values then)
        funkgui::HostServices* host_ = nullptr;
    };

    // The demo signal the displays show (deterministic: the same frames give the same picture): a tone of 220 Hz and two
    // harmonics with a little noise, about -19 dBFS RMS and -13 dBFS at its peaks out (an effect's input a little
    // louder), so a meter sits in its green, a VU near 0 and a spectrum shows its partials; a field moves slowly across
    // its range; a live= mark sits a little under its knob.
    void demoScope(bool input, float rate, float* out, int n);
}
