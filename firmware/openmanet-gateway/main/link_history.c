#include "link_history.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "uplink_halow.h"
#include "uplink_wifi.h"

static const char *TAG = "link_history";

static int16_t s_ring[LINK_HISTORY_SAMPLES];
static size_t s_head = 0;  /* next write index */
static size_t s_count = 0; /* valid samples so far, saturates at LINK_HISTORY_SAMPLES */
static SemaphoreHandle_t s_lock = NULL;
static esp_timer_handle_t s_timer = NULL;

/* Raw per-source readings from the same sample the ring above collapses into
 * one value - see link_history_get_latest_halow_rssi()/_wifi_rssi()'s own
 * comment in link_history.h for why both are kept separately too. Guarded by
 * s_lock, same as the ring - one short critical section in sample_timer_cb()
 * updates all three together. */
static int32_t s_latest_halow_rssi = INT32_MIN;
static int8_t s_latest_wifi_rssi = INT8_MIN;

/* Both getters are safe to call regardless of which role is active - see
 * web_ui.c's status_get_handler() comment on this same pattern: the getter
 * for whichever role's init() never ran just reports its own idle sentinel
 * (INT32_MIN / INT8_MIN) rather than reading stale or garbage state. Exactly
 * one of the two is ever non-sentinel per role, so trying both and taking
 * whichever answers is role-agnostic without this file needing to know
 * gw_config_t at all. */
static int16_t sample_uplink_rssi(void)
{
    int32_t halow_rssi = uplink_halow_get_rssi();
    if (halow_rssi != INT32_MIN) {
        return (int16_t)halow_rssi;
    }
    int8_t wifi_rssi = uplink_wifi_get_rssi();
    if (wifi_rssi != INT8_MIN) {
        return (int16_t)wifi_rssi;
    }
    return INT16_MIN;
}

static void sample_timer_cb(void *arg)
{
    (void)arg;
    int16_t rssi = sample_uplink_rssi();
    int32_t halow_rssi = uplink_halow_get_rssi();
    int8_t wifi_rssi = uplink_wifi_get_rssi();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ring[s_head] = rssi;
    s_head = (s_head + 1) % LINK_HISTORY_SAMPLES;
    if (s_count < LINK_HISTORY_SAMPLES) {
        s_count++;
    }
    s_latest_halow_rssi = halow_rssi;
    s_latest_wifi_rssi = wifi_rssi;
    xSemaphoreGive(s_lock);
}

esp_err_t link_history_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Default dispatch method is ESP_TIMER_TASK (runs in the esp_timer
     * service task, not an ISR - esp_timer.h's own docs on
     * esp_timer_create()), so the plain blocking xSemaphoreTake() above is
     * safe here. */
    const esp_timer_create_args_t timer_args = {
        .callback = sample_timer_cb,
        .name = "link_history",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    sample_timer_cb(NULL); /* first point exists immediately, not after 30s */

    err = esp_timer_start_periodic(s_timer, (uint64_t)LINK_HISTORY_INTERVAL_S * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
    }
    return err;
}

size_t link_history_get_rssi(int16_t *out, size_t out_capacity)
{
    if (out == NULL || out_capacity == 0 || s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t total = s_count;
    size_t n = (total > out_capacity) ? out_capacity : total;
    /* Oldest valid sample sits at s_head once the ring has wrapped (s_head is
     * about to overwrite it next), or at index 0 before it has. If the
     * caller's buffer is smaller than what we're holding, skip the oldest
     * (total - n) of them so the copy is still the most recent n samples. */
    size_t oldest = (total < LINK_HISTORY_SAMPLES) ? 0 : s_head;
    size_t start = (oldest + (total - n)) % LINK_HISTORY_SAMPLES;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(start + i) % LINK_HISTORY_SAMPLES];
    }
    xSemaphoreGive(s_lock);

    return n;
}

int32_t link_history_get_latest_halow_rssi(void)
{
    if (s_lock == NULL) {
        return INT32_MIN;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int32_t rssi = s_latest_halow_rssi;
    xSemaphoreGive(s_lock);
    return rssi;
}

int8_t link_history_get_latest_wifi_rssi(void)
{
    if (s_lock == NULL) {
        return INT8_MIN;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int8_t rssi = s_latest_wifi_rssi;
    xSemaphoreGive(s_lock);
    return rssi;
}
