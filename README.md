# ws-bus-embedded-c

Static-allocation pub/sub message bus for Embedded C (`base_tech_spec.md` §5, `embedded_system_spec.md` §5, `embedded_c_tech_spec.md` §6/§9).

## Purpose

- **Per-queue static storage** via `WS_BUS_QUEUE_STORAGE(name, depth, payload_size)` — each system sizes its own ring depth, payload capacity, overflow policy, and multi-type filter list without global payload bloat.
- **Single-handler commands** via `ws_bus_register_command()` — duplicate handlers fail loudly with `WS_BUS_DUPLICATE_HANDLER`; directed commands are delivered exclusively to their registered handler.
- **ISR-safe deferred publishing** via `ws_bus_publish_from_isr()` + `ws_bus_dispatch()`.
- **64-bit monotonic timestamps** via injected `uint64_t (*now_us)(void)` (matches `ws_time_t`, zero 32-bit rollover).
- **Typed error values** via `WS_ERROR_TYPE(ws_bus_err_t, ws_bus_error_t)` from `<ws/error/error.h>`.

## Quick start

See [examples/basic_bus.c](examples/basic_bus.c).

## API reference

Public header: `<ws/bus/bus.h>` (guard `WS_WS_BUS_BUS_H`).

| Function / Macro | Summary |
|---|---|
| `WS_BUS_QUEUE_STORAGE(name, depth, payload)` | Declares exact-sized static backing arrays for one queue |
| `ws_bus_init(bus, cfg)` | Initialises bus with `max_subscribers` and `uint64_t (*now_us)(void)` |
| `ws_bus_subscribe(bus, qcfg, out_queue)` | Attaches a queue with its own depth, payload cap, overflow policy, and filters |
| `ws_bus_register_command(bus, cmd, queue)` | Registers an exclusive single command handler (rejects duplicates) |
| `ws_bus_publish(bus, hdr, payload, len)` | Publishes event or directed command from thread context |
| `ws_bus_publish_from_isr(bus, hdr, payload, len)` | Enqueues deferred message from ISR (never runs handlers) |
| `ws_bus_dispatch(bus)` | Promotes ISR-deferred messages from thread context |
| `ws_bus_receive(queue, out_hdr, out_buf, cap, out_len)` | Pops oldest visible message in strict FIFO order |
| `ws_bus_next_correlation(bus)` | Allocates next non-zero correlation ID (critical-section guarded) |
| `ws_bus_correlation_matches(resp, req_id)` | True iff both IDs match and are non-zero |

## Config reference

Compile-time maxima (override with `-D`): `WS_BUS_MAX_SUBSCRIBERS` (default 16), `WS_BUS_MAX_COMMANDS` (default 32). Optional critical-section macros: `WS_BUS_ENTER_CRITICAL()` / `WS_BUS_EXIT_CRITICAL()`.

## Build & test (standalone)

```sh
cmake -S . -B build -G Ninja -DWS_ERROR_DIR=/path/to/ws-error-embedded-c
cmake --build build && ctest --test-dir build --output-on-failure
```

## Quirks

- `type` and `source` strings must reside in static storage (string literals) and outlive the bus.
- Define `WS_BUS_ENTER_CRITICAL()`/`WS_BUS_EXIT_CRITICAL()` when an ISR and tasks share a queue or call `ws_bus_next_correlation()` concurrently.
- A command-only queue passes a non-NULL `filters` pointer with `filter_count = 0`; a wildcard event queue passes `filters = NULL` and `filter_count = 0`.
