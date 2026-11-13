// Source/editor/nogpu/SoftEditorHost.h: the editor's host in a build without FunkGui's GPU layer (FP_WITH_GPU off): a
// juce::AudioProcessorEditor that runs the Panel as funkgui::EditorHost does (gpu/CreateEditorGpu.cpp) and draws its
// frames on the CPU with FunkGui's SoftRaster, the rasteriser every probe PNG comes from, so the window shows exactly
// what the probes' pictures show. CMakeLists.txt compiles Source/editor/nogpu/ in place of Source/editor/gpu/ when
// FunkGui::gpu is not built (FP_WITH_GPU off).
//
// What it keeps of EditorHost (funkgui/gpu/EditorHost.h): the Panel's clock (a tick a frame, seconds never frames,
// Panel::idle at least 10 times a second), the frame rate (about 60 Hz while the Panel wants it or input came in the
// last half second, 12 Hz otherwise, nothing while the editor is not showing), the theme and the UI zoom from FunkGui's
// preferences (UiPreferences: the same steps, fitted to the display, followed across editors), resizing within the
// Panel's least size (the logical size is the editor's divided by the zoom; EditorConfig::sizeChanged's callback), the
// input (the pointer with its button, the wheel, the pinch, keys, file drags, the unbounded drag, the host's parameter
// menu) converted to the Panel's structs by EditorHost's rules, and HostServices over JUCE: menus in FunkGui's
// MenuLook, the native file chooser, the clipboard, announcements. What it leaves out: the accessibility mirror (FunkGui's A11yBridge is part of its GPU
// layer: a screen reader hears announcements only) and the capture environment (CANVAS_DUMP, UI_KEYS: the probes'
// HeadlessHost covers them).
//
// The cost: a frame is recorded only when the Panel says something it draws changed (Panel::needsDraw, as EditorHost
// since FunkGui v0.25.0: a still panel costs a tick), and rasterised only when its primitives differ from the last
// one's, at most 30 times a second, at the display's scale times the zoom, one sample a pixel. Message thread only.
#pragma once

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui
{
    class MenuLook;
}

namespace playground::ui
{
    // EditorHost's EditorConfig, the fields this host reads (gpu/EditorHost.h says what each means).
    struct SoftEditorConfig
    {
        std::vector<int> zoomSteps;                  // percent, ascending; empty: no zoom
        int         defaultZoomPercent = 100;
        const char* zoomPrefKey = nullptr;           // UiPreferences int key, machine-wide
        int maxWidth = 0, maxHeight = 0;             // logical; 0: no limit
        std::function<void(int w, int h)> sizeChanged;
    };

    class SoftEditorHost final : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget,
                                 private juce::Timer, private funkgui::HostServices
    {
    public:
        SoftEditorHost(juce::AudioProcessor&, SoftEditorConfig, std::unique_ptr<funkgui::Panel>);
        ~SoftEditorHost() override;

        SoftEditorHost(const SoftEditorHost&) = delete;
        SoftEditorHost& operator=(const SoftEditorHost&) = delete;

        funkgui::Panel& panel() noexcept { return *panel_; }
        int appliedZoomPercent() const noexcept { return zoomApplied_; }   // what the editor is sized and drawn at
        uint32_t ticks() const noexcept { return ticks_; }                 // the Panel's ticks (the settings' Debug page)

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;   // Panel::magnify (FunkGui F1)
        bool keyPressed(const juce::KeyPress&) override;
        bool isInterestedInFileDrag(const juce::StringArray&) override;
        void filesDropped(const juce::StringArray&, int, int) override;
        void fileDragEnter(const juce::StringArray&, int, int) override;
        void fileDragMove(const juce::StringArray&, int, int) override;
        void fileDragExit(const juce::StringArray&) override;

    private:
        void timerCallback() override;

        // HostServices
        void   setUnboundedDrag(bool on) override;
        void   showParamMenu(funkgui::ParamPort&, float x, float y) override;
        void   nudgeFullRate() override;
        double nowSeconds() const override;
        void   beginBatch() override {}
        void   endBatch() override {}
        int    themeIndex() const override;
        juce::Component* ownerComponent() override { return this; }
        int    zoomPercent() const override { return zoomTarget_; }
        void   setZoomPercent(int percent) override;
        std::span<const int> zoomSteps() const override { return zoomSteps_; }
        bool   zoomFits(int percent) const override;
        unsigned services() const override;
        bool   showMenu(const funkgui::MenuRequest&, funkgui::MenuCallback) override;
        void   dismissMenus() override;
        bool   chooseFiles(const funkgui::FileRequest&, funkgui::FilesCallback) override;
        bool   copyText(std::string_view utf8) override;
        std::string clipboardText() const override;
        void   announce(std::string_view text, Announce priority) override;

        void frame(double now);                      // tick, record, and rasterise when the picture changed
        bool sameAsLast(const funkgui::PrimList&) const;
        void keepAsLast(const funkgui::PrimList&);
        void applyTheme(int idx);
        void updateCursor();
        funkgui::PointerEvent pointer(const juce::MouseEvent&) const;
        float deviceScale() const;

        // The zoom and the size (EditorHost's rules).
        bool isZoomStep(int percent) const noexcept;
        int  readZoomPreference() const;
        void updateZoomTarget();
        int  fitZoom(int chosen) const;
        juce::Rectangle<int> fitArea() const;
        void applyZoom();
        void applyResizeLimits();
        void followLogicalSize();
        int  zoomedSize(int logical, int percent) const noexcept;

        // Declared first, destroyed last: the Canvas and the callbacks refer to it.
        std::unique_ptr<funkgui::Panel> panel_;
        SoftEditorConfig config_;
        funkgui::Canvas canvas_;
        funkgui::Theme  theme_ = funkgui::Theme::graphite();
        int      themeIdx_ = 0;
        uint32_t prefsRevision_ = 0;

        // The clock.
        double   start_ = 0.0;
        double   lastFrame_ = 0.0, lastIdle_ = 0.0, lastRaster_ = 0.0, fullRateUntil_ = 0.0;
        double   seconds_ = 0.0;                     // the Panel's clock: the sum of its dts
        uint32_t ticks_ = 0;

        // The picture: the last frame rasterised, and its primitives (to tell a frame that changed).
        juce::Image image_;
        std::vector<funkgui::Prim> lastPrims_;
        std::vector<funkgui::ShaderDraw> lastShaders_;
        std::vector<funkgui::ImageDraw> lastImages_;
        funkgui::FrameInfo lastInfo_{};
        bool dirty_ = true;                          // a change not rasterised yet (the 30 Hz cap held it)

        // Input.
        std::optional<juce::MouseInputSource> dragSource_;
        bool unbounded_ = false;
        juce::Point<float> dragScreen_;

        // The zoom, in whole percents, and the logical size.
        int   logicalW_ = 0, logicalH_ = 0;
        std::vector<int> zoomSteps_;
        int   zoomDefault_ = 100, zoomChosen_ = 100, zoomTarget_ = 100, zoomApplied_ = 100;
        float zoomScale_ = 1.0f;
        bool  resizable_ = false, applyingZoom_ = false;

        // The menus and the file chooser, and their callbacks (HostServicesJuce's rules: a callback runs at most once,
        // never from inside the call that took it, never after this editor is gone).
        struct Pending
        {
            funkgui::MenuCallback  menu;
            funkgui::FilesCallback files;
            uint32_t menuSerial = 0, filesSerial = 0;
            bool menuOpen = false, filesOpen = false, asking = false;
        };
        std::shared_ptr<Pending> pending_;
        std::unique_ptr<funkgui::MenuLook> look_;
        std::unique_ptr<juce::FileChooser> chooser_, retiredChooser_;

        JUCE_LEAK_DETECTOR(SoftEditorHost)
    };
}
