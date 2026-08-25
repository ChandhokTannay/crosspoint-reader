#pragma once

/**
 * Shared headless WiFi helpers for background KOReader sync (sleep push and
 * open pull). Connects to saved networks with a bounded budget and an
 * optional abort flag checked between waits.
 */
namespace KOReaderNet {
// Connect to a saved WiFi network, trying the last-connected first.
// Returns false when no credentials, none reachable, deadline passed, or
// abort was requested. Loads the credential store itself.
bool connectSavedWifi(unsigned long deadlineMs, const volatile bool* abortFlag = nullptr);
void wifiOff();
}  // namespace KOReaderNet
