#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// Simple representation of a pending book on the host server.
struct PendingBook {
  std::string name;
  uint32_t size;
};

// Fetch list of pending books from the host server.
// Returns true on success and fills outBooks; false on error.
bool fetchPendingBooks(std::vector<PendingBook>& outBooks);

// Download a specific book by name from the host server to the given
// absolute SD path (e.g. "/books/foo.epub").
// Returns true on success.
bool downloadBook(const std::string& name, const std::string& targetPathOnSd);

// Acknowledge that a book has been downloaded so the server can archive it.
// Returns true on HTTP 2xx/3xx.
bool ackBookDownloaded(const std::string& name);

// Retrieve / clear the last book sync error message.
const char* getLastBookSyncError();
void clearLastBookSyncError();
