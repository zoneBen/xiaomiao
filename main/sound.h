#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOUND_KEY_CLICK = 0,
    SOUND_MENU_SELECT,
    SOUND_CONNECT_OK,
    SOUND_CONNECT_FAIL,
    SOUND_CHORD
} SoundType;

void sound_init(void);
void sound_play(SoundType sound);

#ifdef __cplusplus
}
#endif

#endif /* SOUND_H */
