/**
 * @file basic_bus.c
 * @brief Minimal example showing per-queue pub/sub and commands in ws-bus-embedded-c.
 */
#include <stdint.h>
#include <stdio.h>

#include <ws/bus/bus.h>

static uint64_t s_now_us(void)
{
    return 123456ULL;
}

WS_BUS_QUEUE_STORAGE(s_sub_storage, 4u, 16u);

int main(void)
{
    static const ws_bus_cfg_t k_bus_cfg = { .max_subscribers = 4u, .now_us = s_now_us };
    static const char *const k_filters[] = { "TemperatureSampled" };
    static const ws_queue_cfg_t k_qcfg = {
        .depth = 4u,
        .payload_size = 16u,
        .overflow = WS_OVERFLOW_DROP_OLDEST,
        .filters = k_filters,
        .filter_count = 1u,
        .storage = &s_sub_storage,
    };

    static ws_bus_t s_bus;
    static ws_queue_t s_queue;

    if (WS_FAILED(ws_bus_init(&s_bus, &k_bus_cfg))) {
        return 1;
    }
    if (WS_FAILED(ws_bus_subscribe(&s_bus, &k_qcfg, &s_queue))) {
        return 1;
    }

    int32_t temp_centi_c = 2500;
    const ws_msg_header_t hdr = { .type = "TemperatureSampled", .source = "example" };
    (void)ws_bus_publish(&s_bus, &hdr, &temp_centi_c, (uint16_t)sizeof(temp_centi_c));

    ws_msg_header_t out_hdr;
    int32_t out_val = 0;
    uint16_t out_len = 0u;
    if (WS_OK(ws_bus_receive(&s_queue, &out_hdr, &out_val, (uint16_t)sizeof(out_val), &out_len))) {
        printf("%s @ %llu us: %d\n", out_hdr.type, (unsigned long long)out_hdr.timestamp_us,
               out_val);
    }
    return 0;
}
