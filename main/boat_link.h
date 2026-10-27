#ifndef BOAT_LINK_H
#define BOAT_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOAT_LINK_MAX_APS 8

typedef enum {
    LINK_DISCONNECTED = 0,
    LINK_CONNECTING,
    LINK_CONNECTED,
    LINK_CONNECT_FAILED
} BoatLinkState;

typedef enum {
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_DONE
} BoatScanState;

typedef enum {
    BOAT_FAIL_NONE = 0,
    BOAT_FAIL_PASSWORD,
    BOAT_FAIL_NOT_FOUND,
    BOAT_FAIL_TIMEOUT,
    BOAT_FAIL_OTHER
} BoatFailReason;

typedef struct {
    char ssid[33];
    int8_t rssi;
} BoatApInfo;

/* Initialize NVS/WiFi, start the 20 Hz UDP task and try to reconnect to the
 * last boat stored in NVS. Returns false only for fatal init failures. */
bool boat_link_init(void);

void boat_link_start_scan(void);
void boat_link_stop_scan(void);
BoatScanState boat_link_get_scan_state(void);
int boat_link_copy_aps(BoatApInfo* out, int max);

void boat_link_connect(const char* ssid);
void boat_link_cancel_connect(void);
BoatLinkState boat_link_get_state(char* ssid_out, size_t ssid_len);
BoatFailReason boat_link_get_fail_reason(void);

/* Command state, consumed by the 20 Hz sender. */
void boat_link_set_command(uint8_t left, uint8_t right, bool brake);
void boat_link_set_outputs(bool light);
void boat_link_set_gear(uint8_t gear);
void boat_link_stop_now(void);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_LINK_H */
