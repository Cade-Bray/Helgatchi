
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Events.h"

class EventBus {
public:
  explicit EventBus(size_t capacity) : cap_(capacity) {
    if (cap_ == 0) cap_ = 1;
    buf_ = new Event[cap_];
  }

  ~EventBus() {
    delete[] buf_;
    buf_ = nullptr;
    cap_ = 0;
  }

  bool push(const Event& e) {
    const size_t nextHead = (head_ + 1) % cap_;
    if (nextHead == tail_) {
      dropped_++;
      return false;
    }
    buf_[head_] = e;
    head_ = nextHead;
    return true;
  }

  bool pop(Event& out) {
    if (tail_ == head_) return false;
    out = buf_[tail_];
    tail_ = (tail_ + 1) % cap_;
    return true;
  }

  size_t droppedCount() const { return dropped_; }

private:
  Event* buf_ = nullptr;
  size_t cap_ = 0;

  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;

  size_t dropped_ = 0;
};