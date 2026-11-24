/**
 * @file bus_test.c
 * @brief Standalone host tests for ws-bus-embedded-c.
 */
#include <stdio.h>
#include <string.h>

#include <ws/bus/bus.h>

static int s_checks = 0;
static int s_failures = 0;
static uint64_t s_fake_us = 1000u;

static uint64_t fake_now_us(void)
{
    return s_fake_us;
}

static void check(int cond, const char *desc)
{
    s_checks++;
    if (!cond) {
        s_failures++;
        fprintf(stderr, "FAIL: %s\n", desc);
    }
}

static void test_init_validation(void)
{
    ws_bus_t bus;
    ws_bus_cfg_t bad_no_clk = { .max_subscribers = 4u, .now_us = NULL };
    ws_bus_cfg_t bad_zero = { .max_subscribers = 0u, .now_us = fake_now_us };
    ws_bus_cfg_t bad_over = { .max_subscribers = WS_BUS_MAX_SUBSCRIBERS + 1u,
                              .now_us = fake_now_us };

    check(ws_bus_init(NULL, &bad_no_clk).code == WS_BUS_BAD_CONFIG, "NULL bus rejected");
    check(ws_bus_init(&bus, NULL).code == WS_BUS_BAD_CONFIG, "NULL cfg rejected");
    check(ws_bus_init(&bus, &bad_no_clk).code == WS_BUS_BAD_CONFIG, "NULL now_us rejected");
    check(ws_bus_init(&bus, &bad_zero).code == WS_BUS_BAD_CONFIG, "0 subscribers rejected");
    check(ws_bus_init(&bus, &bad_over).code == WS_BUS_BAD_CONFIG, "over-max subscribers rejected");
}

static void test_per_queue_sizing_and_filters(void)
{
    static const ws_bus_cfg_t k_cfg = { .max_subscribers = 4u, .now_us = fake_now_us };
    static const char *const k_temp_filters[] = { "TemperatureSampled", "FaultRaised" };
    WS_BUS_QUEUE_STORAGE(s_small_store, 2u, 16u);
    WS_BUS_QUEUE_STORAGE(s_large_store, 2u, 256u);

    ws_bus_t bus;
    ws_queue_t q_small = { 0 };
    ws_queue_t q_large = { 0 };

    check(WS_OK(ws_bus_init(&bus, &k_cfg)), "bus init ok");

    const ws_queue_cfg_t cfg_small = {
        .depth = 2u,
        .payload_size = 16u,
        .overflow = WS_OVERFLOW_DROP_OLDEST,
        .filters = k_temp_filters,
        .filter_count = 2u,
        .storage = &s_small_store,
    };
    const ws_queue_cfg_t cfg_large = {
        .depth = 2u,
        .payload_size = 256u,
        .overflow = WS_OVERFLOW_REJECT,
        .filters = NULL, /* wildcard */
        .filter_count = 0u,
        .storage = &s_large_store,
    };

    check(WS_OK(ws_bus_subscribe(&bus, &cfg_small, &q_small)), "subscribe small multi-filter ok");
    check(WS_OK(ws_bus_subscribe(&bus, &cfg_large, &q_large)), "subscribe large wildcard ok");
    check(ws_bus_subscribe(&bus, &cfg_small, &q_small).code == WS_BUS_NO_SLOT,
          "double subscribe rejected");

    /* 64-bit timestamp above 32-bit wrap (~71.5 min). */
    s_fake_us = 0x100000005ULL;
    int32_t temp = 23500;
    ws_msg_header_t hdr = { .type = "TemperatureSampled", .source = "temp_sampling" };
    check(WS_OK(ws_bus_publish(&bus, &hdr, &temp, (uint16_t)sizeof(temp))), "publish small ok");

    /* 200-byte payload matches wildcard large queue only; does not match small filter. */
    uint8_t big_buf[200];
    memset(big_buf, 0xA5, sizeof(big_buf));
    ws_msg_header_t big_hdr = { .type = "ProfileSaved", .source = "profile_store" };
    check(WS_OK(ws_bus_publish(&bus, &big_hdr, big_buf, (uint16_t)sizeof(big_buf))),
          "publish 200B payload to large queue ok");

    /* Third message to full WS_OVERFLOW_REJECT large queue returns WS_BUS_REJECTED. */
    ws_bus_error_t rej = ws_bus_publish(&bus, &big_hdr, big_buf, (uint16_t)sizeof(big_buf));
    check(WS_FAILED(rej) && rej.code == WS_BUS_REJECTED, "reject policy returns WS_BUS_REJECTED");

    /* Oversized payload to matching small queue returns WS_BUS_PAYLOAD_TOO_LARGE. */
    uint8_t too_big[32] = { 0 };
    ws_msg_header_t fault_hdr = { .type = "FaultRaised", .source = "safety" };
    ws_bus_error_t over = ws_bus_publish(&bus, &fault_hdr, too_big, (uint16_t)sizeof(too_big));
    check(WS_FAILED(over) && over.code == WS_BUS_PAYLOAD_TOO_LARGE, "oversized payload rejected");

    /* Verify 64-bit timestamp and payload round-trip on q_small. */
    ws_msg_header_t out_h;
    int32_t out_temp = 0;
    uint16_t out_len = 0u;
    check(ws_bus_receive(&q_small, &out_h, &out_temp, 2u, &out_len).code == WS_BUS_BUFFER_TOO_SMALL,
          "too-small receive buffer preserves message");
    check(WS_OK(ws_bus_receive(&q_small, &out_h, &out_temp, (uint16_t)sizeof(out_temp), &out_len)),
          "receive small ok");
    check(out_h.timestamp_us == 0x100000005ULL,
          "64-bit timestamp preserved across 32-bit boundary");
    check(out_len == sizeof(temp) && out_temp == 23500, "small payload preserved");

    /* Drain q_large (wildcard REJECT queue) before testing q_small DropOldest. */
    uint8_t drain_buf[256];
    while (
        WS_OK(ws_bus_receive(&q_large, &out_h, drain_buf, (uint16_t)sizeof(drain_buf), &out_len))) {
    }

    /* Verify DropOldest on q_small (depth 2). */
    for (int32_t v = 1; v <= 3; v++) {
        ws_bus_error_t perr = ws_bus_publish(&bus, &hdr, &v, (uint16_t)sizeof(v));
        if (v <= 2) {
            check(WS_OK(perr), "publish 1..2 ok for both queues");
        } else {
            /* q_large (depth 2, wildcard REJECT) rejects #3 while q_small still receives it. */
            check(perr.code == WS_BUS_REJECTED, "publish 3 reports q_large REJECT");
        }
    }
    check(WS_OK(ws_bus_receive(&q_small, &out_h, &out_temp, (uint16_t)sizeof(out_temp), &out_len)),
          "drop-oldest recv 1");
    check(out_temp == 2, "drop-oldest evicted 1, kept 2");
    check(WS_OK(ws_bus_receive(&q_small, &out_h, &out_temp, (uint16_t)sizeof(out_temp), &out_len)),
          "drop-oldest recv 2");
    check(out_temp == 3, "drop-oldest kept 3");
}

static void test_commands_isr_and_correlation(void)
{
    static const ws_bus_cfg_t k_cfg = { .max_subscribers = 4u, .now_us = fake_now_us };
    static const char *const k_no_events[] = { NULL };
    WS_BUS_QUEUE_STORAGE(s_cmd_store, 4u, 32u);
    WS_BUS_QUEUE_STORAGE(s_obs_store, 4u, 32u);

    ws_bus_t bus;
    ws_queue_t q_cmd = { 0 };
    ws_queue_t q_obs = { 0 };
    check(WS_OK(ws_bus_init(&bus, &k_cfg)), "bus init for commands");

    const ws_queue_cfg_t cfg_cmd = {
        .depth = 4u,
        .payload_size = 32u,
        .overflow = WS_OVERFLOW_DROP_NEWEST,
        .filters = k_no_events,
        .filter_count = 0u, /* command-only queue */
        .storage = &s_cmd_store,
    };
    const ws_queue_cfg_t cfg_obs = {
        .depth = 4u,
        .payload_size = 32u,
        .overflow = WS_OVERFLOW_DROP_OLDEST,
        .filters = NULL, /* wildcard event observer */
        .filter_count = 0u,
        .storage = &s_obs_store,
    };

    check(WS_OK(ws_bus_subscribe(&bus, &cfg_cmd, &q_cmd)), "cmd queue subscribed");
    check(WS_OK(ws_bus_subscribe(&bus, &cfg_obs, &q_obs)), "obs queue subscribed");
    check(WS_OK(ws_bus_register_command(&bus, "StartRun", &q_cmd)), "register StartRun handler");
    check(ws_bus_register_command(&bus, "StartRun", &q_obs).code == WS_BUS_DUPLICATE_HANDLER,
          "duplicate command handler rejected");

    /* Command goes ONLY to registered handler, NOT wildcard event observer. */
    uint32_t cid = ws_bus_next_correlation(&bus);
    check(cid == 1u, "first correlation id is 1");
    ws_msg_header_t cmd = {
        .type = "StartRun",
        .source = "gui",
        .correlation_id = cid,
        .expects_response = true,
    };
    uint32_t profile_id = 7u;
    check(WS_OK(ws_bus_publish(&bus, &cmd, &profile_id, (uint16_t)sizeof(profile_id))),
          "publish StartRun command");

    ws_msg_header_t rh;
    uint32_t rpay = 0u;
    uint16_t rlen = 0u;
    check(ws_bus_receive(&q_obs, &rh, &rpay, (uint16_t)sizeof(rpay), &rlen).code == WS_BUS_EMPTY,
          "wildcard event observer does not receive directed command");
    check(WS_OK(ws_bus_receive(&q_cmd, &rh, &rpay, (uint16_t)sizeof(rpay), &rlen)),
          "command handler received StartRun");
    check(rpay == 7u && ws_bus_correlation_matches(&rh, cid), "command payload & correlation ok");
    check(!ws_bus_correlation_matches(&rh, 0u), "correlation 0 never matches");

    /* ISR publish defers strictly until ws_bus_dispatch(). */
    ws_msg_header_t isr_ev = { .type = "SampleTick", .source = "isr" };
    check(WS_OK(ws_bus_publish_from_isr(&bus, &isr_ev, NULL, 0u)), "publish_from_isr ok");
    check(ws_bus_receive(&q_obs, &rh, NULL, 0u, &rlen).code == WS_BUS_EMPTY,
          "ISR message hidden before dispatch");
    check(WS_OK(ws_bus_dispatch(&bus)), "dispatch promotes ISR message");
    check(ws_bus_dispatch(&bus).code == WS_BUS_EMPTY, "second dispatch reports empty");
    check(WS_OK(ws_bus_receive(&q_obs, &rh, NULL, 0u, &rlen)),
          "ISR message visible after dispatch");
    check(rlen == 0u && strcmp(rh.type, "SampleTick") == 0, "header-only ISR event intact");

    /* Verify DropNewest on q_cmd (depth 4, command-only). */
    for (uint32_t v = 10u; v <= 15u; v++) {
        check(WS_OK(ws_bus_publish(&bus, &cmd, &v, (uint16_t)sizeof(v))),
              "drop-newest publish succeeds silently");
    }
    for (uint32_t expected = 10u; expected <= 13u; expected++) {
        check(WS_OK(ws_bus_receive(&q_cmd, &rh, &rpay, (uint16_t)sizeof(rpay), &rlen)) &&
                  rpay == expected,
              "drop-newest preserved oldest 4 items");
    }
    check(ws_bus_receive(&q_cmd, &rh, &rpay, (uint16_t)sizeof(rpay), &rlen).code == WS_BUS_EMPTY,
          "drop-newest dropped items 14 and 15");

    /* Correlation rollover skips 0. */
    bus.next_correlation_id = UINT32_MAX;
    check(ws_bus_next_correlation(&bus) == 1u, "correlation wraps past 0 to 1");
}

int main(void)
{
    test_init_validation();
    test_per_queue_sizing_and_filters();
    test_commands_isr_and_correlation();

    printf("bus_test: %d checks, %d failures: %s\n", s_checks, s_failures,
           s_failures == 0 ? "PASS" : "FAIL");
    return s_failures == 0 ? 0 : 1;
}
