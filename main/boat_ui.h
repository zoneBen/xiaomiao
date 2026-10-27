#ifndef BOAT_UI_H
#define BOAT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the framebuffer. Call after tft_display_init(). */
void boat_ui_init(void);

/* Start the 30 FPS UI task. */
void boat_ui_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BOAT_UI_H */
