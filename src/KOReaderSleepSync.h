#pragma once

/**
 * Best-effort KOReader progress push when the device goes to sleep from the
 * reader. Guarded by the "Sync Progress on Sleep" setting. Connects to a
 * saved WiFi network (bounded), fetches the server position, and uploads the
 * local position only when it is at or beyond the server's — never rolling
 * back further progress made on another device. Any failure falls through
 * silently; sleep proceeds regardless.
 */
namespace KOReaderSleepSync {
void attempt();
}
