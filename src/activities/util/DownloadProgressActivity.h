#pragma once

#include <EInkDisplay.h>
#include <string>

#include "../Activity.h"

// Simple full-screen message with a horizontal progress bar.
// Used for the "Fetch New Books" flow while downloading books
// from the host server.
class DownloadProgressActivity final : public Activity {
  std::string text;   // e.g., "Downloading foo.epub (1/3)"
  float progress;     // 0.0 - 1.0

 public:
  explicit DownloadProgressActivity(GfxRenderer& renderer, InputManager& inputManager,
                                    std::string text, float progress)
      : Activity(renderer, inputManager), text(std::move(text)), progress(progress) {}

  void onEnter() override;
};
