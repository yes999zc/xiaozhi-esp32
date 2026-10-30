# lvgl_kawaii_face — MIT, https://github.com/0015/lvgl_kawaii_face
#
# Local fork notes (see LOCAL_PATCHES.md):
#   * REQUIRES lvgl__lvgl only: the component is consumed from this project's
#     components/ directory, so the managed component name is used directly and
#     no idf_component.yml manifest is shipped (it would make the component
#     manager re-resolve lvgl/lvgl from the registry).
#   * esp_lvgl_port is NOT linked here. lock/unlock callbacks are injected by
#     the board (face_set_lvgl_lock_fns) with the LVGL port recursive mutex,
#     which keeps the component independent of esp_lvgl_port.
idf_component_register(
    SRCS "lvgl_kawaii_face.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl__lvgl
    PRIV_REQUIRES
        log
        heap
)
