#include "radio_control.h"

#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "task_stats.h"

static const char *TAG = "radio_control";

typedef struct {
    radio_control_fn_t fn;
    void *ctx;
    SemaphoreHandle_t done;
} radio_control_req_t;

/* Depth 1, deliberately: this project's radio calls are infrequent and
 * individually fast (see radio_control.h's own comment), so there is no
 * real workload to buffer. A single slot is also the simplest possible
 * "queue full" behavior to reason about and test - the second concurrent
 * submitter just waits for the first to be dequeued, exactly the backpressure
 * review finding F06's acceptance criteria ask to be exercised. */
#define RADIO_CONTROL_QUEUE_DEPTH 1

static QueueHandle_t s_queue = NULL;

static void radio_control_task(void *arg)
{
    (void)arg;
    radio_control_req_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        req.fn(req.ctx);
        xSemaphoreGive(req.done);
    }
}

esp_err_t radio_control_init(void)
{
    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_queue = xQueueCreate(RADIO_CONTROL_QUEUE_DEPTH, sizeof(radio_control_req_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Priority 3, not this project's usual worker-task 5: mmwlan.h's own
     * "Thread priorities" table (L21-28) puts the vendor's spi_irq/drv/
     * evtloop threads at MMOSAL_TASK_PRI_HIGH, which
     * mmosal_shim_freertos_esp32.c's mmosal_task_create() maps to FreeRTOS
     * priority tskIDLE_PRIORITY(0) + 4 = 4 (mmosal.h's
     * enum mmosal_task_priority: IDLE=0, MIN=1, LOW=2, NORM=3, HIGH=4). That
     * same doc comment says outright: "it is expected that application
     * threads run at a lower priority." radio_control_task is exactly that
     * kind of application thread - it never does driver work itself, only
     * calls into morselib on the driver's behalf - so it belongs below 4,
     * not above it.
     *
     * This was originally 5 (matching cot_relay/dns_forward/the reconnect
     * tasks/datapath), which put it *above* drv/evtloop/spi_irq instead of
     * below. Confirmed on real hardware to matter, not just a paper
     * violation: GW_ROLE_RELAY's AP bring-up (downlink_halow_ap_init())
     * fires several radio_control_run() calls back-to-back with essentially
     * no gap, and at priority 5 this task could always preempt drv/evtloop
     * the instant it had anything to do - symbolized crash backtrace showed
     * a watchdog panic (TG1WDT_SYS_RST) inside mmint_morse_pagesets_work
     * (closed-source - no debug info, but confirmed via addr2line against
     * the built ELF) on the vendor's own "drv" task, reproducible across
     * consecutive boots. GW_ROLE_CLIENT's STA connect path (uplink_halow.c)
     * didn't crash at the same priority, but that's a difference in timing
     * / how interleaved its calls are, not evidence priority 5 was ever
     * actually safe - the vendor's documented constraint doesn't carve out
     * an exception for it. 3 matches MMOSAL_TASK_PRI_NORM (the same level
     * morselib itself uses for its own lwIP tcpip/ip threads,
     * FreeRTOSIPConfig.h/lwipopts.h), putting radio_control below
     * drv/evtloop/spi_irq as the vendor's own doc comment calls for. */
    if (xTaskCreate(radio_control_task, "radio_control", GW_STACK_RADIO_CONTROL, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t radio_control_run(radio_control_fn_t fn, void *ctx, uint32_t timeout_ms)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One completion semaphore per call, not a shared static one: two
     * requests can legitimately be in flight at once from this function's
     * own perspective (one just dequeued and executing, one freshly
     * enqueued behind it - see RADIO_CONTROL_QUEUE_DEPTH's comment), and
     * each needs its own signal. Unlike a ctx, a semaphore has no caller
     * *data* to corrupt if abandoned after a timeout - see the leak note
     * below for why that makes it the one thing here that's safe to be
     * short-lived. */
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    radio_control_req_t req = { .fn = fn, .ctx = ctx, .done = done };
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(s_queue, &req, timeout_ticks) != pdTRUE) {
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(done, timeout_ticks) != pdTRUE) {
        /* fn may still be queued or actually running on the owner task, and
         * will call xSemaphoreGive(done) when it eventually finishes or is
         * dequeued. Deleting done out from under that would be a
         * use-after-free from the owner's side - a FreeRTOS semaphore
         * handle, not caller data, so leaking this one small object is the
         * safe choice, not a bug. Expected to be rare: every call this
         * project makes through here is documented/observed to be fast (see
         * radio_control.h), so a timeout here means the request was stuck
         * behind unrelated work or the driver itself is unresponsive -
         * either way, worth knowing about. */
        ESP_LOGW(TAG, "radio_control_run timed out after %" PRIu32 " ms - request may still be pending",
                 timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done);
    return ESP_OK;
}
