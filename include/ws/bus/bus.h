/**
 * @file bus.h
 * @brief Pub/sub message bus for embedded C — the ws-bus-embedded-c base library.
 *
 * Realizes base_tech_spec.md §5, embedded_system_spec.md §5, and
 * embedded_c_tech_spec.md §6/§9:
 *   - Zero dynamic allocation: caller-owned static storage sized per queue.
 *   - Per-queue depth, payload capacity, overflow policy, and type filters
 *     (one inbound queue per system).
 *   - Single-handler command registration (duplicate command handlers fail loud).
 *   - ISR-aware publishing: ws_bus_publish_from_isr() enqueues and defers;
 *     no subscriber work runs in interrupt context.
 *   - 64-bit monotonic timestamps from an injected uint64_t (*now_us)(void) callback.
 *   - Typed error values via WS_ERROR_TYPE() from <ws/error/error.h>.
 *
 * Consumers include this header as <ws/bus/bus.h>.
 */
#ifndef WS_WS_BUS_BUS_H
#define WS_WS_BUS_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ws/error/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time table bounds (override with -D on the compiler command line). */
#ifndef WS_BUS_MAX_SUBSCRIBERS
#define WS_BUS_MAX_SUBSCRIBERS 16u
#endif

#ifndef WS_BUS_MAX_COMMANDS
#define WS_BUS_MAX_COMMANDS 32u
#endif

/* Optional critical-section hooks (no-ops by default; define to platform IRQ mask). */
#ifndef WS_BUS_ENTER_CRITICAL
#define WS_BUS_ENTER_CRITICAL() ((void)0)
#endif

#ifndef WS_BUS_EXIT_CRITICAL
#define WS_BUS_EXIT_CRITICAL() ((void)0)
#endif

/* ---------------------------------------------------------------------------
 * Typed error values (embedded_c_tech_spec.md §6).
 * --------------------------------------------------------------------------- */

typedef enum {
    WS_BUS_OK = 0,
    WS_BUS_BAD_CONFIG,
    WS_BUS_FULL,
    WS_BUS_REJECTED,
    WS_BUS_EMPTY,
    WS_BUS_NO_SLOT,
    WS_BUS_PAYLOAD_TOO_LARGE,
    WS_BUS_DUPLICATE_HANDLER,
    WS_BUS_BUFFER_TOO_SMALL,
} ws_bus_err_t;
WS_ERROR_TYPE(ws_bus_err_t, ws_bus_error_t);

/* ---------------------------------------------------------------------------
 * Messages & overflow policies.
 * --------------------------------------------------------------------------- */

typedef struct {
    const char *type; /**< Message type name, static string literal. */
    const char *source; /**< Publishing system, static string literal (or NULL). */
    uint64_t timestamp_us; /**< Microseconds since boot; stamped at publish. */
    uint32_t correlation_id; /**< 0 = none; else correlates request/response. */
    bool expects_response; /**< Publisher expects a correlated response event. */
} ws_msg_header_t;

typedef enum {
    WS_OVERFLOW_DROP_OLDEST = 0,
    WS_OVERFLOW_DROP_NEWEST = 1,
    WS_OVERFLOW_REJECT = 2,
} ws_overflow_t;

/* ---------------------------------------------------------------------------
 * Per-queue static backing storage (zero heap, exact per-system sizing).
 * --------------------------------------------------------------------------- */

typedef struct {
    ws_msg_header_t *headers; /**< Array of @c depth headers. */
    uint16_t *payload_lens; /**< Array of @c depth lengths. */
    bool *dispatched; /**< Array of @c depth dispatch flags. */
    uint8_t *payload_bytes; /**< Flat byte array of @c depth * @c payload_size bytes. */
} ws_queue_storage_t;

/**
 * @brief Declare exact-sized static backing arrays for one inbound queue.
 */
#define WS_BUS_QUEUE_STORAGE(name, depth_val, payload_val)                                         \
    static ws_msg_header_t name##_headers[(depth_val)];                                            \
    static uint16_t name##_lens[(depth_val)];                                                      \
    static bool name##_dispatched[(depth_val)];                                                    \
    static uint8_t                                                                                 \
        name##_payloads[(size_t)(depth_val) * (size_t)((payload_val) > 0u ? (payload_val) : 1u)];  \
    static const ws_queue_storage_t name = {                                                       \
        .headers = name##_headers,                                                                 \
        .payload_lens = name##_lens,                                                               \
        .dispatched = name##_dispatched,                                                           \
        .payload_bytes = name##_payloads,                                                          \
    }

typedef struct {
    uint16_t depth; /**< Ring capacity (>= 1). */
    uint16_t payload_size; /**< Max payload bytes per slot (0 = header-only). */
    ws_overflow_t overflow; /**< Declared overflow policy for this queue. */
    const char *const *filters; /**< Array of static type names; NULL + 0 = wildcard. */
    uint16_t filter_count; /**< Number of entries in @c filters (0 = wildcard). */
    const ws_queue_storage_t *storage; /**< Caller-owned static storage (must outlive queue). */
} ws_queue_cfg_t;

typedef struct {
    const char *const *filters;
    uint16_t filter_count;
    uint16_t depth;
    uint16_t payload_size;
    ws_overflow_t overflow;
    ws_queue_storage_t storage;
    bool attached;
    uint16_t head;
    uint16_t count;
    volatile bool pending_work;
} ws_queue_t;

/* ---------------------------------------------------------------------------
 * Bus config & instance.
 * --------------------------------------------------------------------------- */

typedef struct {
    const char *command_type;
    ws_queue_t *handler;
} ws_bus_cmd_slot_t;

typedef struct {
    uint16_t max_subscribers; /**< 1 .. WS_BUS_MAX_SUBSCRIBERS. */
    uint64_t (*now_us)(void); /**< Injected 64-bit ISR-safe monotonic time source. */
} ws_bus_cfg_t;

typedef struct {
    const ws_bus_cfg_t *cfg;
    uint32_t next_correlation_id;
    uint16_t subscriber_count;
    uint16_t command_count;
    bool initialized;
    ws_queue_t *subscribers[WS_BUS_MAX_SUBSCRIBERS];
    ws_bus_cmd_slot_t commands[WS_BUS_MAX_COMMANDS];
} ws_bus_t;

/* ---------------------------------------------------------------------------
 * Public API.
 * --------------------------------------------------------------------------- */

/** @brief Initialise a statically allocated bus instance. */
ws_bus_error_t ws_bus_init(ws_bus_t *bus, const ws_bus_cfg_t *cfg);

/**
 * @brief Subscribe a caller-owned inbound queue with its own depth, payload
 *        capacity, overflow policy, and optional event-type filter list.
 *
 * Pass @c filters = NULL and @c filter_count = 0 for a wildcard subscription.
 * Pass @c filters != NULL and @c filter_count == 0 for a command-only queue
 * (receives only commands registered via ws_bus_register_command()).
 * @p out_queue must be zero-initialized before its first ws_bus_subscribe() call.
 */
ws_bus_error_t ws_bus_subscribe(ws_bus_t *bus, const ws_queue_cfg_t *qcfg, ws_queue_t *out_queue);

/**
 * @brief Register @p queue as the SINGLE exclusive handler for @p command_type.
 *
 * @p queue must already be attached to @p bus via ws_bus_subscribe().
 * Registering a second handler for the same @p command_type fails loudly with
 * WS_BUS_DUPLICATE_HANDLER (base_tech_spec.md §5).
 */
ws_bus_error_t ws_bus_register_command(ws_bus_t *bus, const char *command_type, ws_queue_t *queue);

/** @brief Broadcast an event or route a command from thread/task context. */
ws_bus_error_t ws_bus_publish(ws_bus_t *bus, const ws_msg_header_t *header, const void *payload,
                              uint16_t payload_len);

/** @brief ISR-safe enqueue-and-defer publish (never runs handlers in ISR). */
ws_bus_error_t ws_bus_publish_from_isr(ws_bus_t *bus, const ws_msg_header_t *header,
                                       const void *payload, uint16_t payload_len);

/** @brief Promote ISR-deferred messages to visible from thread/task context. */
ws_bus_error_t ws_bus_dispatch(ws_bus_t *bus);

/**
 * @brief Pop the next visible message from @p queue into caller buffers.
 *
 * Strict FIFO: if the oldest queued slot is still ISR-deferred, returns
 * WS_BUS_EMPTY until ws_bus_dispatch() promotes it. If @p out_payload is NULL
 * or @p out_cap is smaller than the queued payload, returns
 * WS_BUS_BUFFER_TOO_SMALL without consuming the slot.
 */
ws_bus_error_t ws_bus_receive(ws_queue_t *queue, ws_msg_header_t *out_header, void *out_payload,
                              uint16_t out_cap, uint16_t *out_len);

/** @brief Allocate next monotonic non-zero correlation ID (critical-section guarded). */
uint32_t ws_bus_next_correlation(ws_bus_t *bus);

/** @brief True when response carries the non-zero request correlation ID. */
bool ws_bus_correlation_matches(const ws_msg_header_t *response, uint32_t request_correlation_id);

#ifdef __cplusplus
}
#endif

#endif /* WS_WS_BUS_BUS_H */
