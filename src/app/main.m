#import <Cocoa/Cocoa.h>

#include "../motion/imu.h"
#include <math.h>

@interface AartiView : NSView
@end

@implementation AartiView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)drawRect:(NSRect)rect {
    [super drawRect:rect];

    NSRect bounds = self.bounds;

    [[NSColor colorWithCalibratedRed:0.055
                               green:0.035
                                blue:0.020
                               alpha:1.0] setFill];

    NSRectFill(bounds);

    MotionState state = motion_get_state();

    NSString *status;

    if (!state.calibrated) {
        status = @"CALIBRATING — KEEP MAC STILL";
    } else if (state.rotating) {
        status =
            state.direction > 0
                ? @"AARTI — CLOCKWISE"
                : @"AARTI — ANTICLOCKWISE";
    } else {
        status = @"READY";
    }

    NSDictionary *attrs = @{
        NSFontAttributeName:
            [NSFont systemFontOfSize:24
                              weight:NSFontWeightMedium],
        NSForegroundColorAttributeName:
            [NSColor whiteColor]
    };

    [status drawAtPoint:NSMakePoint(40, bounds.size.height - 70)
         withAttributes:attrs];

    NSString *motionText =
        [NSString stringWithFormat:
            @"Roll %+.1f°   Pitch %+.1f°   Yaw %+.1f°",
            state.roll,
            state.pitch,
            state.yaw];

    NSDictionary *smallAttrs = @{
        NSFontAttributeName:
            [NSFont monospacedSystemFontOfSize:14
                                         weight:NSFontWeightRegular],
        NSForegroundColorAttributeName:
            [NSColor lightGrayColor]
    };

    [motionText drawAtPoint:NSMakePoint(40, 40)
             withAttributes:smallAttrs];

    /*
     * Temporary motion visualization.
     *
     * This will become the actual thali renderer in the
     * next rendering pass.
     */

    CGFloat centerX = bounds.size.width / 2.0;
    CGFloat centerY = bounds.size.height / 2.0;

    CGFloat motionRadius = MIN(bounds.size.width, bounds.size.height) * 0.22;
    CGFloat normalizedRoll =
        fmax(-1.0, fmin(1.0, (CGFloat)state.roll / 30.0));
    CGFloat normalizedPitch =
        fmax(-1.0, fmin(1.0, (CGFloat)state.pitch / 30.0));

    CGFloat x =
        centerX +
        normalizedRoll * motionRadius;

    CGFloat y =
        centerY +
        normalizedPitch * motionRadius;

    NSPoint p = NSMakePoint(x, y);

    [[NSColor colorWithCalibratedRed:0.75
                               green:0.55
                                blue:0.20
                               alpha:1.0] setFill];

    NSBezierPath *plate =
        [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(
                p.x - 110,
                p.y - 110,
                220,
                220
            )];

    [plate fill];

    [[NSColor colorWithCalibratedRed:0.92
                               green:0.75
                                blue:0.32
                               alpha:1.0] setStroke];

    [plate setLineWidth:5];
    [plate stroke];

    /*
     * Diya flame.
     */

    NSBezierPath *flame =
        [NSBezierPath bezierPath];

    [flame moveToPoint:
        NSMakePoint(p.x, p.y + 55)];

    [flame curveToPoint:
        NSMakePoint(p.x + 22, p.y + 10)
        controlPoint1:
            NSMakePoint(p.x + 5, p.y + 40)
        controlPoint2:
            NSMakePoint(p.x + 28, p.y + 30)];

    [flame curveToPoint:
        NSMakePoint(p.x, p.y - 10)
        controlPoint1:
            NSMakePoint(p.x + 20, p.y - 5)
        controlPoint2:
            NSMakePoint(p.x - 10, p.y + 5)];

    [flame curveToPoint:
        NSMakePoint(p.x - 22, p.y + 10)
        controlPoint1:
            NSMakePoint(p.x - 28, p.y + 30)
        controlPoint2:
            NSMakePoint(p.x - 5, p.y + 40)];

    [flame closePath];

    [[NSColor orangeColor] setFill];
    [flame fill];
}

@end


@interface AartiAppDelegate : NSObject
    <NSApplicationDelegate>

@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) AartiView *view;
@property(nonatomic, strong) NSTimer *timer;

@end


@implementation AartiAppDelegate

- (void)applicationDidFinishLaunching:
    (NSNotification *)notification {

    (void)notification;

    if (!motion_start()) {
        NSLog(@"Could not start motion engine.");
        [NSApp terminate:nil];
        return;
    }

    NSScreen *screen =
        [NSScreen mainScreen];

    NSRect frame =
        [screen frame];

    self.window =
        [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered
            defer:NO];

    self.window.backgroundColor =
        [NSColor blackColor];

    self.window.opaque = YES;

    self.window.level =
        NSMainMenuWindowLevel + 1;

    self.window.collectionBehavior =
        NSWindowCollectionBehaviorFullScreenPrimary;

    self.view =
        [[AartiView alloc]
            initWithFrame:frame];

    self.window.contentView =
        self.view;

    [self.window makeKeyAndOrderFront:nil];

    [self.window makeFirstResponder:self.view];

    /*
     * 60 FPS UI update.
     */
    self.timer =
        [NSTimer scheduledTimerWithTimeInterval:
            (1.0 / 60.0)
            target:self
            selector:@selector(renderFrame:)
            userInfo:nil
            repeats:YES];
}

- (void)renderFrame:(NSTimer *)timer {
    (void)timer;

    motion_poll(0.0);

    [self.view setNeedsDisplay:YES];
}

- (void)applicationWillTerminate:
    (NSNotification *)notification {

    (void)notification;

    [self.timer invalidate];
    self.timer = nil;

    motion_stop();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender {

    (void)sender;

    return YES;
}

@end


int main(int argc, const char *argv[]) {

    @autoreleasepool {

        NSApplication *app =
            [NSApplication sharedApplication];

        AartiAppDelegate *delegate =
            [[AartiAppDelegate alloc] init];

        app.delegate = delegate;

        [app setActivationPolicy:
            NSApplicationActivationPolicyRegular];

        [app run];
    }

    return 0;
}
