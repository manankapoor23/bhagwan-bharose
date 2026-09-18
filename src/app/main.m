#import <Cocoa/Cocoa.h>

#include "../motion/imu.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

#define TRAIL_LENGTH 120
#define DEGREES_TO_RADIANS 0.017453292519943295

static NSString *assetPath(NSString *relativePath) {
    NSString *root = [[NSFileManager defaultManager] currentDirectoryPath];
    return [root stringByAppendingPathComponent:relativePath];
}

static BOOL isCheckerboardPixel(const uint8_t *pixel) {
    int spread = abs((int)pixel[0] - (int)pixel[1]) +
                 abs((int)pixel[1] - (int)pixel[2]);
    return spread < 18 && pixel[0] > 175 && pixel[1] > 175 && pixel[2] > 175;
}

static NSImage *loadKeyedForegroundImage(void) {
    NSData *data = [NSData dataWithContentsOfFile:
        assetPath(@"assets/thali/hands_thali_source.png")];
    NSImage *source = [[NSImage alloc] initWithData:data];
    CGImageRef sourceImage = [source CGImageForProposedRect:NULL context:nil hints:nil];

    if (!sourceImage) return source;

    size_t width = CGImageGetWidth(sourceImage);
    size_t height = CGImageGetHeight(sourceImage);
    NSBitmapImageRep *bitmap =
        [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
            pixelsWide:(NSInteger)width
            pixelsHigh:(NSInteger)height
            bitsPerSample:8
            samplesPerPixel:4
            hasAlpha:YES
            isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace
            bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
            bytesPerRow:0
            bitsPerPixel:0];

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        [bitmap bitmapData],
        width,
        height,
        8,
        [bitmap bytesPerRow],
        colorSpace,
        (CGBitmapInfo)kCGImageAlphaPremultipliedLast
    );

    CGContextDrawImage(context, CGRectMake(0, 0, width, height), sourceImage);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);

    uint8_t *pixels = [bitmap bitmapData];
    size_t pixelCount = width * height;
    uint8_t *visited = calloc(pixelCount, sizeof(uint8_t));
    size_t *queue = malloc(pixelCount * sizeof(size_t));
    size_t queueHead = 0;
    size_t queueTail = 0;

    if (visited && queue) {
        for (size_t index = 0; index < pixelCount; index++) {
            BOOL onEdge = index < width ||
                          index >= pixelCount - width ||
                          index % width == 0 ||
                          index % width == width - 1;
            if (onEdge && isCheckerboardPixel(pixels + index * 4)) {
                visited[index] = 1;
                queue[queueTail++] = index;
            }
        }

        while (queueHead < queueTail) {
            size_t index = queue[queueHead++];
            size_t neighbors[4] = {
                index > width ? index - width : index,
                index + width < pixelCount ? index + width : index,
                index % width > 0 ? index - 1 : index,
                index % width + 1 < width ? index + 1 : index
            };

            for (size_t neighborIndex = 0; neighborIndex < 4; neighborIndex++) {
                size_t neighbor = neighbors[neighborIndex];
                if (!visited[neighbor] &&
                    isCheckerboardPixel(pixels + neighbor * 4)) {
                    visited[neighbor] = 1;
                    queue[queueTail++] = neighbor;
                }
            }
        }

        for (size_t index = 0; index < pixelCount; index++) {
            if (visited[index]) pixels[index * 4 + 3] = 0;
        }
    }

    free(queue);
    free(visited);

    NSImage *result = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [result addRepresentation:bitmap];
    return result;
}

@interface AartiView : NSView
@property(nonatomic, strong) NSImage *templeImage;
@property(nonatomic, strong) NSImage *foregroundImage;
@property(nonatomic) BOOL debugEnabled;
@end

@implementation AartiView

static NSPoint lightTrail[TRAIL_LENGTH];
static NSUInteger lightTrailCount = 0;

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    NSString *key = [[event charactersIgnoringModifiers] lowercaseString];

    if ([key isEqualToString:@"r"]) {
        motion_recalibrate();
        return;
    }

    if ([key isEqualToString:@"d"]) {
        self.debugEnabled = !self.debugEnabled;
        return;
    }

    if ([key isEqualToString:@"escape"]) {
        [NSApp terminate:nil];
        return;
    }

    [super keyDown:event];
}

- (void)drawRect:(NSRect)rect {
    [super drawRect:rect];

    NSRect bounds = self.bounds;

    if (self.templeImage) {
        [self.templeImage drawInRect:bounds
                             fromRect:NSZeroRect
                            operation:NSCompositingOperationSourceOver
                             fraction:1.0
                       respectFlipped:YES
                                hints:nil];
    } else {
        [[NSColor blackColor] setFill];
        NSRectFill(bounds);
    }

    MotionState state = motion_get_state();

    if (!state.calibrated) {
        NSDictionary *calibrationAttrs = @{
            NSFontAttributeName:
                [NSFont systemFontOfSize:30 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName:[NSColor whiteColor]
        };
        NSDictionary *instructionAttrs = @{
            NSFontAttributeName:[NSFont systemFontOfSize:18],
            NSForegroundColorAttributeName:
                [NSColor colorWithWhite:1.0 alpha:0.72]
        };

        [[NSColor colorWithCalibratedWhite:0.0 alpha:0.42] setFill];
        NSRectFill(bounds);

        NSString *title = @"PREPARE FOR AARTI";
        NSString *instruction = @"Keep your Mac still while it calibrates";
        NSSize titleSize = [title sizeWithAttributes:calibrationAttrs];
        NSSize instructionSize = [instruction sizeWithAttributes:instructionAttrs];

        [title drawAtPoint:NSMakePoint(
            (bounds.size.width - titleSize.width) / 2.0,
            bounds.size.height * 0.54
        ) withAttributes:calibrationAttrs];
        [instruction drawAtPoint:NSMakePoint(
            (bounds.size.width - instructionSize.width) / 2.0,
            bounds.size.height * 0.47
        ) withAttributes:instructionAttrs];
        return;
    }

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

    if (self.debugEnabled) {
        [motionText drawAtPoint:NSMakePoint(40, 40)
                 withAttributes:smallAttrs];
    }

    /*
     * Temporary motion visualization.
     *
     * This will become the actual thali renderer in the
     * next rendering pass.
     */

    CGFloat centerX = bounds.size.width / 2.0;
    CGFloat centerY = bounds.size.height / 2.0;

    CGFloat motionRadius = fmin(bounds.size.width, bounds.size.height) * 0.20;
    static CGFloat lightOffsetX = 0.0;
    static CGFloat lightOffsetY = 0.0;
    static CFTimeInterval lastMotionTime = 0.0;
    CFTimeInterval motionTime = CFAbsoluteTimeGetCurrent();
    CGFloat motionDelta = lastMotionTime > 0.0
        ? (CGFloat)(motionTime - lastMotionTime)
        : 0.0;

    motionDelta = fmax(0.0, fmin(0.05, motionDelta));
    lastMotionTime = motionTime;

    if (!state.calibrated) {
        lightOffsetX = 0.0;
        lightOffsetY = 0.0;
    } else {
        lightOffsetX += (CGFloat)state.gyro_y * motionDelta * 4.0;
        lightOffsetY -= (CGFloat)state.gyro_x * motionDelta * 4.0;
        lightOffsetX = fmax(-motionRadius, fmin(motionRadius, lightOffsetX));
        lightOffsetY = fmax(-motionRadius, fmin(motionRadius, lightOffsetY));
    }

    CGFloat x =
        centerX +
        lightOffsetX;

    CGFloat y =
        centerY +
        lightOffsetY;

    NSPoint p = NSMakePoint(x, y);

    if (!state.calibrated) {
        lightTrailCount = 0;
    } else if (lightTrailCount == 0 ||
               hypot(p.x - lightTrail[lightTrailCount - 1].x,
                     p.y - lightTrail[lightTrailCount - 1].y) > 3.0) {
        if (lightTrailCount == TRAIL_LENGTH) {
            memmove(
                lightTrail,
                lightTrail + 1,
                sizeof(NSPoint) * (TRAIL_LENGTH - 1)
            );
            lightTrailCount--;
        }

        lightTrail[lightTrailCount++] = p;
    }

    CGFloat movementEnergy =
        fmin(1.0, state.angular_velocity / 36.0);
    for (NSInteger glow = 4; glow >= 1; glow--) {
        CGFloat glowRadius = 42.0 + glow * 18.0;
        CGFloat glowAlpha = 0.018 + movementEnergy * 0.012;
        [[NSColor colorWithCalibratedRed:1.0
                                   green:0.42
                                    blue:0.05
                                   alpha:glowAlpha] setFill];
        NSBezierPath *glowPath =
            [NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect(
                    p.x - glowRadius,
                    p.y + 38.0 - glowRadius,
                    glowRadius * 2.0,
                    glowRadius * 2.0
                )];
        [glowPath fill];
    }

    for (NSUInteger index = 0; index < lightTrailCount; index++) {
        CGFloat opacity =
            0.05 + 0.35 * ((CGFloat)index / (CGFloat)TRAIL_LENGTH);
        CGFloat dotSize =
            4.0 + 8.0 * ((CGFloat)index / (CGFloat)TRAIL_LENGTH);

        [[NSColor colorWithCalibratedRed:1.0
                                   green:0.55
                                    blue:0.08
                                   alpha:opacity] setFill];

        NSBezierPath *dot =
            [NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect(
                    lightTrail[index].x - dotSize / 2.0,
                    lightTrail[index].y - dotSize / 2.0,
                    dotSize,
                    dotSize
                )];

        [dot fill];
    }

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

    CGFloat foregroundWidth = fmin(bounds.size.width * 0.86, 1240.0);
    CGFloat foregroundHeight = foregroundWidth * 2.0 / 3.0;
    CGFloat foregroundRotation =
        fmax(-12.0, fmin(12.0, (CGFloat)state.yaw * 0.20));

    if (self.foregroundImage) {
        [NSGraphicsContext saveGraphicsState];
        NSAffineTransform *transform = [NSAffineTransform transform];
        [transform translateXBy:p.x yBy:p.y - bounds.size.height * 0.06];
        [transform rotateByDegrees:foregroundRotation];
        [transform concat];

        [self.foregroundImage drawInRect:
            NSMakeRect(
                -foregroundWidth / 2.0,
                -foregroundHeight / 2.0,
                foregroundWidth,
                foregroundHeight
            )
            fromRect:NSZeroRect
            operation:NSCompositingOperationSourceOver
            fraction:1.0
            respectFlipped:YES
            hints:nil];

        [NSGraphicsContext restoreGraphicsState];
    }

    for (NSInteger smoke = 0; smoke < 4; smoke++) {
        CGFloat smokePhase = (CGFloat)smoke * 1.7 + state.yaw * DEGREES_TO_RADIANS;
        CGFloat smokeX = p.x + sin(smokePhase) * (8.0 + smoke * 3.0);
        CGFloat smokeY = p.y + 150.0 + smoke * 23.0;
        CGFloat smokeSize = 18.0 + smoke * 7.0;
        CGFloat smokeAlpha = 0.075 - smoke * 0.014;

        [[NSColor colorWithCalibratedWhite:0.82 alpha:smokeAlpha] setFill];
        NSBezierPath *smokePath =
            [NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect(
                    smokeX - smokeSize / 2.0,
                    smokeY - smokeSize / 2.0,
                    smokeSize,
                    smokeSize
                )];
        [smokePath fill];
    }
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

    self.view.templeImage =
        [[NSImage alloc]
            initWithContentsOfFile:
                assetPath(@"assets/temple/temple_background.png")];
    self.view.foregroundImage = loadKeyedForegroundImage();

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

    (void)argc;
    (void)argv;

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
