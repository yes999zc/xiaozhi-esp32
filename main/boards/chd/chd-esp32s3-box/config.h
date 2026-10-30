#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE     24000
#define AUDIO_OUTPUT_SAMPLE_RATE    24000

#define AUDIO_INPUT_REFERENCE       true

#define AUDIO_I2S_GPIO_MCLK         GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS           GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK         GPIO_NUM_17
#define AUDIO_I2S_GPIO_DIN          GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT         GPIO_NUM_15

#define AUDIO_CODEC_PA_PIN          GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN     GPIO_NUM_8
#define AUDIO_CODEC_I2C_SCL_PIN     GPIO_NUM_18
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR     ES7210_CODEC_DEFAULT_ADDR

#define BUILTIN_LED_GPIO            GPIO_NUM_NC
#define BOOT_BUTTON_GPIO            GPIO_NUM_0
#define EXTRA_BUTTON_GPIO           GPIO_NUM_1
#define VOLUME_UP_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO     GPIO_NUM_NC

// --- Touch controller (task-29 follow-up) -----------------------------------
// The vendor factory image only ever mentions GT911 (no ft5x06/ft6336 strings
// at all), and the old code picked FT5x06 merely because *something* answered at
// 0x38 - which is why "touch panel ft6336" was reported but no events ever
// arrived. The chip answers 0xA3 = 0x64, so it is an FT6336: FT5x06 is the driver
// that runs. CHD_TOUCH_FORCE_GT911 = 1 would force GT911 again (kept as a switch).
// 2026-10-28: the chip at 0x38 answers 0xA3 = 0x64, i.e. it IS an FT6336; GT911
// does not ACK at 0x5D/0x14 at all (the vendor BSP carries a gt911 stanza for
// other boards). FT5x06 is therefore the right driver - forced GT911 is off.
#define CHD_TOUCH_FORCE_GT911       0
// Pin table: CTP_INT = IO3. GT911 latches its I2C address (0x5D / 0x14) from the
// INT level around reset, so leaving INT unconnected can hide the controller.
#define CHD_TOUCH_INT_GPIO          GPIO_NUM_3
// This panel reports a "ghost" touch point from power-on: one point, constant
// coordinates, never a release, even with the finger far away. LVGL therefore
// believes the finger is held down forever and never emits CLICKED. The filter
// drops points that have never moved and never been released for >= 1.5 s;
// a real press releases or moves, which restores normal reporting immediately.
#define CHD_TOUCH_PHANTOM_FILTER    0
// Polling diagnostics (200 ms driver poll, 10 Hz TD_STATUS, 5 s summaries).
// OFF by default: several readers of the same FT6336 clear each other's
// touch/release state (read 0x02 -> read coords -> write 0), which made the panel
// look permanently pressed. With this at 0 only LVGL's indev reads the chip.
// Set to 1 when a chip-level trace is actually needed.
#define CHD_TOUCH_DIAG              0
// Log what LVGL itself sees, from inside the read wrapper (no extra reader, no
// extra I2C traffic). Prints only on (pressed, points, x, y) changes, plus one
// heartbeat every 10 s with the object currently being pressed.
#define CHD_TOUCH_LOG_INDEV         1
// Point filtering in the read wrapper (task-29, final layer).
// The FT5x06 driver reports RAW portrait coordinates (320x480) and never applies
// swap_xy/mirror, so on this landscape 480x320 panel every point lands outside
// the screen (a real tap showed y=437 => no object under it, act=none). The filter
// maps swap/mirror, drops out-of-bounds points, drops points that sit inside a
// +-10 px box for >=2 s (this panel has a stuck ghost electrode), and forces
// points=0 when nothing valid is left - which is what lets LVGL see a release.
#define CHD_TOUCH_POINT_FILTER      1

#define DISPLAY_OFFSET_X            0
#define DISPLAY_OFFSET_Y            0

#define DISPLAY_DC_GPIO             GPIO_NUM_4
#define DISPLAY_CS_GPIO             GPIO_NUM_5
#define DISPLAY_MOSI_GPIO           GPIO_NUM_6
#define DISPLAY_CLK_GPIO            GPIO_NUM_7
#define DISPLAY_RST_GPIO            GPIO_NUM_NC

#define DISPLAY_BACKLIGHT_PIN       GPIO_NUM_47
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false


// *****虫洞开发板配置选择******开始*****
// 开发板硬件版本选择,在下面选项中只打开一个
// 屏幕选择,在下面选项中只打开一个,横屏竖屏模式通过语音直接切换,各版本屏幕重点关注尺寸，
// 如果不清除选择哪个选项，可询问客服

#if CONFIG_CHD_ESP32S3_BOX_LCD_35
    #define USE_LCD_3_5                 // 3.5寸屏幕        ST7796
// #define USE_LCD_3_5_TFT             // 3.5寸屏幕        ST7796
#else
    #define USE_LCD_2_8                    // 2.8寸屏幕    ST7789
#endif

// *****虫洞开发板配置选择******结束*****
#ifdef USE_LCD_3_5
    #define USE_LCD_ST7796_480X320_3_5
#ifdef USE_LCD_ST7796_480X320_3_5       // 3.5寸
    #define DISPLAY_WIDTH               480
    #define DISPLAY_HEIGHT              320
    #define DISPLAY_INVERT_COLOR        true
    #define DISPLAY_MIRROR_X            false
    #define DISPLAY_MIRROR_Y            false
    #define DISPLAY_SWAP_XY             true
    #define DISPLAY_RGB_ORDER           LCD_RGB_ELEMENT_ORDER_BGR
#endif
#endif

#ifdef USE_LCD_3_5_TFT
    #define USE_LCD_ST7796_480X320_3_5_TFT
#ifdef USE_LCD_ST7796_480X320_3_5_TFT   // 3.5寸
    #define DISPLAY_WIDTH               480
    #define DISPLAY_HEIGHT              320
    #define DISPLAY_INVERT_COLOR        false       // 只有这个参数不一样
    #define DISPLAY_MIRROR_X            false
    #define DISPLAY_MIRROR_Y            false
    #define DISPLAY_SWAP_XY             true
    #define DISPLAY_RGB_ORDER           LCD_RGB_ELEMENT_ORDER_BGR
#endif
#endif

#ifdef USE_LCD_2_8
    #define USE_LCD_ST7789_320X240_2_8
#ifdef USE_LCD_ST7789_320X240_2_8       // 2.8寸
    #define DISPLAY_WIDTH               320
    #define DISPLAY_HEIGHT              240
    #define DISPLAY_INVERT_COLOR        false
    #define DISPLAY_MIRROR_X            true
    #define DISPLAY_MIRROR_Y            false
    #define DISPLAY_SWAP_XY             true
    #define DISPLAY_RGB_ORDER           LCD_RGB_ELEMENT_ORDER_RGB
#endif
#endif

// 下面是虫洞esp-box开发板摄像头引脚配置
#define CAMERA_PIN_PWDN                 (GPIO_NUM_NC)
#define CAMERA_PIN_RESET                (GPIO_NUM_NC)

/* I2C */
#define BSP_I2C_PORT                    (1)
#define BSP_I2C_SCL                     (-1) // (GPIO_NUM_18)
#define BSP_I2C_SDA                     (-1) // (GPIO_NUM_8)

#define BSP_CAMERA_XCLK                 (GPIO_NUM_39)
#define BSP_CAMERA_PCLK                 (GPIO_NUM_14)
#define BSP_CAMERA_VSYNC                (GPIO_NUM_42)
#define BSP_CAMERA_HSYNC                (GPIO_NUM_41)
#define BSP_CAMERA_D0                   (GPIO_NUM_12)
#define BSP_CAMERA_D1                   (GPIO_NUM_10)
#define BSP_CAMERA_D2                   (GPIO_NUM_9)
#define BSP_CAMERA_D3                   (GPIO_NUM_11)
#define BSP_CAMERA_D4                   (GPIO_NUM_13)
#define BSP_CAMERA_D5                   (GPIO_NUM_21)
#define BSP_CAMERA_D6                   (GPIO_NUM_38)
#define BSP_CAMERA_D7                   (GPIO_NUM_40)

#define CAMERA_PIN_VSYNC                BSP_CAMERA_VSYNC
#define CAMERA_PIN_HREF                 BSP_CAMERA_HSYNC
#define CAMERA_PIN_PCLK                 BSP_CAMERA_PCLK
#define CAMERA_PIN_XCLK                 BSP_CAMERA_XCLK
#define CAMERA_PIN_D0                   BSP_CAMERA_D0
#define CAMERA_PIN_D1                   BSP_CAMERA_D1
#define CAMERA_PIN_D2                   BSP_CAMERA_D2
#define CAMERA_PIN_D3                   BSP_CAMERA_D3
#define CAMERA_PIN_D4                   BSP_CAMERA_D4
#define CAMERA_PIN_D5                   BSP_CAMERA_D5
#define CAMERA_PIN_D6                   BSP_CAMERA_D6
#define CAMERA_PIN_D7                   BSP_CAMERA_D7

#define CAMERA_XCLK_FREQ                (16000000)

#define ML307_RX_PIN                    GPIO_NUM_44
#define ML307_TX_PIN                    GPIO_NUM_43

#define BAT_CHARGING_PIN                GPIO_NUM_48

#endif // _BOARD_CONFIG_H_
