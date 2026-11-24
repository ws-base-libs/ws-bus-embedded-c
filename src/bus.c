/**
 * @file bus.c
 * @brief Implementation of ws-bus-embedded-c (see <ws/bus/bus.h>).
 */
#include <string.h>

#include <ws/bus/bus.h>

static bool s_bus_ready(const ws_bus_t *bus)
{
    return (bus != NULL) && bus->initialized && (bus->cfg != NULL);
}

static bool s_queue_ready(const ws_queue_t *queue)
{
    return (queue != NULL) && queue->attached;
}

static bool s_queue_attached_to(const ws_bus_t *bus, const ws_queue_t *queue)
{
    for (uint16_t i = 0u; i < bus->subscriber_count; i++) {
        if (bus->subscribers[i] == queue) {
            return true;
        }
    }
    return false;
}

static bool s_queue_matches_event(const ws_queue_t *queue, const char *type)
{
    if (type == NULL) {
        return false;
    }
    if (queue->filters == NULL) {
        return true; /* wildcard subscription */
    }
    for (uint16_t i = 0u; i < queue->filter_count; i++) {
        if (queue->filters[i] != NULL && strcmp(queue->filters[i], type) == 0) {
            return true;
        }
    }
    return false;
}

static ws_queue_t *s_find_command_handler(const ws_bus_t *bus, const char *type)
{
    for (uint16_t i = 0u; i < bus->command_count; i++) {
        if (strcmp(bus->commands[i].command_type, type) == 0) {
            return bus->commands[i].handler;
        }
    }
    return NULL;
}

static ws_bus_error_t s_queue_push(ws_queue_t *queue, const ws_msg_header_t *header,
                                   const void *payload, uint16_t payload_len, bool dispatched)
{
    if (payload_len > queue->payload_size) {
        return WS_ERR(ws_bus_error_t, WS_BUS_PAYLOAD_TOO_LARGE, WS_SEVERITY_RECOVERABLE,
                      "payload exceeds queue slot capacity", "ws_bus.push");
    }

    WS_BUS_ENTER_CRITICAL();

    if (queue->count == queue->depth) {
        switch (queue->overflow) {
        case WS_OVERFLOW_DROP_OLDEST:
            queue->head = (uint16_t)((queue->head + 1u) % queue->depth);
            queue->count = (uint16_t)(queue->count - 1u);
            break;
        case WS_OVERFLOW_DROP_NEWEST:
            WS_BUS_EXIT_CRITICAL();
            return WS_ERR_OK(ws_bus_error_t);
        case WS_OVERFLOW_REJECT:
        default:
            WS_BUS_EXIT_CRITICAL();
            return WS_ERR(ws_bus_error_t, WS_BUS_REJECTED, WS_SEVERITY_RECOVERABLE,
                          "queue full (WS_OVERFLOW_REJECT)", "ws_bus.push");
        }
    }

    const uint16_t tail = (uint16_t)((queue->head + queue->count) % queue->depth);
    queue->storage.headers[tail] = *header;
    queue->storage.payload_lens[tail] = payload_len;
    if (payload != NULL && payload_len > 0u) {
        uint8_t *dst = &queue->storage.payload_bytes[(size_t)tail * (size_t)queue->payload_size];
        (void)memcpy(dst, payload, payload_len);
    }
    queue->storage.dispatched[tail] = dispatched;
    queue->count = (uint16_t)(queue->count + 1u);

    if (!dispatched) {
        queue->pending_work = true;
    }

    WS_BUS_EXIT_CRITICAL();
    return WS_ERR_OK(ws_bus_error_t);
}

static ws_bus_error_t s_publish_impl(ws_bus_t *bus, const ws_msg_header_t *header,
                                     const void *payload, uint16_t payload_len, bool dispatched)
{
    if (!s_bus_ready(bus) || header == NULL || header->type == NULL) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid bus or message header", "ws_bus.publish");
    }
    if (payload == NULL && payload_len > 0u) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "NULL payload with non-zero length", "ws_bus.publish");
    }

    ws_msg_header_t stamped = *header;
    stamped.timestamp_us = bus->cfg->now_us();

    /* Directed command: deliver exclusively to its registered handler. */
    ws_queue_t *cmd_handler = s_find_command_handler(bus, stamped.type);
    if (cmd_handler != NULL) {
        return s_queue_push(cmd_handler, &stamped, payload, payload_len, dispatched);
    }

    /* Broadcast event: deliver to every matching subscription independently. */
    ws_bus_error_t worst = WS_ERR_OK(ws_bus_error_t);
    for (uint16_t i = 0u; i < bus->subscriber_count; i++) {
        ws_queue_t *sub = bus->subscribers[i];
        if (s_queue_matches_event(sub, stamped.type)) {
            ws_bus_error_t err = s_queue_push(sub, &stamped, payload, payload_len, dispatched);
            if (WS_FAILED(err) && WS_OK(worst)) {
                worst = err;
            }
        }
    }
    return worst;
}

ws_bus_error_t ws_bus_init(ws_bus_t *bus, const ws_bus_cfg_t *cfg)
{
    if (bus == NULL) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_FATAL, "NULL bus",
                      "ws_bus.init");
    }

    (void)memset(bus, 0, sizeof(*bus));

    if (cfg == NULL || cfg->now_us == NULL || cfg->max_subscribers == 0u ||
        cfg->max_subscribers > WS_BUS_MAX_SUBSCRIBERS) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_FATAL,
                      "invalid bus config (now_us or max_subscribers)", "ws_bus.init");
    }

    bus->cfg = cfg;
    bus->initialized = true;
    return WS_ERR_OK(ws_bus_error_t);
}

ws_bus_error_t ws_bus_subscribe(ws_bus_t *bus, const ws_queue_cfg_t *qcfg, ws_queue_t *out_queue)
{
    if (!s_bus_ready(bus) || qcfg == NULL || out_queue == NULL) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid bus, queue config, or queue handle", "ws_bus.subscribe");
    }
    if (s_queue_attached_to(bus, out_queue) || out_queue->attached) {
        return WS_ERR(ws_bus_error_t, WS_BUS_NO_SLOT, WS_SEVERITY_RECOVERABLE,
                      "queue already attached", "ws_bus.subscribe");
    }
    if (qcfg->depth == 0u || qcfg->storage == NULL || qcfg->storage->headers == NULL ||
        qcfg->storage->payload_lens == NULL || qcfg->storage->dispatched == NULL ||
        (qcfg->payload_size > 0u && qcfg->storage->payload_bytes == NULL)) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid queue depth or backing storage", "ws_bus.subscribe");
    }
    if (qcfg->overflow != WS_OVERFLOW_DROP_OLDEST && qcfg->overflow != WS_OVERFLOW_DROP_NEWEST &&
        qcfg->overflow != WS_OVERFLOW_REJECT) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid overflow policy", "ws_bus.subscribe");
    }
    if (qcfg->filters == NULL && qcfg->filter_count != 0u) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "NULL filters with non-zero filter_count", "ws_bus.subscribe");
    }
    if (bus->subscriber_count >= bus->cfg->max_subscribers) {
        return WS_ERR(ws_bus_error_t, WS_BUS_FULL, WS_SEVERITY_RECOVERABLE,
                      "bus subscriber table full", "ws_bus.subscribe");
    }

    out_queue->filters = qcfg->filters;
    out_queue->filter_count = qcfg->filter_count;
    out_queue->depth = qcfg->depth;
    out_queue->payload_size = qcfg->payload_size;
    out_queue->overflow = qcfg->overflow;
    out_queue->storage = *qcfg->storage;
    out_queue->attached = true;
    out_queue->head = 0u;
    out_queue->count = 0u;
    out_queue->pending_work = false;
    for (uint16_t i = 0u; i < qcfg->depth; i++) {
        out_queue->storage.dispatched[i] = true;
        out_queue->storage.payload_lens[i] = 0u;
    }

    bus->subscribers[bus->subscriber_count] = out_queue;
    bus->subscriber_count = (uint16_t)(bus->subscriber_count + 1u);
    return WS_ERR_OK(ws_bus_error_t);
}

ws_bus_error_t ws_bus_register_command(ws_bus_t *bus, const char *command_type, ws_queue_t *queue)
{
    if (!s_bus_ready(bus) || command_type == NULL || !s_queue_ready(queue) ||
        !s_queue_attached_to(bus, queue)) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid bus, command type, or unattached queue", "ws_bus.register_command");
    }
    if (s_find_command_handler(bus, command_type) != NULL) {
        return WS_ERR(ws_bus_error_t, WS_BUS_DUPLICATE_HANDLER, WS_SEVERITY_RECOVERABLE,
                      "command already has a registered handler", "ws_bus.register_command");
    }
    if (bus->command_count >= WS_BUS_MAX_COMMANDS) {
        return WS_ERR(ws_bus_error_t, WS_BUS_FULL, WS_SEVERITY_RECOVERABLE,
                      "bus command table full", "ws_bus.register_command");
    }

    bus->commands[bus->command_count].command_type = command_type;
    bus->commands[bus->command_count].handler = queue;
    bus->command_count = (uint16_t)(bus->command_count + 1u);
    return WS_ERR_OK(ws_bus_error_t);
}

ws_bus_error_t ws_bus_publish(ws_bus_t *bus, const ws_msg_header_t *header, const void *payload,
                              uint16_t payload_len)
{
    return s_publish_impl(bus, header, payload, payload_len, true);
}

ws_bus_error_t ws_bus_publish_from_isr(ws_bus_t *bus, const ws_msg_header_t *header,
                                       const void *payload, uint16_t payload_len)
{
    return s_publish_impl(bus, header, payload, payload_len, false);
}

ws_bus_error_t ws_bus_dispatch(ws_bus_t *bus)
{
    if (!s_bus_ready(bus)) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "bus not initialised", "ws_bus.dispatch");
    }

    bool promoted = false;
    for (uint16_t s = 0u; s < bus->subscriber_count; s++) {
        ws_queue_t *sub = bus->subscribers[s];
        if (sub->pending_work) {
            WS_BUS_ENTER_CRITICAL();
            for (uint16_t i = 0u; i < sub->count; i++) {
                const uint16_t idx = (uint16_t)((sub->head + i) % sub->depth);
                if (!sub->storage.dispatched[idx]) {
                    sub->storage.dispatched[idx] = true;
                    promoted = true;
                }
            }
            sub->pending_work = false;
            WS_BUS_EXIT_CRITICAL();
        }
    }

    if (!promoted) {
        return WS_ERR(ws_bus_error_t, WS_BUS_EMPTY, WS_SEVERITY_RECOVERABLE, "no deferred messages",
                      "ws_bus.dispatch");
    }
    return WS_ERR_OK(ws_bus_error_t);
}

ws_bus_error_t ws_bus_receive(ws_queue_t *queue, ws_msg_header_t *out_header, void *out_payload,
                              uint16_t out_cap, uint16_t *out_len)
{
    if (!s_queue_ready(queue) || out_header == NULL) {
        return WS_ERR(ws_bus_error_t, WS_BUS_BAD_CONFIG, WS_SEVERITY_RECOVERABLE,
                      "invalid queue or NULL out_header", "ws_bus.receive");
    }

    WS_BUS_ENTER_CRITICAL();

    if (queue->count == 0u) {
        WS_BUS_EXIT_CRITICAL();
        return WS_ERR(ws_bus_error_t, WS_BUS_EMPTY, WS_SEVERITY_RECOVERABLE, "queue empty",
                      "ws_bus.receive");
    }

    const uint16_t head = queue->head;
    if (!queue->storage.dispatched[head]) {
        WS_BUS_EXIT_CRITICAL();
        return WS_ERR(ws_bus_error_t, WS_BUS_EMPTY, WS_SEVERITY_RECOVERABLE,
                      "oldest message still ISR-deferred", "ws_bus.receive");
    }

    const uint16_t len = queue->storage.payload_lens[head];
    if (len > 0u && (out_payload == NULL || out_cap < len)) {
        WS_BUS_EXIT_CRITICAL();
        return WS_ERR(ws_bus_error_t, WS_BUS_BUFFER_TOO_SMALL, WS_SEVERITY_RECOVERABLE,
                      "caller payload buffer too small", "ws_bus.receive");
    }

    *out_header = queue->storage.headers[head];
    if (len > 0u) {
        const uint8_t *src =
            &queue->storage.payload_bytes[(size_t)head * (size_t)queue->payload_size];
        (void)memcpy(out_payload, src, len);
    }
    if (out_len != NULL) {
        *out_len = len;
    }

    queue->head = (uint16_t)((queue->head + 1u) % queue->depth);
    queue->count = (uint16_t)(queue->count - 1u);

    WS_BUS_EXIT_CRITICAL();
    return WS_ERR_OK(ws_bus_error_t);
}

uint32_t ws_bus_next_correlation(ws_bus_t *bus)
{
    if (bus == NULL) {
        return 0u;
    }

    WS_BUS_ENTER_CRITICAL();
    bus->next_correlation_id += 1u;
    if (bus->next_correlation_id == 0u) {
        bus->next_correlation_id = 1u;
    }
    const uint32_t id = bus->next_correlation_id;
    WS_BUS_EXIT_CRITICAL();
    return id;
}

bool ws_bus_correlation_matches(const ws_msg_header_t *response, uint32_t request_correlation_id)
{
    return (response != NULL) && (request_correlation_id != 0u) &&
           (response->correlation_id == request_correlation_id);
}
