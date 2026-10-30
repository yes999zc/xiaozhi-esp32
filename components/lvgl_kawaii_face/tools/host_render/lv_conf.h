#ifndef LV_CONF_H
#define LV_CONF_H
/* Host-only LVGL configuration for the offline face renderer.
 * Do NOT copy into the firmware: the ESP build uses IDF Kconfig (CONFIG_LV_*). */
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_MEM_SIZE (256 * 1024)
#define LV_USE_DRAW_SW 1
#define LV_USE_CANVAS 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_THEME_DEFAULT 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_DEF_REFR_PERIOD 20
#endif
