// Source/editor/nogpu/SoftEditorHost.cpp: see SoftEditorHost.h. The input, zoom and resizing rules are FunkGui's
// EditorHost's (src/gpu/EditorHost.cpp), the menus and the chooser its HostServicesJuce's, kept the same so that the
// Panel cannot tell the hosts apart but by the pixels' path.
#include "editor/nogpu/SoftEditorHost.h"

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace playground::ui
{
    namespace
    {
        constexpr int    kTimerHz = 60;
        constexpr double kIdleFrame = 1.0 / 12.0;        // the frame period while nothing moves
        constexpr double kIdlePeriod = 0.1;              // Panel::idle, at least 10 Hz
        constexpr double kRasterPeriod = 1.0 / 30.0;     // at most 30 pictures a second
        constexpr double kNudgeSeconds = 0.5;            // full rate after input, as FramePump's nudge
        constexpr int    kMinZoomPercent = 25, kMaxZoomPercent = 400;

        double wallSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

        funkgui::Mods modsFrom(const juce::ModifierKeys& m)
        {
            funkgui::Mods out;
            out.shift = m.isShiftDown();
            out.cmd = m.isCommandDown();
            out.alt = m.isAltDown();
            out.ctrl = m.isCtrlDown();
            return out;
        }

        // FunkGui's EditorHost's rule (F1): the button of a press, its drags and its release (JUCE's mouseUp carries the
        // buttons as they were just before the release). Left when none is down (a move) and when several are: the
        // left one first, then the right, then the middle.
        funkgui::Button buttonFrom(const juce::ModifierKeys& m)
        {
            if (m.isLeftButtonDown())
                return funkgui::Button::left;
            if (m.isRightButtonDown())
                return funkgui::Button::right;
            if (m.isMiddleButtonDown())
                return funkgui::Button::middle;
            return funkgui::Button::left;
        }

        std::vector<std::string> pathsOf(const juce::StringArray& files)
        {
            std::vector<std::string> paths;
            paths.reserve(static_cast<size_t>(files.size()));
            for (const auto& f : files)
                paths.emplace_back(f.toStdString());
            return paths;
        }

        juce::String fromUtf8(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

        // A save pattern's one extension ("*.fmpack" -> ".fmpack"), else none.
        juce::String saveExtension(std::string_view pattern)
        {
            if (pattern.size() < 3 || pattern[0] != '*' || pattern[1] != '.')
                return {};
            const std::string_view ext = pattern.substr(1);
            return ext.find_first_of("*?;, ") == std::string_view::npos ? fromUtf8(ext) : juce::String();
        }

        template <typename T>
        bool sameBytes(const std::vector<T>& a, const std::vector<T>& b)
        {
            return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
        }
    }

    //==================================================================================================================
    // Lifetime
    //==================================================================================================================

    SoftEditorHost::SoftEditorHost(juce::AudioProcessor& owner, SoftEditorConfig config,
                                   std::unique_ptr<funkgui::Panel> panel)
        : juce::AudioProcessorEditor(owner), panel_(std::move(panel)), config_(std::move(config)),
          canvas_(funkgui::FontService::get().atlas()), pending_(std::make_shared<Pending>())
    {
        jassert(panel_ != nullptr);
        setOpaque(true);
        setWantsKeyboardFocus(true);

        logicalW_ = panel_->width();
        logicalH_ = panel_->height();
        resizable_ = panel_->resizable();
        setResizable(resizable_, resizable_);

        auto& prefs = funkgui::UiPreferences::get();
        prefs.reload();
        prefsRevision_ = prefs.revision();
        applyTheme(prefs.theme());

        for (const int s : config_.zoomSteps)
            if (s >= kMinZoomPercent && s <= kMaxZoomPercent)
                zoomSteps_.push_back(s);
        std::sort(zoomSteps_.begin(), zoomSteps_.end());
        zoomSteps_.erase(std::unique(zoomSteps_.begin(), zoomSteps_.end()), zoomSteps_.end());
        if (!zoomSteps_.empty())
        {
            zoomDefault_ = zoomSteps_.front();
            for (const int s : zoomSteps_)
                if (std::abs(s - config_.defaultZoomPercent) < std::abs(zoomDefault_ - config_.defaultZoomPercent))
                    zoomDefault_ = s;
        }
        zoomChosen_ = readZoomPreference();
        updateZoomTarget();
        applyZoom();
        applyResizeLimits();

        panel_->attach(*this);
        start_ = lastFrame_ = lastIdle_ = wallSeconds();
        fullRateUntil_ = start_ + kNudgeSeconds;
        startTimerHz(kTimerHz);
    }

    SoftEditorHost::~SoftEditorHost()
    {
        stopTimer();
        // No menu or chooser calls back into a Panel that is going.
        dismissMenus();
        pending_->filesOpen = false;
        pending_->files = nullptr;
        chooser_.reset();
        retiredChooser_.reset();
        // A host can close the editor with the mouse still down: the gesture must not stay open.
        panel_->closeGestures();
    }

    void SoftEditorHost::applyTheme(int idx)
    {
        themeIdx_ = std::clamp(idx, 0, funkgui::Theme::kCount - 1);
        theme_ = funkgui::Theme::byIndex(themeIdx_);
        dirty_ = true;
    }

    int SoftEditorHost::themeIndex() const
    {
        return funkgui::UiPreferences::get().theme();
    }

    float SoftEditorHost::deviceScale() const
    {
        const float s = static_cast<float>(juce::Component::getApproximateScaleFactorForComponent(this));
        return s > 0.05f && std::isfinite(s) ? s : 1.0f;
    }

    //==================================================================================================================
    // The frame
    //==================================================================================================================

    void SoftEditorHost::timerCallback()
    {
        const double now = wallSeconds();

        // The preferences another editor changed: the theme, the zoom; then a zoom chosen since the last frame.
        auto& prefs = funkgui::UiPreferences::get();
        if (prefs.revision() != prefsRevision_)
        {
            prefsRevision_ = prefs.revision();
            applyTheme(prefs.theme());
            const int chosen = readZoomPreference();
            if (chosen != zoomChosen_)
            {
                zoomChosen_ = chosen;
                updateZoomTarget();
            }
        }
        if (zoomTarget_ != zoomApplied_)
            applyZoom();

        if (now - lastIdle_ >= kIdlePeriod)
        {
            lastIdle_ = now;
            panel_->idle(now);
        }
        if (!isShowing())
            return;
        const bool fullRate = panel_->wantsFullRate() || now < fullRateUntil_ || dirty_;
        if (!fullRate && now - lastFrame_ < kIdleFrame)
            return;
        frame(now);
    }

    void SoftEditorHost::frame(double now)
    {
        const float dt = static_cast<float>(std::clamp(now - lastFrame_, 0.0, 0.1));
        lastFrame_ = now;
        panel_->tick(dt);
        seconds_ += static_cast<double>(dt);
        ++ticks_;

        const float dpi = deviceScale() * zoomScale_;
        // Unchanged frames (EditorHost's rule since FunkGui v0.25.0): nothing the Panel draws changed (needsDraw()) and
        // the picture up is of this size, scale and theme: nothing is recorded.
        if (!dirty_ && image_.isValid() && !panel_->needsDraw() && lastInfo_.logicalW == panel_->width()
            && lastInfo_.logicalH == panel_->height() && lastInfo_.dpi == dpi && lastInfo_.theme == themeIdx_)
            return;
        funkgui::FrameInfo info;
        info.logicalW = panel_->width();
        info.logicalH = panel_->height();
        info.dpi = dpi;
        info.clear = theme_.ground;
        info.textGamma = theme_.textGamma;
        info.theme = themeIdx_;
        info.seconds = static_cast<float>(seconds_);
        info.frame = ticks_;
        info.dt = dt;
        info.fullRate = panel_->wantsFullRate();
        canvas_.begin(info);
        panel_->draw(canvas_, theme_);
        const funkgui::PrimList& list = canvas_.end();

        if (!dirty_ && image_.isValid() && sameAsLast(list))
            return;                                      // the same picture: nothing to draw
        dirty_ = true;
        if (now - lastRaster_ < kRasterPeriod)
            return;                                      // drawn at the next frame (dirty_ keeps the rate up)
        lastRaster_ = now;
        keepAsLast(list);

        const funkgui::Image frame = funkgui::rasterise(list, funkgui::FontService::get().atlas(), 1);
        if (frame.w <= 0 || frame.h <= 0 || frame.rgba.size() < static_cast<size_t>(frame.w) * static_cast<size_t>(frame.h) * 4u)
            return;
        if (!image_.isValid() || image_.getWidth() != frame.w || image_.getHeight() != frame.h)
            image_ = juce::Image(juce::Image::ARGB, frame.w, frame.h, false, juce::SoftwareImageType());
        {
            const juce::Image::BitmapData bits(image_, juce::Image::BitmapData::writeOnly);
            const uint8_t* src = frame.rgba.data();
            for (int y = 0; y < frame.h; ++y)
            {
                uint8_t* line = bits.getLinePointer(y);
                for (int x = 0; x < frame.w; ++x, src += 4)
                {
                    auto* px = reinterpret_cast<juce::PixelARGB*>(line + x * bits.pixelStride);
                    const uint32_t a = src[3];
                    // Straight alpha to JUCE's premultiplied (the frame over its ground is opaque: a = 255).
                    px->setARGB(static_cast<uint8_t>(a), static_cast<uint8_t>((src[0] * a + 127u) / 255u),
                                static_cast<uint8_t>((src[1] * a + 127u) / 255u),
                                static_cast<uint8_t>((src[2] * a + 127u) / 255u));
                }
            }
        }
        dirty_ = false;
        repaint();
    }

    bool SoftEditorHost::sameAsLast(const funkgui::PrimList& list) const
    {
        const funkgui::FrameInfo& a = list.info;
        const funkgui::FrameInfo& b = lastInfo_;
        if (a.logicalW != b.logicalW || a.logicalH != b.logicalH || a.dpi != b.dpi || a.theme != b.theme
            || a.textGamma != b.textGamma || std::memcmp(&a.clear, &b.clear, sizeof a.clear) != 0)
            return false;
        // A picture's pixels may change under the same id (the registry's): those frames list an image draw, and
        // are drawn again whenever anything else moves; a frame with pictures alone in motion is rare.
        return sameBytes(list.prims, lastPrims_) && sameBytes(list.shaders, lastShaders_)
            && sameBytes(list.images, lastImages_);
    }

    void SoftEditorHost::keepAsLast(const funkgui::PrimList& list)
    {
        lastInfo_ = list.info;
        lastPrims_ = list.prims;
        lastShaders_ = list.shaders;
        lastImages_ = list.images;
    }

    void SoftEditorHost::paint(juce::Graphics& g)
    {
        g.fillAll(juce::Colour::fromRGB(theme_.ground.r, theme_.ground.g, theme_.ground.b));
        if (!image_.isValid() || getWidth() <= 0 || getHeight() <= 0)
            return;
        // The picture is the editor's size in device pixels: drawn back at 1 : 1 on the display.
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImageTransformed(image_, juce::AffineTransform::scale(static_cast<float>(getWidth()) / static_cast<float>(image_.getWidth()),
                                                                    static_cast<float>(getHeight()) / static_cast<float>(image_.getHeight())));
    }

    void SoftEditorHost::resized()
    {
        followLogicalSize();
        dirty_ = true;
    }

    //==================================================================================================================
    // Input (EditorHost's conversions)
    //==================================================================================================================

    funkgui::PointerEvent SoftEditorHost::pointer(const juce::MouseEvent& e) const
    {
        funkgui::PointerEvent p;
        p.x = e.position.x / zoomScale_;
        p.y = e.position.y / zoomScale_;
        p.mods = modsFrom(e.mods);
        p.clicks = e.getNumberOfClicks();
        p.popup = e.mods.isPopupMenu();
        p.button = buttonFrom(e.mods);                   // F1: a middle press was (and is) a press; now it says so
        return p;
    }

    void SoftEditorHost::updateCursor()
    {
        using MC = juce::MouseCursor;
        using funkgui::Cursor;
        switch (panel_->cursor())
        {
            case Cursor::normal:       setMouseCursor(MC::NormalCursor); break;
            case Cursor::leftRight:    setMouseCursor(MC::LeftRightResizeCursor); break;
            case Cursor::upDown:       setMouseCursor(MC::UpDownResizeCursor); break;
            case Cursor::pointingHand: setMouseCursor(MC::PointingHandCursor); break;
            case Cursor::crosshair:    setMouseCursor(MC::CrosshairCursor); break;
            case Cursor::grab:         setMouseCursor(MC::DraggingHandCursor); break;
            case Cursor::grabbing:     setMouseCursor(MC::DraggingHandCursor); break;   // JUCE has no closed hand
            case Cursor::copy:         setMouseCursor(MC::CopyingCursor); break;
            case Cursor::notAllowed:   setMouseCursor(MC::NormalCursor); break;         // the refusal shows in the frame
        }
    }

    void SoftEditorHost::mouseMove(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        panel_->pointerMove(pointer(e));
        updateCursor();
    }

    void SoftEditorHost::mouseExit(const juce::MouseEvent&)
    {
        panel_->pointerExit();
        updateCursor();
    }

    void SoftEditorHost::mouseDown(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        dragSource_ = e.source;
        dragScreen_ = e.getScreenPosition().toFloat();
        panel_->pointerDown(pointer(e));
        updateCursor();
    }

    void SoftEditorHost::mouseDrag(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        dragScreen_ = e.getScreenPosition().toFloat();
        panel_->pointerDrag(pointer(e));
        updateCursor();
    }

    void SoftEditorHost::mouseUp(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        dragScreen_ = e.getScreenPosition().toFloat();
        panel_->pointerUp(pointer(e));
        if (unbounded_)
            setUnboundedDrag(false);
        dragSource_.reset();
        updateCursor();
    }

    void SoftEditorHost::mouseDoubleClick(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        panel_->doubleClick(pointer(e));
        updateCursor();
    }

    void SoftEditorHost::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        funkgui::WheelEvent w;
        w.x = e.position.x / zoomScale_;
        w.y = e.position.y / zoomScale_;
        w.dx = wheel.deltaX;
        w.dy = wheel.deltaY;
        w.smooth = wheel.isSmooth;
        w.reversed = wheel.isReversed;
        w.inertial = wheel.isInertial;
        w.mods = modsFrom(e.mods);
        nudgeFullRate();
        const bool used = panel_->wheel(w);
        updateCursor();
        if (!used)
            juce::Component::mouseWheelMove(e, wheel);   // the host's window scrolls, never a dial
    }

    // FunkGui's EditorHost's (F1): a trackpad's pinch; one the Panel does not use goes to the parent, as the wheel.
    void SoftEditorHost::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
    {
        funkgui::MagnifyEvent g;
        g.x = e.position.x / zoomScale_;
        g.y = e.position.y / zoomScale_;
        g.scale = scaleFactor;
        g.mods = modsFrom(e.mods);
        nudgeFullRate();
        const bool used = panel_->magnify(g);
        updateCursor();
        if (!used)
            juce::Component::mouseMagnify(e, scaleFactor);
    }

    bool SoftEditorHost::keyPressed(const juce::KeyPress& k)
    {
        using KP = juce::KeyPress;
        using funkgui::Key;
        funkgui::KeyEvent ev;
        ev.mods = modsFrom(k.getModifiers());
        const int code = k.getKeyCode();
        if (code == KP::tabKey)                ev.key = Key::tab;
        else if (code == KP::upKey)            ev.key = Key::up;
        else if (code == KP::downKey)          ev.key = Key::down;
        else if (code == KP::leftKey)          ev.key = Key::left;
        else if (code == KP::rightKey)         ev.key = Key::right;
        else if (code == KP::pageUpKey)        ev.key = Key::pageUp;
        else if (code == KP::pageDownKey)      ev.key = Key::pageDown;
        else if (code == KP::homeKey)          ev.key = Key::home;
        else if (code == KP::endKey)           ev.key = Key::end;
        else if (code == KP::escapeKey)        ev.key = Key::escape;
        else if (code == KP::returnKey)        ev.key = Key::enter;
        else if (code == KP::backspaceKey)     ev.key = Key::backspace;
        else if (code == KP::deleteKey)        ev.key = Key::del;
        else if (code == KP::spaceKey)
        {
            ev.key = Key::space;
            ev.ch = U' ';
        }
        else
        {
            // A character as the OS delivers it: its text, or, with the command key (Ctrl here) held, no text or a
            // control code, and then the key's own (lower-case) code, as HeadlessHost::keys gives it.
            const juce::juce_wchar text = k.getTextCharacter();
            char32_t ch = 0;
            if (text >= 0x20 && !(ev.mods.cmd && !ev.mods.alt))
                ch = static_cast<char32_t>(text);
            else if (code >= 0x20 && code < 0x7f)
                ch = static_cast<char32_t>(juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code)));
            else if (text >= 0x20)
                ch = static_cast<char32_t>(text);
            if (ch == 0)
                return false;
            ev.key = Key::character;
            ev.ch = ch;
        }
        nudgeFullRate();
        const bool used = panel_->key(ev);
        updateCursor();
        return used;
    }

    bool SoftEditorHost::isInterestedInFileDrag(const juce::StringArray& files)
    {
        return panel_->filesInterest(pathsOf(files));
    }

    void SoftEditorHost::filesDropped(const juce::StringArray& files, int, int)
    {
        panel_->filesDropped(pathsOf(files));
        nudgeFullRate();
    }

    void SoftEditorHost::fileDragEnter(const juce::StringArray& files, int x, int y)
    {
        panel_->filesDragEnter(pathsOf(files), static_cast<float>(x) / zoomScale_, static_cast<float>(y) / zoomScale_);
        nudgeFullRate();
    }

    void SoftEditorHost::fileDragMove(const juce::StringArray&, int x, int y)
    {
        panel_->filesDragMove(static_cast<float>(x) / zoomScale_, static_cast<float>(y) / zoomScale_);
        nudgeFullRate();
    }

    void SoftEditorHost::fileDragExit(const juce::StringArray&)
    {
        panel_->filesDragExit();
        nudgeFullRate();
    }

    //==================================================================================================================
    // HostServices
    //==================================================================================================================

    void SoftEditorHost::setUnboundedDrag(bool on)
    {
        if (on)
        {
            if (dragSource_.has_value() && dragSource_->canDoUnboundedMovement())
            {
                dragSource_->enableUnboundedMouseMovement(true, true);
                unbounded_ = true;
            }
            return;
        }
        if (unbounded_ && dragSource_.has_value())
        {
            // The pointer shown again where the drag would have it, inside the display's user area.
            const auto area = getParentMonitorArea().reduced(2, 2).toFloat();
            const auto shown = area.getConstrainedPoint(dragScreen_);
            dragSource_->enableUnboundedMouseMovement(false, true);
            dragSource_->setScreenPosition(shown);
        }
        unbounded_ = false;
    }

    void SoftEditorHost::showParamMenu(funkgui::ParamPort& port, float x, float y)
    {
        auto* host = getHostContext();
        auto* param = static_cast<juce::RangedAudioParameter*>(port.native());
        if (host == nullptr || param == nullptr)
            return;
        if (auto menu = host->getContextMenuForParameter(param))
            menu->showNativeMenu({ juce::roundToInt(x * zoomScale_), juce::roundToInt(y * zoomScale_) });
    }

    void SoftEditorHost::nudgeFullRate()
    {
        fullRateUntil_ = wallSeconds() + kNudgeSeconds;
    }

    double SoftEditorHost::nowSeconds() const
    {
        return wallSeconds();
    }

    unsigned SoftEditorHost::services() const
    {
        return funkgui::hostservice::menus | funkgui::hostservice::fileChooser | funkgui::hostservice::clipboard;
    }

    bool SoftEditorHost::showMenu(const funkgui::MenuRequest& request, funkgui::MenuCallback done)
    {
        dismissMenus();                                  // a second menu replaces the first, even when refused
        if (!funkgui::validMenu(request))                // HostServices' rule: headers and submenus too (0.25.0)
            return false;
        if (look_ == nullptr)
            look_ = std::make_unique<funkgui::MenuLook>(request.theme);
        look_->setTheme(request.theme);

        // Every field of the request, as HostServicesJuce gives it to JUCE (its menuOptions): the rows (headers, radio
        // items, shortcuts, submenus), one column that scrolls, the dropdown's width and current choice, and the anchor
        // that puts the look's panel, not its shadow's margin, against the trigger.
        juce::PopupMenu m = funkgui::MenuLook::popupMenuFor(request);
        m.setLookAndFeel(look_.get());
        const float scale = logicalW_ > 0 ? static_cast<float>(getWidth()) / static_cast<float>(logicalW_) : 1.0f;
        const funkgui::Rect& r = request.anchor;
        const juce::Rectangle<int> anchor = localAreaToGlobal(
            juce::Rectangle<float>(r.x * scale, r.y * scale, r.w * scale, r.h * scale).toNearestInt());
        const float menuScale = juce::Component::getApproximateScaleFactorForComponent(this);
        juce::PopupMenu::Options options = juce::PopupMenu::Options()
                                               .withTargetComponent(this)
                                               .withTargetScreenArea(look_->targetAreaFor(anchor, menuScale))
                                               .withMaximumNumColumns(1);
        if (request.minWidth > 0.0f)                     // in the window's px, which are this component's
            options = options.withMinimumWidth(juce::roundToInt(request.minWidth * scale) + 2 * look_->shadowPx());
        if (request.selectedId > 0)
            options = options.withInitiallySelectedItem(request.selectedId)
                          .withItemThatMustBeVisible(request.selectedId);

        Pending& p = *pending_;
        const uint32_t serial = ++p.menuSerial;
        p.menu = std::move(done);
        p.menuOpen = true;
        const auto finish = [pending = pending_, serial](int id) {
            if (!pending->menuOpen || pending->menuSerial != serial)
                return;
            pending->menuOpen = false;
            const funkgui::MenuCallback callback = std::exchange(pending->menu, nullptr);
            if (callback)
                callback(id > 0 ? id : 0);
        };
        p.asking = true;
        m.showMenuAsync(options,
                        [pending = pending_, finish](int id) {
                            if (pending->asking)
                                juce::MessageManager::callAsync([finish, id] { finish(id); });
                            else
                                finish(id);
                        });
        p.asking = false;
        return true;
    }

    void SoftEditorHost::dismissMenus()
    {
        Pending& p = *pending_;
        if (!p.menuOpen)
            return;
        p.menuOpen = false;
        p.menu = nullptr;
        juce::PopupMenu::dismissAllActiveMenus();
    }

    bool SoftEditorHost::chooseFiles(const funkgui::FileRequest& request, funkgui::FilesCallback done)
    {
        using FB = juce::FileBrowserComponent;
        using Mode = funkgui::FileRequest::Mode;
        const bool save = request.mode == Mode::save;
        const juce::String extension = save ? saveExtension(request.pattern) : juce::String();

        juce::File initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        if (save && !request.suggestedName.empty())
        {
            const juce::String name = fromUtf8(request.suggestedName);
            const bool hasExtension = extension.isNotEmpty() && name.length() > extension.length()
                                      && name.endsWithIgnoreCase(extension);
            initial = initial.getChildFile(
                hasExtension ? juce::File::createLegalFileName(name.dropLastCharacters(extension.length()))
                                   + name.getLastCharacters(extension.length())
                             : juce::File::createLegalFileName(name));
        }
        int flags = FB::canSelectFiles;
        if (save)
            flags |= FB::saveMode | FB::warnAboutOverwriting;
        else if (request.mode == Mode::openMany)
            flags |= FB::openMode | FB::canSelectMultipleItems;
        else
            flags |= FB::openMode;

        Pending& p = *pending_;
        if (p.filesOpen)
            chooser_.reset();                            // the chooser still open is replaced, its callback unrun
        else
            retiredChooser_ = std::move(chooser_);       // one that finished may be the one calling back now
        const uint32_t serial = ++p.filesSerial;
        p.files = std::move(done);
        p.filesOpen = true;
        const auto finish = [pending = pending_, serial](const std::vector<std::string>& paths) {
            if (!pending->filesOpen || pending->filesSerial != serial)
                return;
            pending->filesOpen = false;
            const funkgui::FilesCallback callback = std::exchange(pending->files, nullptr);
            if (callback)
                callback(paths);
        };
        chooser_ = std::make_unique<juce::FileChooser>(fromUtf8(request.title), initial, fromUtf8(request.pattern),
                                                       true, false, this);
        p.asking = true;
        chooser_->launchAsync(flags, [pending = pending_, finish, save, extension](const juce::FileChooser& fc) {
            std::vector<std::string> paths;
            if (save)
            {
                juce::File f = fc.getResult();
                if (f != juce::File())
                {
                    if (extension.isNotEmpty() && !f.hasFileExtension(extension))
                        f = f.withFileExtension(extension);
                    paths.push_back(f.getFullPathName().toStdString());
                }
            }
            else
            {
                for (const juce::File& f : fc.getResults())
                    paths.push_back(f.getFullPathName().toStdString());
            }
            if (pending->asking)
                juce::MessageManager::callAsync([finish, paths] { finish(paths); });
            else
                finish(paths);
        });
        p.asking = false;
        return true;
    }

    bool SoftEditorHost::copyText(std::string_view utf8)
    {
        juce::SystemClipboard::copyTextToClipboard(fromUtf8(utf8));
        return true;
    }

    std::string SoftEditorHost::clipboardText() const
    {
        return juce::SystemClipboard::getTextFromClipboard().toStdString();
    }

    void SoftEditorHost::announce(std::string_view text, Announce priority)
    {
        if (text.empty())
            return;
        using P = juce::AccessibilityHandler::AnnouncementPriority;
        const P p = priority == Announce::low ? P::low : (priority == Announce::high ? P::high : P::medium);
        juce::AccessibilityHandler::postAnnouncement(fromUtf8(text), p);
    }

    //==================================================================================================================
    // The zoom and the size
    //==================================================================================================================

    void SoftEditorHost::setZoomPercent(int percent)
    {
        if (!isZoomStep(percent))
            return;
        if (config_.zoomPrefKey != nullptr && config_.zoomPrefKey[0] != '\0')
            funkgui::UiPreferences::get().setInt(config_.zoomPrefKey, percent);
        zoomChosen_ = percent;
        updateZoomTarget();
        nudgeFullRate();
    }

    bool SoftEditorHost::zoomFits(int percent) const
    {
        return isZoomStep(percent) && fitZoom(percent) == percent;
    }

    bool SoftEditorHost::isZoomStep(int percent) const noexcept
    {
        return std::binary_search(zoomSteps_.begin(), zoomSteps_.end(), percent);
    }

    int SoftEditorHost::readZoomPreference() const
    {
        if (zoomSteps_.empty())
            return 100;
        if (config_.zoomPrefKey == nullptr || config_.zoomPrefKey[0] == '\0')
            return zoomDefault_;
        const int v = funkgui::UiPreferences::get().getInt(config_.zoomPrefKey, zoomDefault_,
                                                           std::numeric_limits<int>::min(),
                                                           std::numeric_limits<int>::max());
        return isZoomStep(v) ? v : zoomDefault_;
    }

    void SoftEditorHost::updateZoomTarget()
    {
        zoomTarget_ = zoomSteps_.empty() ? 100 : fitZoom(zoomChosen_);
    }

    int SoftEditorHost::zoomedSize(int logical, int percent) const noexcept
    {
        return percent == 100 ? logical : static_cast<int>(std::lround(static_cast<double>(logical) * percent / 100.0));
    }

    juce::Rectangle<int> SoftEditorHost::fitArea() const
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const juce::Displays::Display* d = getPeer() != nullptr
                                             ? displays.getDisplayForRect(getScreenBounds())
                                             : displays.getDisplayForPoint(juce::Desktop::getMousePosition());
        if (d == nullptr)
            d = displays.getPrimaryDisplay();
        return d != nullptr ? d->userArea : juce::Rectangle<int>();
    }

    int SoftEditorHost::fitZoom(int chosen) const
    {
        if (zoomSteps_.empty())
            return 100;
        const auto area = fitArea();
        if (area.isEmpty())
            return chosen;
        int best = 0;
        for (const int s : zoomSteps_)
        {
            if (s > chosen)
                break;
            if (zoomedSize(logicalW_, s) <= area.getWidth() && zoomedSize(logicalH_, s) <= area.getHeight())
                best = s;
        }
        return best > 0 ? best : zoomSteps_.front();
    }

    void SoftEditorHost::applyResizeLimits()
    {
        if (!resizable_)
            return;
        constexpr int kNoLimit = 16384;
        const int minW = zoomedSize(std::max(1, panel_->minWidth()), zoomApplied_);
        const int minH = zoomedSize(std::max(1, panel_->minHeight()), zoomApplied_);
        const int maxW = config_.maxWidth > 0 ? std::max(minW, zoomedSize(config_.maxWidth, zoomApplied_)) : kNoLimit;
        const int maxH = config_.maxHeight > 0 ? std::max(minH, zoomedSize(config_.maxHeight, zoomApplied_)) : kNoLimit;
        applyingZoom_ = true;
        setResizeLimits(minW, minH, maxW, maxH);
        applyingZoom_ = false;
    }

    void SoftEditorHost::applyZoom()
    {
        const int before = zoomApplied_;
        zoomApplied_ = zoomTarget_;
        zoomScale_ = static_cast<float>(zoomApplied_) / 100.0f;
        if (before != zoomApplied_)
            applyResizeLimits();
        const int w = zoomedSize(logicalW_, zoomApplied_);
        const int h = zoomedSize(logicalH_, zoomApplied_);
        dirty_ = true;
        if (w == getWidth() && h == getHeight())
            return;
        applyingZoom_ = true;
        setSize(w, h);
        applyingZoom_ = false;
    }

    void SoftEditorHost::followLogicalSize()
    {
        if (!resizable_ || applyingZoom_ || !(zoomScale_ > 0.0f) || getWidth() <= 0 || getHeight() <= 0)
            return;
        const int w = std::max(panel_->minWidth(), juce::roundToInt(static_cast<float>(getWidth()) / zoomScale_));
        const int h = std::max(panel_->minHeight(), juce::roundToInt(static_cast<float>(getHeight()) / zoomScale_));
        if (w == logicalW_ && h == logicalH_)
            return;
        logicalW_ = w;
        logicalH_ = h;
        if (w != panel_->width() || h != panel_->height())
            panel_->resize(w, h);
        nudgeFullRate();
        if (config_.sizeChanged)
            config_.sizeChanged(w, h);
    }
}
