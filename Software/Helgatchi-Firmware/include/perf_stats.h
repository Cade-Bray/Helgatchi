#pragma once
#include <stdint.h>
#include <Arduino.h>   // micros()

// ---------------------------------------------------------------------------
// Loop-phase timing
//
// Written by loop() in main.cpp — one micros() read per phase, which is
// negligible, so it runs unconditionally regardless of debug level. Read and
// reset once per second by LogService when the debug level is DEBUG_PERF.
//
// Each field holds the WORST (max) single-iteration duration for that phase in
// the current window. A lone slow tick is what the user perceives as a freeze
// (e.g. the device-list rebuild on EV_SCAN_COMPLETE, which runs inside
// g_bus.dispatch()), so the per-phase max is far more diagnostic than an
// average — an average of thousands of fast loops would bury the one bad one.
// ---------------------------------------------------------------------------

struct LoopPerf {
    uint32_t hal_us     = 0;
    uint32_t bus_us     = 0;   // g_bus.dispatch — device-list rebuild runs here
    uint32_t console_us = 0;
    uint32_t power_us   = 0;
    uint32_t scan_us    = 0;
    uint32_t rules_us   = 0;
    uint32_t leds_us    = 0;
    uint32_t vibe_us    = 0;
    uint32_t ui_us      = 0;   // lv_timer_handler — LVGL render
    uint32_t loop_us    = 0;   // worst full iteration
    uint32_t iterations = 0;   // loop passes this window

    void reset() { *this = LoopPerf{}; }
};

extern LoopPerf g_loop_perf;

// Time a single loop phase: run `call`, fold its duration into `field`'s max,
// and advance the running timestamp `_t`. A uint32_t named `_t` must be in
// scope, seeded from micros() before the first phase.
#define PERF_TIME(field, call)                                   \
    do {                                                         \
        call;                                                    \
        uint32_t _now = micros();                                \
        uint32_t _dt  = _now - _t;                               \
        if (_dt > g_loop_perf.field) g_loop_perf.field = _dt;    \
        _t = _now;                                               \
    } while (0)
