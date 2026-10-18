#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stack sizes, in bytes, for the tasks this firmware creates.
 *
 * Named here rather than written as literals at each xTaskCreate() call so the
 * headroom report below can compute a percentage against the size actually in
 * force, instead of against a second copy of the number that drifts away from
 * it. Change a size here and both the task and its report move together.
 *
 * ESP-IDF's xTaskCreate() takes the stack depth in *bytes*, unlike vanilla
 * FreeRTOS which takes words - its port defines StackType_t as uint8_t
 * (freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h L88,
 * L91 at v5.5.1). These are byte counts. */
#define GW_STACK_DATAPATH        4096
#define GW_STACK_COT_RELAY       4096
#define GW_STACK_WIFI_RECONNECT  4096
#define GW_STACK_HALOW_RECONNECT 4096
#define GW_STACK_FACTORY_RESET   4096
/* Runs every non-TX mmwlan_ and mmhalow_ driver call in the firmware
 * (review finding F06, design/PROJECT_REVIEW_2026-09-10.md - see
 * main/radio_control.c).
 * Matched to this project's other worker tasks rather than trimmed, same
 * "budget generously, not exactly" reasoning as the rest of this table -
 * the calls it makes are individually small (config structs, a getter, a
 * scan-start kickoff), but it's a new task with no real-hardware measurement
 * yet; re-measure via gwcfg-tasks/GET /api/tasks once it's exercised and
 * raise if headroom is thin. */
#define GW_STACK_RADIO_CONTROL   4096
/* select() over two sockets plus a bounded pending-query table walk -
 * lighter than cot_relay's own budget (no cJSON, no multi-KB scan buffer),
 * but matched to it rather than trimmed, same reasoning as every other
 * socket-handling task here. */
#define GW_STACK_DNS_FORWARD     4096
/* 6144 -> 7168 for design/ROADMAP.md item 1's web UI auth: an existing
 * measurement (see "Stack budgets" below) already showed only ~1648 bytes
 * free (74% used) under a max-field POST /api/config, before any auth code
 * existed. The new login/password handlers add one mbedtls_md_hmac(SHA256)
 * call (small - a context plus two SHA-256 block computations, since the
 * expensive PBKDF2 loop runs client-side, never on this device - see
 * auth.c's own comment) plus cookie parsing, hex encode/decode scratch, and
 * cJSON parsing of new small bodies on top of an already-thin budget.
 * +1024 is a proactive margin, not a tightly-derived number - re-measure via
 * gwcfg-tasks/GET /api/tasks under a real login attempt on hardware and
 * raise again if headroom is still thin. */
/* 7168 -> 10240 for review finding F02's HTTPS switch
 * (design/PROJECT_REVIEW_2026-09-10.md): a TLS handshake runs the ECDSA
 * sign/verify and key-exchange math directly on this task, well beyond the
 * single small HMAC call the 6144->7168 raise above was sized for.
 * 10240 is esp_https_server.h's own HTTPD_SSL_CONFIG_DEFAULT() stack_size -
 * ESP-IDF's documented figure for exactly this task shape, not a guess -
 * used as the starting point rather than re-deriving it. Proactive, same as
 * the raise above: re-measure via gwcfg-tasks/GET /api/tasks under a real
 * TLS handshake on hardware and raise again if headroom is thin. */
#define GW_STACK_WEB_UI          10240

/* One-shot, self-deleting task (same shape as GW_STACK_DATAPATH - "absent" in
 * gwcfg-tasks/GET /api/tasks is its normal post-boot state, not a fault) that
 * runs ECDSA P-256 key generation and X.509 certificate signing for
 * tls_identity_init()/tls_identity_regenerate() (main/tls_identity.c, review
 * finding F02). Confirmed necessary on real hardware, not sized by guesswork:
 * running that work directly on FreeRTOS's default "main" task - which is
 * where app_main() itself executes, and where tls_identity_init() used to be
 * called from directly - overflowed it and crashed the node on first boot.
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE is 3584 in this project's sdkconfig, never
 * sized for work like this (every dedicated task here runs at >=4096), and
 * mbedtls's ECP/bignum code during key generation and signing walks deep
 * enough to need real room. The exact class of bug design/ROADMAP.md's item 8
 * already documents - a deep call chain landing on a task sized for something
 * else entirely. */
#define GW_STACK_TLS_IDENTITY_GEN 8192

/* Deliberately the smallest in the firmware: status_led_task() reads an enum
 * and toggles a GPIO. It has no room for a log call, and shouldn't grow one -
 * see design/ROADMAP.md "Stack budgets". */
#define GW_STACK_STATUS_LED      2048

/* One task's stack headroom, as reported by task_stats_each_stack(). */
typedef struct {
    const char *name;      /* FreeRTOS task name that was queried */
    bool present;          /* false = not running right now; the fields below are then meaningless */
    size_t stack_total;    /* bytes the task was created with */
    size_t stack_free_min; /* bytes still free at the task's deepest point so far */
} task_stack_info_t;

typedef void (*task_stack_cb_t)(const task_stack_info_t *info, void *ctx);

/* Reports the worst-case stack headroom of every task this firmware cares
 * about - its own, the ESP-IDF ones whose budget it depends on, and the HaLow
 * SDK's event loop. Invokes `cb` once per task in the table, including tasks
 * that aren't currently running (with .present = false), so the caller can
 * show "not running" rather than silently omitting a row. Returns how many
 * were actually found.
 *
 * This exists because a stack overflow is invisible until it isn't: the node
 * ran for weeks looking healthy, then panicked into a reboot loop the moment a
 * handler got two frames deeper (design/ROADMAP.md item 8). The high-water
 * mark is the only way to see how much margin is really left rather than
 * guessing at it, and it has to be readable from the web UI as well as the
 * console, because the failure it predicts happens on nodes with no cable
 * attached.
 *
 * Call on demand, not in a loop: it resolves each name with xTaskGetHandle(),
 * which walks every task list in the scheduler, and measures each mark by
 * scanning the task's stack for the fill pattern. Neither is O(1). */
size_t task_stats_each_stack(task_stack_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
