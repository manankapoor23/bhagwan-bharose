#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>


/* ============================================================
 *
 *                  CONFIGURATION
 *
 * ============================================================ */

#define REPORT_BUFFER_SIZE 4096

#define PI 3.14159265358979323846
#define RAD_TO_DEG (180.0 / PI)
#define DEG_TO_RAD (PI / 180.0)

/*
 * Apple SPU report scale.
 *
 * The working POC established the report structure:
 *
 * X = bytes 6..9
 * Y = bytes 10..13
 * Z = bytes 14..17
 *
 * Values are signed int32 little-endian.
 */
#define SENSOR_SCALE 65536.0


/*
 * Complementary filter.
 *
 * Higher value = trust gyro more.
 * Lower value = trust accelerometer more.
 *
 * 0.98 is a good starting point for smooth motion.
 */
#define COMPLEMENTARY_ALPHA 0.98


/*
 * Gyro dead-zone.
 *
 * Tiny gyro noise below this is ignored.
 */
#define GYRO_DEADZONE_DPS 0.15


/*
 * Acceleration low-pass filter.
 *
 * 0.15 means strong smoothing.
 */
#define ACCEL_FILTER_ALPHA 0.15


/*
 * A circular aarti movement generally has meaningful
 * angular velocity.
 */
#define AARTI_GYRO_THRESHOLD_DPS 8.0


/*
 * Require some movement before classifying it.
 */
#define AARTI_MIN_RADIUS 0.08


/*
 * How much accumulated angular motion is required before
 * declaring a rotation.
 */
#define AARTI_ROTATION_THRESHOLD_DEG 120.0


/*
 * Calibration samples.
 *
 * Keep the Mac completely still during calibration.
 */
#define CALIBRATION_SAMPLES 150


/* ============================================================
 *
 *                  GLOBAL STATE
 *
 * ============================================================ */

static volatile sig_atomic_t running = 1;


/* ============================================================
 *
 *                  VECTOR
 *
 * ============================================================ */

typedef struct
{
    double x;
    double y;
    double z;

} Vec3;


/* ============================================================
 *
 *                  SENSOR
 *
 * ============================================================ */

typedef struct
{
    const char *name;

    IOHIDDeviceRef device;

    uint8_t *buffer;

    CFIndex buffer_size;

    uint64_t reports;

} Sensor;


/* ============================================================
 *
 *                  MOTION STATE
 *
 * ============================================================ */

typedef struct
{
    /*
     * Raw values.
     */
    Vec3 accel_raw;
    Vec3 gyro_raw;


    /*
     * Bias estimated during calibration.
     */
    Vec3 accel_bias;
    Vec3 gyro_bias;


    /*
     * Filtered acceleration.
     */
    Vec3 accel_filtered;


    /*
     * Orientation.
     */
    double roll;
    double pitch;
    double yaw;


    /*
     * Previous timestamp.
     */
    double last_time;


    /*
     * Whether the system has received its first
     * synchronized sensor sample.
     */
    bool initialized;


    /*
     * Calibration status.
     */
    bool calibrated;


    /*
     * Calibration accumulation.
     */
    Vec3 accel_sum;
    Vec3 gyro_sum;

    int calibration_count;


    /*
     * Aarti motion.
     */
    double circular_angle;

    double accumulated_rotation;

    int rotation_direction;

    bool rotating;


} MotionState;


static MotionState motion;


/* ============================================================
 *
 *                  UTILITY
 *
 * ============================================================ */

static double clamp(
    double value,
    double min_value,
    double max_value
)
{
    if (value < min_value)
        return min_value;

    if (value > max_value)
        return max_value;

    return value;
}


static double normalize_angle(
    double angle
)
{
    while (angle > 180.0)
        angle -= 360.0;

    while (angle < -180.0)
        angle += 360.0;

    return angle;
}


static double vector_magnitude(
    Vec3 v
)
{
    return sqrt(
        v.x * v.x +
        v.y * v.y +
        v.z * v.z
    );
}


/* ============================================================
 *
 *                  TIME
 *
 * ============================================================ */

static double current_time_seconds(void)
{
    struct timespec ts;

    clock_gettime(
        CLOCK_MONOTONIC,
        &ts
    );

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec / 1000000000.0;
}


/* ============================================================
 *
 *                  CTRL+C
 *
 * ============================================================ */

static void stop_handler(
    int signal_number
)
{
    (void)signal_number;

    running = 0;

    CFRunLoopStop(
        CFRunLoopGetCurrent()
    );
}


/* ============================================================
 *
 *                  LITTLE-ENDIAN INT32
 *
 * ============================================================ */

static int32_t read_i32_le(
    const uint8_t *p
)
{
    return (int32_t)(
        ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24)
    );
}


/* ============================================================
 *
 *                  RAW SENSOR DECODING
 *
 * ============================================================ */

static Vec3 decode_vec3(
    const uint8_t *report
)
{
    Vec3 result;

    int32_t raw_x =
        read_i32_le(&report[6]);

    int32_t raw_y =
        read_i32_le(&report[10]);

    int32_t raw_z =
        read_i32_le(&report[14]);


    result.x =
        (double)raw_x / SENSOR_SCALE;

    result.y =
        (double)raw_y / SENSOR_SCALE;

    result.z =
        (double)raw_z / SENSOR_SCALE;


    return result;
}


/* ============================================================
 *
 *                  ACCEL FILTER
 *
 * ============================================================ */

static Vec3 low_pass_accel(
    Vec3 input
)
{
    Vec3 result;


    result.x =
        ACCEL_FILTER_ALPHA * input.x +
        (1.0 - ACCEL_FILTER_ALPHA) *
            motion.accel_filtered.x;


    result.y =
        ACCEL_FILTER_ALPHA * input.y +
        (1.0 - ACCEL_FILTER_ALPHA) *
            motion.accel_filtered.y;


    result.z =
        ACCEL_FILTER_ALPHA * input.z +
        (1.0 - ACCEL_FILTER_ALPHA) *
            motion.accel_filtered.z;


    return result;
}


/* ============================================================
 *
 *                  CALIBRATION
 *
 * ============================================================ */

static void calibration_update(
    Vec3 accel,
    Vec3 gyro
)
{
    if (motion.calibrated)
        return;


    motion.accel_sum.x += accel.x;
    motion.accel_sum.y += accel.y;
    motion.accel_sum.z += accel.z;


    motion.gyro_sum.x += gyro.x;
    motion.gyro_sum.y += gyro.y;
    motion.gyro_sum.z += gyro.z;


    motion.calibration_count++;


    if (motion.calibration_count >=
        CALIBRATION_SAMPLES)
    {
        /*
         * Gyroscope:
         *
         * When stationary, average gyro should be
         * approximately zero.
         */

        motion.gyro_bias.x =
            motion.gyro_sum.x /
            CALIBRATION_SAMPLES;

        motion.gyro_bias.y =
            motion.gyro_sum.y /
            CALIBRATION_SAMPLES;

        motion.gyro_bias.z =
            motion.gyro_sum.z /
            CALIBRATION_SAMPLES;


        /*
         * Accelerometer:
         *
         * We DON'T remove the complete acceleration
         * vector because gravity is useful for roll/pitch.
         *
         * Instead, estimate the stationary gravity vector
         * and normalize it later.
         */

        motion.accel_bias.x =
            motion.accel_sum.x /
            CALIBRATION_SAMPLES;

        motion.accel_bias.y =
            motion.accel_sum.y /
            CALIBRATION_SAMPLES;

        motion.accel_bias.z =
            motion.accel_sum.z /
            CALIBRATION_SAMPLES;


        motion.calibrated = true;


        printf(
            "\n"
            "====================================================\n"
            "              CALIBRATION COMPLETE\n"
            "====================================================\n"
            "\n"
            "Gyro bias:\n"
            "  X = %+.5f\n"
            "  Y = %+.5f\n"
            "  Z = %+.5f\n"
            "\n"
            "Accel stationary vector:\n"
            "  X = %+.5f g\n"
            "  Y = %+.5f g\n"
            "  Z = %+.5f g\n"
            "\n"
            "Move the MacBook gently to begin aarti motion.\n"
            "\n",
            motion.gyro_bias.x,
            motion.gyro_bias.y,
            motion.gyro_bias.z,
            motion.accel_bias.x,
            motion.accel_bias.y,
            motion.accel_bias.z
        );

        fflush(stdout);
    }
}


/* ============================================================
 *
 *                  ACCEL ORIENTATION
 *
 * ============================================================ */

static void calculate_accel_angles(
    Vec3 accel,
    double *roll,
    double *pitch
)
{
    /*
     * Remove stationary bias only as a small correction.
     *
     * Gravity remains present.
     */

    double ax = accel.x;
    double ay = accel.y;
    double az = accel.z;


    /*
     * Roll:
     *
     * Rotation around X.
     */

    *roll =
        atan2(
            ay,
            sqrt(
                ax * ax +
                az * az
            )
        ) * RAD_TO_DEG;


    /*
     * Pitch:
     *
     * Rotation around Y.
     */

    *pitch =
        atan2(
            -ax,
            sqrt(
                ay * ay +
                az * az
            )
        ) * RAD_TO_DEG;
}


/* ============================================================
 *
 *                  GYRO DEADZONE
 *
 * ============================================================ */

static Vec3 apply_gyro_deadzone(
    Vec3 gyro
)
{
    if (fabs(gyro.x) <
        GYRO_DEADZONE_DPS)
        gyro.x = 0.0;


    if (fabs(gyro.y) <
        GYRO_DEADZONE_DPS)
        gyro.y = 0.0;


    if (fabs(gyro.z) <
        GYRO_DEADZONE_DPS)
        gyro.z = 0.0;


    return gyro;
}


/* ============================================================
 *
 *                  MOTION UPDATE
 *
 * ============================================================ */

static void update_motion(
    Vec3 accel,
    Vec3 gyro
)
{
    /*
     * Wait until calibration has completed.
     */

    calibration_update(
        accel,
        gyro
    );


    if (!motion.calibrated)
        return;


    /*
     * Remove gyro bias.
     */

    gyro.x -= motion.gyro_bias.x;
    gyro.y -= motion.gyro_bias.y;
    gyro.z -= motion.gyro_bias.z;


    /*
     * Remove tiny noise.
     */

    gyro =
        apply_gyro_deadzone(
            gyro
        );


    /*
     * Smooth acceleration.
     */

    motion.accel_filtered =
        low_pass_accel(
            accel
        );


    /*
     * Accelerometer orientation.
     */

    double accel_roll;
    double accel_pitch;


    calculate_accel_angles(
        motion.accel_filtered,
        &accel_roll,
        &accel_pitch
    );


    /*
     * Timestamp.
     */

    double now =
        current_time_seconds();


    if (!motion.initialized)
    {
        motion.roll =
            accel_roll;

        motion.pitch =
            accel_pitch;

        motion.yaw =
            0.0;

        motion.last_time =
            now;

        motion.initialized =
            true;

        return;
    }


    double dt =
        now - motion.last_time;


    motion.last_time =
        now;


    /*
     * Protect against abnormal timestamps.
     */

    if (dt <= 0.0 ||
        dt > 0.1)
    {
        return;
    }


    /*
     * --------------------------------------------------------
     * GYRO INTEGRATION
     * --------------------------------------------------------
     */

    double gyro_roll =
        motion.roll +
        gyro.x * dt;


    double gyro_pitch =
        motion.pitch +
        gyro.y * dt;


    /*
     * Yaw has no accelerometer correction.
     *
     * It is therefore relative and will drift slowly.
     */

    motion.yaw +=
        gyro.z * dt;


    motion.yaw =
        normalize_angle(
            motion.yaw
        );


    /*
     * --------------------------------------------------------
     * COMPLEMENTARY FILTER
     * --------------------------------------------------------
     *
     * Gyroscope:
     *     fast
     *     smooth
     *     responsive
     *
     * Accelerometer:
     *     absolute gravity reference
     *     slower
     *     noisy
     *
     * Combining them gives stable pitch/roll.
     */

    motion.roll =
        COMPLEMENTARY_ALPHA *
            gyro_roll
        +
        (1.0 - COMPLEMENTARY_ALPHA) *
            accel_roll;


    motion.pitch =
        COMPLEMENTARY_ALPHA *
            gyro_pitch
        +
        (1.0 - COMPLEMENTARY_ALPHA) *
            accel_pitch;


    motion.roll =
        normalize_angle(
            motion.roll
        );


    motion.pitch =
        normalize_angle(
            motion.pitch
        );


    /*
     * --------------------------------------------------------
     * AARTI ROTATION DETECTION
     * --------------------------------------------------------
     *
     * We primarily use gyro Z for rotation around the
     * MacBook's vertical axis.
     *
     * Positive Z = clockwise or anticlockwise depending
     * on physical coordinate orientation.
     *
     * We report both direction possibilities and can flip
     * this mapping once you test the physical movement.
     */

    double angular_speed =
        gyro.z;


    if (fabs(angular_speed) >=
        AARTI_GYRO_THRESHOLD_DPS)
    {
        motion.rotating =
            true;


        motion.circular_angle +=
            angular_speed * dt;


        motion.accumulated_rotation +=
            fabs(angular_speed * dt);


        /*
         * Direction:
         *
         * +1 = positive Z
         * -1 = negative Z
         */

        motion.rotation_direction =
            angular_speed > 0.0
                ? 1
                : -1;


        /*
         * Full gesture threshold.
         */

        if (motion.accumulated_rotation >=
            AARTI_ROTATION_THRESHOLD_DEG)
        {
            if (motion.rotation_direction > 0)
            {
                printf(
                    "\n"
                    ">>> AARTI ROTATION: CLOCKWISE <<<\n\n"
                );
            }
            else
            {
                printf(
                    "\n"
                    ">>> AARTI ROTATION: ANTICLOCKWISE <<<\n\n"
                );
            }


            /*
             * Start accumulating the next gesture.
             */

            motion.accumulated_rotation =
                0.0;
        }

    }
    else
    {
        /*
         * Slowly decay the accumulated rotation when
         * the user stops moving.
         */

        motion.accumulated_rotation *=
            0.90;


        if (motion.accumulated_rotation < 5.0)
        {
            motion.accumulated_rotation =
                0.0;

            motion.rotating =
                false;
        }
    }


    /*
     * --------------------------------------------------------
     * OUTPUT
     * --------------------------------------------------------
     */

    printf(
        "ROLL %7.2f° | "
        "PITCH %7.2f° | "
        "YAW %7.2f° | "
        "GYRO-Z %7.2f°/s | "
        "ROT %s\n",

        motion.roll,
        motion.pitch,
        motion.yaw,

        gyro.z,

        motion.rotating
            ? (motion.rotation_direction > 0
                ? "CW"
                : "CCW")
            : "-"
    );


    fflush(stdout);
}


/* ============================================================
 *
 *                  HID CALLBACK
 *
 * ============================================================ */

static void report_callback(
    void *context,
    IOReturn result,
    void *sender,
    IOHIDReportType type,
    uint32_t reportID,
    uint8_t *report,
    CFIndex reportLength
)
{
    (void)sender;
    (void)type;
    (void)reportID;


    Sensor *sensor =
        (Sensor *)context;


    if (!sensor)
        return;


    if (result !=
        kIOReturnSuccess)
    {
        fprintf(
            stderr,
            "[%s] callback error: 0x%08X\n",
            sensor->name,
            result
        );

        return;
    }


    if (!report ||
        reportLength < 18)
    {
        return;
    }


    sensor->reports++;


    Vec3 value =
        decode_vec3(
            report
        );


    /*
     * The working sensor format uses:
     *
     * ACCEL -> g
     * GYRO  -> deg/s
     */

    if (strcmp(
            sensor->name,
            "ACCEL"
        ) == 0)
    {
        motion.accel_raw =
            value;

    }
    else
    {
        motion.gyro_raw =
            value;


        /*
         * Update the complete motion state on gyro
         * reports because this is the primary high-rate
         * orientation source.
         */

        update_motion(
            motion.accel_raw,
            motion.gyro_raw
        );
    }
}


/* ============================================================
 *
 *                  FIND SPU SENSOR
 *
 * ============================================================ */

static IOHIDDeviceRef find_spu_device(
    uint32_t usage
)
{
    CFMutableDictionaryRef matching =
        IOServiceMatching(
            "AppleSPUHIDDevice"
        );


    if (!matching)
    {
        fprintf(
            stderr,
            "Could not create AppleSPUHIDDevice matching dictionary.\n"
        );

        return NULL;
    }


    int usage_page =
        0xFF00;


    int usage_value =
        (int)usage;


    CFNumberRef usage_page_number =
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
        usage_page_number
    );


    CFDictionarySetValue(
        matching,
        CFSTR(kIOHIDPrimaryUsageKey),
        usage_number
    );


    CFRelease(
        usage_page_number
    );


    CFRelease(
        usage_number
    );


    io_iterator_t iterator =
        IO_OBJECT_NULL;


    kern_return_t kr =
        IOServiceGetMatchingServices(
            kIOMainPortDefault,
            matching,
            &iterator
        );


    if (kr != KERN_SUCCESS)
    {
        fprintf(
            stderr,
            "IOServiceGetMatchingServices failed: 0x%08X\n",
            kr
        );

        return NULL;
    }


    io_service_t service;


    while (
        (service =
            IOIteratorNext(iterator))
        != IO_OBJECT_NULL
    )
    {
        char name[128];

        memset(
            name,
            0,
            sizeof(name)
        );


        IORegistryEntryGetName(
            service,
            name
        );


        printf(
            "Found AppleSPUHIDDevice: %s\n",
            name
        );


        IOHIDDeviceRef device =
            IOHIDDeviceCreate(
                kCFAllocatorDefault,
                service
            );


        IOObjectRelease(
            service
        );


        if (device)
        {
            IOObjectRelease(
                iterator
            );

            return device;
        }
    }


    IOObjectRelease(
        iterator
    );


    return NULL;
}


/* ============================================================
 *
 *                  SETUP
 *
 * ============================================================ */

static bool setup_sensor(
    Sensor *sensor
)
{
    if (!sensor ||
        !sensor->device)
    {
        return false;
    }


    IOReturn result =
        IOHIDDeviceOpen(
            sensor->device,
            kIOHIDOptionsTypeNone
        );


    if (result !=
        kIOReturnSuccess)
    {
        fprintf(
            stderr,
            "[%s] IOHIDDeviceOpen failed: 0x%08X\n",
            sensor->name,
            result
        );

        return false;
    }


    sensor->buffer_size =
        REPORT_BUFFER_SIZE;


    sensor->buffer =
        calloc(
            1,
            (size_t)sensor->buffer_size
        );


    if (!sensor->buffer)
    {
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


    printf(
        "[%s] ready.\n",
        sensor->name
    );


    return true;
}


/* ============================================================
 *
 *                  CLEANUP
 *
 * ============================================================ */

static void cleanup_sensor(
    Sensor *sensor
)
{
    if (!sensor)
        return;


    if (sensor->device)
    {
        IOHIDDeviceUnscheduleFromRunLoop(
            sensor->device,
            CFRunLoopGetCurrent(),
            kCFRunLoopDefaultMode
        );


        IOHIDDeviceClose(
            sensor->device,
            kIOHIDOptionsTypeNone
        );


        CFRelease(
            sensor->device
        );


        sensor->device =
            NULL;
    }


    free(
        sensor->buffer
    );


    sensor->buffer =
        NULL;
}


/* ============================================================
 *
 *                  MAIN
 *
 * ============================================================ */

int main(void)
{
    signal(
        SIGINT,
        stop_handler
    );


    memset(
        &motion,
        0,
        sizeof(motion)
    );


    printf(
        "\n"
        "====================================================\n"
        "          MACBOOK AARTI MOTION ENGINE\n"
        "====================================================\n"
        "\n"
        "Initializing Apple Silicon IMU...\n"
        "\n"
    );


    /*
     * Usage 3 = accelerometer
     * Usage 9 = gyroscope
     */

    IOHIDDeviceRef accel_device =
        find_spu_device(3);


    IOHIDDeviceRef gyro_device =
        find_spu_device(9);


    if (!accel_device)
    {
        fprintf(
            stderr,
            "Accelerometer not found.\n"
        );
    }


    if (!gyro_device)
    {
        fprintf(
            stderr,
            "Gyroscope not found.\n"
        );
    }


    if (!accel_device ||
        !gyro_device)
    {
        if (accel_device)
            CFRelease(accel_device);

        if (gyro_device)
            CFRelease(gyro_device);

        return EXIT_FAILURE;
    }


    Sensor accel = {
        .name = "ACCEL",
        .device = accel_device,
        .buffer = NULL,
        .buffer_size = 0,
        .reports = 0
    };


    Sensor gyro = {
        .name = "GYRO",
        .device = gyro_device,
        .buffer = NULL,
        .buffer_size = 0,
        .reports = 0
    };


    if (!setup_sensor(&accel))
    {
        cleanup_sensor(&accel);
        cleanup_sensor(&gyro);

        return EXIT_FAILURE;
    }


    if (!setup_sensor(&gyro))
    {
        cleanup_sensor(&accel);
        cleanup_sensor(&gyro);

        return EXIT_FAILURE;
    }


    printf(
        "\n"
        "====================================================\n"
        "                    CALIBRATION\n"
        "====================================================\n"
        "\n"
        "Keep the MacBook COMPLETELY STILL.\n"
        "\n"
        "Calibrating...\n"
        "\n"
    );


    fflush(stdout);


    /*
     * --------------------------------------------------------
     * Event loop
     * --------------------------------------------------------
     */

    while (running)
    {
        CFRunLoopRunInMode(
            kCFRunLoopDefaultMode,
            1.0,
            true
        );
    }


    printf(
        "\n"
        "====================================================\n"
        "                   SHUTTING DOWN\n"
        "====================================================\n"
        "\n"
        "Accelerometer reports: %llu\n"
        "Gyroscope reports:     %llu\n"
        "\n",
        (unsigned long long)accel.reports,
        (unsigned long long)gyro.reports
    );


    cleanup_sensor(
        &accel
    );


    cleanup_sensor(
        &gyro
    );


    return EXIT_SUCCESS;
}