#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <cstdint>

#include "../Activity.h"

class HomeActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onContinueReadingOpen;
  const std::function<void()> onBrowseFilesOpen;
  const std::function<void()> onSyncProgress;
  const std::function<void()> onSettingsOpen;
  // Reused for the "Fetch New Books" action on the home menu.
  const std::function<void()> onFileTransferOpen;
  const std::string currentEpubName;

  // Cached 2bpp thumbnail for the currently open book (if available).
  uint8_t* currentThumbData = nullptr;  // buffer returned by Epub::readItemContentsToBytes
  uint16_t currentThumbWidth = 0;
  uint16_t currentThumbHeight = 0;
  bool hasCurrentThumb = false;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, InputManager& inputManager,
                        const std::function<void()>& onContinueReadingOpen,
                        const std::function<void()>& onBrowseFilesOpen,
                        const std::function<void()>& onSyncProgress,
                        const std::function<void()>& onSettingsOpen,
                        const std::function<void()>& onFileTransferOpen,
                        const std::string& currentEpubName)
      : Activity(renderer, inputManager),
        onContinueReadingOpen(onContinueReadingOpen),
        onBrowseFilesOpen(onBrowseFilesOpen),
        onSyncProgress(onSyncProgress),
        onSettingsOpen(onSettingsOpen),
        onFileTransferOpen(onFileTransferOpen),
        currentEpubName(currentEpubName) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
