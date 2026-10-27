#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEY_UP = 0,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_A,
    KEY_B,
    KEY_COUNT
} KeyCode;

/* Configure the six buttons (active-low). */
void input_init(void);

/* Start the 10 ms polling task. */
void input_start(void);

/* Debounced held state; safe to call from any task. */
bool input_is_held(KeyCode key);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_H */
