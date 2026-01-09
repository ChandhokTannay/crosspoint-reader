#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <array>

#include "../Activity.h"

class FileSelectionActivity final : public Activity {
  enum class Status { NORMAL, INDEXING, INDEX_DONE };

  static constexpr size_t THUMBNAIL_MAX_PATH = 128;
  static constexpr int THUMBNAIL_CACHE_SIZE = 6;
  static constexpr int THUMBNAIL_QUEUE_LENGTH = 16;  // allow enough slots for a full grid page

  struct ThumbnailCacheEntry {
    std::string path;
    uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    unsigned long lastUsedMs = 0;
  };

  struct ThumbnailRequest {
    char path[THUMBNAIL_MAX_PATH];
  };

  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t thumbnailTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  SemaphoreHandle_t thumbnailCacheMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  // For each entry in `files`, stores the number of EPUBs directly inside
  // that directory (for series), or 0 for regular book files.
  std::vector<int> seriesBookCounts;
  // Set of full EPUB paths that have been completed ("Read").
  std::unordered_set<std::string> completedBooks;
  bool completedLoaded = false;
  int selectorIndex = 0;
  bool updateRequired = false;
  Status status = Status::NORMAL;
  unsigned long statusStartMs = 0;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  // Small in-memory cache of embedded 2bpp thumbnails keyed by full EPUB path.
  mutable std::array<ThumbnailCacheEntry, THUMBNAIL_CACHE_SIZE> thumbnailCache{};
  QueueHandle_t thumbnailQueue = nullptr;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  static void thumbnailTaskTrampoline(void* param);
  [[noreturn]] void thumbnailTaskLoop();
  void render() const;
  void loadFiles();
  void loadCompletedBooks();
  bool isBookCompleted(const std::string& fullPath) const;
  bool isInBooksTree() const;
  void renderListView(int pageWidth, int pageHeight) const;
  void renderBooksGrid(int pageWidth, int pageHeight) const;

  bool getThumbnailFromCache(const std::string& fullPath, uint8_t** outData, uint16_t* outWidth,
                             uint16_t* outHeight) const;
  void enqueueThumbnailRequest(const std::string& fullPath) const;
  bool getOrLoadThumbnail(const std::string& fullPath, uint8_t** outData, size_t* outSize,
                          uint16_t* outWidth, uint16_t* outHeight) const;

 public:
  explicit FileSelectionActivity(GfxRenderer& renderer, InputManager& inputManager,
                                 const std::function<void(const std::string&)>& onSelect,
                                 const std::function<void()>& onGoHome)
      : Activity(renderer, inputManager), onSelect(onSelect), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
