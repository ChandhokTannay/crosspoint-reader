#pragma once

// WiFi + sync server configuration for progress sync.
// Moved out of platformio.ini to avoid quoting issues with SSID/password.

#ifndef PROGRESS_SYNC_SSID
#define PROGRESS_SYNC_SSID "Tan's Bakery"
#endif

#ifndef PROGRESS_SYNC_PASSWORD
#define PROGRESS_SYNC_PASSWORD "8MarcyAve$3M!"
#endif

#ifndef PROGRESS_SYNC_URL
// Default to the local Python progress server
#define PROGRESS_SYNC_URL "http://192.168.86.78:8080/syncs/progress"
#endif
