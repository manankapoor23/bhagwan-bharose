/*
 * Maccelerometer - Simple Access to Mac Accelerometer data
 * Author: Aarav Gupta <atpugvaraa@gmail.com>
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// Constants
#define PAGE_VENDOR 0xFF00
#define USAGE_ACCEL 3
#define IMU_REPORT_LENGTH 22
#define IMU_DATA_OFFSET 6
#define REPORT_BUFFER_SIZE 4096
#define REPORT_INTERVAL_US 1000

static uint8_t reportBuffer[REPORT_BUFFER_SIZE];

// function decls
static int getRegistryIntegerProperty(io_service_t service, const char *key, int64_t *value);
static int setRegistryIntegerProperty(io_service_t service, const char *key, int32_t value);
static void inputReportCallback(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex reportLength);
static int wakeSPUDrivers(void);
static IOHIDDeviceRef findAccelerometer(void);

int main(void) {
    // 1. Wake Apple SPU Drivers
    if (!wakeSPUDrivers()) {
        printf("Warning: could not find AppleSPUHIDDriver.\n");

        printf("Continuing anyway...\n\n");
    }

    // 2. Find accelerometer logically (to be refactored for a deterministic search)
    IOHIDDeviceRef accelerometer = findAccelerometer();

    if (accelerometer == NULL) {
        printf("\nNo accelerometer found.\n");
        return 1;
    }

    // 3. Open HID device manager
    printf("\nOpening accelerometer...\n");

    IOReturn result = IOHIDDeviceOpen(
        accelerometer,
        kIOHIDOptionsTypeNone
    );


    if (result != kIOReturnSuccess) {
        printf(
            "IOHIDDeviceOpen failed: 0x%08X\n",
            result
        );

        CFRelease(accelerometer);

        return 1;
    }

    printf(">>> ACCELEROMETER OPENED <<<\n");

    // 4. Schedule device
    CFRunLoopRef runLoop = CFRunLoopGetCurrent();

    IOHIDDeviceScheduleWithRunLoop(
        accelerometer,
        runLoop,
        kCFRunLoopDefaultMode
    );

    printf(">>> DEVICE SCHEDULED <<<\n");

    // 5. Register a callback
    IOHIDDeviceRegisterInputReportCallback(
        accelerometer,
        reportBuffer,
        sizeof(reportBuffer),
        inputReportCallback,
        NULL
    );

    printf(">>> CALLBACK REGISTERED <<<\n");

    // 6. Run
    printf("\nListening for accelerometer data...\n");

    printf("Try moving or tapping the MacBook.\n\n");

    CFRunLoopRun();

    // Cleanup and free
    IOHIDDeviceUnscheduleFromRunLoop(
        accelerometer,
        runLoop,
        kCFRunLoopDefaultMode
    );

    IOHIDDeviceClose(
        accelerometer,
        kIOHIDOptionsTypeNone
    );

    CFRelease(accelerometer);

    return 0;
}

static int getRegistryIntegerProperty(
    io_service_t service,
    const char *key,
    int64_t *value
) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault,
        key,
        kCFStringEncodingUTF8
    );

    if (keyString == NULL) {
        return 0;
    }

    CFTypeRef property = IORegistryEntryCreateCFProperty(
        service,
        keyString,
        kCFAllocatorDefault,
        0
    );

    CFRelease(keyString);

    if (property == NULL) {
        return 0;
    }

    if (CFGetTypeID(property) != CFNumberGetTypeID()) {
        CFRelease(property);
        return 0;
    }

    Boolean success = CFNumberGetValue(
        (CFNumberRef)property,
        kCFNumberSInt64Type,
        value
    );

    CFRelease(property);

    return success;
}

static int setRegistryIntegerProperty(
    io_service_t service,
    const char *key,
    int32_t value
) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault,
        key,
        kCFStringEncodingUTF8
    );

    if (keyString == NULL) {
        return 0;
    }

    CFNumberRef number = CFNumberCreate(
        kCFAllocatorDefault,
        kCFNumberSInt32Type,
        &value
    );

    if (number == NULL) {
        CFRelease(keyString);
        return 0;
    }

    kern_return_t result = IORegistryEntrySetCFProperty(
        service,
        keyString,
        number
    );

    CFRelease(number);
    CFRelease(keyString);

    return result == KERN_SUCCESS;
}

static void inputReportCallback(
    void *context,
    IOReturn result,
    void *sender,
    IOHIDReportType type,
    uint32_t reportID,
    uint8_t *report,
    CFIndex reportLength
) {
    if (result != kIOReturnSuccess) {
        printf(
            "Input report error: 0x%08X\n",
            result
        );
        return;
    }

    if (reportLength == IMU_REPORT_LENGTH) {
        int32_t x = (int32_t)(
            ((uint32_t)report[6]) |
            ((uint32_t)report[7] << 8) |
            ((uint32_t)report[8] << 16) |
            ((uint32_t)report[9] << 24)
        );

        int32_t y = (int32_t)(
            ((uint32_t)report[10]) |
            ((uint32_t)report[11] << 8) |
            ((uint32_t)report[12] << 16) |
            ((uint32_t)report[13] << 24)
        );

        int32_t z = (int32_t)(
            ((uint32_t)report[14]) |
            ((uint32_t)report[15] << 8) |
            ((uint32_t)report[16] << 16) |
            ((uint32_t)report[17] << 24)
        );

        double xG = (double)x / 65536.0;
        double yG = (double)y / 65536.0;
        double zG = (double)z / 65536.0;

        double magnitude = sqrt(xG * xG + yG * yG + zG * zG);

        double deviation = magnitude - 1.0;

        if (deviation > 0.09) {
            printf(
                "ACCEL:     Magnitude=%.4f | Deviation=%+.4f g\n",
                magnitude,
                deviation
            );
        }
    }
}

static int wakeSPUDrivers(void) {

    printf("Searching for AppleSPUHIDDriver...\n");


    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");

    if (matching == NULL) {
        printf("Failed to create AppleSPUHIDDriver matching dictionary.\n");

        return 0;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;

    kern_return_t result = IOServiceGetMatchingServices(
        kIOMainPortDefault,
        matching,
        &iterator
    );

    if (result != KERN_SUCCESS) {
        printf(
            "IOServiceGetMatchingServices failed: 0x%08X\n",
            result
        );

        return 0;
    }

    int count = 0;

    while (1) {
        io_service_t service = IOIteratorNext(iterator);

        if (service == IO_OBJECT_NULL) {
            break;
        }

        count++;

        printf("Found AppleSPUHIDDriver\n");

        // Enable sensor reporting.
        if (
            !setRegistryIntegerProperty(
                service,
                "SensorPropertyReportingState",
                1
            )
        ) {
            printf(
                "Warning: failed to set "
                "SensorPropertyReportingState\n"
            );
        }

        // Enable sensor power.
        if (
            !setRegistryIntegerProperty(
                service,
                "SensorPropertyPowerState",
                1
            )
        ) {
            printf(
                "Warning: failed to set "
                "SensorPropertyPowerState\n"
            );
        }

        // Set report interval.
        if (
            !setRegistryIntegerProperty(
                service,
                "ReportInterval",
                REPORT_INTERVAL_US
            )
        ) {
            printf("Warning: failed to set ReportInterval\n");
        }

        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    printf(
        "AppleSPUHIDDriver count: %d\n",
        count
    );

    return count > 0;
}


static IOHIDDeviceRef findAccelerometer(void) {
    printf("\nSearching for AppleSPUHIDDevice...\n");

    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDevice");

    if (matching == NULL) {
        printf("Failed to create AppleSPUHIDDevice matching dictionary.\n");

        return NULL;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;

    kern_return_t result = IOServiceGetMatchingServices(
        kIOMainPortDefault,
        matching,
        &iterator
    );

    if (result != KERN_SUCCESS) {
        printf(
            "IOServiceGetMatchingServices failed: 0x%08X\n",
            result
        );

        return NULL;
    }

    IOHIDDeviceRef accelerometer = NULL;

    while (1) {
        io_service_t service = IOIteratorNext(iterator);

        if (service == IO_OBJECT_NULL) {
            break;
        }

        int64_t usagePage = 0;
        int64_t usage = 0;

        int hasUsagePage = getRegistryIntegerProperty(
            service,
            "PrimaryUsagePage",
            &usagePage
        );

        int hasUsage = getRegistryIntegerProperty(
            service,
            "PrimaryUsage",
            &usage
        );

        printf(
            "AppleSPUHIDDevice: "
            "UsagePage=0x%04llX Usage=0x%04llX\n",
            usagePage,
            usage
        );

        if (
            hasUsagePage &&
            hasUsage &&
            usagePage == PAGE_VENDOR &&
            usage == USAGE_ACCEL
        ) {
            printf(">>> FOUND ACCELEROMETER SERVICE <<<\n");

            accelerometer = IOHIDDeviceCreate(
                kCFAllocatorDefault,
                service
            );

            if (accelerometer == NULL) {
                printf("Failed to create IOHIDDevice.\n");
            } else {
                printf(">>> IOHIDDevice CREATED <<<\n");
            }

            IOObjectRelease(service);

            break;
        }

        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    return accelerometer;
}
