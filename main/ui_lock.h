#ifndef UI_LOCK_H
#define UI_LOCK_H

#include <sys/lock.h>

#ifdef __cplusplus
extern "C" {
#endif

extern _lock_t lvgl_api_lock;

void ui_lock_init(void);
void ui_lock_acquire(void);
void ui_lock_release(void);

#ifdef __cplusplus
}
#endif

#endif
