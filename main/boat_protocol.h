#ifndef BOAT_PROTOCOL_H
#define BOAT_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi AP created by the boat. */
#define BOAT_SSID_PREFIX    "XC_BOT-"
#define BOAT_AP_PASSWORD    "BOT88888888"

/* UDP command channel. Destination is the DHCP gateway (the boat in SoftAP
 * mode); BOAT_UDP_TARGET_IP is only a fallback when the gateway is 0. */
#define BOAT_UDP_PORT       8888
#define BOAT_UDP_TARGET_IP  "192.168.4.1"

#define BOAT_PROTO_VERSION  1
#define BOAT_PACKET_MAGIC0  'X'
#define BOAT_PACKET_MAGIC1  'C'

#define BOAT_THROTTLE_MAX   100

#define BOAT_FLAG_LIGHT     0x01
#define BOAT_FLAG_BUZZER    0x02
#define BOAT_FLAG_BRAKE     0x04

/* 8-byte command packet, sent at 20 Hz. All fields are single bytes, so the
 * layout is independent of host endianness. */
typedef struct __attribute__((packed)) {
    uint8_t magic0;
    uint8_t magic1;
    uint8_t version;
    uint8_t seq;
    uint8_t left;   /* left thruster, 0..100 */
    uint8_t right;  /* right thruster, 0..100 */
    uint8_t flags;  /* BOAT_FLAG_* */
    uint8_t checksum;
} boat_packet_t;

_Static_assert(sizeof(boat_packet_t) == 8, "boat_packet_t must be 8 bytes");

/* XOR of bytes 0..6. */
static inline uint8_t boat_packet_checksum(const boat_packet_t* packet)
{
    return (uint8_t)(packet->magic0 ^ packet->magic1 ^ packet->version ^
                     packet->seq ^ packet->left ^ packet->right ^ packet->flags);
}

static inline void boat_packet_finalize(boat_packet_t* packet)
{
    packet->checksum = boat_packet_checksum(packet);
}

#ifdef __cplusplus
}
#endif

#endif /* BOAT_PROTOCOL_H */
