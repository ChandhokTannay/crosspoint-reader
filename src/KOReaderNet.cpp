#include "KOReaderNet.h"

#include <Logging.h>
#include <cstring>
#include <cstdlib>
#include <WiFi.h>

#include <string>
#include <vector>

#include "WifiCredentialStore.h"

namespace {
constexpr unsigned long PER_NETWORK_WAIT_MS = 7000;
}

namespace KOReaderNet {

void wifiOff() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

bool connectSavedWifi(const unsigned long deadlineMs, const volatile bool* abortFlag) {
  auto& store = WifiCredentialStore::getInstance();
  // The credential store is lazily loaded (normally by the WiFi selection
  // screen); on a fresh boot it is empty until loaded from file.
  store.loadFromFile();
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
    if (millis() > deadlineMs || (abortFlag && *abortFlag)) break;
    LOG_DBG("KONet", "Trying WiFi: %s", cred->ssid.c_str());
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    const unsigned long start = millis();
    while (millis() - start < PER_NETWORK_WAIT_MS && millis() <= deadlineMs) {
      if (abortFlag && *abortFlag) break;
      if (WiFi.status() == WL_CONNECTED) {
        LOG_DBG("KONet", "WiFi connected: %s", cred->ssid.c_str());
        return true;
      }
      delay(100);
    }
    WiFi.disconnect(false);
  }
  return false;
}

int spineOrdinalFromPointer(const char* pointer) {
  if (!pointer) return -1;
  const char* marker = strstr(pointer, "/DocFragment[");
  if (!marker) return -1;
  return atoi(marker + strlen("/DocFragment["));
}

}  // namespace KOReaderNet
