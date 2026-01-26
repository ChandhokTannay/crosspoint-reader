#include "BookSyncClient.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstdio>

#include "BookSyncConfig.h"

namespace {

std::string lastBookSyncError;

void setLastBookSyncError(const char* msg) {
  lastBookSyncError = msg ? msg : "";
}

bool ensureWifiConnectedForBooks(unsigned long timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) {
    setLastBookSyncError("");
    return true;
  }

  Serial.printf("[%lu] [BKS] Connecting WiFi to SSID '%s'...\n", millis(), BOOK_SYNC_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(BOOK_SYNC_SSID, BOOK_SYNC_PASSWORD);

  const auto start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [BKS] WiFi connect timeout after %lums\n", millis(), timeoutMs);
    setLastBookSyncError("WiFi connect timeout");
    return false;
  }

  Serial.printf("[%lu] [BKS] WiFi connected, IP=%s\n", millis(), WiFi.localIP().toString().c_str());
  setLastBookSyncError("");
  return true;
}

struct ParsedBaseUrl {
  String host;
  uint16_t port;
  String basePath;  // leading '/') or empty
};

ParsedBaseUrl parseBaseUrl() {
  String url = BOOK_SYNC_BASE_URL;
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

  ParsedBaseUrl out;
  out.host = host;
  out.port = port;
  out.basePath = path;
  return out;
}

// Helper to URL-encode a simple filename for use in query string.
String urlEncode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

bool readHttpStatusLine(WiFiClient& client, int& statusCode, unsigned long timeoutMs = 3000) {
  const unsigned long start = millis();
  while (!client.available() && millis() - start < timeoutMs) {
    delay(50);
  }
  if (!client.available()) {
    setLastBookSyncError("HTTP response timeout");
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[%lu] [BKS] HTTP status line: %s\n", millis(), statusLine.c_str());

  statusCode = 0;
  if (!statusLine.startsWith("HTTP/")) {
    setLastBookSyncError("Bad status line");
    return false;
  }
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace <= 0) {
    setLastBookSyncError("Bad status line");
    return false;
  }
  int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  String codeStr = (secondSpace > firstSpace) ? statusLine.substring(firstSpace + 1, secondSpace)
                                              : statusLine.substring(firstSpace + 1);
  statusCode = codeStr.toInt();
  return true;
}

void skipHttpHeaders(WiFiClient& client, unsigned long timeoutMs = 3000) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 2 || line == "\r\n" || line == "\n") {
      break;
    }
  }
}

}  // namespace

bool fetchPendingBooks(std::vector<PendingBook>& outBooks) {
  outBooks.clear();

  if (!ensureWifiConnectedForBooks()) {
    if (getLastBookSyncError()[0] == '\0') {
      setLastBookSyncError("WiFi not connected");
    }
    return false;
  }

  ParsedBaseUrl base = parseBaseUrl();

  WiFiClient client;
  const unsigned long connectTimeoutMs = 3000;
  const unsigned long connectStart = millis();
  Serial.printf("[%lu] [BKS] Connecting to %s:%u for pending books...\n", millis(), base.host.c_str(), base.port);
  while (!client.connected() && millis() - connectStart < connectTimeoutMs) {
    if (client.connect(base.host.c_str(), base.port)) {
      break;
    }
    delay(100);
  }

  if (!client.connected()) {
    Serial.printf("[%lu] [BKS] HTTP connect timeout to %s:%u\n", millis(), base.host.c_str(), base.port);
    setLastBookSyncError("HTTP connect timeout");
    client.stop();
    return false;
  }

  String path = "/books/pending";

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(base.host);
  if (base.port != 80) {
    client.print(":");
    client.print(base.port);
  }
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  int status = 0;
  if (!readHttpStatusLine(client, status)) {
    client.stop();
    return false;
  }

  if (status != 200) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "HTTP %d", status);
    setLastBookSyncError(buf);
    client.stop();
    return false;
  }

  skipHttpHeaders(client);

  String body;
  unsigned long readStart = millis();
  const unsigned long responseTimeoutMs = 3000;
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

  // Very small and naive JSON parsing for: {"ok": true, "books":[{"name":"foo.epub","size":123}]}
  int booksIdx = body.indexOf("\"books\"");
  if (booksIdx < 0) {
    setLastBookSyncError("Missing books field");
    return false;
  }
  int bracket = body.indexOf('[', booksIdx);
  if (bracket < 0) {
    setLastBookSyncError("Missing books array");
    return false;
  }
  int endBracket = body.indexOf(']', bracket);
  if (endBracket < 0) {
    setLastBookSyncError("Unterminated books array");
    return false;
  }

  String arr = body.substring(bracket + 1, endBracket);
  int pos = 0;
  while (pos < arr.length()) {
    int nameKey = arr.indexOf("\"name\"", pos);
    if (nameKey < 0) {
      break;
    }
    int colon = arr.indexOf(':', nameKey);
    int firstQuote = arr.indexOf('"', colon + 1);
    if (firstQuote < 0) {
      break;
    }
    int secondQuote = arr.indexOf('"', firstQuote + 1);
    if (secondQuote < 0) {
      break;
    }
    String name = arr.substring(firstQuote + 1, secondQuote);

    // Optional size field
    uint32_t size = 0;
    int sizeKey = arr.indexOf("\"size\"", secondQuote);
    if (sizeKey >= 0) {
      int colon2 = arr.indexOf(':', sizeKey);
      if (colon2 > 0) {
        int idx = colon2 + 1;
        while (idx < arr.length() && (arr[idx] == ' ' || arr[idx] == '\t')) {
          ++idx;
        }
        uint32_t value = 0;
        bool any = false;
        while (idx < arr.length() && arr[idx] >= '0' && arr[idx] <= '9') {
          any = true;
          value = value * 10 + (arr[idx] - '0');
          ++idx;
        }
        if (any) {
          size = value;
        }
      }
    }

    PendingBook b;
    b.name = name.c_str();
    b.size = size;
    outBooks.push_back(b);

    pos = secondQuote + 1;
  }

  setLastBookSyncError("");
  return true;
}

bool downloadBook(const std::string& name, const std::string& targetPathOnSd) {
  if (!ensureWifiConnectedForBooks()) {
    if (getLastBookSyncError()[0] == '\0') {
      setLastBookSyncError("WiFi not connected");
    }
    return false;
  }

  ParsedBaseUrl base = parseBaseUrl();

  WiFiClient client;
  const unsigned long connectTimeoutMs = 3000;
  const unsigned long connectStart = millis();
  Serial.printf("[%lu] [BKS] Connecting to %s:%u for download...\n", millis(), base.host.c_str(), base.port);
  while (!client.connected() && millis() - connectStart < connectTimeoutMs) {
    if (client.connect(base.host.c_str(), base.port)) {
      break;
    }
    delay(100);
  }

  if (!client.connected()) {
    Serial.printf("[%lu] [BKS] HTTP connect timeout to %s:%u\n", millis(), base.host.c_str(), base.port);
    setLastBookSyncError("HTTP connect timeout");
    client.stop();
    return false;
  }

  String nameStr = name.c_str();
  String encodedName = urlEncode(nameStr);
  String path = "/books/download?name=" + encodedName;

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(base.host);
  if (base.port != 80) {
    client.print(":");
    client.print(base.port);
  }
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  int status = 0;
  if (!readHttpStatusLine(client, status)) {
    client.stop();
    return false;
  }

  if (status != 200) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "HTTP %d", status);
    setLastBookSyncError(buf);
    client.stop();
    return false;
  }

  skipHttpHeaders(client);

  // Ensure /Books directory exists (this matches the library browser view
  // and is case-insensitive on typical SD filesystems).
  if (!SD.exists("/Books")) {
    SD.mkdir("/Books");
  }

  File f = SD.open(targetPathOnSd.c_str(), FILE_WRITE);
  if (!f) {
    setLastBookSyncError("Failed to open SD file");
    client.stop();
    return false;
  }

  size_t totalWritten = 0;
  unsigned long readStart = millis();
  // Generous timeout for large EPUBs; this is reset each time we
  // successfully read data so we only time out on long idle periods.
  const unsigned long responseTimeoutMs = 60000;

  while (millis() - readStart < responseTimeoutMs) {
    while (client.available()) {
      uint8_t buf[1024];
      int n = client.read(buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      size_t written = f.write(buf, static_cast<size_t>(n));
      if (written != static_cast<size_t>(n)) {
        setLastBookSyncError("SD write failed");
        f.close();
        SD.remove(targetPathOnSd.c_str());
        client.stop();
        return false;
      }
      totalWritten += written;
      readStart = millis();  // extend timeout while data flows
    }
    if (!client.connected() && !client.available()) {
      break;
    }
    delay(10);
  }

  f.close();
  client.stop();

  if (totalWritten == 0) {
    setLastBookSyncError("Empty response");
    SD.remove(targetPathOnSd.c_str());
    return false;
  }

  Serial.printf("[%lu] [BKS] Downloaded %s to %s (%u bytes)\n", millis(), name.c_str(), targetPathOnSd.c_str(),
                static_cast<unsigned>(totalWritten));
  setLastBookSyncError("");
  return true;
}

bool ackBookDownloaded(const std::string& name) {
  if (!ensureWifiConnectedForBooks()) {
    if (getLastBookSyncError()[0] == '\0') {
      setLastBookSyncError("WiFi not connected");
    }
    return false;
  }

  ParsedBaseUrl base = parseBaseUrl();

  WiFiClient client;
  const unsigned long connectTimeoutMs = 3000;
  const unsigned long connectStart = millis();
  Serial.printf("[%lu] [BKS] Connecting to %s:%u for ack...\n", millis(), base.host.c_str(), base.port);
  while (!client.connected() && millis() - connectStart < connectTimeoutMs) {
    if (client.connect(base.host.c_str(), base.port)) {
      break;
    }
    delay(100);
  }

  if (!client.connected()) {
    Serial.printf("[%lu] [BKS] HTTP connect timeout to %s:%u\n", millis(), base.host.c_str(), base.port);
    setLastBookSyncError("HTTP connect timeout");
    client.stop();
    return false;
  }

  String payload = "{";
  payload += "\"name\":\"";
  payload += name.c_str();
  payload += "\"}";

  String path = "/books/ack";

  client.print("POST ");
  client.print(path);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(base.host);
  if (base.port != 80) {
    client.print(":");
    client.print(base.port);
  }
  client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: ");
  client.print(payload.length());
  client.print("\r\n\r\n");
  client.print(payload);

  int status = 0;
  if (!readHttpStatusLine(client, status)) {
    client.stop();
    return false;
  }

  client.stop();

  if (status >= 200 && status < 400) {
    setLastBookSyncError("");
    return true;
  }

  char buf[32];
  std::snprintf(buf, sizeof(buf), "HTTP %d", status);
  setLastBookSyncError(buf);
  return false;
}

const char* getLastBookSyncError() {
  return lastBookSyncError.c_str();
}

void clearLastBookSyncError() {
  setLastBookSyncError("");
}
