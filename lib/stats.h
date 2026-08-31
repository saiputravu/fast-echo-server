#pragma once
#include <cmath>
#include <cstdint>

// Statistics accumulates a running count/sum/sum-of-squares and derives the
// mean/variance/stddev from them. Used for rolling-window latency stats: call
// add() as samples arrive, snapshot the derived values, then reset() to start
// the next window.
struct Statistics {
  uint64_t count = 0;
  double sum = 0;
  double sumsq = 0;

  void add(double x) {
    ++count;
    sum += x;
    sumsq += x * x;
  }

  void reset() {
    count = 0;
    sum = 0;
    sumsq = 0;
  }

  double mean() const { return count ? sum / count : 0.0; }

  double variance() const {
    double m = mean();
    return count ? sumsq / count - m * m : 0.0;
  }

  double stddev() const { return std::sqrt(variance()); }
};

// MetricSnapshot is the small, plain value handed from each sender worker to the
// dashboard consumer via the shared SyncMap.
struct MetricSnapshot {
  double mean = 0;
  double stddev = 0;
  uint64_t count = 0;
};
