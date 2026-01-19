#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SD.h>
#include <Epub.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "config.h"

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

namespace {
constexpr unsigned long REINDEX_HOLD_MS = 1000;  // ms to hold OK to re-index

// Count how many EPUB files are directly inside a given series directory.
// dirEntry is a single path component relative to basepath and typically
// ends with a trailing '/'.
int countBooksInSeries(const std::string& basepath, const std::string& dirEntry) {
  std::string dirName = dirEntry;
  if (!dirName.empty() && dirName.back() == '/') {
    dirName.pop_back();
  }

  // Build absolute path: basepath (with trailing '/') + dirName
  std::string path = basepath;
  if (path.empty()) {
    path = "/";
  }
  if (path.back() != '/') {
    path += '/';
  }
  path += dirName;

  File root = SD.open(path.c_str());
  if (!root) {
    return 0;
  }

  int count = 0;
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    auto filename = std::string(file.name());
    if (!filename.empty() && filename[0] == '.') {
      file.close();
      continue;
    }

    if (!file.isDirectory()) {
      if (filename.size() >= 5 && filename.substr(filename.size() - 5) == ".epub") {
        ++count;
      }
    }
    file.close();
  }
  root.close();
  return count;
}
}

void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

void FileSelectionActivity::loadFiles() {
  files.clear();
  seriesBookCounts.clear();
  selectorIndex = 0;

  // Ensure we have the latest completed-books set loaded.
  loadCompletedBooks();

  const bool inBooksTree = isInBooksTree();

  // Collect raw entries (name + series book count) before sorting so that
  // counts remain aligned with their corresponding files after sorting.
  std::vector<std::pair<std::string, int>> entries;

  auto root = SD.open(basepath.c_str());
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    auto filename = std::string(file.name());
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    const bool isDir = file.isDirectory();
    const bool isEpub = !isDir && filename.size() >= 5 && filename.substr(filename.size() - 5) == ".epub";

    // Inside the Books tree, only show directories and EPUB files.
    // Outside /Books, show all files and directories.
    if (inBooksTree) {
      if (!isDir && !isEpub) {
        file.close();
        continue;
      }
    }

    std::string entryName = isDir ? filename + "/" : filename;
    int bookCount = 0;

    // Only compute series book counts when we're inside the Books tree
    // and this entry is a directory.
    if (isDir && inBooksTree) {
      bookCount = countBooksInSeries(basepath, entryName);
    }

    entries.emplace_back(std::move(entryName), bookCount);
    file.close();
  }
  root.close();

  // Sort entries by their display name, with directories first, case-insensitive,
  // mirroring the original sortFileList behavior.
  std::sort(entries.begin(), entries.end(),
            [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
              const std::string& str1 = a.first;
              const std::string& str2 = b.first;
              const bool dir1 = !str1.empty() && str1.back() == '/';
              const bool dir2 = !str2.empty() && str2.back() == '/';
              if (dir1 && !dir2) return true;
              if (!dir1 && dir2) return false;
              return lexicographical_compare(
                  begin(str1), end(str1), begin(str2), end(str2),
                  [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
            });

  // Populate the parallel vectors used elsewhere in the activity.
  for (const auto& entry : entries) {
    files.push_back(entry.first);
    seriesBookCounts.push_back(entry.second);
  }
}

void FileSelectionActivity::loadCompletedBooks() {
  if (completedLoaded) {
    return;
  }
  completedLoaded = true;
  completedBooks.clear();

  std::ifstream in("/sd/.crosspoint/completed_books.txt");
  if (!in.good()) {
    return;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      completedBooks.insert(line);
    }
  }
}

bool FileSelectionActivity::isBookCompleted(const std::string& fullPath) const {
  if (completedBooks.empty()) {
    return false;
  }
  return completedBooks.find(fullPath) != completedBooks.end();
}

bool FileSelectionActivity::isInBooksTree() const {
  const std::string booksRoot = "/Books";
  if (basepath.size() < booksRoot.size()) {
    return false;
  }
  if (basepath.compare(0, booksRoot.size(), booksRoot) != 0) {
    return false;
  }
  if (basepath.size() == booksRoot.size()) {
    return true;  // exactly "/Books"
  }
  // Ensure we only match "/Books" or "/Books/..." and not paths like "/Bookstore"
  return basepath[booksRoot.size()] == '/';
}

void FileSelectionActivity::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();
  thumbnailCacheMutex = xSemaphoreCreateMutex();

  // Start browsing directly in the Books directory
  basepath = "/Books";
  loadFiles();
  selectorIndex = 0;
  status = Status::NORMAL;

  // Reset thumbnail loading state for the new library view.
  pendingThumbnailRequests = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Create thumbnail request queue and worker task for async thumbnail loading.
  thumbnailQueue = xQueueCreate(THUMBNAIL_QUEUE_LENGTH, sizeof(ThumbnailRequest));
  if (thumbnailQueue != nullptr) {
    xTaskCreate(&FileSelectionActivity::thumbnailTaskTrampoline, "ThumbnailLoaderTask",
                8192,              // Stack size (increased to avoid stack overflow during EPUB parsing)
                this,              // Parameters
                1,                 // Priority
                &thumbnailTaskHandle);
  }
}

void FileSelectionActivity::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  if (thumbnailTaskHandle) {
    vTaskDelete(thumbnailTaskHandle);
    thumbnailTaskHandle = nullptr;
  }
  if (thumbnailQueue) {
    vQueueDelete(thumbnailQueue);
    thumbnailQueue = nullptr;
  }
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
  if (thumbnailCacheMutex) {
    vSemaphoreDelete(thumbnailCacheMutex);
    thumbnailCacheMutex = nullptr;
  }
  files.clear();
  seriesBookCounts.clear();
  pendingThumbnailRequests = 0;

  // Free any cached thumbnails.
  for (auto& entry : thumbnailCache) {
    if (entry.data) {
      free(entry.data);
      entry.data = nullptr;
      entry.size = 0;
      entry.width = 0;
      entry.height = 0;
      entry.lastUsedMs = 0;
      entry.path.clear();
    }
  }
}

void FileSelectionActivity::loop() {
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

  // When showing a status message, automatically return to normal after a short delay
  if (status == Status::INDEX_DONE) {
    if (millis() - statusStartMs > 1200) {
      status = Status::NORMAL;
      updateRequired = true;
    }
    return;
  }

  // While thumbnails are still loading for the current page, ignore all navigation
  // input so we don't leave the library view in a half-loaded state.
  if (pendingThumbnailRequests > 0) {
    return;
  }

  // Short-press OK: open; long-press OK: re-index selected EPUB
  if (inputManager.wasReleased(InputManager::BTN_CONFIRM)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    const auto& selected = files[selectorIndex];
    const bool isDirectory = !selected.empty() && selected.back() == '/';

    if (inputManager.getHeldTime() > REINDEX_HOLD_MS && !isDirectory) {
      // Re-index: clear cached data for this EPUB
      const std::string fullPath = basepath + selected;

      status = Status::INDEXING;
      statusStartMs = millis();
      updateRequired = true;  // trigger "Indexing..." screen

      Epub epub(fullPath, "/.crosspoint");
      epub.clearCache();

      status = Status::INDEX_DONE;
      statusStartMs = millis();
      updateRequired = true;  // trigger "Indexing complete" screen
      return;
    }

    // Normal open behavior
    if (isDirectory) {
      basepath += selected.substr(0, selected.length() - 1);
      loadFiles();
      updateRequired = true;
    } else {
      onSelect(basepath + selected);
    }
  } else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    // Treat "/Books" as the root for library browsing so we don't expose
    // other top-level filesystem entries in the file picker.
    if (basepath == "/Books") {
      onGoHome();
    } else if (basepath != "/") {
      basepath = basepath.substr(0, basepath.rfind('/'));
      if (basepath.empty()) basepath = "/";
      loadFiles();
      updateRequired = true;
    } else {
      // At root level, go back home
      onGoHome();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % files.size();
    updateRequired = true;
  }
}

void FileSelectionActivity::displayTaskLoop() {
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

void FileSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();
  renderer.drawCenteredText(READER_FONT_ID, 10, "Tannay's Reader", true, BOLD);

  // Status overlays for re-indexing
  if (status == Status::INDEXING) {
    renderer.drawCenteredText(READER_FONT_ID, pageHeight / 2 - 10, "Indexing...", true, BOLD);
    renderer.displayBuffer();
    return;
  }
  if (status == Status::INDEX_DONE) {
    renderer.drawCenteredText(READER_FONT_ID, pageHeight / 2 - 10, "Indexing complete", true, BOLD);
    renderer.displayBuffer();
    return;
  }

  // Help text (common to list and grid views)
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 30,
                    "BACK: Home   |   HOLD OK: Re-index");

  if (files.empty()) {
    renderer.drawText(UI_FONT_ID, 20, 60, isInBooksTree() ? "No books found" : "No EPUBs found");
  } else {
    if (isInBooksTree()) {
      renderBooksGrid(pageWidth, pageHeight);
    } else {
      renderListView(pageWidth, pageHeight);
    }
  }

  renderer.displayBuffer();
}

void FileSelectionActivity::renderListView(int pageWidth, int /*pageHeight*/) const {
  // Preserve original linear list behavior outside the Books tree.
  // Draw selection highlight behind the currently selected row.
  renderer.fillRect(0, 60 + selectorIndex * 30 + 2, pageWidth - 1, 30);

  for (size_t i = 0; i < files.size(); i++) {
    const auto& file = files[i];
    renderer.drawText(UI_FONT_ID, 20, 60 + static_cast<int>(i) * 30, file.c_str(),
                      static_cast<int>(i) != selectorIndex);
  }
}

void FileSelectionActivity::thumbnailTaskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->thumbnailTaskLoop();
}

[[noreturn]] void FileSelectionActivity::thumbnailTaskLoop() {
  while (true) {
    if (!thumbnailQueue) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    ThumbnailRequest req{};
    if (xQueueReceive(thumbnailQueue, &req, portMAX_DELAY) == pdTRUE) {
      std::string fullPath(req.path);
      uint8_t* data = nullptr;
      size_t size = 0;
      uint16_t w = 0;
      uint16_t h = 0;
      // Warm the cache; if a thumbnail was loaded, trigger a re-render so the
      // newly available image can be drawn without requiring user navigation.
      if (getOrLoadThumbnail(fullPath, &data, &size, &w, &h)) {
        updateRequired = true;
      }
      if (pendingThumbnailRequests > 0) {
        --pendingThumbnailRequests;
      }
    }
  }
}

void FileSelectionActivity::renderBooksGrid(int pageWidth, int pageHeight) const {
  if (files.empty()) {
    return;
  }

  constexpr int columns = 3;
  constexpr int rows = 3;
  constexpr int itemsPerPage = columns * rows;  // 9 items per page

  const int totalItems = static_cast<int>(files.size());
  const int clampedSelector = selectorIndex < 0 ? 0 : (selectorIndex >= totalItems ? totalItems - 1 : selectorIndex);
  const int currentPage = clampedSelector / itemsPerPage;
  const int pageStartIndex = currentPage * itemsPerPage;
  const int pageEndIndex = std::min(pageStartIndex + itemsPerPage, totalItems);

  // Layout metrics
  const int leftMargin = 20;
  const int rightMargin = 20;
  const int topMargin = 60;              // below header
  const int bottomMargin = 50;           // above help text at bottom
  const int availableWidth = pageWidth - leftMargin - rightMargin;
  const int availableHeight = pageHeight - topMargin - bottomMargin;

  const int hSpacing = 10;
  const int vSpacing = 10;
  const int cellWidth = (availableWidth - (columns - 1) * hSpacing) / columns;
  const int cellHeight = (availableHeight - (rows - 1) * vSpacing) / rows;

  // Optional page indicator (e.g., 1/3) in the top-right
  if (totalItems > itemsPerPage) {
    const int totalPages = (totalItems + itemsPerPage - 1) / itemsPerPage;
    const std::string pageLabel = std::to_string(currentPage + 1) + "/" + std::to_string(totalPages);
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, pageLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, pageWidth - rightMargin - labelWidth, topMargin - 18, pageLabel.c_str());
  }

  for (int idx = pageStartIndex; idx < pageEndIndex; ++idx) {
    const int localIndex = idx - pageStartIndex;
    const int row = localIndex / columns;
    const int col = localIndex % columns;

    const int x = leftMargin + col * (cellWidth + hSpacing);
    const int y = topMargin + row * (cellHeight + vSpacing);

    const bool selected = (idx == clampedSelector);

    // Determine if this entry is a completed book (for EPUB files only).
    bool completed = false;
    const bool isDirectoryEntry = !files[idx].empty() && files[idx].back() == '/';

    std::string fullPath;
    if (!isDirectoryEntry) {
      fullPath = basepath;
      if (!fullPath.empty() && fullPath.back() != '/') {
        fullPath += '/';
      }
      fullPath += files[idx];
      completed = isBookCompleted(fullPath);

      // Proactively enqueue thumbnail loads for all visible EPUBs on the
      // current page. enqueueThumbnailRequest() will no-op if the thumbnail
      // is already cached.
      enqueueThumbnailRequest(fullPath);
    }

    // Try to retrieve a cached Crosspoint 2bpp thumbnail for the book.
    uint8_t* thumbBuffer = nullptr;
    uint16_t thumbWidth = 0;
    uint16_t thumbHeight = 0;
    bool hasThumbnail = false;

    if (!isDirectoryEntry) {
      hasThumbnail = getThumbnailFromCache(fullPath, &thumbBuffer, &thumbWidth, &thumbHeight);
    }

    if (selected) {
      renderer.fillRect(x, y, cellWidth, cellHeight);
    } else {
      renderer.drawRect(x, y, cellWidth, cellHeight);
    }

    // If we have a thumbnail for the selected book, render it in the
    // upper part of the card before drawing any text.
    const int paddingX = 8;
    const int paddingY = 8;
    if (hasThumbnail && thumbBuffer) {
      const uint8_t* pixels = thumbBuffer + 4;
      const int coverX = x + (cellWidth - static_cast<int>(thumbWidth)) / 2;
      const int coverY = y + paddingY;
      // Invert the thumbnail when the card is selected so it stays visible on
      // the dark highlight background.
      renderer.draw2bppImage(pixels, coverX, coverY, thumbWidth, thumbHeight, selected);
    }

    // For completed (but not currently selected) books, draw a light hatch overlay
    // to give a "greyed out" appearance on the monochrome display.
    if (completed && !selected && !isDirectoryEntry) {
      for (int dx = 2; dx < cellWidth; dx += 4) {
        renderer.drawLine(x + dx, y + 1, x + dx, y + cellHeight - 2);
      }
    }

    // If this is a completed book, draw a small checkmark in the top-left corner of the card.
    // if (completed) {
    //   const int cx = x + 6;
    //   const int cy = y + 6;
    //   renderer.drawLine(cx - 2, cy, cx, cy + 2);
    //   renderer.drawLine(cx, cy + 2, cx + 4, cy - 2);
    // }

    // Derive a display name: last path component, drop trailing slash, strip .epub, and uppercase.
    std::string name = files[idx];
    if (!name.empty() && name.back() == '/') {
      name.pop_back();
    }

    const auto slashPos = name.find_last_of('/');
    if (slashPos != std::string::npos) {
      name = name.substr(slashPos + 1);
    }

    const std::string suffix = ".epub";
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      name.erase(name.size() - suffix.size());
    }

    std::string displayName = name;
    for (char& c : displayName) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // Choose font: smaller when a thumbnail is present so longer titles fit.
    const int titleFontId = hasThumbnail ? SMALL_FONT_ID : UI_FONT_ID;

    // Word-wrap the title within the card.
    const int maxLineWidth = cellWidth - 2 * paddingX;
    const int lineHeight = renderer.getLineHeight(titleFontId);

    const int seriesLabelLineHeight = (!files[idx].empty() && files[idx].back() == '/')
                                          ? renderer.getLineHeight(SMALL_FONT_ID)
                                          : 0;

    int textAreaHeight = cellHeight - 2 * paddingY;
    if (hasThumbnail && thumbHeight > 0) {
      // Reserve vertical space for the thumbnail at the top of the card.
      textAreaHeight -= static_cast<int>(thumbHeight) + 4;
    }
    if (isDirectoryEntry) {
      // Reserve vertical space at the bottom for the "X Books" label.
      textAreaHeight -= (seriesLabelLineHeight + 4);
    }
    if (textAreaHeight < lineHeight) {
      textAreaHeight = lineHeight;
    }

    int maxLines = textAreaHeight / lineHeight;
    if (maxLines < 1) {
      maxLines = 1;
    }

    std::vector<std::string> words;
    words.reserve(8);
    size_t pos = 0;
    while (pos < displayName.size()) {
      while (pos < displayName.size() && displayName[pos] == ' ') {
        ++pos;
      }
      if (pos >= displayName.size()) {
        break;
      }
      const size_t start = pos;
      while (pos < displayName.size() && displayName[pos] != ' ') {
        ++pos;
      }
      words.emplace_back(displayName.substr(start, pos - start));
    }

    std::vector<std::string> lines;
    std::string currentLine;

    for (size_t i = 0; i < words.size(); ++i) {
      const std::string candidate = currentLine.empty() ? words[i] : currentLine + " " + words[i];
      if (renderer.getTextWidth(titleFontId, candidate.c_str()) <= maxLineWidth || currentLine.empty()) {
        currentLine = candidate;
      } else {
        lines.push_back(currentLine);
        currentLine = words[i];
      }
    }
    if (!currentLine.empty()) {
      lines.push_back(currentLine);
    }

    if (static_cast<int>(lines.size()) > maxLines) {
      std::string lastLine;
      for (size_t i = static_cast<size_t>(maxLines - 1); i < lines.size(); ++i) {
        if (!lastLine.empty()) {
          lastLine += " ";
        }
        lastLine += lines[i];
      }
      lastLine += "...";
      while (!lastLine.empty() && renderer.getTextWidth(titleFontId, lastLine.c_str()) > maxLineWidth) {
        lastLine.pop_back();
      }
      lines.resize(static_cast<size_t>(maxLines - 1));
      if (!lastLine.empty()) {
        lines.push_back(lastLine);
      }
    }

    const int totalTextHeight = lineHeight * static_cast<int>(lines.size());
    const int textAreaTop = y + paddingY + (hasThumbnail && thumbHeight > 0 ? static_cast<int>(thumbHeight) + 4 : 0);
    int textY = textAreaTop + (textAreaHeight - totalTextHeight) / 2;
    if (textY < textAreaTop) {
      textY = textAreaTop;
    }

    for (const auto& line : lines) {
      const int textWidth = renderer.getTextWidth(titleFontId, line.c_str());
      const int textX = x + (cellWidth - textWidth) / 2;
      renderer.drawText(titleFontId, textX, textY, line.c_str(), !selected);
      textY += lineHeight;
    }

    // For a series directory, show "X Books" at the bottom of the card.
    if (isDirectoryEntry) {
      int bookCount = 0;
      if (idx >= 0 && idx < static_cast<int>(seriesBookCounts.size())) {
        bookCount = seriesBookCounts[static_cast<size_t>(idx)];
      }
      if (bookCount > 0) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d Book%s", bookCount, bookCount == 1 ? "" : "s");
        const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, buf);
        const int labelX = x + (cellWidth - labelWidth) / 2;
        const int labelY = y + cellHeight - paddingY - seriesLabelLineHeight;
        renderer.drawText(SMALL_FONT_ID, labelX, labelY, buf, !selected);
      }
    }
  }
}

bool FileSelectionActivity::getThumbnailFromCache(const std::string& fullPath, uint8_t** outData,
                                                    uint16_t* outWidth, uint16_t* outHeight) const {
  if (!outData || !outWidth || !outHeight || !thumbnailCacheMutex) {
    return false;
  }

  bool found = false;
  xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
  for (auto& entry : thumbnailCache) {
    if (entry.data && entry.path == fullPath) {
      *outData = entry.data;
      *outWidth = entry.width;
      *outHeight = entry.height;
      found = true;
      break;
    }
  }
  xSemaphoreGive(thumbnailCacheMutex);
  return found;
}

void FileSelectionActivity::enqueueThumbnailRequest(const std::string& fullPath) const {
  if (!thumbnailQueue) {
    return;
  }

  // Avoid enqueuing if it's already cached.
  uint8_t* dummyData = nullptr;
  uint16_t dummyW = 0;
  uint16_t dummyH = 0;
  if (getThumbnailFromCache(fullPath, &dummyData, &dummyW, &dummyH)) {
    return;
  }

  ThumbnailRequest req{};
  strncpy(req.path, fullPath.c_str(), THUMBNAIL_MAX_PATH - 1);
  if (xQueueSendToBack(thumbnailQueue, &req, 0) == pdTRUE) {
    ++pendingThumbnailRequests;
  }
}

bool FileSelectionActivity::getOrLoadThumbnail(const std::string& fullPath, uint8_t** outData, size_t* outSize,
                                               uint16_t* outWidth, uint16_t* outHeight) const {
  if (!outData || !outSize || !outWidth || !outHeight) {
    return false;
  }

  const unsigned long now = millis();

  // 1. Look for an existing cached entry under lock.
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
    for (auto& entry : thumbnailCache) {
      if (entry.data && entry.path == fullPath) {
        entry.lastUsedMs = now;
        *outData = entry.data;
        *outSize = entry.size;
        *outWidth = entry.width;
        *outHeight = entry.height;
        xSemaphoreGive(thumbnailCacheMutex);
        return true;
      }
    }
    xSemaphoreGive(thumbnailCacheMutex);
  }

  // We'll try to populate a buffer (buf/size/w/h) either from a precomputed
  // thumbnail file on the SD card, or by extracting it from the EPUB as a
  // fallback. Both paths converge on the same in-memory cache insert logic.
  uint8_t* buf = nullptr;
  size_t size = 0;
  uint16_t w = 0;
  uint16_t h = 0;

  // 2. Derive the per-book thumbnail cache file path and try to load it from SD.
  //    This avoids re-opening the EPUB and parsing content.opf on every
  //    thumbnail cache miss.
  Epub epub(fullPath, "/.crosspoint");
  const std::string thumbCacheFile = epub.getCachePath() + "/thumb_2bpp.bin";

  if (SD.exists(thumbCacheFile.c_str())) {
    File f = SD.open(thumbCacheFile.c_str(), FILE_READ);
    if (f) {
      const size_t fileSize = f.size();
      if (fileSize >= 4) {
        uint8_t* fileBuf = static_cast<uint8_t*>(malloc(fileSize));
        if (fileBuf) {
          const size_t bytesRead = f.read(fileBuf, fileSize);
          if (bytesRead == fileSize) {
            const uint16_t fw = static_cast<uint16_t>(fileBuf[0] | (fileBuf[1] << 8));
            const uint16_t fh = static_cast<uint16_t>(fileBuf[2] | (fileBuf[3] << 8));
            if (fw != 0 && fh != 0) {
              buf = fileBuf;
              size = fileSize;
              w = fw;
              h = fh;
            } else {
              free(fileBuf);
            }
          } else {
            free(fileBuf);
          }
        }
      }
      f.close();
    }
  }

  // 3. If we don't have a valid precomputed thumbnail, fall back to reading it
  //    from the EPUB and persist the result to the SD card for next time.
  if (!buf) {
    if (!epub.loadMetadataOnly()) {
      return false;
    }
    const std::string& thumbItem = epub.getThumbnail2bppItem();
    if (thumbItem.empty()) {
      return false;
    }

    size_t epubSize = 0;
    uint8_t* epubBuf = epub.readItemContentsToBytes(thumbItem, &epubSize, false);
    if (!epubBuf || epubSize < 4) {
      if (epubBuf) {
        free(epubBuf);
      }
      return false;
    }

    const uint16_t ew = static_cast<uint16_t>(epubBuf[0] | (epubBuf[1] << 8));
    const uint16_t eh = static_cast<uint16_t>(epubBuf[2] | (epubBuf[3] << 8));
    if (ew == 0 || eh == 0) {
      free(epubBuf);
      return false;
    }

    // Ensure the cache directory exists and write the thumbnail blob to disk so
    // subsequent loads can skip EPUB parsing entirely.
    epub.setupCacheDir();
    File outFile = SD.open(thumbCacheFile.c_str(), FILE_WRITE);
    if (outFile) {
      outFile.write(epubBuf, epubSize);
      outFile.close();
    }

    buf = epubBuf;
    size = epubSize;
    w = ew;
    h = eh;
  }

  if (!buf || size < 4 || w == 0 || h == 0) {
    if (buf) {
      free(buf);
    }
    return false;
  }

  // 4. Choose a cache slot (empty or least-recently-used) and store the
  //    thumbnail in the in-memory LRU cache.
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);

    ThumbnailCacheEntry* target = nullptr;
    for (auto& entry : thumbnailCache) {
      if (!entry.data) {
        target = &entry;
        break;
      }
    }
    if (!target) {
      // Evict the least-recently-used entry.
      target = &thumbnailCache[0];
      for (auto& entry : thumbnailCache) {
        if (entry.lastUsedMs < target->lastUsedMs) {
          target = &entry;
        }
      }
      if (target->data) {
        free(target->data);
      }
    }

    target->path = fullPath;
    target->data = buf;
    target->size = size;
    target->width = w;
    target->height = h;
    target->lastUsedMs = now;

    xSemaphoreGive(thumbnailCacheMutex);
  }

  *outData = buf;
  *outSize = size;
  *outWidth = w;
  *outHeight = h;
  return true;
}
