#pragma once

// WiFi + book delivery server configuration.
// Modeled after SyncConfig.h used for progress sync.
//
// You can override these at compile time via build_flags, but for
// typical local use it is convenient to just edit this file.

#ifndef BOOK_SYNC_SSID
#define BOOK_SYNC_SSID "Tan's Bakery"
#endif

#ifndef BOOK_SYNC_PASSWORD
#define BOOK_SYNC_PASSWORD "8MarcyAve$3M!"
#endif

#ifndef BOOK_SYNC_BASE_URL
// Default to a local Python book server on your LAN.
// Example: http://<your-host-ip>:8081
#define BOOK_SYNC_BASE_URL "http://192.168.86.78:8081"
#endif
