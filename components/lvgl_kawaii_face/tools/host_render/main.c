/**
 * @file main.c
 * @brief Offline (host) renderer for the kawaii face component.
 *
 * Renders the real component with the real LVGL at LV_COLOR_DEPTH 16, then dumps
 * the three canvas buffers as raw little-endian RGB565. No ESP32, no flashing,
 * no display needed - use it to answer "does the component draw the right
 * pixels, or is the device path corrupting them?".
 *
 * Build/run: ./run.sh (see README.md). Output: out/{eye_left,eye_right,mouth}.raw
 * Convert: ../raw565_to_png.py out/*.raw
 */

#include "lvgl.h"
#include "lvgl_kawaii_face.h"

#include <stdio.h>
#include <stdlib.h>

static int dump_canvas(lv_obj_t *canvas, const char *path)
{
    const void *src = lv_image_get_src(canvas);
    if (!src) {
        printf("no src for %s\n", path);
        return 1;
    }
    const lv_draw_buf_t *db = (const lv_draw_buf_t *)src;
    printf("%s: %ux%u cf=%d stride=%u data_size=%u\n", path, db->header.w, db->header.h,
           (int)db->header.cf, db->header.stride, db->data_size);
    if (db->header.cf != LV_COLOR_FORMAT_RGB565) {
        printf("  ! expected LV_COLOR_FORMAT_RGB565\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  ! cannot open %s\n", path);
        return 1;
    }
    fwrite(db->data, 1, (size_t)db->header.stride * db->header.h, f);
    fclose(f);
    return 0;
}

int main(void)
{
    lv_init();

    /* A display is only needed so LVGL has a screen object; nothing is shown. */
    lv_display_t *disp = lv_display_create(320, 320);
    static uint8_t disp_buf[320 * 320 * 2];
    lv_display_set_buffers(disp, disp_buf, NULL, sizeof(disp_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    /* Same geometry as main/boards/chd/chd-esp32s3-box: 320 px panel, short
     * side S = 320. Keep this in sync with kFacePanelSize. */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 320, 320);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    face_config_t cfg = {
        .parent = panel,
        .animation_speed = 100000, /* effectively no animation while dumping */
        .blink_interval = 100000,
        .auto_blink = false,
    };
    esp_err_t err = face_animation_init(&cfg);
    printf("face_animation_init: %d\n", (int)err);
    if (err != ESP_OK) {
        return 1;
    }
    lv_obj_update_layout(panel);

    lv_obj_t *cont = face_get_container();
    if (cont == NULL || lv_obj_get_child_count(cont) != 3) {
        printf("unexpected container: %p children=%d\n", (void *)cont,
               cont ? lv_obj_get_child_count(cont) : -1);
        return 1;
    }

    int rc = 0;
    rc |= dump_canvas(lv_obj_get_child(cont, 0), "out/eye_left.raw");
    rc |= dump_canvas(lv_obj_get_child(cont, 1), "out/eye_right.raw");
    rc |= dump_canvas(lv_obj_get_child(cont, 2), "out/mouth.raw");
    return rc;
}
