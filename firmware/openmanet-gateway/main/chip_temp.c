#include "chip_temp.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "chip_temp";

static temperature_sensor_handle_t s_handle = NULL;

esp_err_t chip_temp_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&cfg, &s_handle);
    if (err != ESP_OK) {
        s_handle = NULL;
        return err;
    }
    err = temperature_sensor_enable(s_handle);
    if (err != ESP_OK) {
        temperature_sensor_uninstall(s_handle);
        s_handle = NULL;
        return err;
    }
    return ESP_OK;
}

bool chip_temp_read_celsius(float *out_celsius)
{
    if (s_handle == NULL) {
        return false;
    }
    float celsius;
    esp_err_t err = temperature_sensor_get_celsius(s_handle, &celsius);
    if (err != ESP_OK) {
        /* Rare in practice (would mean a reading fell outside the installed
         * 20-100C range) but not worth escalating past a warning - the web
         * UI already treats a false return the same as "no sensor". */
        ESP_LOGW(TAG, "temperature_sensor_get_celsius failed: %s", esp_err_to_name(err));
        return false;
    }
    *out_celsius = celsius;
    return true;
}
