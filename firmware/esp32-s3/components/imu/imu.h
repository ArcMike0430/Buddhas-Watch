/**
 * imu.h — QMI8658 6-axis IMU driver
 *
 * The QMI8658 provides 3-axis accelerometer + 3-axis gyroscope over I²C.
 * Used for motion subtraction: CSI spike with IMU motion = body movement
 * (low priority); CSI spike without IMU motion = RF-acoustic event (high).
 */
#pragma once

#include <stdbool.h>

/** IMU sample — raw accelerometer + gyroscope + timestamp */
typedef struct {
    float   acc_x_g;   /**< Accelerometer X [g] */
    float   acc_y_g;   /**< Accelerometer Y [g] */
    float   acc_z_g;   /**< Accelerometer Z [g] */
    float   gyr_x_dps; /**< Gyroscope X [deg/s] */
    float   gyr_y_dps; /**< Gyroscope Y [deg/s] */
    float   gyr_z_dps; /**< Gyroscope Z [deg/s] */
    int32_t timestamp_us;
} imu_sample_t;

/** Initialise the QMI8658 over I²C. */
void imu_init(void);

/** Read one sample. Returns true on success. */
bool imu_read(imu_sample_t *out);

/**
 * Returns true if significant motion was detected in the last
 * motion_window_ms milliseconds (magnitude above threshold_g).
 */
bool imu_motion_detected(float threshold_g, int motion_window_ms);
