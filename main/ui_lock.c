#include "ui_lock.h"

_lock_t lvgl_api_lock;

void ui_lock_init(void) {
    _lock_init(&lvgl_api_lock);
}

void ui_lock_acquire(void) {
    _lock_acquire(&lvgl_api_lock);
}

void ui_lock_release(void) {
    _lock_release(&lvgl_api_lock);
}
