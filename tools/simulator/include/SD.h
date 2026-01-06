#pragma once

#include <filesystem>

class SDClass {
 public:
  bool exists(const char* path) const { return std::filesystem::exists(path); }
  bool mkdir(const char* path) const {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
  }
  bool remove(const char* path) const {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
  }
  bool rmdir(const char* path) const {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
  }
};

inline SDClass SD;
