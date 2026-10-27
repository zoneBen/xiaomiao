#ifndef BOAT_UDP_H
#define BOAT_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sock;
    uint8_t seq;
    uint32_t fail_count;
} boat_udp_t;

void boat_udp_init(boat_udp_t* udp);
bool boat_udp_open(boat_udp_t* udp);
void boat_udp_close(boat_udp_t* udp);
void boat_udp_send(boat_udp_t* udp, esp_ip4_addr_t target,
                   uint8_t left, uint8_t right, uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_UDP_H */
