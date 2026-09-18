#ifndef IMU_H
#define IMU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double x;
    double y;
    double z;
} Vec3;

typedef struct {
    double roll;
    double pitch;
    double yaw;

    double gyro_x;
    double gyro_y;
    double gyro_z;

    double angular_velocity;
    double rotation_progress;

    int direction;       /* -1 CCW, 0 none, +1 CW */
    bool rotating;
    bool calibrated;
} MotionState;

/* Starts AppleSPU accelerometer + gyroscope streaming. */
bool motion_start(void);

/* Stops all HID devices and releases resources. */
void motion_stop(void);

/* Returns the latest motion state. */
MotionState motion_get_state(void);

/* Starts a fresh calibration. Keep the Mac still. */
void motion_recalibrate(void);

/* Pumps the native CFRunLoop. */
void motion_poll(double seconds);

#endif
