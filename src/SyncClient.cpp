#include "SyncClient.h"

#include <Arduino.h>
#include "SyncConfig.h"

#if defined(ENABLE_PROGRESS_SYNC)

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <string>
#include <cstdio>

namespace {

std::string lastSyncError;

void setLastSyncError(const char* msg) {
  lastSyncError = msg ? msg : "";
}

bool ensureWifiConnected(unsigned long timeoutMs = 3000) {
  if (WiFi.status() == WL_CONNECTED) {
    setLastSyncError("");
    return true;
  }

  Serial.printf("[%lu] [SYN] Connecting WiFi to SSID '%s'...\n", millis(), PROGRESS_SYNC_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(PROGRESS_SYNC_SSID, PROGRESS_SYNC_PASSWORD);

  const auto start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [SYN] WiFi connect timeout after %lums\n", millis(), timeoutMs);
    setLastSyncError("WiFi connect timeout");
    return false;
  }

  Serial.printf("[%lu] [SYN] WiFi connected, IP=%s\n", millis(), WiFi.localIP().toString().c_str());
  setLastSyncError("");
  return true;
}
}  // namespace

static bool parseIntField(const String& body, const char* key, int16_t& outValue) {
  // Very small and naive JSON int extractor: looks for "key": <number>
  String pattern = String("\"") + key + "\":";
  int idx = body.indexOf(pattern);
  if (idx < 0) {
    return false;
  }
  idx += pattern.length();
  // Skip spaces
  while (idx < body.length() && (body[idx] == ' ' || body[idx] == '\t')) {
    ++idx;
  }
  bool neg = false;
  if (idx < body.length() && body[idx] == '-') {
    neg = true;
    ++idx;
  }
  long value = 0;
  bool anyDigit = false;
  while (idx < body.length() && body[idx] >= '0' && body[idx] <= '9') {
    anyDigit = true;
    value = value * 10 + (body[idx] - '0');
    ++idx;
  }
  if (!anyDigit) {
    return false;
  }
  if (neg) {
    value = -value;
  }
  outValue = static_cast<int16_t>(value);
  return true;
}

bool syncProgress(const std::string& documentPath, uint8_t percentage, int16_t spineIndex, int16_t page) {
  if (!ensureWifiConnected()) {
    Serial.printf("[%lu] [SYN] Skipping sync; WiFi not connected\\n", millis());
    if (getLastSyncError()[0] == '\\0') {
      setLastSyncError("WiFi not connected");
    }
    return false;
  }

  // Build a small JSON payload manually to avoid extra dependencies.
  const float progress = static_cast<float>(percentage) / 100.0f;
  const unsigned long ts = millis();  // Not real Unix time, but good enough for ordering.

  String payload = "{";
  payload += "\"document\":\"";
  payload += documentPath.c_str();
  payload += "\",";
  payload += "\"progress\":";
  payload += String(progress, 3);
  payload += ",";
  payload += "\"percentage\":";
  payload += String(percentage);
  payload += ",";
  payload += "\"device\":\"crosspoint-x4\",";
  payload += "\"timestamp\":";
  payload += String(ts);
  payload += ",";
  payload += "\"spine_index\":";
  payload += String(spineIndex);
  payload += ",";
  payload += "\"page\":";
  payload += String(page);
  payload += "}";

  // Parse PROGRESS_SYNC_URL into host, port, and path.
  String url = PROGRESS_SYNC_URL;
  if (url.startsWith("http://")) {
    url = url.substring(7);
  }
  int slashIdx = url.indexOf('/');
  String hostPort = (slashIdx >= 0) ? url.substring(0, slashIdx) : url;
  String path = (slashIdx >= 0) ? url.substring(slashIdx) : "/";

  String host = hostPort;
  uint16_t port = 80;
  int colonIdx = hostPort.indexOf(':');
  if (colonIdx >= 0) {
    host = hostPort.substring(0, colonIdx);
    port = static_cast<uint16_t>(hostPort.substring(colonIdx + 1).toInt());
    if (port == 0) {
      port = 80;
    }
  }

  WiFiClient client;
  const unsigned long connectTimeoutMs = 3000;
  const unsigned long connectStart = millis();
  Serial.printf("[%lu] [SYN] Connecting to %s:%u for progress sync...\\n", millis(), host.c_str(), port);
  while (!client.connected() && millis() - connectStart < connectTimeoutMs) {
    if (client.connect(host.c_str(), port)) {
      break;
    }
    delay(100);
  }

  if (!client.connected()) {
    Serial.printf("[%lu] [SYN] HTTP PUT connect timeout to %s:%u\\n", millis(), host.c_str(), port);
    setLastSyncError("HTTP PUT connect timeout");
    client.stop();
    return false;
  }

  // Send minimal HTTP/1.1 PUT request.
  client.print("PUT ");
  client.print(path);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  if (port != 80) {
    client.print(":");
    client.print(port);
  }
  client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: ");
  client.print(payload.length());
  client.print("\r\n\r\n");
  client.print(payload);

  // Wait for a response with a hard timeout so the UI cannot freeze indefinitely.
  const unsigned long responseTimeoutMs = 3000;
  const unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < responseTimeoutMs) {
    delay(50);
  }

  if (!client.available()) {
    Serial.printf("[%lu] [SYN] HTTP PUT response timeout from %s:%u\\n", millis(), host.c_str(), port);
    setLastSyncError("HTTP PUT timeout");
    client.stop();
    return false;
  }

  // Read the status line.
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[%lu] [SYN] HTTP status line: %s\\n", millis(), statusLine.c_str());

  bool ok = false;
  if (statusLine.startsWith("HTTP/")) {
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace > 0) {
      int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
      String codeStr = (secondSpace > firstSpace)
                           ? statusLine.substring(firstSpace + 1, secondSpace)
                           : statusLine.substring(firstSpace + 1);
      int code = codeStr.toInt();
      if (code >= 200 && code < 400) {
        ok = true;
      }
    }
  }

  client.stop();

  if (ok) {
    setLastSyncError("");
    return true;
  }

  setLastSyncError("HTTP PUT failed");
  return false;
}

bool fetchRemoteProgress(const std::string& documentPath, int16_t& spineIndexOut, int16_t& pageOut) {
  // Allow up to ~10s to establish WiFi during a manual sync.
  if (!ensureWifiConnected(10000)) {
    Serial.printf("[%lu] [SYN] Skipping pull; WiFi not connected\\n", millis());
    if (getLastSyncError()[0] == '\\0') {
      setLastSyncError("WiFi not connected");
    }
    return false;
  }

  // Parse PROGRESS_SYNC_URL into host, port, and path.
  String baseUrl = PROGRESS_SYNC_URL;
  if (baseUrl.startsWith("http://")) {
    baseUrl = baseUrl.substring(7);
  }
  int slashIdx = baseUrl.indexOf('/');
  String hostPort = (slashIdx >= 0) ? baseUrl.substring(0, slashIdx) : baseUrl;
  String path = (slashIdx >= 0) ? baseUrl.substring(slashIdx) : "/";

  String host = hostPort;
  uint16_t port = 80;
  int colonIdx = hostPort.indexOf(':');
  if (colonIdx >= 0) {
    host = hostPort.substring(0, colonIdx);
    port = static_cast<uint16_t>(hostPort.substring(colonIdx + 1).toInt());
    if (port == 0) {
      port = 80;
    }
  }

  // Build path with query string: ?document=<escaped path>
  String doc = documentPath.c_str();
  doc.replace(" ", "%20");
  String pathWithQuery = path + "?document=" + doc;

  WiFiClient client;
  const unsigned long connectTimeoutMs = 3000;
  const unsigned long connectStart = millis();
  Serial.printf("[%lu] [SYN] Connecting to %s:%u for progress pull...\\n", millis(), host.c_str(), port);
  while (!client.connected() && millis() - connectStart < connectTimeoutMs) {
    if (client.connect(host.c_str(), port)) {
      break;
    }
    delay(100);
  }

  if (!client.connected()) {
    Serial.printf("[%lu] [SYN] HTTP GET connect timeout to %s:%u\\n", millis(), host.c_str(), port);
    setLastSyncError("HTTP GET connect timeout");
    client.stop();
    return false;
  }

  // Send minimal HTTP/1.1 GET request.
  client.print("GET ");
  client.print(pathWithQuery);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  if (port != 80) {
    client.print(":");
    client.print(port);
  }
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  const unsigned long responseTimeoutMs = 3000;
  const unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < responseTimeoutMs) {
    delay(50);
  }

  if (!client.available()) {
    Serial.printf("[%lu] [SYN] HTTP GET response timeout from %s:%u\\n", millis(), host.c_str(), port);
    setLastSyncError("HTTP GET timeout");
    client.stop();
    return false;
  }

  // Read status line
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[%lu] [SYN] HTTP GET status line: %s\\n", millis(), statusLine.c_str());
  bool okStatus = false;
  if (statusLine.startsWith("HTTP/")) {
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace > 0) {
      int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
      String codeStr = (secondSpace > firstSpace)
                           ? statusLine.substring(firstSpace + 1, secondSpace)
                           : statusLine.substring(firstSpace + 1);
      int code = codeStr.toInt();
      if (code == 200) {
        okStatus = true;
      } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "HTTP GET %d", code);
        setLastSyncError(buf);
      }
    }
  }

  if (!okStatus) {
    client.stop();
    return false;
  }

  // Skip headers
  String line;
  do {
    line = client.readStringUntil('\n');
  } while (line.length() > 1 && line != "\r\n" && line != "\n");

  // Read body
  String body;
  unsigned long readStart = millis();
  while (millis() - readStart < responseTimeoutMs) {
    while (client.available()) {
      body += static_cast<char>(client.read());
    }
    if (!client.connected()) {
      break;
    }
    delay(10);
  }

  client.stop();

  int16_t spine = 0;
  int16_t page = 0;
  bool haveSpine = parseIntField(body, "spine_index", spine);
  bool havePage = parseIntField(body, "page", page);

  if (!haveSpine || !havePage) {
    Serial.printf("[%lu] [SYN] Pull ok but missing spine/page for '%s'\\n", millis(), documentPath.c_str());
    setLastSyncError("Missing spine/page in response");
    return false;
  }

  spineIndexOut = spine;
  pageOut = page;
  Serial.printf("[%lu] [SYN] Pulled remote progress: spine=%d, page=%d for '%s'\\n", millis(), spine, page,
                documentPath.c_str());
  setLastSyncError("");
  return true;
}

#else  // no ENABLE_PROGRESS_SYNC

bool syncProgress(const std::string& documentPath, uint8_t percentage, int16_t spineIndex, int16_t page) {
  // Stub implementation: just log. Real HTTP sync is compiled in only when
  // ENABLE_PROGRESS_SYNC and related macros are defined in build_flags.
  Serial.printf(
      "[%lu] [SYN] (stub) Would sync progress: doc=%s, percentage=%u%%, spine=%d, page=%d\n", millis(),
      documentPath.c_str(), static_cast<unsigned>(percentage), static_cast<int>(spineIndex),
      static_cast<int>(page));
  return true;
}

bool fetchRemoteProgress(const std::string& documentPath, int16_t& spineIndexOut, int16_t& pageOut) {
  (void)documentPath;
  (void)spineIndexOut;
  (void)pageOut;
  // Stub: pulling is disabled when ENABLE_PROGRESS_SYNC is not defined.
  return false;
}

#endif

const char* getLastSyncError() {
  return lastSyncError.c_str();
}

void clearLastSyncError() {
  setLastSyncError("");
}
