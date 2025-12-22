#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <SD.h>

#include <vector>
#include <cctype>

#include "config.h"

namespace {
// 0 = book card (Continue Reading)
// 1 = Browse files
// 2 = File transfer
// 3 = Settings
constexpr int menuItemCount = 4;
}

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

void HomeActivity::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void HomeActivity::loop() {
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (selectorIndex == 0) {
      // Continue reading current book (or open reader home if none)
      onContinueReadingOpen();
    } else if (selectorIndex == 1) {
      // Explicitly browse files in the reader
      onBrowseFilesOpen();
    } else if (selectorIndex == 2) {
      onFileTransferOpen();
    } else if (selectorIndex == 3) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuItemCount - 1) % menuItemCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuItemCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  // Header
  renderer.drawCenteredText(READER_FONT_ID, 10, "Tannay's Reader", true, BOLD);

  const int margin = 20;
  const int footerHeight = 60;  // space reserved for the button legend at the bottom

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  const int bookWidth = pageWidth / 2;
  const int bookHeight = pageHeight / 2;
  const int bookX = (pageWidth - bookWidth) / 2;
  const int bookY = 50;
  const bool bookSelected = (selectorIndex == 0);

  if (!currentEpubName.empty()) {
    if (bookSelected) {
      renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
    } else {
      renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
    }

    // Book title centered inside the card, wrapped to multiple lines
    std::string title = currentEpubName;
    // Convert to uppercase for display
    for (char& c : title) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // Split into words (avoid stringstream to keep this light on the MCU)
    std::vector<std::string> words;
    words.reserve(8);
    size_t pos = 0;
    while (pos < title.size()) {
      while (pos < title.size() && title[pos] == ' ') {
        ++pos;
      }
      if (pos >= title.size()) {
        break;
      }
      size_t start = pos;
      while (pos < title.size() && title[pos] != ' ') {
        ++pos;
      }
      words.emplace_back(title.substr(start, pos - start));
    }

    std::vector<std::string> lines;
    std::string currentLine;
    // Extra padding inside the card so text doesn't hug the border
    const int maxLineWidth = bookWidth - 40;

    for (size_t i = 0; i < words.size(); ++i) {
      const std::string candidate = currentLine.empty() ? words[i] : currentLine + " " + words[i];
      if (renderer.getTextWidth(READER_FONT_ID, candidate.c_str(), BOLD) <= maxLineWidth || currentLine.empty()) {
        currentLine = candidate;
      } else {
        lines.push_back(currentLine);
        currentLine = words[i];
      }
    }
    if (!currentLine.empty()) {
      lines.push_back(currentLine);
    }

    // Limit the number of lines to avoid overflowing the card
    const int maxLines = 3;
    if (static_cast<int>(lines.size()) > maxLines) {
      std::string lastLine;
      for (size_t i = 2; i < lines.size(); ++i) {
        if (!lastLine.empty()) lastLine += " ";
        lastLine += lines[i];
      }
      lastLine += "...";
      // Ensure the last line with ellipsis fits
      while (!lastLine.empty() && renderer.getTextWidth(READER_FONT_ID, lastLine.c_str(), BOLD) > maxLineWidth) {
        lastLine.pop_back();
      }
      lines.resize(2);
      if (!lastLine.empty()) {
        lines.push_back(lastLine);
      }
    }

    const int lineHeight = renderer.getLineHeight(READER_FONT_ID);
    const int totalTextHeight = lineHeight * static_cast<int>(lines.size());

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;
    if (titleYStart < bookY + 4) {
      titleYStart = bookY + 4;
    }

    for (const auto& line : lines) {
      const int lineWidth = renderer.getTextWidth(READER_FONT_ID, line.c_str(), BOLD);
      const int lineX = bookX + (bookWidth - lineWidth) / 2;
      renderer.drawText(READER_FONT_ID, lineX, titleYStart, line.c_str(), !bookSelected, BOLD);
      titleYStart += lineHeight;
    }

    // Bookmark icon in the top-right corner of the card
    const bool iconColor = !bookSelected;  // match inverted text color
    const int bookmarkWidth = bookWidth / 8;           // slightly wider
    const int bookmarkHeight = lineHeight * 3;          // taller for stronger visual
    const int bookmarkX = bookX + bookWidth - bookmarkWidth - 8;
    const int bookmarkY = bookY + 1;                    // touch inner top border of the card

    // Main bookmark body (solid)
    renderer.fillRect(bookmarkX, bookmarkY, bookmarkWidth, bookmarkHeight, iconColor);

    // Carve out an inverted triangle notch at the bottom center to create angled points
    const int notchHeight = bookmarkHeight / 2;  // depth of the notch
    for (int i = 0; i < notchHeight; ++i) {
      const int y = bookmarkY + bookmarkHeight - 1 - i;
      const int xStart = bookmarkX + i;
      const int width = bookmarkWidth - 2 * i;
      if (width <= 0) {
        break;
      }
      // Draw a horizontal strip in the opposite color to "cut" the notch
      renderer.fillRect(xStart, y, width, 1, !iconColor);
    }
  }

  // --- Bottom menu tiles (indices 1-3) ---
  const int menuTileWidth = pageWidth - 2 * margin;
  const int menuTileHeight = 50;
  const int menuSpacing = 10;
  const int totalMenuHeight = 3 * menuTileHeight + 2 * menuSpacing;

  // Primary placement: sit fairly close under the book card
  int menuStartY = bookY + bookHeight + 20;
  // Ensure we don't collide with the bottom button legend
  const int maxMenuStartY = pageHeight - footerHeight - totalMenuHeight - margin;
  if (menuStartY > maxMenuStartY) {
    menuStartY = maxMenuStartY;
  }

  const char* const labels[3] = {"Browse files", "File transfer", "Settings"};

  for (int i = 0; i < 3; ++i) {
    const int overallIndex = i + 1;  // map to selectorIndex values 1..3
    const int tileX = margin;
    const int tileY = menuStartY + i * (menuTileHeight + menuSpacing);
    const bool selected = (selectorIndex == overallIndex);

    if (selected) {
      renderer.fillRect(tileX, tileY, menuTileWidth, menuTileHeight);
    } else {
      renderer.drawRect(tileX, tileY, menuTileWidth, menuTileHeight);
    }

    const char* label = labels[i];
    const int textWidth = renderer.getTextWidth(UI_FONT_ID, label);
    const int textX = tileX + (menuTileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_FONT_ID);
    const int textY = tileY + (menuTileHeight - lineHeight) / 2;  // vertically centered assuming y is top of text

    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_FONT_ID, textX, textY, label, !selected);
  }

  // Button legend at the bottom
  renderer.drawRect(25, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 25 + (105 - renderer.getTextWidth(UI_FONT_ID, "Back")) / 2, pageHeight - 35, "Back");

  renderer.drawRect(130, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 130 + (105 - renderer.getTextWidth(UI_FONT_ID, "Confirm")) / 2, pageHeight - 35,
                    "Confirm");

  renderer.drawRect(245, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 245 + (105 - renderer.getTextWidth(UI_FONT_ID, "Left")) / 2, pageHeight - 35, "Left");

  renderer.drawRect(350, pageHeight - 40, 106, 40);
  renderer.drawText(UI_FONT_ID, 350 + (105 - renderer.getTextWidth(UI_FONT_ID, "Right")) / 2, pageHeight - 35, "Right");

  renderer.displayBuffer();
}
