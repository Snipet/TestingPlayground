// Source/editor/gpu/CodeWindow.cpp: the plugin's editor and its code window (CodeWindow.h).
#include "editor/gpu/CodeWindow.h"

#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/FramePump.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace playground::ui
{
    namespace
    {
        constexpr int kTickHz = 4;                       // the session followed, the host's activity watched
        constexpr int kGap = 12;                         // between the editor's window and the code window beside it
    }

    // The settings' Debug page's graphics rows for an editor on the GPU (Panel::setHostDebug): what FunkGui's
    // EditorHost, its frame pump, the bgfx context and the sink say at the pin. The renderer's own name, the GPU's and its
    // limits wait for FunkGui's gpuInfo(); the last frame's draw calls, vertices and bytes are the sink's, which the
    // EditorHost keeps to itself.
    namespace
    {
    std::vector<DebugRow> hostDebugRows(const funkgui::EditorHost& host)
    {
        std::vector<DebugRow> rows;
        const funkgui::EditorHost::Diagnostics d = host.diagnostics();
        char b[160];
#if defined(_WIN32)
        rows.push_back({ "Host", "the GPU: bgfx on Direct3D 11" });
#elif defined(__APPLE__)
        rows.push_back({ "Host", "the GPU: bgfx on Metal" });
#else
        rows.push_back({ "Host", "the GPU: bgfx on Vulkan" });
#endif
        rows.push_back({ "Surface", host.showingFallback() ? "the fallback (the GPU did not start)"
                                    : host.surfaceAttached() ? "attached" : "not attached" });
        const funkgui::FramePump& pump = funkgui::FramePump::get();
        std::snprintf(b, sizeof b, "%.1f a second (%s)", static_cast<double>(d.fps),
                      pump.isDisplayLinked() ? "display-linked" : "the timer");
        rows.push_back({ "Frames", b });
        std::snprintf(b, sizeof b, "%u drawn, %u skipped, %u unchanged; the pump %llu", d.frames, d.skipped, d.unchanged,
                      static_cast<unsigned long long>(pump.framesRun()));
        rows.push_back({ "Frames counted", b });
        const funkgui::BgfxContext& ctx = funkgui::BgfxContext::get();
        std::snprintf(b, sizeof b, "%u KB, %u frames dropped (full)", ctx.transientVbBytes() / 1024u, d.overflows);
        rows.push_back({ "Transient buffer", b });
        rows.push_back({ "Instancing", ctx.transientVbBytes() == 0 ? "not started" : ctx.instancing() ? "on" : "off (vertices)" });
        rows.push_back({ "Image textures", std::to_string(funkgui::BgfxSink::imageTextureCount()) });
        std::snprintf(b, sizeof b, "%.2f, zoom %d %%, %u failed attaches", d.scale, d.zoomPercent, d.attachFailures);
        rows.push_back({ "Scale", b });
        return rows;
    }
    }

    funkgui::EditorConfig editorConfig()
    {
        funkgui::EditorConfig config;                  // width/height 0: the Panel's own size
        config.zoomSteps = { 100, 125, 150, 200 };     // UI zoom: the Panel's ZOOM cells offer these steps
        config.defaultZoomPercent = 100;
        config.zoomPrefKey = "uiZoom";                 // machine-wide, beside the theme; never in the session
        return config;
    }

    // ---- the window -------------------------------------------------------------------------------------------------
    // The code window: the system's title bar over a second EditorHost, its limits the code Panel's at the zoom (with
    // any native frame around them, as the Standalone's window has them: plugin/standalone/App.cpp).
    class Editor::Window final : public juce::DocumentWindow
    {
    public:
        Window(Editor& owner, Processor& processor, std::unique_ptr<Panel> panel)
            : juce::DocumentWindow(juce::String::fromUTF8(product::kName) + juce::String::fromUTF8(" \xe2\x80\x93 Code"),
                                   juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                                       juce::ResizableWindow::backgroundColourId),
                                   juce::DocumentWindow::allButtons, true),
              owner_(owner), port_(processor.editorPort())
        {
            constrainer_.setWindow(this);
            setUsingNativeTitleBar(true);
            setConstrainer(&constrainer_);
            funkgui::EditorConfig config = editorConfig();
            // Each size the user gives it (a drag of its edges or its corner, a zoom) goes to the session.
            config.sizeChanged = [this](int w, int h) {
                CodeWindow cw = port_.codeWindow();
                cw.width = w;
                cw.height = h;
                port_.setCodeWindow(cw);
            };
            Panel* shown = panel.get();
            host_ = new funkgui::EditorHost(processor, std::move(config), std::move(panel));
            shown->setHostDebug([host = host_] { return hostDebugRows(*host); });
            constrainer_.setEditor(host_);
            setContentOwned(host_, true);
            setResizable(true, false);
        }

        ~Window() override
        {
            constrainer_.setEditor(nullptr);
            clearContentComponent();                     // the EditorHost and its Panel go before the window
        }

        funkgui::EditorHost& host() noexcept { return *host_; }

        // Shown at `where` (its content's top-left on the screen), then kept: its moves from now on are the user's.
        void showAt(juce::Point<int> where)
        {
            setTopLeftPosition(where);
            setVisible(true);
            toFront(true);
            host_->grabKeyboardFocus();
            shown_ = true;
        }

        void closeButtonPressed() override
        {
            // The editor back, after this event: the window goes then (Editor::follow).
            CodeWindow cw = port_.codeWindow();
            cw.out = false;
            port_.setCodeWindow(cw);
            owner_.followLater();
        }

        void moved() override
        {
            juce::DocumentWindow::moved();
            if (!shown_ || isMinimised() || isFullScreen())
                return;
            CodeWindow cw = port_.codeWindow();
            cw.placed = true;
            cw.x = getX();
            cw.y = getY();
            port_.setCodeWindow(cw);
        }

    private:
        // The window's limits are the editor's (its own constrainer: the Panel's least size at its zoom) with any native
        // frame around it.
        class Constrainer final : public juce::BorderedComponentBoundsConstrainer
        {
        public:
            void setWindow(juce::DocumentWindow* window) { window_ = window; }
            void setEditor(juce::AudioProcessorEditor* editor) { editor_ = editor; }

            juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override
            {
                return editor_ != nullptr ? editor_->getConstrainer() : nullptr;
            }

            juce::BorderSize<int> getAdditionalBorder() const override
            {
                juce::BorderSize<int> b;
                if (window_ != nullptr)
                {
                    if (auto* peer = window_->getPeer())
                        if (const auto frame = peer->getFrameSizeIfPresent())
                            b = *frame;
                    b = b.addedTo(window_->getContentComponentBorder());
                }
                return b;
            }

        private:
            juce::DocumentWindow* window_ = nullptr;
            juce::AudioProcessorEditor* editor_ = nullptr;
        };

        Editor& owner_;
        EditorPort& port_;
        Constrainer constrainer_;
        funkgui::EditorHost* host_ = nullptr;            // the content (owned by the window)
        bool shown_ = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Window)
    };

    // ---- the editor -------------------------------------------------------------------------------------------------
    Editor::Editor(Processor& owner, funkgui::EditorConfig config, std::unique_ptr<Panel> panel)
        : funkgui::EditorHost(owner, std::move(config), std::move(panel)), processor_(owner),
          panel_(static_cast<Panel&>(this->panel())), timer_([this] { tick(); })
    {
        Panel::WindowHooks hooks;
        hooks.out = [this](bool) { followLater(); };
        hooks.raise = [this] { raiseCode(); };
        panel_.setWindowHooks(std::move(hooks));
        panel_.setHostDebug([this] { return hostDebugRows(*this); });
        timer_.startTimerHz(kTickHz);
    }

    Editor::~Editor()
    {
        timer_.stopTimer();
        // The code window goes first, with the editor it has (the session keeps it out: the next editor opens it).
        window_.reset();
        panel_.setWindowHooks({});
        panel_.setHostDebug({});
    }

    void Editor::followLater()
    {
        const juce::Component::SafePointer<Editor> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr)
                safe->follow();
        });
    }

    void Editor::follow()
    {
        const bool out = processor_.editorPort().codeWindow().out;
        if (out && window_ == nullptr && isShowing())
            open();
        else if (!out && window_ != nullptr)
            close();
    }

    void Editor::tick()
    {
        follow();                                        // a session loaded (or an editor opened) with it out or in
        // In a host, the window floats over the host's windows (as its plug-in windows do) while the host is the
        // active app, and is a plain window while another app is (it then sits behind that app's windows).
        if (window_ != nullptr && processor_.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        {
            const bool front = juce::Process::isForegroundProcess();
            if (front != hostFloat_ && !window_->isMinimised())
            {
                hostFloat_ = front;
                window_->setAlwaysOnTop(front);
            }
        }
    }

    void Editor::open()
    {
        EditorPort& port = processor_.editorPort();
        const CodeWindow cw = port.codeWindow();
        auto code = std::make_unique<Panel>(processor_.ports(), port, Panel::Role::code);
        if (cw.width > 0 && cw.height > 0)
            code->resize(cw.width, cw.height);
        Panel& codePanel = *code;
        Panel::WindowHooks hooks;
        hooks.out = [this](bool) { followLater(); };
        hooks.raise = [this] { raiseMain(); };
        codePanel.setWindowHooks(std::move(hooks));
        panel_.lendEditor(codePanel);                    // the same editor, not a copy: it shows there from now on
        window_ = std::make_unique<Window>(*this, processor_, std::move(code));

        // Where the user left it, on a display that still shows it; else beside the editor's window (its right, else
        // its left, else over it a little down and right), within that display's user area.
        const int w = window_->getWidth(), h = window_->getHeight();
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        juce::Point<int> at;
        bool done = false;
        const juce::Rectangle<int> kept{ cw.x, cw.y, w, h };
        if (cw.placed)
            if (const auto* d = displays.getDisplayForRect(kept); d != nullptr && d->userArea.intersects(kept))
            {
                const auto area = d->userArea;
                at = { juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - w), cw.x),
                       juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - h), cw.y) };
                done = true;
            }
        if (!done)
        {
            const juce::Rectangle<int> main = getScreenBounds();
            const auto* d = displays.getDisplayForRect(main);
            const juce::Rectangle<int> area = d != nullptr ? d->userArea : main;
            const int y = juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - h), main.getY());
            if (main.getRight() + kGap + w <= area.getRight())
                at = { main.getRight() + kGap, y };
            else if (main.getX() - kGap - w >= area.getX())
                at = { main.getX() - kGap - w, y };
            else
                at = { juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - w), main.getX() + 40),
                       juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - h), main.getY() + 40) };
        }
        hostFloat_ = false;
        window_->showAt(at);
        tick();                                          // floating at once in a host that is the active app
    }

    void Editor::close()
    {
        panel_.takeEditorBack();                         // the editor back in this window, then the window goes
        window_.reset();
        hostFloat_ = false;
        grabKeyboardFocus();
    }

    void Editor::raiseCode()
    {
        if (window_ == nullptr)
            return;
        if (window_->isMinimised())
            window_->setMinimised(false);
        window_->toFront(true);
        window_->host().grabKeyboardFocus();
    }

    void Editor::raiseMain()
    {
        if (auto* top = getTopLevelComponent())
        {
            if (auto* peer = top->getPeer(); peer != nullptr && peer->isMinimised())
                peer->setMinimised(false);
            top->toFront(true);
        }
        grabKeyboardFocus();
    }
}
