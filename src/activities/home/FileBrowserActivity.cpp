#include "FileBrowserActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../util/ConfirmationActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

// Sanity limits for embedded 2bpp thumbnails. These are intentionally
// conservative and well within the device's display capabilities.
constexpr uint16_t MAX_THUMBNAIL_WIDTH = 600;
constexpr uint16_t MAX_THUMBNAIL_HEIGHT = 800;

// Grid layout for the library view (must match renderBooksGrid).
constexpr int GRID_ITEMS_PER_PAGE = 9;

// Height (px) for on-device generated cover thumbnails in the grid.
constexpr int GRID_COVER_HEIGHT = 140;
// Anything smaller than a BMP header is a failed-decode stub.
constexpr size_t MIN_VALID_BMP_SIZE = 54;

constexpr char COMPLETED_BOOKS_FILE[] = "/.crosspoint/completed_books.txt";

// Count how many EPUB files are directly inside a given series directory.
// dirEntry is a single path component relative to basepath and typically
// ends with a trailing '/'.
int countBooksInSeries(const std::string& basepath, const std::string& dirEntry) {
  std::string dirName = dirEntry;
  if (!dirName.empty() && dirName.back() == '/') {
    dirName.pop_back();
  }

  std::string path = basepath;
  if (path.empty()) {
    path = "/";
  }
  if (path.back() != '/') {
    path += '/';
  }
  path += dirName;

  auto root = Storage.open(path.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  int count = 0;
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] != '.' && !file.isDirectory() && StringUtils::checkFileExtension(std::string(name), ".epub")) {
      ++count;
    }
    file.close();
  }
  root.close();
  return count;
}
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void FileBrowserActivity::loadFiles() {
  files.clear();
  seriesBookCounts.clear();

  loadCompletedBooks();

  const bool inBooksTree = isInBooksTree();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  // Collect raw entries (name + series book count) before sorting so that
  // counts remain aligned with their corresponding files after sorting.
  std::vector<std::pair<std::string, int>> entries;

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    const bool isDir = file.isDirectory();
    auto filename = std::string(name);
    const bool isEpub = !isDir && StringUtils::checkFileExtension(filename, ".epub");

    if (inBooksTree) {
      // Inside the Books tree, only show directories and EPUB files.
      if (!isDir && !isEpub) {
        file.close();
        continue;
      }
    } else if (!isDir && !isEpub && !StringUtils::checkFileExtension(filename, ".xtch") &&
               !StringUtils::checkFileExtension(filename, ".xtc") && !StringUtils::checkFileExtension(filename, ".txt") &&
               !StringUtils::checkFileExtension(filename, ".md") && !StringUtils::checkFileExtension(filename, ".bmp")) {
      file.close();
      continue;
    }

    std::string entryName = isDir ? filename + "/" : filename;
    int bookCount = 0;
    if (isDir && inBooksTree) {
      bookCount = countBooksInSeries(basepath, entryName);
    }
    entries.emplace_back(std::move(entryName), bookCount);
    file.close();
  }
  root.close();

  std::vector<std::string> names;
  names.reserve(entries.size());
  for (const auto& e : entries) names.push_back(e.first);
  sortFileList(names);

  files.reserve(names.size());
  seriesBookCounts.reserve(names.size());
  for (const auto& n : names) {
    files.push_back(n);
    int count = 0;
    for (const auto& e : entries) {
      if (e.first == n) {
        count = e.second;
        break;
      }
    }
    seriesBookCounts.push_back(count);
  }
}

void FileBrowserActivity::loadCompletedBooks() {
  if (completedLoaded) {
    return;
  }
  completedLoaded = true;
  completedBooks.clear();

  HalFile in;
  if (!Storage.openFileForRead("FileBrowser", COMPLETED_BOOKS_FILE, in)) {
    return;
  }
  std::string contents;
  char buf[129];
  int n;
  while ((n = in.read(buf, 128)) > 0) {
    buf[n] = 0;
    contents += buf;
  }
  in.close();

  size_t pos = 0;
  while (pos < contents.size()) {
    const size_t eol = contents.find('\n', pos);
    const std::string line = (eol == std::string::npos) ? contents.substr(pos) : contents.substr(pos, eol - pos);
    if (!line.empty()) {
      completedBooks.insert(line);
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
}

bool FileBrowserActivity::isBookCompleted(const std::string& fullPath) const {
  if (completedBooks.empty()) {
    return false;
  }
  return completedBooks.find(fullPath) != completedBooks.end();
}

bool FileBrowserActivity::isInBooksTree() const {
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

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  // Library-first: when launched at the filesystem root, start browsing in
  // /Books (the cover grid view) if that directory exists.
  if (basepath == "/" && Storage.exists("/Books")) {
    basepath = "/Books";
  }

  loadFiles();
  selectorIndex = 0;
  pendingThumbnailRequests = 0;

  thumbnailCacheMutex = xSemaphoreCreateMutex();
  thumbnailQueue = xQueueCreate(THUMBNAIL_QUEUE_LENGTH, sizeof(ThumbnailRequest));
  if (thumbnailQueue != nullptr) {
    xTaskCreate(&FileBrowserActivity::thumbnailTaskTrampoline, "ThumbnailLoaderTask",
                8192,  // Stack size (large enough for EPUB metadata parsing)
                this, 1, &thumbnailTaskHandle);
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  if (thumbnailTaskHandle) {
    vTaskDelete(thumbnailTaskHandle);
    thumbnailTaskHandle = nullptr;
  }
  if (thumbnailQueue) {
    vQueueDelete(thumbnailQueue);
    thumbnailQueue = nullptr;
  }
  clearThumbnailCache();
  if (thumbnailCacheMutex) {
    vSemaphoreDelete(thumbnailCacheMutex);
    thumbnailCacheMutex = nullptr;
  }
  files.clear();
  seriesBookCounts.clear();
  pendingThumbnailRequests = 0;
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (StringUtils::checkFileExtension(fullPath, ".epub")) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  const bool inBooksTree = isInBooksTree();

  // While thumbnails are still loading for the current library page, ignore
  // navigation input so we don't leave the grid in a half-loaded state.
  if (inBooksTree && pendingThumbnailRequests > 0) {
    return;
  }

  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/" && basepath != "/Books") {
    basepath = Storage.exists("/Books") ? "/Books" : "/";
    loadFiles();
    clearThumbnailCache();
    selectorIndex = 0;
    return;
  }

  const int pageItems = inBooksTree ? GRID_ITEMS_PER_PAGE
                                    : UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mappedInput.getHeldTime() >= GO_HOME_MS && !isDirectory) {
      // --- LONG PRESS ACTION: DELETE FILE ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          clearFileMetadata(fullPath);
          if (Storage.remove(fullPath.c_str())) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            clearThumbnailCache();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              // Move selection to the new "last" item
              selectorIndex = files.size() - 1;
            }

            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        // When changing into a new folder, flush the in-RAM thumbnail cache
        // so thumbnails for the new directory can be loaded without being
        // capped by entries from the previous view.
        clearThumbnailCache();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root.
    // Treat "/Books" as the browsing root so BACK from the library goes home.
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath == "/Books" || basepath == "/") {
        onGoHome();
      } else {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        clearThumbnailCache();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  const auto onIndexChanged = [this](int newIndex) {
    const int oldPage = static_cast<int>(selectorIndex) / GRID_ITEMS_PER_PAGE;
    selectorIndex = newIndex;
    const int newPage = static_cast<int>(selectorIndex) / GRID_ITEMS_PER_PAGE;
    if (isInBooksTree() && newPage != oldPage) {
      clearThumbnailCache();
    }
    requestUpdate();
  };

  buttonNavigator.onNextRelease([this, listSize, onIndexChanged] {
    onIndexChanged(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  });

  buttonNavigator.onPreviousRelease([this, listSize, onIndexChanged] {
    onIndexChanged(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems, onIndexChanged] {
    onIndexChanged(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems));
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems, onIndexChanged] {
    onIndexChanged(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems));
  });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    return filename.substr(0, filename.length() - 1);
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const bool inBooksTree = isInBooksTree();

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else if (inBooksTree) {
    renderBooksGrid(pageWidth, pageHeight);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  // Help text
  const auto labels = mappedInput.mapLabels((basepath == "/" || basepath == "/Books") ? tr(STR_HOME) : tr(STR_BACK),
                                            files.empty() ? "" : tr(STR_OPEN), files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void FileBrowserActivity::renderBooksGrid(const int pageWidth, const int pageHeight) {
  if (files.empty()) {
    return;
  }

  constexpr int columns = 3;
  constexpr int rows = 3;
  constexpr int itemsPerPage = columns * rows;  // 9 items per page
  static_assert(itemsPerPage == GRID_ITEMS_PER_PAGE, "grid paging must match navigation");

  const int totalItems = static_cast<int>(files.size());
  const int clampedSelector =
      static_cast<int>(selectorIndex) >= totalItems ? totalItems - 1 : static_cast<int>(selectorIndex);
  const int currentPage = clampedSelector / itemsPerPage;
  const int pageStartIndex = currentPage * itemsPerPage;
  const int pageEndIndex = std::min(pageStartIndex + itemsPerPage, totalItems);

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Layout metrics
  const int leftMargin = 20;
  const int rightMargin = 20;
  const int topMargin = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomMargin = metrics.buttonHintsHeight + metrics.verticalSpacing;
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

      // Enqueue thumbnail loads for all EPUBs on the current page. The UI
      // ignores navigation while pendingThumbnailRequests > 0 so we don't
      // leave the page in a half-loaded state.
      enqueueThumbnailRequest(fullPath);
    }

    // Try to retrieve a cached thumbnail for the book: either an embedded
    // 2bpp image or an on-device generated cover BMP.
    uint8_t* thumbBuffer = nullptr;
    uint16_t thumbWidth = 0;
    uint16_t thumbHeight = 0;
    std::string thumbBmpPath;
    bool hasThumbnail = false;

    if (!isDirectoryEntry) {
      hasThumbnail = getThumbnailFromCache(fullPath, &thumbBuffer, &thumbWidth, &thumbHeight, &thumbBmpPath);
    }

    if (selected) {
      renderer.fillRect(x, y, cellWidth, cellHeight);
    } else {
      renderer.drawRect(x, y, cellWidth, cellHeight);
    }

    // If we have a thumbnail for the book, render it in the upper part of
    // the card before drawing any text.
    const int paddingX = 8;
    const int paddingY = 8;
    if (hasThumbnail && thumbBuffer) {
      const uint8_t* pixels = thumbBuffer + 4;
      const int coverX = x + (cellWidth - static_cast<int>(thumbWidth)) / 2;
      const int coverY = y + paddingY;
      // Invert the thumbnail when the card is selected so it stays visible on
      // the dark highlight background.
      renderer.draw2bppImage(pixels, coverX, coverY, thumbWidth, thumbHeight, selected);
    } else if (hasThumbnail && !thumbBmpPath.empty()) {
      // On-device generated cover BMP: stream it from SD into the card.
      HalFile f;
      if (Storage.openFileForRead("FileBrowser", thumbBmpPath, f)) {
        Bitmap bitmap(f, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          thumbWidth = static_cast<uint16_t>(std::min(bitmap.getWidth(), cellWidth - 2 * paddingX));
          thumbHeight = static_cast<uint16_t>(std::min(bitmap.getHeight(), cellHeight - 2 * paddingY));
          const int coverX = x + (cellWidth - static_cast<int>(thumbWidth)) / 2;
          const int coverY = y + paddingY;
          renderer.drawBitmap(bitmap, coverX, coverY, thumbWidth, thumbHeight, 0, 0);
        }
        f.close();
      }
    }

    // For completed (but not currently selected) books, draw a light hatch overlay
    // to give a "greyed out" appearance on the monochrome display.
    if (completed && !selected && !isDirectoryEntry) {
      for (int dx = 2; dx < cellWidth; dx += 4) {
        renderer.drawLine(x + dx, y + 1, x + dx, y + cellHeight - 2);
      }
    }

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
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      name.erase(name.size() - suffix.size());
    }

    std::string displayName = name;
    for (char& c : displayName) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // Choose font: smaller when a thumbnail is present so longer titles fit.
    const int titleFontId = hasThumbnail ? SMALL_FONT_ID : UI_10_FONT_ID;

    // Word-wrap the title within the card.
    const int maxLineWidth = cellWidth - 2 * paddingX;
    const int lineHeight = renderer.getLineHeight(titleFontId);

    const int seriesLabelLineHeight = isDirectoryEntry ? renderer.getLineHeight(SMALL_FONT_ID) : 0;

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

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}

void FileBrowserActivity::thumbnailTaskTrampoline(void* param) {
  auto* self = static_cast<FileBrowserActivity*>(param);
  self->thumbnailTaskLoop();
}

[[noreturn]] void FileBrowserActivity::thumbnailTaskLoop() {
  while (true) {
    if (!thumbnailQueue) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    ThumbnailRequest req{};
    if (xQueueReceive(thumbnailQueue, &req, portMAX_DELAY) == pdTRUE) {
      std::string fullPath(req.path);
      // Warm the cache; if a thumbnail was loaded, trigger a re-render so the
      // newly available image can be drawn without requiring user navigation.
      if (loadThumbnailIntoCache(fullPath)) {
        requestUpdate();
      }
      if (pendingThumbnailRequests > 0) {
        --pendingThumbnailRequests;
        if (pendingThumbnailRequests == 0) {
          // Last outstanding request: re-render so navigation resumes with
          // every cover for the page in place.
          requestUpdate();
        }
      }
    }
  }
}

bool FileBrowserActivity::getThumbnailFromCache(const std::string& fullPath, uint8_t** outData, uint16_t* outWidth,
                                                uint16_t* outHeight, std::string* outBmpPath) const {
  if (!outData || !outWidth || !outHeight || !outBmpPath || !thumbnailCacheMutex) {
    return false;
  }

  bool found = false;
  xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
  for (auto& entry : thumbnailCache) {
    if (entry.resolved && entry.path == fullPath) {
      *outData = entry.data;
      *outWidth = entry.width;
      *outHeight = entry.height;
      *outBmpPath = entry.bmpPath;
      found = entry.data != nullptr || !entry.bmpPath.empty();
      break;
    }
  }
  xSemaphoreGive(thumbnailCacheMutex);
  return found;
}

void FileBrowserActivity::clearThumbnailCache() {
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
  }

  for (auto& entry : thumbnailCache) {
    if (entry.data) {
      free(entry.data);
      entry.data = nullptr;
    }
    entry.size = 0;
    entry.width = 0;
    entry.height = 0;
    entry.lastUsedMs = 0;
    entry.path.clear();
    entry.bmpPath.clear();
    entry.resolved = false;
  }

  if (thumbnailCacheMutex) {
    xSemaphoreGive(thumbnailCacheMutex);
  }

  pendingThumbnailRequests = 0;
  if (thumbnailQueue) {
    xQueueReset(thumbnailQueue);
  }
}

void FileBrowserActivity::enqueueThumbnailRequest(const std::string& fullPath) const {
  if (!thumbnailQueue) {
    return;
  }

  // Thumbnails are optional; if heap is low, skip loading new ones to avoid
  // stressing the allocator / ZIP parser. The reader and UI still function
  // with text-only cards.
  if (ESP.getFreeHeap() < 120000) {
    return;
  }

  // Avoid enqueuing if this book already has a resolved cache entry
  // (including the negative "no thumbnail available" marker).
  bool alreadyResolved = false;
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
    for (auto& entry : thumbnailCache) {
      if (entry.resolved && entry.path == fullPath) {
        alreadyResolved = true;
        break;
      }
    }
    xSemaphoreGive(thumbnailCacheMutex);
  }
  if (alreadyResolved) {
    return;
  }

  ThumbnailRequest req{};
  strncpy(req.path, fullPath.c_str(), THUMBNAIL_MAX_PATH - 1);
  if (xQueueSendToBack(thumbnailQueue, &req, 0) == pdTRUE) {
    ++pendingThumbnailRequests;
  }
}

bool FileBrowserActivity::loadThumbnailIntoCache(const std::string& fullPath) const {
  const unsigned long now = millis();

  // 1. Already resolved (positively or negatively)?
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);
    for (auto& entry : thumbnailCache) {
      if (entry.resolved && entry.path == fullPath) {
        entry.lastUsedMs = now;
        const bool drawable = entry.data != nullptr || !entry.bmpPath.empty();
        xSemaphoreGive(thumbnailCacheMutex);
        return drawable;
      }
    }
    xSemaphoreGive(thumbnailCacheMutex);
  }

  Epub epub(fullPath, "/.crosspoint");
  const std::string thumbCachePath = epub.getCachePath() + "/thumb_2bpp.bin";

  uint8_t* buf = nullptr;
  size_t size = 0;
  std::string bmpPath;
  bool metadataLoaded = false;

  // 2. Embedded 2bpp fast path: SD-backed blob cache first.
  if (Storage.exists(thumbCachePath.c_str())) {
    HalFile f;
    if (Storage.openFileForRead("FileBrowser", thumbCachePath, f)) {
      const size_t fileSize = f.size();
      if (fileSize >= 4) {
        buf = static_cast<uint8_t*>(malloc(fileSize));
        if (buf) {
          const int n = f.read(buf, fileSize);
          if (n == static_cast<int>(fileSize)) {
            size = fileSize;
          } else {
            free(buf);
            buf = nullptr;
          }
        }
      }
      f.close();
    }
  }

  // 3. Blob cache miss: parse EPUB metadata for an embedded 2bpp thumbnail.
  if (!buf) {
    metadataLoaded = epub.loadMetadataOnly();
    if (metadataLoaded) {
      const std::string& thumbItem = epub.getThumbnail2bppItem();
      if (!thumbItem.empty()) {
        buf = epub.readItemContentsToBytes(thumbItem, &size, false);
        if (buf && size >= 4) {
          // Persist the raw 2bpp blob so future loads for this book never
          // need to touch the EPUB metadata again.
          epub.setupCacheDir();
          HalFile out;
          if (Storage.openFileForWrite("FileBrowser", thumbCachePath, out)) {
            out.write(buf, size);
            out.close();
          }
        } else if (buf) {
          free(buf);
          buf = nullptr;
        }
      }
    }
  }

  // Validate the embedded thumbnail dimensions/size.
  uint16_t w = 0;
  uint16_t h = 0;
  if (buf) {
    w = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    h = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t expectedPixelBytes = (pixelCount + 3) / 4;  // 4 pixels per byte
    if (w == 0 || h == 0 || w > MAX_THUMBNAIL_WIDTH || h > MAX_THUMBNAIL_HEIGHT || size < 4 + expectedPixelBytes) {
      free(buf);
      buf = nullptr;
    }
  }

  // 4. No embedded thumbnail: generate a cover BMP on-device from the book's
  // regular cover image (JPG/PNG), reusing the native pipeline.
  if (!buf) {
    if (!metadataLoaded) {
      metadataLoaded = epub.loadMetadataOnly();
    }
    if (metadataLoaded && epub.generateThumbBmp(GRID_COVER_HEIGHT)) {
      const std::string candidate = epub.getThumbBmpPath(GRID_COVER_HEIGHT);
      HalFile f;
      if (Storage.openFileForRead("FileBrowser", candidate, f)) {
        // A tiny file is the failed-decode stub written by generateThumbBmp.
        if (f.size() >= MIN_VALID_BMP_SIZE) {
          bmpPath = candidate;
        }
        f.close();
      }
    }
  }

  // 5. Store the result — positive (2bpp buffer or BMP path) or the negative
  // "no thumbnail" marker so this book is not probed again.
  bool stored = false;
  if (thumbnailCacheMutex) {
    xSemaphoreTake(thumbnailCacheMutex, portMAX_DELAY);

    ThumbnailCacheEntry* target = nullptr;
    for (auto& entry : thumbnailCache) {
      if (!entry.resolved) {
        target = &entry;
        break;
      }
    }

    if (target) {
      target->path = fullPath;
      target->data = buf;
      target->size = size;
      target->width = w;
      target->height = h;
      target->bmpPath = bmpPath;
      target->resolved = true;
      target->lastUsedMs = now;
      stored = true;
    }

    xSemaphoreGive(thumbnailCacheMutex);
  }

  if (!stored) {
    // Cache full: drop this thumbnail; the card renders text-only for now.
    if (buf) free(buf);
    return false;
  }

  return buf != nullptr || !bmpPath.empty();
}
