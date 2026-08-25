#include "KOReaderSleepSync.h"

#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <ProgressMapper.h>
#include <WiFi.h>

#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderNet.h"
#include "util/StringUtils.h"

namespace {
// Overall budget for the whole attempt; sleep must not be held up longer.
constexpr unsigned long TOTAL_BUDGET_MS = 15000;
}  // namespace

void KOReaderSleepSync::attempt() {
  if (!SETTINGS.syncOnSleep) return;
  if (!APP_STATE.lastSleepFromReader) return;

  const std::string& path = APP_STATE.openEpubPath;
  if (path.empty() || !StringUtils::checkFileExtension(path, ".epub")) return;
  if (!KOREADER_STORE.hasCredentials()) return;

  const unsigned long deadline = millis() + TOTAL_BUDGET_MS;

  // Load the last saved reader position (persisted on every page turn).
  auto epub = std::make_shared<Epub>(path, "/.crosspoint");
  uint8_t data[6];
  {
    FsFile f;
    if (!Storage.openFileForRead("KOSleep", epub->getCachePath() + "/progress.bin", f)) return;
    const int n = f.read(data, 6);
    f.close();
    if (n != 6) return;
  }
  CrossPointPosition pos;
  pos.spineIndex = data[0] | (data[1] << 8);
  pos.pageNumber = data[2] | (data[3] << 8);
  pos.totalPages = data[4] | (data[5] << 8);

  // Book cache is needed for percentage math; this is a fast cache hit.
  if (!epub->load(true, /*skipLoadingCss=*/true)) return;

  const KOReaderPosition local = ProgressMapper::toKOReader(epub, pos);

  const std::string hash = (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME)
                               ? KOReaderDocumentId::calculateFromFilename(path)
                               : KOReaderDocumentId::calculate(path);
  if (hash.empty()) return;

  LOG_DBG("KOSleep", "Sleep sync: %s at %.2f%%", path.c_str(), local.percentage * 100);

  if (!KOReaderNet::connectSavedWifi(deadline)) {
    LOG_DBG("KOSleep", "No WiFi; skipping sleep sync");
    KOReaderNet::wifiOff();
    return;
  }

  // Only push when local progress is at or beyond the server's — never roll
  // back further progress made on another device.
  KOReaderProgress remote{};
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote);
  if (getResult == KOReaderSyncClient::OK && remote.percentage > local.percentage) {
    LOG_DBG("KOSleep", "Server is further (%.2f%% > %.2f%%); not pushing", remote.percentage * 100,
            local.percentage * 100);
    KOReaderNet::wifiOff();
    return;
  }

  KOReaderProgress upload{};
  upload.document = hash;
  upload.progress = local.xpath;
  upload.percentage = local.percentage;
  const auto putResult = KOReaderSyncClient::updateProgress(upload);
  LOG_DBG("KOSleep", "Sleep sync push %s", putResult == KOReaderSyncClient::OK ? "OK" : "failed");

  KOReaderNet::wifiOff();
}
