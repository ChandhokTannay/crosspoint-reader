#pragma once

#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Small action menu shown when long-pressing a book in the file browser.
 * Returns MenuResult{action}: REPROCESS regenerates the book's cached
 * metadata and cover thumbnails; DELETE proceeds to delete confirmation.
 */
class BookActionsActivity final : public Activity {
 public:
  enum Action { REPROCESS = 0, DELETE = 1 };

 private:
  ButtonNavigator buttonNavigator;
  const std::string bookName;
  int selectedIndex = 0;

 public:
  explicit BookActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookName)
      : Activity("BookActions", renderer, mappedInput), bookName(std::move(bookName)) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
