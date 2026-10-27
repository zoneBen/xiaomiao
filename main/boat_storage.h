#ifndef BOAT_STORAGE_H
#define BOAT_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load the last successfully connected SSID from NVS.
 * Returns false when no valid value is stored. */
bool boat_storage_load_ssid(char* out, size_t len);

/* Persist the SSID used for the next boot's automatic reconnect. */
bool boat_storage_save_ssid(const char* ssid);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_STORAGE_H */
