// Source/plugin/standalone/NativeWindow.mm: see NativeWindow.h. Objective-C++ with ARC (CMakeLists.txt), the Standalone
// app only: its classes live in the app's own process, where no other product's can be.
#import <Cocoa/Cocoa.h>

#include "plugin/standalone/NativeWindow.h"

#include <algorithm>
#include <cmath>
#include <utility>

using playground::standalone::Command;

// A view of ours in the window's title bar (beside the buttons, under them, the size of the title bar's view): AppKit
// lays the title bar out again on a resize or a style change and puts the buttons back in its own place, then lays this
// view out, which puts them where placeButtons() said. A button AppKit moves at another time is put back on the next
// turn of the run loop. It takes no clicks, and reports full screen, where the system shows the title bar itself.
@interface PGTitleBarKeeper : NSView
- (void)setX:(CGFloat)x middle:(CGFloat)middle;
- (void)setFullScreenChanged:(std::function<void(bool)>)changed;
- (void)place;
@end

@implementation PGTitleBarKeeper
{
    CGFloat x_, middle_;                         // the buttons' left edge and middle, from the window's top left
    CGFloat baseHeight_;                         // the title bar's own height, before it was made taller
    BOOL placing_, placeQueued_, fullScreen_;
    std::function<void(bool)> fullScreenChanged_;
    NSMutableArray* observers_;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]) != nil)
    {
        x_ = -1.0;
        middle_ = 0.0;
        baseHeight_ = 0.0;
        observers_ = [NSMutableArray array];
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    return self;
}

- (NSView*)hitTest:(NSPoint)point
{
    (void) point;
    return nil;
}

- (BOOL)isOpaque { return NO; }

- (void)layout
{
    [super layout];
    [self place];
}

- (void)setX:(CGFloat)x middle:(CGFloat)middle
{
    x_ = x;
    middle_ = middle;
    [self place];
}

- (void)setFullScreenChanged:(std::function<void(bool)>)changed { fullScreenChanged_ = std::move(changed); }

- (void)queuePlace
{
    if (placeQueued_)
        return;
    placeQueued_ = YES;
    __weak PGTitleBarKeeper* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        PGTitleBarKeeper* keeper = weakSelf;
        if (keeper == nil)
            return;
        keeper->placeQueued_ = NO;
        [keeper place];
    });
}

- (void)place
{
    NSWindow* w = self.window;
    if (w == nil || placing_ || fullScreen_ || x_ < 0.0 || (w.styleMask & NSWindowStyleMaskFullScreen) != 0)
        return;
    NSButton* close = [w standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [w standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [w standardWindowButton:NSWindowZoomButton];
    NSView* bar = close.superview;
    NSView* container = bar.superview;
    if (close == nil || bar == nil || container == nil || container.superview == nil)
        return;
    placing_ = YES;
    const CGFloat H = NSHeight(w.frame);
    const CGFloat bw = NSWidth(close.frame), bh = NSHeight(close.frame);
    const CGFloat step = mini != nil ? NSMinX(mini.frame) - NSMinX(close.frame) : bw + 9.0;
    const CGFloat top = std::round(middle_ - 0.5 * bh);              // the buttons' top, from the window's top

    // The title bar's container, at the window's top, made tall enough to hold the buttons where they go.
    NSView* frameView = container.superview;
    NSRect c = [frameView convertRect:container.frame toView:nil];   // window coordinates (from the bottom left)
    if (baseHeight_ <= 0.0)
        baseHeight_ = NSHeight(c);
    const CGFloat h = std::max(baseHeight_, top + bh + 4.0);
    if (std::fabs(NSHeight(c) - h) > 0.5 || std::fabs(NSMaxY(c) - H) > 0.5)
    {
        c.size.height = h;
        c.origin.y = H - h;
        container.frame = [frameView convertRect:c fromView:nil];
    }

    NSButton* buttons[] = { close, mini, zoom };
    for (int i = 0; i < 3; ++i)
    {
        NSButton* b = buttons[i];
        if (b == nil)
            continue;
        const NSRect want = NSMakeRect(x_ + step * i, H - top - bh, bw, bh);       // window coordinates
        const NSPoint origin = [bar convertRect:want fromView:nil].origin;
        if (std::fabs(origin.x - b.frame.origin.x) > 0.25 || std::fabs(origin.y - b.frame.origin.y) > 0.25)
            [b setFrameOrigin:origin];
    }
    placing_ = NO;
}

- (void)viewWillMoveToWindow:(NSWindow*)window
{
    for (id o in observers_)
        [[NSNotificationCenter defaultCenter] removeObserver:o];
    [observers_ removeAllObjects];
    if (window == nil)
        return;

    __weak PGTitleBarKeeper* weakSelf = self;
    NSNotificationCenter* centre = [NSNotificationCenter defaultCenter];
    // A button AppKit moved without laying this view out: put back on the next turn.
    if (NSButton* close = [window standardWindowButton:NSWindowCloseButton])
    {
        close.postsFrameChangedNotifications = YES;
        [observers_ addObject:[centre addObserverForName:NSViewFrameDidChangeNotification object:close queue:nil
                                              usingBlock:^(NSNotification*) { [weakSelf queuePlace]; }]];
    }
    [observers_ addObject:[centre addObserverForName:NSWindowWillEnterFullScreenNotification object:window queue:nil
                                          usingBlock:^(NSNotification*) {
                                              PGTitleBarKeeper* keeper = weakSelf;
                                              if (keeper == nil)
                                                  return;
                                              keeper->fullScreen_ = YES;
                                              if (keeper->fullScreenChanged_)
                                                  keeper->fullScreenChanged_(true);
                                          }]];
    // JUCE gives the window its own style again once it has left full screen (NSViewComponentPeer's
    // resetWindowPresentation): the look goes back on after it, on the next turn.
    [observers_ addObject:[centre addObserverForName:NSWindowDidExitFullScreenNotification object:window queue:nil
                                          usingBlock:^(NSNotification*) {
                                              dispatch_async(dispatch_get_main_queue(), ^{
                                                  PGTitleBarKeeper* keeper = weakSelf;
                                                  NSWindow* w = keeper.window;
                                                  if (keeper == nil || w == nil)
                                                      return;
                                                  w.titlebarAppearsTransparent = YES;
                                                  w.titleVisibility = NSWindowTitleHidden;
                                                  w.styleMask |= NSWindowStyleMaskFullSizeContentView;
                                                  keeper->fullScreen_ = NO;
                                                  if (keeper->fullScreenChanged_)
                                                      keeper->fullScreenChanged_(false);
                                                  [keeper place];
                                              });
                                          }]];
}

- (void)dealloc
{
    for (id o in observers_)
        [[NSNotificationCenter defaultCenter] removeObserver:o];
}
@end

// The menu bar's target for the app's own commands: an item's tag is its Command. An Edit command the app does not take
// (the key window is not one of JUCE's: a save panel, the About panel) goes to the first responder as AppKit's own
// action, so its text fields cut, copy and paste as usual.
@interface PGMenuTarget : NSObject
- (void)setRun:(std::function<bool(Command)>)run;
- (void)run:(NSMenuItem*)item;
@end

@implementation PGMenuTarget
{
    std::function<bool(Command)> run_;
}

- (void)setRun:(std::function<bool(Command)>)run { run_ = std::move(run); }

- (void)run:(NSMenuItem*)item
{
    const auto command = static_cast<Command>(item.tag);
    if (run_ && run_(command))
        return;
    SEL standard = nil;
    switch (command)
    {
        case Command::undo:      standard = @selector(undo:); break;
        case Command::redo:      standard = @selector(redo:); break;
        case Command::cut:       standard = @selector(cut:); break;
        case Command::copy:      standard = @selector(copy:); break;
        case Command::paste:     standard = @selector(paste:); break;
        case Command::selectAll: standard = @selector(selectAll:); break;
        case Command::separator:
        case Command::about:
        case Command::settings:
        case Command::services:
        case Command::hide:
        case Command::hideOthers:
        case Command::showAll:
        case Command::quit:
        case Command::loadState:
        case Command::saveState:
        case Command::resetState:
        case Command::minimize:
        case Command::zoom:
        case Command::bringAllToFront:
            break;
    }
    if (standard != nil)
        [NSApp sendAction:standard to:nil from:item];
}
@end

namespace playground::standalone::native
{
    namespace
    {
        PGMenuTarget* menuTarget = nil;          // the menu bar's, for the app's life

        NSWindow* windowOf(void* peerView)
        {
            NSView* view = (__bridge NSView*) peerView;
            return view != nil ? view.window : nil;
        }

        PGTitleBarKeeper* keeperOf(NSWindow* w)
        {
            NSView* bar = [w standardWindowButton:NSWindowCloseButton].superview;
            for (NSView* v in bar.subviews)
                if ([v isKindOfClass:[PGTitleBarKeeper class]])
                    return (PGTitleBarKeeper*) v;
            return nil;
        }

        NSString* _Nonnull text(const char* s)
        {
            NSString* t = s != nullptr ? [NSString stringWithUTF8String:s] : nil;
            return t != nil ? (NSString* _Nonnull) t : @"";
        }

        NSString* _Nonnull text(const std::string& s) { return text(s.c_str()); }
    }

    ButtonsSize styleWindow(void* peerView, std::function<void(bool fullScreen)> fullScreenChanged)
    {
        NSWindow* w = windowOf(peerView);
        if (w == nil || (w.styleMask & NSWindowStyleMaskTitled) == 0)
            return {};
        w.titlebarAppearsTransparent = YES;
        w.titleVisibility = NSWindowTitleHidden;
        // A new window gets full-size content around the content it has: JUCE's component keeps its place and size,
        // and the window shrinks onto it. AppKit does so itself (measured on macOS 27, on a window not yet shown); one
        // that kept the frame instead would grow the content by the title bar, and the editor's saved size with it, at
        // every launch.
        if ((w.styleMask & NSWindowStyleMaskFullSizeContentView) == 0)
        {
            const NSRect content = [w contentRectForFrameRect:w.frame];
            w.styleMask |= NSWindowStyleMaskFullSizeContentView;
            if ((w.styleMask & NSWindowStyleMaskFullScreen) == 0 && !NSEqualRects(w.frame, content))
                [w setFrame:content display:NO];
        }

        NSButton* close = [w standardWindowButton:NSWindowCloseButton];
        NSButton* zoom = [w standardWindowButton:NSWindowZoomButton];
        NSView* bar = close.superview;
        if (close == nil || zoom == nil || bar == nil)
            return {};
        PGTitleBarKeeper* keeper = keeperOf(w);
        if (keeper == nil)
        {
            keeper = [[PGTitleBarKeeper alloc] initWithFrame:bar.bounds];
            [bar addSubview:keeper positioned:NSWindowBelow relativeTo:nil];
        }
        [keeper setFullScreenChanged:std::move(fullScreenChanged)];
        return { static_cast<float>(NSMaxX(zoom.frame) - NSMinX(close.frame)),
                 static_cast<float>(NSHeight(close.frame)) };
    }

    void placeButtons(void* peerView, float x, float middle)
    {
        if (NSWindow* w = windowOf(peerView))
            if (PGTitleBarKeeper* keeper = keeperOf(w))
                [keeper setX:static_cast<CGFloat>(x) middle:static_cast<CGFloat>(middle)];
    }

    void dragWindow(void* peerView, int clicks)
    {
        NSWindow* w = windowOf(peerView);
        NSEvent* e = NSApp.currentEvent;
        if (w == nil || e == nil || e.type != NSEventTypeLeftMouseDown || e.window != w)
            return;
        if (clicks >= 2 || e.clickCount >= 2)
        {
            // The title bar's double-click, as System Settings has it (Desktop & Dock): "Maximize" (zoom, the default),
            // "Fill" (zoom here: the editor's limits allow the whole screen), "Minimize" or "None". Before that setting
            // there was only AppleMiniaturizeOnDoubleClick.
            NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
            NSString* action = [defaults stringForKey:@"AppleActionOnDoubleClick"];
            if (action == nil)
                action = [defaults boolForKey:@"AppleMiniaturizeOnDoubleClick"] ? @"Minimize" : @"Maximize";
            if ([action isEqualToString:@"Minimize"])
                [w miniaturize:nil];
            else if (![action isEqualToString:@"None"])
                [w zoom:nil];
            return;
        }
        [w performWindowDragWithEvent:e];
        // The window server took the press: the view never sees its release. JUCE gets one, so it does not hold the
        // button down until the next move (and counts a second press as a double-click's).
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:e.locationInWindow
                                    modifierFlags:e.modifierFlags timestamp:e.timestamp windowNumber:e.windowNumber
                                          context:nil eventNumber:e.eventNumber clickCount:e.clickCount pressure:0.0f];
        if (up != nil)
            [NSApp postEvent:up atStart:YES];
    }

    void installMenuBar(const std::vector<Menu>& menus, std::function<bool(Command)> run)
    {
        if (menuTarget == nil)
            menuTarget = [[PGMenuTarget alloc] init];
        [menuTarget setRun:std::move(run)];

        NSMenu* bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
        NSMenu* windows = nil;
        for (const Menu& m : menus)
        {
            NSMenu* menu = [[NSMenu alloc] initWithTitle:text(m.title)];
            for (const MenuItem& it : m.items)
            {
                if (it.command == Command::separator)
                {
                    [menu addItem:[NSMenuItem separatorItem]];
                    continue;
                }
                SEL action = @selector(run:);
                id target = menuTarget;
                switch (it.command)
                {
                    case Command::hide:            action = @selector(hide:); target = NSApp; break;
                    case Command::hideOthers:      action = @selector(hideOtherApplications:); target = NSApp; break;
                    case Command::showAll:         action = @selector(unhideAllApplications:); target = NSApp; break;
                    case Command::quit:            action = @selector(terminate:); target = NSApp; break;
                    case Command::minimize:        action = @selector(performMiniaturize:); target = nil; break;
                    case Command::zoom:            action = @selector(performZoom:); target = nil; break;
                    case Command::bringAllToFront: action = @selector(arrangeInFront:); target = NSApp; break;
                    case Command::services:        action = nil; target = nil; break;
                    case Command::separator:
                    case Command::about:
                    case Command::settings:
                    case Command::loadState:
                    case Command::saveState:
                    case Command::resetState:
                    case Command::undo:
                    case Command::redo:
                    case Command::cut:
                    case Command::copy:
                    case Command::paste:
                    case Command::selectAll:
                        break;
                }
                const char key[2] = { it.shortcut.key, 0 };
                NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:text(it.title) action:action
                                                       keyEquivalent:text(key)];
                item.target = target;
                item.tag = static_cast<NSInteger>(it.command);
                if (it.shortcut.key != 0)
                    item.keyEquivalentModifierMask = NSEventModifierFlagCommand
                                                   | (it.shortcut.shift ? NSEventModifierFlagShift : 0)
                                                   | (it.shortcut.option ? NSEventModifierFlagOption : 0);
                if (it.command == Command::services)
                {
                    NSMenu* services = [[NSMenu alloc] initWithTitle:text(it.title)];
                    item.submenu = services;
                    NSApp.servicesMenu = services;
                }
                [menu addItem:item];
            }
            NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:text(m.title) action:nil keyEquivalent:@""];
            top.submenu = menu;
            [bar addItem:top];
            if (m.title == "Window")
                windows = menu;
        }
        NSApp.mainMenu = bar;
        if (windows != nil)
            NSApp.windowsMenu = windows;
    }

    void showAbout(const char* name, const char* version, const char* credits)
    {
        NSMutableParagraphStyle* centred = [[NSMutableParagraphStyle alloc] init];
        centred.alignment = NSTextAlignmentCenter;
        NSDictionary* style = @{ NSFontAttributeName : [NSFont systemFontOfSize:[NSFont smallSystemFontSize]],
                                 NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
                                 NSParagraphStyleAttributeName : centred };
        NSDictionary* options = @{
            NSAboutPanelOptionApplicationName : text(name),
            NSAboutPanelOptionApplicationVersion : text(version),
            NSAboutPanelOptionVersion : @"",
            NSAboutPanelOptionCredits : [[NSAttributedString alloc] initWithString:text(credits) attributes:style],
        };
        [NSApp orderFrontStandardAboutPanelWithOptions:options];
    }
}
