#pragma once

#include <stdint.h>

class SensorDebounce {
 public:
  explicit SensorDebounce(uint32_t debounceMs) : debounceMs_(debounceMs) {}

  void begin(bool initialState, uint32_t now) {
    raw_ = initialState;
    stable_ = initialState;
    rawChangedAt_ = now;
    bounceCount_ = 0;
    initialized_ = true;
  }

  bool update(bool sample, uint32_t now) {
    if (!initialized_) begin(sample, now);

    if (sample != raw_) {
      if (raw_ != stable_) ++bounceCount_;
      raw_ = sample;
      rawChangedAt_ = now;
    }

    if (raw_ != stable_ && static_cast<uint32_t>(now - rawChangedAt_) >= debounceMs_) {
      stable_ = raw_;
      return true;
    }
    return false;
  }

  bool raw() const { return raw_; }
  bool stable() const { return stable_; }
  uint32_t bounceCount() const { return bounceCount_; }

 private:
  const uint32_t debounceMs_;
  uint32_t rawChangedAt_ = 0;
  uint32_t bounceCount_ = 0;
  bool raw_ = false;
  bool stable_ = false;
  bool initialized_ = false;
};
