#pragma once

#include <stdint.h>
#include <string>

// Send reading progress for a given document path.
// percentage: 0-100
// spineIndex/page: zero-based indices within EPUB
// Returns true on HTTP 2xx/3xx, false on failure (WiFi/HTTP error).
bool syncProgress(const std::string& documentPath, uint8_t percentage, int16_t spineIndex, int16_t page);

// Fetch remote progress for a document, if available.
// Returns true and fills spineIndex/page on success.
bool fetchRemoteProgress(const std::string& documentPath, int16_t& spineIndexOut, int16_t& pageOut);

// Retrieve the last sync error (for on-screen display). Empty string if none.
const char* getLastSyncError();
// Clear the last sync error.
void clearLastSyncError();
