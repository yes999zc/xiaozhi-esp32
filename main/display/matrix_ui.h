// Matrix-style data stream overlay for the CHD ESP32-S3-BOX display.
//
// Why an overlay: the stock XiaoZhi UI (smiley emoji + chat bubbles) is what the
// vendor ships. This draws a full-screen canvas on the top LVGL layer, so the
// native UI keeps working underneath (subtitles, gestures) but the screen reads
// as a live data stream instead of a smiley face.
//
// Rendering is done by writing pixels straight into an RGB565 canvas buffer:
// LVGL's draw API is unnecessary overhead for a fade + glyph blit, and the fade
// is a plain 0.82 multiply over 153k pixels (~2 ms on the S3 at 240 MHz).
#ifndef MATRIX_UI_H
#define MATRIX_UI_H

#include <lvgl.h>
#include <string>

// Create/show the overlay. Safe to call more than once.
void matrix_ui_start(int width, int height);
bool matrix_ui_active();

// Feed a line of real conversation into the stream (role: "user"/"assistant").
void matrix_ui_push(const char *role, const char *text);

// Short ASCII status shown at the bottom, e.g. "LISTENING", "TOOL take_photo".
void matrix_ui_status(const char *text);

#endif  // MATRIX_UI_H
