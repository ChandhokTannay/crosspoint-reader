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
#include "WifiCredentialStore.h"
#include "util/StringUtils.h"

namespace {
// Overall budget for the whole attempt; sleep must not be held up longer.
constexpr unsigned long TOTAL_BUDGET_MS = 15000;
// Per-network connection wait.
constexpr unsigned long PER_NETWORK_WAIT_MS = 7000;

void wifiOff() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

bool connectSavedWifi(const unsigned long deadline) {
  auto& store = WifiCredentialStore::getInstance();
  const auto& creds = store.getCredentials();
  if (creds.empty()) {
    return false;
  }

  WiFi.mode(WIFI_STA);

  // Try the last-connected network first, then the rest.
  const std::string& last = store.getLastConnectedSsid();
  std::vector<const WifiCredential*> order;
  for (const auto& c : creds) {
    if (c.ssid == last) order.insert(order.begin(), &c);
    else order.push_back(&c);
  }

  for (const auto* cred : order) {
    if (millis() > deadline) break;
    LOG_DBG("KOSleep", "Trying WiFi: %s", cred->ssid.c_str());
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    const unsigned long start = millis();
    while (millis() - start < PER_NETWORK_WAIT_MS && millis() <= deadline) {
      if (WiFi.status() == WL_CONNECTED) {
        LOG_DBG("KOSleep", "WiFi connected: %s", cred->ssid.c_str());
        return true;
      }
      delay(100);
    }
    WiFi.disconnect(false);
  }
  return false;
}
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

  if (!connectSavedWifi(deadline)) {
    LOG_DBG("KOSleep", "No WiFi; skipping sleep sync");
    wifiOff();
    return;
  }

  // Only push when local progress is at or beyond the server's — never roll
  // back further progress made on another device.
  KOReaderProgress remote{};
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote);
  if (getResult == KOReaderSyncClient::OK && remote.percentage > local.percentage) {
    LOG_DBG("KOSleep", "Server is further (%.2f%% > %.2f%%); not pushing", remote.percentage * 100,
            local.percentage * 100);
    wifiOff();
    return;
  }

  KOReaderProgress upload{};
  upload.document = hash;
  upload.progress = local.xpath;
  upload.percentage = local.percentage;
  const auto putResult = KOReaderSyncClient::updateProgress(upload);
  LOG_DBG("KOSleep", "Sleep sync push %s", putResult == KOReaderSyncClient::OK ? "OK" : "failed");

  wifiOff();
}
