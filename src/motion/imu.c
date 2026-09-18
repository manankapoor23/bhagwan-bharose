#include "imu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>

#define REPORT_BUFFER_SIZE 4096
#define SENSOR_SCALE 65536.0
#define PI 3.14159265358979323846
#define RAD_TO_DEG (180.0 / PI)

#define COMPLEMENTARY_ALPHA 0.98
#define ACCEL_FILTER_ALPHA 0.15
#define GYRO_DEADZONE_DPS 0.15
#define AARTI_GYRO_THRESHOLD_DPS 8.0
#define AARTI_ROTATION_THRESHOLD_DEG 120.0
#define CALIBRATION_SAMPLES 150

typedef struct {
    const char *name;
    IOHIDDeviceRef device;
    uint8_t *buffer;
    CFIndex buffer_size;
    uint64_t reports;
} Sensor;

typedef struct {
    Vec3 accel_raw;
    Vec3 gyro_raw;

    Vec3 gyro_bias;
    Vec3 accel_stationary;

    Vec3 accel_filtered;

    double roll;
    double pitch;
    double yaw;

    double last_time;
    bool initialized;
    bool calibrated;

    Vec3 accel_sum;
    Vec3 gyro_sum;
    int calibration_count;

    double accumulated_rotation;
    int rotation_direction;
    bool rotating;
} MotionInternal;

static Sensor g_accel = {0};
static Sensor g_gyro = {0};
static MotionInternal g_motion = {0};
static bool g_started = false;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double normalize_angle(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

static int32_t read_i32_le(const uint8_t *p) {
    return (int32_t)(
        ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24)
    );
}

static Vec3 decode_vec3(const uint8_t *report) {
    Vec3 v;
    v.x = (double)read_i32_le(&report[6]) / SENSOR_SCALE;
    v.y = (double)read_i32_le(&report[10]) / SENSOR_SCALE;
    v.z = (double)read_i32_le(&report[14]) / SENSOR_SCALE;
    return v;
}

static Vec3 accel_low_pass(Vec3 in) {
    Vec3 out;
    out.x = ACCEL_FILTER_ALPHA * in.x +
            (1.0 - ACCEL_FILTER_ALPHA) * g_motion.accel_filtered.x;
    out.y = ACCEL_FILTER_ALPHA * in.y +
            (1.0 - ACCEL_FILTER_ALPHA) * g_motion.accel_filtered.y;
    out.z = ACCEL_FILTER_ALPHA * in.z +
            (1.0 - ACCEL_FILTER_ALPHA) * g_motion.accel_filtered.z;
    return out;
}

static Vec3 gyro_deadzone(Vec3 g) {
    if (fabs(g.x) < GYRO_DEADZONE_DPS) g.x = 0.0;
    if (fabs(g.y) < GYRO_DEADZONE_DPS) g.y = 0.0;
    if (fabs(g.z) < GYRO_DEADZONE_DPS) g.z = 0.0;
    return g;
}

static void accel_angles(Vec3 a, double *roll, double *pitch) {
    *roll = atan2(
        a.y,
        sqrt(a.x * a.x + a.z * a.z)
    ) * RAD_TO_DEG;

    *pitch = atan2(
        -a.x,
        sqrt(a.y * a.y + a.z * a.z)
    ) * RAD_TO_DEG;
}

static void reset_calibration(void) {
    g_motion.calibrated = false;
    g_motion.initialized = false;
    g_motion.calibration_count = 0;
    g_motion.accel_sum = (Vec3){0,0,0};
    g_motion.gyro_sum = (Vec3){0,0,0};
    g_motion.accumulated_rotation = 0.0;
    g_motion.rotation_direction = 0;
    g_motion.rotating = false;
}

void motion_recalibrate(void) {
    reset_calibration();
}

static void finish_calibration(void) {
    const double n = (double)CALIBRATION_SAMPLES;

    g_motion.gyro_bias.x = g_motion.gyro_sum.x / n;
    g_motion.gyro_bias.y = g_motion.gyro_sum.y / n;
    g_motion.gyro_bias.z = g_motion.gyro_sum.z / n;

    g_motion.accel_stationary.x = g_motion.accel_sum.x / n;
    g_motion.accel_stationary.y = g_motion.accel_sum.y / n;
    g_motion.accel_stationary.z = g_motion.accel_sum.z / n;

    g_motion.accel_filtered = g_motion.accel_stationary;
    g_motion.calibrated = true;

    printf(
        "\nCalibration complete. "
        "Gyro bias = [%+.4f, %+.4f, %+.4f]\n",
        g_motion.gyro_bias.x,
        g_motion.gyro_bias.y,
        g_motion.gyro_bias.z
    );
    fflush(stdout);
}

static void calibration_sample(Vec3 accel, Vec3 gyro) {
    if (g_motion.calibrated) return;

    g_motion.accel_sum.x += accel.x;
    g_motion.accel_sum.y += accel.y;
    g_motion.accel_sum.z += accel.z;

    g_motion.gyro_sum.x += gyro.x;
    g_motion.gyro_sum.y += gyro.y;
    g_motion.gyro_sum.z += gyro.z;

    g_motion.calibration_count++;

    if (g_motion.calibration_count >= CALIBRATION_SAMPLES) {
        finish_calibration();
    }
}

static void update_motion(Vec3 accel, Vec3 gyro) {
    calibration_sample(accel, gyro);
    if (!g_motion.calibrated) return;

    gyro.x -= g_motion.gyro_bias.x;
    gyro.y -= g_motion.gyro_bias.y;
    gyro.z -= g_motion.gyro_bias.z;
    gyro = gyro_deadzone(gyro);

    g_motion.accel_filtered = accel_low_pass(accel);

    double accel_roll, accel_pitch;
    accel_angles(
        g_motion.accel_filtered,
        &accel_roll,
        &accel_pitch
    );

    double t = now_seconds();

    if (!g_motion.initialized) {
        g_motion.roll = accel_roll;
        g_motion.pitch = accel_pitch;
        g_motion.yaw = 0.0;
        g_motion.last_time = t;
        g_motion.initialized = true;
        return;
    }

    double dt = t - g_motion.last_time;
    g_motion.last_time = t;

    if (dt <= 0.0 || dt > 0.1) return;

    double gyro_roll = g_motion.roll + gyro.x * dt;
    double gyro_pitch = g_motion.pitch + gyro.y * dt;

    g_motion.yaw = normalize_angle(
        g_motion.yaw + gyro.z * dt
    );

    g_motion.roll = normalize_angle(
        COMPLEMENTARY_ALPHA * gyro_roll +
        (1.0 - COMPLEMENTARY_ALPHA) * accel_roll
    );

    g_motion.pitch = normalize_angle(
        COMPLEMENTARY_ALPHA * gyro_pitch +
        (1.0 - COMPLEMENTARY_ALPHA) * accel_pitch
    );

    double az = gyro.z;

    if (fabs(az) >= AARTI_GYRO_THRESHOLD_DPS) {
        g_motion.rotating = true;
        g_motion.rotation_direction = az > 0.0 ? 1 : -1;

        g_motion.accumulated_rotation += fabs(az * dt);

        if (g_motion.accumulated_rotation >=
            AARTI_ROTATION_THRESHOLD_DEG) {

            printf(
                "\nAARTI_GESTURE %s\n",
                g_motion.rotation_direction > 0
                    ? "CW"
                    : "CCW"
            );

            fflush(stdout);

            g_motion.accumulated_rotation = 0.0;
        }
    } else {
        g_motion.accumulated_rotation *= 0.90;

        if (g_motion.accumulated_rotation < 5.0) {
            g_motion.accumulated_rotation = 0.0;
            g_motion.rotating = false;
            g_motion.rotation_direction = 0;
        }
    }

    g_motion.gyro_raw = gyro;
}

static void report_callback(
    void *context,
    IOReturn result,
    void *sender,
    IOHIDReportType type,
    uint32_t reportID,
    uint8_t *report,
    CFIndex reportLength
) {
    (void)sender;
    (void)type;
    (void)reportID;

    Sensor *sensor = (Sensor *)context;
    if (!sensor) return;

    if (result != kIOReturnSuccess) return;
    if (!report || reportLength < 18) return;

    sensor->reports++;

    Vec3 value = decode_vec3(report);

    if (strcmp(sensor->name, "ACCEL") == 0) {
        g_motion.accel_raw = value;
    } else {
        update_motion(
            g_motion.accel_raw,
            value
        );
    }
}

static IOHIDDeviceRef find_spu_device(uint32_t usage) {
    CFMutableDictionaryRef matching =
        IOServiceMatching("AppleSPUHIDDevice");

    if (!matching) return NULL;

    int usage_page = 0xFF00;
    int usage_value = (int)usage;

    CFNumberRef page_number =
        CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberIntType,
            &usage_page
        );

    CFNumberRef usage_number =
        CFNumberCreate(
            kCFAllocatorDefault,
            kCFNumberIntType,
            &usage_value
        );

    CFDictionarySetValue(
        matching,
        CFSTR(kIOHIDPrimaryUsagePageKey),
        page_number
    );

    CFDictionarySetValue(
        matching,
        CFSTR(kIOHIDPrimaryUsageKey),
        usage_number
    );

    CFRelease(page_number);
    CFRelease(usage_number);

    io_iterator_t iterator = IO_OBJECT_NULL;

    kern_return_t kr =
        IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator
        );

    if (kr != KERN_SUCCESS)
        return NULL;

    io_service_t service;

    while ((service = IOIteratorNext(iterator))
           != IO_OBJECT_NULL) {

        IOHIDDeviceRef device =
            IOHIDDeviceCreate(
                kCFAllocatorDefault,
                service
            );

        IOObjectRelease(service);

        if (device) {
            IOObjectRelease(iterator);
            return device;
        }
    }

    IOObjectRelease(iterator);
    return NULL;
}

static bool setup_sensor(Sensor *sensor) {
    if (!sensor || !sensor->device)
        return false;

    IOReturn result =
        IOHIDDeviceOpen(
            sensor->device,
            kIOHIDOptionsTypeNone
        );

    if (result != kIOReturnSuccess) {
        fprintf(
            stderr,
            "[%s] IOHIDDeviceOpen failed: 0x%08X\n",
            sensor->name,
            result
        );
        return false;
    }

    sensor->buffer_size = REPORT_BUFFER_SIZE;

    sensor->buffer =
        calloc(
            1,
            (size_t)sensor->buffer_size
        );

    if (!sensor->buffer) {
        IOHIDDeviceClose(
            sensor->device,
            kIOHIDOptionsTypeNone
        );
        return false;
    }

    IOHIDDeviceRegisterInputReportCallback(
        sensor->device,
        sensor->buffer,
        sensor->buffer_size,
        report_callback,
        sensor
    );

    IOHIDDeviceScheduleWithRunLoop(
        sensor->device,
        CFRunLoopGetCurrent(),
        kCFRunLoopDefaultMode
    );

    return true;
}

static void cleanup_sensor(Sensor *sensor) {
    if (!sensor) return;

    if (sensor->device) {
        IOHIDDeviceUnscheduleFromRunLoop(
            sensor->device,
            CFRunLoopGetCurrent(),
            kCFRunLoopDefaultMode
        );

        IOHIDDeviceClose(
            sensor->device,
            kIOHIDOptionsTypeNone
        );

        CFRelease(sensor->device);
        sensor->device = NULL;
    }

    free(sensor->buffer);
    sensor->buffer = NULL;
}

bool motion_start(void) {
    if (g_started)
        return true;

    memset(&g_motion, 0, sizeof(g_motion));

    g_accel.name = "ACCEL";
    g_gyro.name = "GYRO";

    /*
     * Apple SPU usage 3 = accelerometer.
     * Apple SPU usage 9 = gyroscope.
     */
    g_accel.device = find_spu_device(3);
    g_gyro.device = find_spu_device(9);

    if (!g_accel.device || !g_gyro.device) {
        cleanup_sensor(&g_accel);
        cleanup_sensor(&g_gyro);
        return false;
    }

    if (!setup_sensor(&g_accel)) {
        cleanup_sensor(&g_accel);
        cleanup_sensor(&g_gyro);
        return false;
    }

    if (!setup_sensor(&g_gyro)) {
        cleanup_sensor(&g_accel);
        cleanup_sensor(&g_gyro);
        return false;
    }

    g_started = true;

    printf(
        "Motion engine started.\n"
        "Keep the Mac still while calibrating.\n"
    );
    fflush(stdout);

    return true;
}

void motion_stop(void) {
    if (!g_started)
        return;

    cleanup_sensor(&g_accel);
    cleanup_sensor(&g_gyro);

    g_started = false;
}

MotionState motion_get_state(void) {
    MotionState state;

    memset(&state, 0, sizeof(state));

    state.roll = g_motion.roll;
    state.pitch = g_motion.pitch;
    state.yaw = g_motion.yaw;

    state.gyro_x = g_motion.gyro_raw.x;
    state.gyro_y = g_motion.gyro_raw.y;
    state.gyro_z = g_motion.gyro_raw.z;

    state.acceleration_x =
        g_motion.accel_filtered.x - g_motion.accel_stationary.x;
    state.acceleration_y =
        g_motion.accel_filtered.y - g_motion.accel_stationary.y;

    state.angular_velocity =
        sqrt(
            g_motion.gyro_raw.x * g_motion.gyro_raw.x +
            g_motion.gyro_raw.y * g_motion.gyro_raw.y +
            g_motion.gyro_raw.z * g_motion.gyro_raw.z
        );

    state.rotation_progress =
        fmin(
            g_motion.accumulated_rotation /
                AARTI_ROTATION_THRESHOLD_DEG,
            1.0
        );

    state.direction =
        g_motion.rotating
            ? g_motion.rotation_direction
            : 0;

    state.rotating = g_motion.rotating;
    state.calibrated = g_motion.calibrated;

    return state;
}

void motion_poll(double seconds) {
    CFRunLoopRunInMode(
        kCFRunLoopDefaultMode,
        seconds,
        true
    );
}
