#include "CrossPointWebServer.h"

#include <SD.h>
#include <WiFi.h>

#include <algorithm>

#include "config.h"
#include "html/FilesPageFooterHtml.generated.h"
#include "html/FilesPageHeaderHtml.generated.h"
#include "html/HomePageHtml.generated.h"

namespace {

// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
const size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);

// Helper function to escape HTML special characters to prevent XSS
String escapeHtml(const String& input) {
  String output;
  output.reserve(input.length() * 1.1);  // Pre-allocate with some extra space

  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    Serial.printf("[%lu] [WEB] Web server already running\n", millis());
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [WEB] Cannot start webserver - WiFi not connected\n", millis());
    return;
  }

  Serial.printf("[%lu] [WEB] [MEM] Free heap before begin: %d bytes\n", millis(), ESP.getFreeHeap());

  Serial.printf("[%lu] [WEB] Creating web server on port %d...\n", millis(), port);
  server = new WebServer(port);
  Serial.printf("[%lu] [WEB] [MEM] Free heap after WebServer allocation: %d bytes\n", millis(), ESP.getFreeHeap());

  if (!server) {
    Serial.printf("[%lu] [WEB] Failed to create WebServer!\n", millis());
    return;
  }

  // Setup routes
  Serial.printf("[%lu] [WEB] Setting up routes...\n", millis());
  server->on("/", HTTP_GET, [this]() { handleRoot(); });
  server->on("/status", HTTP_GET, [this]() { handleStatus(); });
  server->on("/files", HTTP_GET, [this]() { handleFileList(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this]() { handleUploadPost(); }, [this]() { handleUpload(); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this]() { handleCreateFolder(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this]() { handleDelete(); });

  // Move/rename file or folder endpoint
  server->on("/move", HTTP_POST, [this]() { handleMove(); });

  server->onNotFound([this]() { handleNotFound(); });
  Serial.printf("[%lu] [WEB] [MEM] Free heap after route setup: %d bytes\n", millis(), ESP.getFreeHeap());

  server->begin();
  running = true;

  Serial.printf("[%lu] [WEB] Web server started on port %d\n", millis(), port);
  Serial.printf("[%lu] [WEB] Access at http://%s/\n", millis(), WiFi.localIP().toString().c_str());
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server.begin(): %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    Serial.printf("[%lu] [WEB] stop() called but already stopped (running=%d, server=%p)\n", millis(), running, server);
    return;
  }

  Serial.printf("[%lu] [WEB] STOP INITIATED - setting running=false first\n", millis());
  running = false;  // Set this FIRST to prevent handleClient from using server

  Serial.printf("[%lu] [WEB] [MEM] Free heap before stop: %d bytes\n", millis(), ESP.getFreeHeap());

  // Add delay to allow any in-flight handleClient() calls to complete
  delay(100);
  Serial.printf("[%lu] [WEB] Waited 100ms for handleClient to finish\n", millis());

  server->stop();
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server->stop(): %d bytes\n", millis(), ESP.getFreeHeap());

  // Add another delay before deletion to ensure server->stop() completes
  delay(50);
  Serial.printf("[%lu] [WEB] Waited 50ms before deleting server\n", millis());

  delete server;
  server = nullptr;

  Serial.printf("[%lu] [WEB] Web server stopped and deleted\n", millis());
  Serial.printf("[%lu] [WEB] [MEM] Free heap after delete server: %d bytes\n", millis(), ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  Serial.printf("[%lu] [WEB] [MEM] Free heap final: %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    Serial.printf("[%lu] [WEB] WARNING: handleClient called with null server!\n", millis());
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    Serial.printf("[%lu] [WEB] handleClient active, server running on port %d\n", millis(), port);
    lastDebugPrint = millis();
  }

  server->handleClient();
}

void CrossPointWebServer::handleRoot() {
  if (!server) {
    Serial.printf("[%lu] [WEB] handleRoot called with null server!\n", millis());
    return;
  }

  // Very small, safe homepage: just shows basic info and a link to /files.
  String html;
  html.reserve(1024);
  html += "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Tannay's Reader</title></head><body>";
  html += "<h1>Tannay's Reader</h1>";
  html += "<p><strong>Version:</strong> ";
  html += CROSSPOINT_VERSION;
  html += "</p><p><strong>IP:</strong> ";
  html += WiFi.localIP().toString();
  html += "</p><p><strong>Free heap:</strong> ";
  html += String(ESP.getFreeHeap());
  html += " bytes</p>";
  html += "<p><a href=\"/files\">Open File Manager</a></p>";
  html += "</body></html>";

  server->send(200, "text/html", html);
  Serial.printf("[%lu] [WEB] Served SIMPLE root page\n", millis());
}

void CrossPointWebServer::handleNotFound() {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() {
  String json = "{";
  json += "\"version\":\"" + String(CROSSPOINT_VERSION) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";

  server->send(200, "application/json", json);
}

std::vector<FileInfo> CrossPointWebServer::scanFiles(const char* path) {
  std::vector<FileInfo> files;

  File root = SD.open(path);
  if (!root) {
    Serial.printf("[%lu] [WEB] Failed to open directory: %s\n", millis(), path);
    return files;
  }

  if (!root.isDirectory()) {
    Serial.printf("[%lu] [WEB] Not a directory: %s\n", millis(), path);
    root.close();
    return files;
  }

  Serial.printf("[%lu] [WEB] Scanning files in: %s\n", millis(), path);

  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());

    // Skip hidden items (starting with ".")
    bool shouldHide = fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (fileName.equals(HIDDEN_ITEMS[i])) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      files.push_back(info);
    }

    file.close();
    file = root.openNextFile();
  }
  root.close();

  Serial.printf("[%lu] [WEB] Found %d items (files and folders)\n", millis(), files.size());
  return files;
}

String CrossPointWebServer::formatFileSize(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + " B";
  } else if (bytes < 1024 * 1024) {
    return String(bytes / 1024.0, 1) + " KB";
  } else {
    return String(bytes / (1024.0 * 1024.0), 1) + " MB";
  }
}

bool CrossPointWebServer::isEpubFile(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

void CrossPointWebServer::handleFileList() {
  // Extremely minimal file listing page, streaming directly from SD
  // without building intermediate vectors to avoid crashes on deeper paths.
  if (!server) {
    Serial.printf("[%lu] [WEB] handleFileList called with null server!\n", millis());
    return;
  }

  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  // Open the requested directory directly from SD.
  File dir = SD.open(currentPath.c_str());
  if (!dir || !dir.isDirectory()) {
    Serial.printf("[%lu] [WEB] handleFileList: not a directory or cannot open: %s\n",
                  millis(), currentPath.c_str());
    server->send(404, "text/plain", "Directory not found");
    if (dir) dir.close();
    return;
  }

  String html;
  html.reserve(2048);
  html += "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Files</title></head><body>";
  html += "<h1>Files at ";
  html += escapeHtml(currentPath);
  html += "</h1>";

  // Up one level link (if not at root)
  if (currentPath != "/") {
    String parent = currentPath;
    int slash = parent.lastIndexOf('/');
    if (slash > 0) {
      parent = parent.substring(0, slash);
    } else {
      parent = "/";
    }
    html += "<p><a href=\"/files?path=";
    html += escapeHtml(parent);
    html += "\">Up</a></p>";
  }

  // Begin list
  html += "<ul>";

  // Iterate directory entries one by one to keep memory usage low.
  for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    String name = String(file.name());
    bool isDir = file.isDirectory();

    // Skip hidden items and the special/system ones, same as scanFiles.
    bool shouldHide = name.startsWith(".");
    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (name.equals(HIDDEN_ITEMS[i])) {
          shouldHide = true;
          break;
        }
      }
    }
    if (shouldHide) {
      file.close();
      continue;
    }

    String fullPath = currentPath;
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += name;

    html += "<li>";
    if (isDir) {
      html += "<a href=\"/files?path=";
      html += escapeHtml(fullPath);
      html += "\">";
      html += escapeHtml(name);
      html += "/</a>";
    } else {
      html += escapeHtml(name);
      html += " (";
      html += formatFileSize(file.size());
      html += ")";
    }
    html += "</li>";

    file.close();
  }

  dir.close();

  html += "</ul>";
  html += "<p><a href=\"/\">Back to Home</a></p></body></html>";

  server->send(200, "text/html", html);
  Serial.printf("[%lu] [WEB] Served SIMPLE file listing page for path: %s\n",
                millis(), currentPath.c_str());
}

// Static variables for upload handling
static File uploadFile;
static String uploadFileName;
static String uploadPath = "/";
static size_t uploadSize = 0;
static bool uploadSuccess = false;
static String uploadError = "";

void CrossPointWebServer::handleUpload() {
  static unsigned long lastWriteTime = 0;
  static unsigned long uploadStartTime = 0;
  static size_t lastLoggedSize = 0;

  // Safety check: ensure server is still valid
  if (!running || !server) {
    Serial.printf("[%lu] [WEB] [UPLOAD] ERROR: handleUpload called but server not running!\n", millis());
    return;
  }

  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadStartTime = millis();
    lastWriteTime = millis();
    lastLoggedSize = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      uploadPath = server->arg("path");
      // Ensure path starts with /
      if (!uploadPath.startsWith("/")) {
        uploadPath = "/" + uploadPath;
      }
      // Remove trailing slash unless it's root
      if (uploadPath.length() > 1 && uploadPath.endsWith("/")) {
        uploadPath = uploadPath.substring(0, uploadPath.length() - 1);
      }
    } else {
      uploadPath = "/";
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] START: %s to path: %s\n", millis(), uploadFileName.c_str(), uploadPath.c_str());
    Serial.printf("[%lu] [WEB] [UPLOAD] Free heap: %d bytes\n", millis(), ESP.getFreeHeap());

    // Validate file extension
    if (!isEpubFile(uploadFileName)) {
      uploadError = "Only .epub files are allowed";
      Serial.printf("[%lu] [WEB] [UPLOAD] REJECTED - not an epub file\n", millis());
      return;
    }

    // Create file path
    String filePath = uploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += uploadFileName;

    // Check if file already exists
    if (SD.exists(filePath.c_str())) {
      Serial.printf("[%lu] [WEB] [UPLOAD] Overwriting existing file: %s\n", millis(), filePath.c_str());
      SD.remove(filePath.c_str());
    }

    // Open file for writing
    uploadFile = SD.open(filePath.c_str(), FILE_WRITE);
    if (!uploadFile) {
      uploadError = "Failed to create file on SD card";
      Serial.printf("[%lu] [WEB] [UPLOAD] FAILED to create file: %s\n", millis(), filePath.c_str());
      return;
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] File created successfully: %s\n", millis(), filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      unsigned long writeStartTime = millis();
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      unsigned long writeEndTime = millis();
      unsigned long writeDuration = writeEndTime - writeStartTime;

      if (written != upload.currentSize) {
        uploadError = "Failed to write to SD card - disk may be full";
        uploadFile.close();
        Serial.printf("[%lu] [WEB] [UPLOAD] WRITE ERROR - expected %d, wrote %d\n", millis(), upload.currentSize,
                      written);
      } else {
        uploadSize += written;

        // Log progress every 50KB or if write took >100ms
        if (uploadSize - lastLoggedSize >= 51200 || writeDuration > 100) {
          unsigned long timeSinceStart = millis() - uploadStartTime;
          unsigned long timeSinceLastWrite = millis() - lastWriteTime;
          float kbps = (uploadSize / 1024.0) / (timeSinceStart / 1000.0);

          Serial.printf(
              "[%lu] [WEB] [UPLOAD] Progress: %d bytes (%.1f KB), %.1f KB/s, write took %lu ms, gap since last: %lu "
              "ms\n",
              millis(), uploadSize, uploadSize / 1024.0, kbps, writeDuration, timeSinceLastWrite);
          lastLoggedSize = uploadSize;
        }
        lastWriteTime = millis();
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();

      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        Serial.printf("[%lu] [WEB] Upload complete: %s (%d bytes)\n", millis(), uploadFileName.c_str(), uploadSize);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
      // Try to delete the incomplete file
      String filePath = uploadPath;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += uploadFileName;
      SD.remove(filePath.c_str());
    }
    uploadError = "Upload aborted";
    Serial.printf("[%lu] [WEB] Upload aborted\n", millis());
  }
}

void CrossPointWebServer::handleUploadPost() {
  if (uploadSuccess) {
    server->send(200, "text/plain", "File uploaded successfully: " + uploadFileName);
  } else {
    String error = uploadError.isEmpty() ? "Unknown error during upload" : uploadError;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  Serial.printf("[%lu] [WEB] Creating folder: %s\n", millis(), folderPath.c_str());

  // Check if already exists
  if (SD.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (SD.mkdir(folderPath.c_str())) {
    Serial.printf("[%lu] [WEB] Folder created successfully: %s\n", millis(), folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    Serial.printf("[%lu] [WEB] Failed to create folder: %s\n", millis(), folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleMove() {
  // Move/rename a file or folder using SD.rename(from, to).
  if (!server->hasArg("from") || !server->hasArg("to")) {
    server->send(400, "text/plain", "Missing from/to");
    return;
  }

  String fromPath = server->arg("from");
  String toPath = server->arg("to");

  if (!fromPath.startsWith("/")) fromPath = "/" + fromPath;
  if (!toPath.startsWith("/")) toPath = "/" + toPath;

  if (!SD.exists(fromPath.c_str())) {
    server->send(404, "text/plain", "Source not found");
    return;
  }

  // Basic safety: don't allow renaming root.
  if (fromPath == "/") {
    server->send(400, "text/plain", "Cannot move root");
    return;
  }

  bool ok = SD.rename(fromPath.c_str(), toPath.c_str());
  if (!ok) {
    Serial.printf("[%lu] [WEB] Move failed: %s -> %s\n", millis(), fromPath.c_str(), toPath.c_str());
    server->send(500, "text/plain", "Move failed");
    return;
  }

  Serial.printf("[%lu] [WEB] Move succeeded: %s -> %s\n", millis(), fromPath.c_str(), toPath.c_str());
  server->send(200, "text/plain", "OK");
}

void CrossPointWebServer::handleDelete() {
  // Get path from form data
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  String itemType = server->hasArg("type") ? server->arg("type") : "file";

  // Validate path
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Cannot delete root directory");
    return;
  }

  // Ensure path starts with /
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  // Security check: prevent deletion of protected items
  String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  // Check if item starts with a dot (hidden/system file)
  if (itemName.startsWith(".")) {
    Serial.printf("[%lu] [WEB] Delete rejected - hidden/system item: %s\n", millis(), itemPath.c_str());
    server->send(403, "text/plain", "Cannot delete system files");
    return;
  }

  // Check against explicitly protected items
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      Serial.printf("[%lu] [WEB] Delete rejected - protected item: %s\n", millis(), itemPath.c_str());
      server->send(403, "text/plain", "Cannot delete protected items");
      return;
    }
  }

  // Check if item exists
  if (!SD.exists(itemPath.c_str())) {
    Serial.printf("[%lu] [WEB] Delete failed - item not found: %s\n", millis(), itemPath.c_str());
    server->send(404, "text/plain", "Item not found");
    return;
  }

  Serial.printf("[%lu] [WEB] Attempting to delete %s: %s\n", millis(), itemType.c_str(), itemPath.c_str());

  bool success = false;

  if (itemType == "folder") {
    // For folders, try to remove (will fail if not empty)
    File dir = SD.open(itemPath.c_str());
    if (dir && dir.isDirectory()) {
      // Check if folder is empty
      File entry = dir.openNextFile();
      if (entry) {
        // Folder is not empty
        entry.close();
        dir.close();
        Serial.printf("[%lu] [WEB] Delete failed - folder not empty: %s\n", millis(), itemPath.c_str());
        server->send(400, "text/plain", "Folder is not empty. Delete contents first.");
        return;
      }
      dir.close();
    }
    success = SD.rmdir(itemPath.c_str());
  } else {
    // For files, use remove
    success = SD.remove(itemPath.c_str());
  }

  if (success) {
    Serial.printf("[%lu] [WEB] Successfully deleted: %s\n", millis(), itemPath.c_str());
    server->send(200, "text/plain", "Deleted successfully");
  } else {
    Serial.printf("[%lu] [WEB] Failed to delete: %s\n", millis(), itemPath.c_str());
    server->send(500, "text/plain", "Failed to delete item");
  }
}
