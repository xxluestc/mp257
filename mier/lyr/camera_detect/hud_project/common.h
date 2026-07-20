#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define UDP_PORT        8888
#define UDP_BUFFER_SIZE 512
#define RECV_TIMEOUT_MS 3000

#define OLED_I2C_DEV    "/dev/i2c-3"
#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64

#define TURN_UNKNOWN        0
#define TURN_SELF_CAR       1
#define TURN_LEFT           2
#define TURN_RIGHT          3
#define TURN_SLIGHT_LEFT    4
#define TURN_SLIGHT_RIGHT   5
#define TURN_BACK_LEFT      6
#define TURN_BACK_RIGHT     7
#define TURN_UTURN_LEFT     8
#define TURN_STRAIGHT       9
#define TURN_VIA_POINT      10
#define TURN_ROUNDABOUT     11
#define TURN_EXIT_ROUNDABOUT 12
#define TURN_SERVICE        13
#define TURN_TOLL           14
#define TURN_DESTINATION    15
#define TURN_UTURN_RIGHT    19
#define MAX_EMERGENCY_CONTACTS 10
extern char emergency_contacts[MAX_EMERGENCY_CONTACTS][20];
extern int emergency_contact_count;

typedef struct {
    int turn;
    int distance;
    char direction_text[32];
} NavData;

#endif
