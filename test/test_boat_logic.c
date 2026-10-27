/* Host-side tests for the pure boat control core.
 *
 * Build and run (from the project root):
 *   gcc -std=c11 -Wall -Wextra -Werror -I main test/test_boat_logic.c -o test/test_boat_logic
 *   ./test/test_boat_logic
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "boat_control.h"

static void expect_command(bool forward, bool back, bool left, bool right,
                           uint8_t expected_left, uint8_t expected_right,
                           bool expected_brake)
{
    uint8_t out_left = 0xAA;
    uint8_t out_right = 0xAA;
    bool out_brake = false;

    boat_compute_command(forward, back, left, right,
                         &out_left, &out_right, &out_brake);

    assert(out_left == expected_left);
    assert(out_right == expected_right);
    assert(out_brake == expected_brake);
}

static void test_thruster_mapping(void)
{
    expect_command(false, false, false, false, 0, 0, false);       /* idle */
    expect_command(true, false, false, false, 100, 100, false);    /* forward */
    expect_command(false, false, true, false, 0, 100, false);      /* left */
    expect_command(false, false, false, true, 100, 0, false);      /* right */
    expect_command(false, true, false, false, 0, 0, true);         /* back */
    expect_command(true, false, true, false, 0, 100, false);       /* forward+left */
    expect_command(true, false, false, true, 100, 0, false);       /* forward+right */
    expect_command(false, false, true, true, 0, 0, true);          /* both turns */
    expect_command(true, true, false, false, 0, 0, true);          /* back wins */
    expect_command(true, true, true, true, 0, 0, true);            /* back wins */
}

static void test_protocol_packet(void)
{
    boat_packet_t packet = {
        .magic0 = BOAT_PACKET_MAGIC0,
        .magic1 = BOAT_PACKET_MAGIC1,
        .version = BOAT_PROTO_VERSION,
        .seq = 7,
        .left = 100,
        .right = 0,
        .flags = BOAT_FLAG_LIGHT | BOAT_FLAG_BRAKE,
        .checksum = 0,
    };

    _Static_assert(sizeof(boat_packet_t) == 8, "packet must stay 8 bytes");

    boat_packet_finalize(&packet);
    assert(packet.checksum ==
           (uint8_t)(packet.magic0 ^ packet.magic1 ^ packet.version ^
                     packet.seq ^ packet.left ^ packet.right ^ packet.flags));
}

int main(void)
{
    test_thruster_mapping();
    test_protocol_packet();
    printf("boat logic tests: PASS\n");
    return 0;
}
