#include <QGuiApplication>
#include <QWindow>

#import <AppKit/AppKit.h>

@interface SteadyOverlayPanel : NSPanel
@end

@implementation SteadyOverlayPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

struct MacOverlayHost {
    SteadyOverlayPanel *panel;
    QWindow *view;
};

MacOverlayHost *createMacOverlay(QWindow *strip)
{
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) return nullptr;

    // AppKit needs the nonactivating style at panel creation for other apps' full-screen Spaces.
    auto *panel = [[SteadyOverlayPanel alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    panel.opaque = NO;
    panel.backgroundColor = NSColor.clearColor;
    panel.hasShadow = NO;
    panel.hidesOnDeactivate = NO;
    panel.ignoresMouseEvents = YES;
    panel.level = NSStatusWindowLevel;

    NSWindowCollectionBehavior behavior = NSWindowCollectionBehaviorCanJoinAllSpaces
        | NSWindowCollectionBehaviorStationary
        | NSWindowCollectionBehaviorFullScreenAuxiliary
        | NSWindowCollectionBehaviorIgnoresCycle;
    if (@available(macOS 13.0, *)) {
        behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
    }
    panel.collectionBehavior = behavior;

    // Embed the same Qt raster view used by the other platforms.
    QWindow *view = QWindow::fromWinId(reinterpret_cast<WId>(panel.contentView));
    if (!view) {
        [panel release];
        return nullptr;
    }
    strip->setParent(view);
    return new MacOverlayHost{panel, view};
}

void positionMacOverlay(MacOverlayHost *host, const QRect &geometry)
{
    const NSRect primary = NSScreen.screens.firstObject.frame;
    [host->panel setFrame:NSMakeRect(geometry.x(), NSMaxY(primary) - geometry.y() - geometry.height(),
                                    geometry.width(), geometry.height())
                  display:NO];
}

void showMacOverlay(MacOverlayHost *host, bool visible)
{
    SteadyOverlayPanel *panel = host->panel;
    if (visible) {
        [panel orderFrontRegardless];
        if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS")) {
            qWarning("overlay collection=%lu level=%ld ignoresMouse=%d hidesOnDeactivate=%d canBecomeKey=%d canBecomeMain=%d",
                     static_cast<unsigned long>(panel.collectionBehavior), static_cast<long>(panel.level),
                     panel.ignoresMouseEvents, panel.hidesOnDeactivate,
                     panel.canBecomeKeyWindow, panel.canBecomeMainWindow);
        }
    } else {
        [panel orderOut:nil];
    }
}

void destroyMacOverlay(MacOverlayHost *host, QWindow *strip)
{
    [host->panel orderOut:nil];
    strip->destroy();
    strip->setParent(nullptr);
    delete host->view;
    [host->panel release];
    delete host;
}
