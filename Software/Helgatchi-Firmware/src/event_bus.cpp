#include "event_bus.h"
#include <Arduino.h>          // micros()
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

EventBus g_bus;

void EventBus::begin() {
    _mutex = xSemaphoreCreateMutex();
}

bool EventBus::subscribe(EventId id, IEventHandler* handler) {
    if (_sub_count >= MAX_SUBSCRIPTIONS) return false;
    _subs[_sub_count++] = { id, handler };
    return true;
}

bool EventBus::subscribeAll(IEventHandler* handler) {
    return subscribe(EVENT_ID_INVALID, handler);
}

bool EventBus::post(EventId id) {
    EventPayload empty{};
    return post(id, empty);
}

bool EventBus::post(EventId id, const EventPayload& payload) {
    auto mx = static_cast<SemaphoreHandle_t>(_mutex);
    if (xSemaphoreTake(mx, portMAX_DELAY) != pdTRUE) return false;

    bool ok = false;
    if (_count < QUEUE_DEPTH) {
        Event& slot = _queue[_tail];
        slot.id   = id;
        slot.data = payload;
        _tail = (_tail + 1) % QUEUE_DEPTH;
        _count++;
        ok = true;
    } else {
        _dropped++;
    }

    xSemaphoreGive(mx);
    return ok;
}

void EventBus::dispatch() {
    uint32_t start_us = micros();

    // Snapshot how many events to drain so handler-posted events
    // are deferred to the next dispatch() call (bounded execution).
    auto mx = static_cast<SemaphoreHandle_t>(_mutex);
    xSemaphoreTake(mx, portMAX_DELAY);
    uint8_t to_drain = _count;
    xSemaphoreGive(mx);

    uint32_t handled = 0;
    while (to_drain-- > 0) {
        Event e;

        xSemaphoreTake(mx, portMAX_DELAY);
        if (_count == 0) { xSemaphoreGive(mx); break; }
        e = _queue[_head];
        _head = (_head + 1) % QUEUE_DEPTH;
        _count--;
        xSemaphoreGive(mx);

        // Dispatch without holding lock so handlers can post new events.
        for (uint8_t i = 0; i < _sub_count; i++) {
            if (_subs[i].id == e.id || _subs[i].id == EVENT_ID_INVALID) {
                _subs[i].handler->onEvent(e);
            }
        }
        handled++;
    }

    uint32_t elapsed = micros() - start_us;
    _perf_dispatches++;
    _perf_events   += handled;
    _total_events  += handled;
    _perf_total_us += elapsed;
    if (elapsed > _perf_max_us) _perf_max_us = elapsed;
}

EventBus::PerfStats EventBus::perfSnapshotAndReset() {
    auto mx = static_cast<SemaphoreHandle_t>(_mutex);
    xSemaphoreTake(mx, portMAX_DELAY);
    PerfStats s = { _perf_events, _perf_dispatches, _perf_total_us, _perf_max_us };
    _perf_events     = 0;
    _perf_dispatches = 0;
    _perf_total_us   = 0;
    _perf_max_us     = 0;
    xSemaphoreGive(mx);
    return s;
}
