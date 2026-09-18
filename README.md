# Aarti — Native macOS Motion Application

A native macOS prototype controlled by the MacBook accelerometer and gyroscope.

## Architecture

AppleSPUHIDDevice
        ↓
IOKit HID
        ↓
C motion engine
        ↓
MotionState
        ↓
Cocoa renderer

There is no Flutter dependency.

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

## Build

```bash
make
```

## Run

The AppleSPU sensor interface requires elevated access on the tested configuration:

```bash
sudo ./aarti
```

Keep the Mac still during calibration.

## Current visual

The current Cocoa renderer intentionally uses a simple procedural plate and diya.

This is the motion/rendering integration stage. Replace the procedural artwork with final temple, thali, hand, diya and lighting assets once the interaction tuning is locked.

## Next production work

1. Replace procedural plate with final art assets.
2. Add first-person hands.
3. Add temple/deity background.
4. Add animated diya flame.
5. Add audio through AVFoundation.
6. Improve motion-to-thali mapping.
7. Add gesture confidence and gesture cooldown.
8. Add calibration UI.
9. Add performance instrumentation.
10. Package as a normal macOS `.app`.
