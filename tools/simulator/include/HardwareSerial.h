#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <chrono>

inline uint32_t millis() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

class HardwareSerial {
 public:
  template <typename... Args>
  int printf(const char* fmt, Args... args) {
    return std::fprintf(stderr, fmt, args...);
  }
};

inline HardwareSerial Serial;
