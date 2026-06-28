#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>
#include <stdbool.h>

// Sound types
typedef enum {
    SOUND_KEY_CLICK,
    SOUND_MOVE,
    SOUND_CAPTURE,
    SOUND_CHECK,
    SOUND_CHECKMATE,
    SOUND_MENU_SELECT,
    SOUND_GAME_START,
    SOUND_INVALID_MOVE,
    SOUND_MAX
} SoundType;

// Musical notes frequencies (Hz)
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988

// Initialize sound system
void sound_init(void);

// Play a specific sound effect
void sound_play(SoundType sound);

// Play a single tone
void sound_play_tone(uint32_t frequency, uint32_t duration_ms);

// Enable/disable sound effects (including key sounds)
void sound_set_effects_enabled(bool enabled);

// Get whether sound effects are enabled
bool sound_get_effects_enabled(void);

#endif
