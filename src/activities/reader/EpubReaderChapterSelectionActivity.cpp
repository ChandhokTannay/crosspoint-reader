#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <SD.h>

#include "config.h"

namespace {
constexpr int ITEM_HEIGHT_PX = 30;
constexpr int LIST_TOP_Y = 60;
constexpr int LIST_BOTTOM_PADDING_PX = 20;
constexpr int SKIP_PAGE_MS = 700;

int getPageItems(const GfxRenderer& renderer) {
  const int available = renderer.getScreenHeight() - LIST_TOP_Y - LIST_BOTTOM_PADDING_PX;
  if (available <= ITEM_HEIGHT_PX) {
    return 1;
  }
  return available / ITEM_HEIGHT_PX;
}
}  // namespace

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = currentSpineIndex;

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionActivity::taskTrampoline, "EpubReaderChapterSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderChapterSelectionActivity::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderChapterSelectionActivity::loop() {
  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems(renderer);

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    onSelectSpineIndex(selectorIndex);
  } else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex =
          ((selectorIndex / pageItems - 1) * pageItems + epub->getSpineItemsCount()) % epub->getSpineItemsCount();
    } else {
      selectorIndex = (selectorIndex + epub->getSpineItemsCount() - 1) % epub->getSpineItemsCount();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % epub->getSpineItemsCount();
    } else {
      selectorIndex = (selectorIndex + 1) % epub->getSpineItemsCount();
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(READER_FONT_ID, 10, "Select Chapter", true, BOLD);

  const int pageItems = getPageItems(renderer);
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, LIST_TOP_Y + (selectorIndex % pageItems) * ITEM_HEIGHT_PX + 2, pageWidth - 1, ITEM_HEIGHT_PX);
  for (int i = pageStartIndex; i < epub->getSpineItemsCount() && i < pageStartIndex + pageItems; i++) {
    const int tocIndex = epub->getTocIndexForSpineIndex(i);
    if (tocIndex == -1) {
      renderer.drawText(UI_FONT_ID, 20, LIST_TOP_Y + (i % pageItems) * ITEM_HEIGHT_PX, "Unnamed", i != selectorIndex);
    } else {
      auto item = epub->getTocItem(tocIndex);
      renderer.drawText(UI_FONT_ID, 20 + (item.level - 1) * 15, LIST_TOP_Y + (i % pageItems) * ITEM_HEIGHT_PX,
                        item.title.c_str(), i != selectorIndex);
    }
  }

  renderer.displayBuffer();
}
