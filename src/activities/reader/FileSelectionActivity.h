#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../Activity.h"

class FileSelectionActivity final : public Activity {
  enum class Status { NORMAL, INDEXING, INDEX_DONE };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
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

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();
  void loadCompletedBooks();
  bool isBookCompleted(const std::string& fullPath) const;
  bool isInBooksTree() const;
  void renderListView(int pageWidth, int pageHeight) const;
  void renderBooksGrid(int pageWidth, int pageHeight) const;

 public:
  explicit FileSelectionActivity(GfxRenderer& renderer, InputManager& inputManager,
                                 const std::function<void(const std::string&)>& onSelect,
                                 const std::function<void()>& onGoHome)
      : Activity(renderer, inputManager), onSelect(onSelect), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
