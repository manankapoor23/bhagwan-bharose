# Bhagwan Bharose — Native macOS Motion Application

A native macOS prototype controlled by the MacBook accelerometer and gyroscope.
Tilt and rotate the machine to perform aarti.

*Bhagwan bharose* — "left to God" — the operating principle of both this
prototype and the average midsem.

## Architecture

```
AppleSPUHIDDevice
        ↓
IOKit HID
        ↓
C motion engine
        ↓
MotionState
        ↓
Cocoa renderer
```

There is no Flutter dependency.

## Repository layout

```
src/app/main.m        Cocoa renderer and window/scene code
src/motion/imu.c      Motion engine: HID reports, filtering, gesture detection
src/motion/imu.h      Motion engine public interface
main.c                Standalone terminal accelerometer renderer (legacy target)
assets/temple/        Temple background art
assets/thali/         Hands + thali foreground art
Makefile              Build for the Cocoa app
build.sh              Equivalent one-shot clang invocation
```

## Current functionality

- AppleSPU accelerometer
- AppleSPU gyroscope
- IOKit HID input reports
- calibration
- accelerometer smoothing
- complementary roll/pitch filter
- relative yaw integration
- gyro dead-zone
- CW / CCW aarti rotation detection
- native macOS fullscreen window
- 60 FPS motion-driven prototype

## Requirements

- macOS on Apple Silicon (the AppleSPU HID interface is Apple Silicon only)
- Xcode command line tools (`xcode-select --install`)

## Build

```bash
make
```

Or equivalently:

```bash
./build.sh
```

Both produce the `bhagwan-bharose` binary.

### Standalone terminal renderer

`main.c` is an earlier, self-contained terminal visualiser kept for reference. It is
not part of the `make` target and builds separately:

```bash
clang -O2 main.c -framework IOKit -framework CoreFoundation -lm -o mac_motion
```

## Run

The AppleSPU sensor interface requires elevated access on the tested configuration:

```bash
sudo ./bhagwan-bharose
```

Keep the Mac still during calibration.

Keys: `Esc` quits, `R` recalibrates, `D` toggles the debug overlay.

## Current visual

The Cocoa renderer composites the temple background with a keyed hands/thali
foreground, driven by live motion.

This is the motion/rendering integration stage. Replace the artwork with final
temple, thali, hand, diya and lighting assets once the interaction tuning is locked.

## Next production work

1. Replace placeholder art with final assets.
2. Add first-person hands.
3. Add temple/deity background depth.
4. Add animated diya flame.
5. Add audio through AVFoundation.
6. Improve motion-to-thali mapping.
7. Add gesture confidence and gesture cooldown.
8. Add calibration UI.
9. Add performance instrumentation.
10. Package as a normal macOS `.app`.
