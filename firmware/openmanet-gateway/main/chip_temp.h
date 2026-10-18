#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs and enables the ESP32-S3's internal die temperature sensor
 * (esp_driver_tsens - components/esp_driver_tsens/include/driver/
 * temperature_sensor.h at v5.5.1). This is the SoC's own die reading, not
 * the HaLow module's: the vendored Morse Micro SDK (mmwlan.h, mmhal_wlan.h,
 * mmhal_app.h, mmosal.h) exposes no thermal API for the MM6108/FGH100M-H at
 * all, so the HaLow board's temperature isn't software-readable without
 * external sensor hardware.
 *
 * Range is 20-100C (TEMPERATURE_SENSOR_ATTR_RANGE_NUM table,
 * soc/esp32s3/temperature_sensor_periph.c: the {-1, 7, 20, 100, +/-2C}
 * entry) rather than the narrower-but-more-accurate -10..80C one, because
 * the concern this exists for is confirming the enclosure runs hot, not
 * precision at room temperature - 80C would clip exactly the readings that
 * matter.
 *
 * Safe to call once at boot regardless of role; non-fatal on failure
 * (chip_temp_read_celsius() then always returns false). */
esp_err_t chip_temp_init(void);

/* Reads the die temperature. Returns false (and leaves *out_celsius
 * untouched) if chip_temp_init() never succeeded or the read itself failed -
 * callers should treat that the same as "no data" rather than a fault. */
bool chip_temp_read_celsius(float *out_celsius);

#ifdef __cplusplus
}
#endif
