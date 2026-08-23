#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <array>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 private:
  static constexpr size_t THUMBNAIL_MAX_PATH = 128;
  // Allow more distinct thumbnails to be cached in memory. This is
  // bounded to avoid exhausting heap when many large covers exist.
  static constexpr int THUMBNAIL_CACHE_SIZE = 10;
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

  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  // For each entry in `files`, the number of EPUBs directly inside that
  // directory (for series), or 0 for regular book files.
  std::vector<int> seriesBookCounts;
  // Set of full EPUB paths that have been completed ("Read").
  std::unordered_set<std::string> completedBooks;
  bool completedLoaded = false;

  // In-memory cache of embedded 2bpp thumbnails keyed by full EPUB path,
  // filled by a background worker task so the UI stays responsive.
  mutable std::array<ThumbnailCacheEntry, THUMBNAIL_CACHE_SIZE> thumbnailCache{};
  SemaphoreHandle_t thumbnailCacheMutex = nullptr;
  QueueHandle_t thumbnailQueue = nullptr;
  TaskHandle_t thumbnailTaskHandle = nullptr;
  // Number of thumbnail requests that have been enqueued but not yet processed.
  mutable volatile int pendingThumbnailRequests = 0;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;
  void loadCompletedBooks();
  bool isBookCompleted(const std::string& fullPath) const;
  bool isInBooksTree() const;

  // Cover-art grid (library view inside /Books)
  void renderBooksGrid(int pageWidth, int pageHeight);

  static void thumbnailTaskTrampoline(void* param);
  [[noreturn]] void thumbnailTaskLoop();
  bool getThumbnailFromCache(const std::string& fullPath, uint8_t** outData, uint16_t* outWidth,
                             uint16_t* outHeight) const;
  void enqueueThumbnailRequest(const std::string& fullPath) const;
  bool getOrLoadThumbnail(const std::string& fullPath, uint8_t** outData, size_t* outSize, uint16_t* outWidth,
                          uint16_t* outHeight) const;
  void clearThumbnailCache();

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
