#include <Arduino.h>
#include <EInkDisplay.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <builtinFonts/bookerly_2b.h>
#include <builtinFonts/bookerly_bold_2b.h>
#include <builtinFonts/bookerly_bold_italic_2b.h>
#include <builtinFonts/bookerly_italic_2b.h>
#include <builtinFonts/pixelarial14.h>
#include <builtinFonts/ubuntu_10.h>
#include <builtinFonts/ubuntu_bold_10.h>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "SyncClient.h"
#include "config.h"

#define SPI_FQ 40000000
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define UART0_RXD 20  // Used for USB connection detection

#define SD_SPI_CS 12
#define SD_SPI_MISO 7

EInkDisplay einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
InputManager inputManager;
GfxRenderer renderer(einkDisplay);
Activity* currentActivity;

// Persist a flag across deep sleep resets so we can distinguish cold boot
// from wake-from-sleep and skip the boot screen on resume.
RTC_DATA_ATTR bool wokeFromDeepSleepFlag = false;

bool getCachedReadingProgress(const std::string& epubPath, uint8_t& progressOut) {
  if (epubPath.empty()) {
    return false;
  }

  const std::string cacheRoot = "/.crosspoint";
  const std::string cachePath = cacheRoot + "/epub_" + std::to_string(std::hash<std::string>{}(epubPath));

  if (!SD.exists(cachePath.c_str())) {
    return false;
  }

  // Read last-known spine and page index from progress.bin
  File progressFile = SD.open((cachePath + "/progress.bin").c_str());
  if (!progressFile) {
    return false;
  }

  uint8_t data[4];
  if (progressFile.read(data, 4) != 4) {
    progressFile.close();
    return false;
  }
  progressFile.close();

  const uint16_t spineIndex = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  const uint16_t pageIndex = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);

  // Read cumulative spine item sizes from spine_size.bin
  File spineFile = SD.open((cachePath + "/spine_size.bin").c_str());
  if (!spineFile) {
    return false;
  }

  uint32_t bookSize = 0;
  uint32_t prevChapterSize = 0;
  uint32_t chapterEndSize = 0;
  uint8_t buf[4];
  uint16_t index = 0;

  while (spineFile.read(buf, 4) == 4) {
    const uint32_t cumulative = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                                (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);

    if (index == spineIndex - 1) {
      prevChapterSize = cumulative;
    }
    if (index == spineIndex) {
      chapterEndSize = cumulative;
    }

    bookSize = cumulative;
    ++index;
  }
  spineFile.close();

  if (bookSize == 0 || spineIndex >= index) {
    return false;
  }

  // Fallbacks in case we didn't find an entry for the current spine index
  if (spineIndex == 0) {
    prevChapterSize = 0;
  }
  if (chapterEndSize <= prevChapterSize) {
    chapterEndSize = prevChapterSize;
  }

  const uint32_t curChapterSize = chapterEndSize - prevChapterSize;

  // Default to start-of-chapter if we can't recover page-level info
  float sectionProg = 0.0f;

  // Try to refine within-chapter progress using cached page metadata (section.bin)
  const std::string sectionDir = cachePath + "/" + std::to_string(spineIndex);
  if (SD.exists(sectionDir.c_str())) {
    File sectionFile = SD.open((sectionDir + "/section.bin").c_str());
    if (sectionFile) {
      const uint32_t sz = sectionFile.size();
      if (sz >= 4 && sectionFile.seek(sz - 4)) {
        uint8_t pageBuf[4];
        if (sectionFile.read(pageBuf, 4) == 4) {
          const uint32_t pageCount = static_cast<uint32_t>(pageBuf[0]) | (static_cast<uint32_t>(pageBuf[1]) << 8) |
                                     (static_cast<uint32_t>(pageBuf[2]) << 16) |
                                     (static_cast<uint32_t>(pageBuf[3]) << 24);
          if (pageCount > 0 && pageIndex < pageCount) {
            sectionProg = static_cast<float>(pageIndex) / static_cast<float>(pageCount);
          }
        }
      }
      sectionFile.close();
    }
  }

  const float progress =
      static_cast<float>(prevChapterSize + static_cast<uint32_t>(sectionProg * static_cast<float>(curChapterSize))) /
      static_cast<float>(bookSize);
  progressOut = static_cast<uint8_t>(progress * 100.0f + 0.5f);
  return true;
}

// Fonts
EpdFont bookerlyFont(&bookerly_2b);
EpdFont bookerlyBoldFont(&bookerly_bold_2b);
EpdFont bookerlyItalicFont(&bookerly_italic_2b);
EpdFont bookerlyBoldItalicFont(&bookerly_bold_italic_2b);
EpdFontFamily bookerlyFontFamily(&bookerlyFont, &bookerlyBoldFont, &bookerlyItalicFont, &bookerlyBoldItalicFont);

EpdFont smallFont(&pixelarial14);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ubuntu10Font(&ubuntu_10);
EpdFont ubuntuBold10Font(&ubuntu_bold_10);
EpdFontFamily ubuntuFontFamily(&ubuntu10Font, &ubuntuBold10Font);

// Auto-sleep timeout (10 minutes of inactivity)
constexpr unsigned long AUTO_SLEEP_TIMEOUT_MS = 10 * 60 * 1000;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

// Verify long press on wake-up from deep sleep
void verifyWakeupLongPress() {
  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;

  inputManager.update();
  // Verify the user has actually pressed
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    inputManager.update();
  }

  if (inputManager.isPressed(InputManager::BTN_POWER)) {
    do {
      delay(10);
      inputManager.update();
    } while (inputManager.isPressed(InputManager::BTN_POWER) &&
             inputManager.getHeldTime() < SETTINGS.getPowerButtonDuration());
    abort = inputManager.getHeldTime() < SETTINGS.getPowerButtonDuration();
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
}

void waitForPowerRelease() {
  inputManager.update();
  while (inputManager.isPressed(InputManager::BTN_POWER)) {
    delay(50);
    inputManager.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  exitActivity();
  enterNewActivity(new SleepActivity(renderer, inputManager));

  Serial.printf("[%lu] [   ] Entering deep sleep.\\n", millis());
  delay(1000);  // Allow Serial buffer to empty and display to update

  // Mark that we're intentionally entering deep sleep so setup() can
  // distinguish a wake-from-sleep from a cold boot.
  wokeFromDeepSleepFlag = true;

  // Enable Wakeup on LOW (button press)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

  einkDisplay.deepSleep();

  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void onGoHome();
void onGoToReader(const std::string& initialEpubPath) {
  exitActivity();
  enterNewActivity(new ReaderActivity(renderer, inputManager, initialEpubPath, onGoHome));
}

// From the home screen, prefer to resume the last-opened EPUB (if any),
// otherwise fall back to the reader's file selection.
void onGoToReaderHome() {
  if (APP_STATE.openEpubPath.empty()) {
    onGoToReader(std::string());
  } else {
    onGoToReader(APP_STATE.openEpubPath);
  }
}

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, inputManager, onGoHome));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, inputManager, onGoHome));
}
namespace {
void showSyncStatus(const char* msg) {
  exitActivity();
  enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, msg, REGULAR));
  delay(500);
}
}

void onSyncProgress() {
  clearLastSyncError();

  if (APP_STATE.openEpubPath.empty()) {
    showSyncStatus("Failed to Sync: No open EPUB");
    delay(1500);
    onGoHome();
    return;
  }

  // Derive cache path for the current EPUB (must match Epub::getCachePath()).
  const std::string cacheRoot = "/.crosspoint";
  const std::string cachePath =
      cacheRoot + "/epub_" + std::to_string(std::hash<std::string>{}(APP_STATE.openEpubPath));

  if (!SD.exists(cachePath.c_str())) {
    showSyncStatus("Failed to Sync: No cache for book");
    delay(1500);
    onGoHome();
    return;
  }

  showSyncStatus("Sync: reading local progress...");

  // Read local spine/page from progress.bin
  File progressFile = SD.open((cachePath + "/progress.bin").c_str());
  if (!progressFile) {
    showSyncStatus("Failed to Sync: progress.bin missing");
    delay(1500);
    onGoHome();
    return;
  }

  uint8_t data[4];
  if (progressFile.read(data, 4) != 4) {
    progressFile.close();
    showSyncStatus("Failed to Sync: progress.bin read error");
    delay(1500);
    onGoHome();
    return;
  }
  progressFile.close();

  int16_t localSpine = static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                            (static_cast<uint16_t>(data[1]) << 8));
  int16_t localPage = static_cast<int16_t>(static_cast<uint16_t>(data[2]) |
                                           (static_cast<uint16_t>(data[3]) << 8));

  // First, try to pull remote progress and, if it is ahead, apply it locally.
  int16_t remoteSpine = 0;
  int16_t remotePage = 0;
  showSyncStatus("Sync: pulling remote...");
  if (fetchRemoteProgress(APP_STATE.openEpubPath, remoteSpine, remotePage)) {
    const bool remoteAhead =
        (remoteSpine > localSpine) || (remoteSpine == localSpine && remotePage > localPage);
    if (remoteAhead) {
      Serial.printf("[%lu] [SYN] Applying remote progress spine=%d, page=%d over local spine=%d, page=%d\n",
                    millis(), remoteSpine, remotePage, localSpine, localPage);

      // Overwrite local progress.bin with the remote position so the reader
      // will resume from the remote location on next open.
      File out = SD.open((cachePath + "/progress.bin").c_str(), FILE_WRITE);
      if (out) {
        uint8_t outData[4];
        outData[0] = static_cast<uint16_t>(remoteSpine) & 0xFF;
        outData[1] = (static_cast<uint16_t>(remoteSpine) >> 8) & 0xFF;
        outData[2] = static_cast<uint16_t>(remotePage) & 0xFF;
        outData[3] = (static_cast<uint16_t>(remotePage) >> 8) & 0xFF;
        out.write(outData, 4);
        out.close();

        localSpine = remoteSpine;
        localPage = remotePage;
      } else {
        Serial.printf("[%lu] [SYN] Failed to reopen progress.bin for write when applying remote progress\n",
                      millis());
      }
    } else {
      Serial.printf("[%lu] [SYN] Remote progress is not ahead; keeping local spine=%d, page=%d\n", millis(),
                    localSpine, localPage);
    }
  } else {
    // Remote pull failed; show last sync error from SyncClient.
    const char* err = getLastSyncError();
    if (!err || err[0] == '\0') {
      err = "Remote pull failed";
    }
    std::string fullMsg = std::string("Failed to Sync: ") + err;
    showSyncStatus(fullMsg.c_str());
    delay(1500);
    onGoHome();
    return;
  }

  // Compute approximate percentage for the (possibly updated) local position.
  uint8_t percentage = 0;
  if (!getCachedReadingProgress(APP_STATE.openEpubPath, percentage)) {
    Serial.printf("[%lu] [SYN] Could not compute approximate percentage for %s; defaulting to 0%%\n", millis(),
                  APP_STATE.openEpubPath.c_str());
  }

  // Finally, push the (potentially updated) progress to the sync server.
  showSyncStatus("Sync: pushing local...");
  const bool ok = syncProgress(APP_STATE.openEpubPath, percentage, localSpine, localPage);
  if (!ok) {
    const char* err = getLastSyncError();
    if (!err || err[0] == '\0') {
      err = "Push failed";
    }
    std::string fullMsg = std::string("Failed to Sync: ") + err;
    showSyncStatus(fullMsg.c_str());
    delay(1500);
    onGoHome();
    return;
  }

  showSyncStatus("Sync complete");
  delay(1000);
  onGoHome();
}

void onGoHome() {
  exitActivity();

  // Derive a display name for the currently open EPUB, if any.
  std::string epubName;
  if (!APP_STATE.openEpubPath.empty()) {
    auto pos = APP_STATE.openEpubPath.find_last_of("/");
    if (pos == std::string::npos) {
      epubName = APP_STATE.openEpubPath;
    } else {
      epubName = APP_STATE.openEpubPath.substr(pos + 1);
    }

    // Strip .epub extension if present for display purposes
    const std::string suffix = ".epub";
    if (epubName.size() >= suffix.size() &&
        epubName.compare(epubName.size() - suffix.size(), suffix.size(), suffix) == 0) {
      epubName.erase(epubName.size() - suffix.size());
    }
  }

  auto onBrowseFiles = []() { onGoToReader(std::string()); };

  enterNewActivity(new HomeActivity(renderer, inputManager, onGoToReaderHome, onBrowseFiles, onSyncProgress,
                                    onGoToSettings, onGoToFileTransfer, epubName));
}

void setup() {
  Serial.begin(115200);

  Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  inputManager.begin();
  // Initialize pins
  pinMode(BAT_GPIO0, INPUT);

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK, SD_SPI_MISO, EPD_MOSI, EPD_CS);

  // SD Card Initialization
  if (!SD.begin(SD_SPI_CS, SPI, SPI_FQ)) {
    Serial.printf("[%lu] [   ] SD card initialization failed\n", millis());
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "SD card error", BOLD));
    return;
  }

  SETTINGS.loadFromFile();

  // verify power button press duration after we've read settings.
  verifyWakeupLongPress();

  // Initialize display
  einkDisplay.begin();
  Serial.printf("[%lu] [   ] Display initialized\\n", millis());

  renderer.insertFont(READER_FONT_ID, bookerlyFontFamily);
  renderer.insertFont(UI_FONT_ID, ubuntuFontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  Serial.printf("[%lu] [   ] Fonts setup\\n", millis());

  // Determine whether we're resuming from deep sleep or doing a cold boot.
  // We rely primarily on the RTC flag set just before esp_deep_sleep_start(),
  // which is robust across Arduino/ESP32 core versions.
  const bool wokeFromDeepSleep = wokeFromDeepSleepFlag;
  // Clear the flag so a subsequent cold reset doesn't look like a resume.
  wokeFromDeepSleepFlag = false;

  exitActivity();
  // Only show the boot screen on cold boot; when resuming from sleep, jump
  // straight to the last state (home or reader) without flashing the logo.
  if (!wokeFromDeepSleep) {
    enterNewActivity(new BootActivity(renderer, inputManager));
  }

  APP_STATE.loadFromFile();
  if (APP_STATE.openEpubPath.empty()) {
    onGoHome();
  } else {
    onGoToReader(APP_STATE.openEpubPath);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long lastLoopTime = 0;
  static unsigned long maxLoopDuration = 0;

  unsigned long loopStartTime = millis();

  static unsigned long lastMemPrint = 0;
  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  inputManager.update();

  // Check for any user activity (button press or release)
  static unsigned long lastActivityTime = millis();
  if (inputManager.wasAnyPressed() || inputManager.wasAnyReleased()) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  if (millis() - lastActivityTime >= AUTO_SLEEP_TIMEOUT_MS) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), AUTO_SLEEP_TIMEOUT_MS);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (inputManager.wasReleased(InputManager::BTN_POWER) &&
      inputManager.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  unsigned long activityDuration = millis() - activityStartTime;

  unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      Serial.printf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
                    activityDuration);
    }
  }

  lastLoopTime = loopStartTime;

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    delay(10);  // Normal delay when no activity requires fast response
  }
}
