#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

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
  const std::function<void()> onFileTransferOpen;
  const std::string currentEpubName;

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
