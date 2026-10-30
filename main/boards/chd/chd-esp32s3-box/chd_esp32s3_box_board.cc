#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "application.h"
#include "ui/ui_log.h"
#include "ui/hermes_face_64x72.h"
#include "ui/chrome_dino_sprites.h"

#include <esp_heap_caps.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include "button.h"
#include "config.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include "power_save_timer.h"

#include <esp_lcd_touch_gt911.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
// Upstream header has no extern "C" guards (it is a C-only API), so wrap the
// include instead of patching the vendored header.
extern "C" {
#include "lvgl_kawaii_face.h"
}

#include "esp32_camera.h"
#include "power_manager.h"

#include <nvs_flash.h>
#include "system_reset.h"

#include "mcp_server.h"
#include "settings.h"

#include <cstring>
#include <functional>

#define TAG "Chd-Esp32s3-Box-Board"

// ---------------------------------------------------------------------------
// TF card smoke test (task-30) - "can this card mount at all on this firmware?"
//
// Pins come from the vendor pin table (CHD-ESP32-S3-BOX):
//   SD_CMD  = IO0   (shared with the BOOT strap pin)
//   SD_CLK  = IO43  (shared with UART0 TXD)
//   SD_DAT0 = IO44  (shared with UART0 RXD)
// Only CMD/CLK/DAT0 are wired, so this can only be 1-bit SDMMC. No card-detect
// pin exists, so the card is treated as permanently inserted.
//
// UART0 console vs. SD_CLK/D0: both console pins are reused, so the UART
// console output is garbled while the SD driver owns them. Logs still arrive on
// the USB-Serial-JTAG console (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y).
//
// CHD_SD_ENABLE is the kill switch: pass -DCHD_SD_ENABLE=0 (or edit this define)
// to build a firmware that never touches IO0/IO43/IO44 - use it if the card
// holds IO0 low at reset and the board boots into download mode instead of
// running the app.
// ---------------------------------------------------------------------------
#ifndef CHD_SD_ENABLE
#define CHD_SD_ENABLE 1
#endif

#if CHD_SD_ENABLE
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static constexpr int kSdCmdGpio = 0;   // also BOOT strap: keep the pull-up intact
static constexpr int kSdClkGpio = 43;  // also UART0 TXD
static constexpr int kSdD0Gpio = 44;   // also UART0 RXD

static const char* ErrName(esp_err_t err) { return esp_err_to_name(err); }

// Read the three SD lines as inputs with pull-ups before touching the driver:
// a line that stays low with a pull-up enabled is shorted/pulled by the card or
// the board (e.g. the BOOT button circuit on IO0), which no amount of retrying
// will fix. This also covers the "did IO0 keep the chip out of download mode"
// question for free.
static void SdPinSanity() {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << kSdCmdGpio) | (1ULL << kSdClkGpio) | (1ULL << kSdD0Gpio);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(10));  // let the pull-ups settle
    ESP_LOGI("CHD_SD", "pin sanity (input+pull-up): IO%d(CMD)=%d IO%d(CLK)=%d IO%d(D0)=%d "
                       "[a 0 here means the line is held low]",
             kSdCmdGpio, gpio_get_level(static_cast<gpio_num_t>(kSdCmdGpio)),
             kSdClkGpio, gpio_get_level(static_cast<gpio_num_t>(kSdClkGpio)),
             kSdD0Gpio, gpio_get_level(static_cast<gpio_num_t>(kSdD0Gpio)));
    gpio_reset_pin(static_cast<gpio_num_t>(kSdCmdGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(kSdClkGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(kSdD0Gpio));
}

// One full "clock the card up" attempt at a given frequency. Logs every step so a
// failure can be attributed: host_init / init_slot / card_init.
// Returns ESP_OK only if the card answered (CID read), which proves wiring+timing.
static esp_err_t SdCardProbe(int freq_khz, sdmmc_card_t* card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = freq_khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(kSdClkGpio);
    slot.cmd = static_cast<gpio_num_t>(kSdCmdGpio);
    slot.d0 = static_cast<gpio_num_t>(kSdD0Gpio);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    ESP_LOGI("CHD_SD", "[%d kHz] sdmmc_host_init     -> %s (0x%x)", freq_khz, ErrName(err),
             static_cast<unsigned>(err));
    if (err != ESP_OK) {
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    ESP_LOGI("CHD_SD", "[%d kHz] sdmmc_host_init_slot-> %s (0x%x)", freq_khz, ErrName(err),
             static_cast<unsigned>(err));
    if (err != ESP_OK) {
        sdmmc_host_deinit();
        return err;
    }
    err = sdmmc_card_init(&host, card);
    ESP_LOGI("CHD_SD", "[%d kHz] sdmmc_card_init    -> %s (0x%x)", freq_khz, ErrName(err),
             static_cast<unsigned>(err));
    if (err != ESP_OK) {
        sdmmc_host_deinit();
        return err;
    }
    return ESP_OK;  // host+slot left up; caller deinits before the FAT stage
}

// Sector 0 tells "card fine, no filesystem" apart from "card never answered".
static void SdCheckPartitionTable(sdmmc_card_t* card) {
    static uint8_t sector0[512] __attribute__((aligned(4)));  // static: keep off the task stack
    esp_err_t err = sdmmc_read_sectors(card, sector0, 0, 1);
    if (err != ESP_OK) {
        ESP_LOGW("CHD_SD", "read sector 0 failed: %s (0x%x)", ErrName(err),
                 static_cast<unsigned>(err));
        return;
    }
    const bool mbr = sector0[510] == 0x55 && sector0[511] == 0xAA;
    ESP_LOGI("CHD_SD", "partition table (sector 0): %s [bytes 510/511 = 0x%02x 0x%02x, "
                       "first part type=0x%02x, start LBA=%u]",
             mbr ? "PRESENT" : "ABSENT", sector0[511], sector0[510], sector0[450],
             static_cast<unsigned>(sector0[454]) | (static_cast<unsigned>(sector0[455]) << 8) |
                 (static_cast<unsigned>(sector0[456]) << 16) |
                 (static_cast<unsigned>(sector0[457]) << 24));
}

// Runs in its own task so a missing/slow card can never delay or block boot.
static void SdSmokeTask(void* arg) {
    (void)arg;
    ESP_LOGI("CHD_SD", "probing TF card (1-bit SDMMC: cmd=IO%d clk=IO%d d0=IO%d)",
             kSdCmdGpio, kSdClkGpio, kSdD0Gpio);
    SdPinSanity();

    static sdmmc_card_t card;  // ~1 KiB: keep it out of the task stack
    bool card_ok = false;
    int used_freq = 0;
    // Two frequency levels per round: 400 kHz first (timing-forgiving probing
    // clock), then 20 MHz. 400 kHz OK + 20 MHz failing = signal integrity;
    // neither working = wiring / power / no card.
    const int freqs[2] = {SDMMC_FREQ_PROBING, SDMMC_FREQ_DEFAULT};
    for (int round = 0; round < 3 && !card_ok; round++) {
        ESP_LOGI("CHD_SD", "--- card init attempt %d/3 ---", round + 1);
        for (int i = 0; i < 2 && !card_ok; i++) {
            if (SdCardProbe(freqs[i], &card) == ESP_OK) {
                card_ok = true;
                used_freq = freqs[i];
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }
    if (!card_ok) {
        ESP_LOGE("CHD_SD", "card init FAILED at 400 kHz and 20 MHz, 3 rounds -> "
                           "wiring/power/card problem, NOT a filesystem problem");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI("CHD_SD", "card answered at %d kHz - wiring and timing are OK", used_freq);
    sdmmc_card_print_info(stdout, &card);
    const uint64_t card_bytes =
        static_cast<uint64_t>(card.csd.capacity) * static_cast<uint64_t>(card.csd.sector_size);
    ESP_LOGI("CHD_SD", "card size: %llu MiB (capacity=%u sectors, sector_size=%u B)",
             card_bytes / 1048576ULL, static_cast<unsigned>(card.csd.capacity),
             static_cast<unsigned>(card.csd.sector_size));
    SdCheckPartitionTable(&card);

    // Hand the bus back, then let the VFS/FAT layer do its own (identical) card
    // init - that keeps the FAT step independent from the diagnosis above.
    sdmmc_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;  // NEVER format
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = used_freq;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = static_cast<gpio_num_t>(kSdClkGpio);
    slot.cmd = static_cast<gpio_num_t>(kSdCmdGpio);
    slot.d0 = static_cast<gpio_num_t>(kSdD0Gpio);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* mounted_card = nullptr;
    errno = 0;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_config, &mounted_card);
    const int mount_errno = errno;
    ESP_LOGI("CHD_SD", "esp_vfs_fat_sdmmc_mount -> %s (0x%x), errno=%d", ErrName(err),
             static_cast<unsigned>(err), mount_errno);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE("CHD_SD", "card is fine but FAT mount failed -> the data area is not a "
                               "filesystem this build understands (MBR/exFAT/unformatted). "
                               "Not formatting (smoke test only).");
        } else {
            ESP_LOGE("CHD_SD", "mount failed at the card level (%s) - see the per-stage codes "
                               "above", ErrName(err));
        }
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI("CHD_SD", "mount OK");

    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK) {
        ESP_LOGI("CHD_SD", "capacity: total=%llu KiB free=%llu KiB (%.1f / %.1f MiB)",
                 total / 1024ULL, free_bytes / 1024ULL, total / 1048576.0,
                 free_bytes / 1048576.0);
    } else {
        ESP_LOGW("CHD_SD", "esp_vfs_fat_info failed");
    }

    DIR* dir = opendir("/sdcard");
    if (dir == nullptr) {
        ESP_LOGW("CHD_SD", "opendir(/sdcard) failed (errno=%d)", errno);
    } else {
        struct dirent* entry = nullptr;
        int shown = 0;
        while ((entry = readdir(dir)) != nullptr && shown < 10) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char path[300];
            snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
            struct stat st = {};
            if (stat(path, &st) == 0) {
                ESP_LOGI("CHD_SD", "  [%d] %s%s  %ld B", ++shown, entry->d_name,
                         S_ISDIR(st.st_mode) ? "/" : "", static_cast<long>(st.st_size));
            } else {
                ESP_LOGI("CHD_SD", "  [%d] %s  (stat failed)", ++shown, entry->d_name);
            }
        }
        closedir(dir);
        if (shown == 0) {
            ESP_LOGI("CHD_SD", "root directory is empty");
        }
    }

    const char* test_path = "/sdcard/_sdtest.txt";
    FILE* f = fopen(test_path, "w");
    if (f == nullptr) {
        ESP_LOGW("CHD_SD", "write test: fopen(w) failed (errno=%d)", errno);
    } else {
        const size_t written = fwrite("ok", 1, 2, f);
        fclose(f);
        char readback[8] = {0};
        f = fopen(test_path, "r");
        size_t got = 0;
        if (f != nullptr) {
            got = fread(readback, 1, sizeof(readback) - 1, f);
            fclose(f);
        }
        ESP_LOGI("CHD_SD", "write/read test: wrote=%u read=%u content='%s' -> %s",
                 static_cast<unsigned>(written), static_cast<unsigned>(got), readback,
                 (written == 2 && got == 2 && strcmp(readback, "ok") == 0) ? "OK" : "MISMATCH");
        if (remove(test_path) == 0) {
            ESP_LOGI("CHD_SD", "cleanup: _sdtest.txt removed");
        } else {
            ESP_LOGW("CHD_SD", "cleanup: remove failed (errno=%d)", errno);
        }
    }
    ESP_LOGI("CHD_SD", "smoke test done (card stays mounted at /sdcard)");
    vTaskDelete(nullptr);
}
// task-29 observability: keep the SD bus busy so a "does the card make the
// device toggle the dialogue on its own?" test is possible on the bench.
// Started by the MCP tool self.debug.sd_storm; logs CHD_SD: storm lines once a
// second, so an unexpected CHD_UI: line in between is a real false trigger.
static volatile bool s_sd_storm_stop = false;

static void SdStormTask(void* arg) {
    const int seconds = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    const char* path = "/sdcard/_sdtest_storm.bin";
    constexpr size_t kChunk = 64 * 1024;
    static uint8_t chunk[kChunk];

    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size < static_cast<off_t>(kChunk)) {
        FILE* f = fopen(path, "wb");
        if (f == nullptr) {
            ESP_LOGE("CHD_SD", "storm: cannot create %s (errno=%d)", path, errno);
            vTaskDelete(nullptr);
            return;
        }
        memset(chunk, 0xA5, sizeof(chunk));
        for (int i = 0; i < 8; i++) {  // 512 KiB is plenty to keep the bus busy
            fwrite(chunk, 1, sizeof(chunk), f);
        }
        fclose(f);
        ESP_LOGI("CHD_SD", "storm: created %s", path);
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE("CHD_SD", "storm: cannot open %s (errno=%d)", path, errno);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI("CHD_SD", "storm: START %d s (watch for unexpected CHD_UI lines)", seconds);
    s_sd_storm_stop = false;
    size_t bytes_this_second = 0;
    size_t total    = 0;
    int elapsed = 0;
    const TickType_t t0 = xTaskGetTickCount();
    while (!s_sd_storm_stop) {
        const size_t got = fread(chunk, 1, sizeof(chunk), f);
        if (got < sizeof(chunk)) {
            fseek(f, 0, SEEK_SET);  // wrap around
        }
        bytes_this_second += got;
        total += got;
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(1000 * (elapsed + 1))) {
            elapsed++;
            ESP_LOGI("CHD_SD", "storm: %d s, %u KiB/s, total %u KiB", elapsed,
                     static_cast<unsigned>(bytes_this_second / 1024),
                     static_cast<unsigned>(total / 1024));
            bytes_this_second = 0;
            if (elapsed >= seconds) {
                break;
            }
        }
    }
    fclose(f);
    ESP_LOGI("CHD_SD", "storm: STOP after %d s, total %u KiB", elapsed,
             static_cast<unsigned>(total / 1024));
    vTaskDelete(nullptr);
}
#endif  // CHD_SD_ENABLE#endif  // CHD_SD_ENABLE

// M7-S1: kawaii face + bottom subtitle bar on the 480x320 panel.
//
// Layout (M7 subtitle design):
//   y   0..228   face panel 480x228 (kawaii takes min(w,h)=228 as face size)
//   y 228..312   subtitle bar 464x84 (r=12, black 60%, padding 6)
//   y 312..320   bottom margin 8
//   idle:  face panel y+46 (vertically centred in the free area), subtitle faded out
//   speak: face panel y+0, subtitle bar fades in
// The face size is FIXED at init: lvgl_kawaii_face latches face_sz=min(w,h) when
// face_animation_init() runs and does not re-layout on parent resize, and
// resizing would mean destroying/re-creating the canvas buffers in PSRAM. So the
// panel size never changes - only its y position moves (lv_anim, 160 ms).
static constexpr int kFacePanelHeight = 228;
static constexpr int kFaceIdleOffsetY = 46;   // face down, subtitle hidden
static constexpr int kFaceSpeakOffsetY = 0;   // face up, subtitle visible
static constexpr int kSubtitleBarWidth = 464;
static constexpr int kSubtitleBarMargin = 8;  // (480-464)/2, and 320-312 = 8 below
static constexpr int kSubtitleBarPad = 6;
static constexpr int kSubtitleBarRadius = 12;
static constexpr int kSubtitleBarOpa = LV_OPA_60;
static constexpr uint32_t kFaceMoveMs = 160;
static constexpr uint32_t kTypewriterPeriodMs = 90;  // ~11 chars/s
static constexpr int kSubtitleHoldTicks = 22;        // ~2 s at 90 ms/tick

// task-1/A5: the panel reports a ghost contact from the very first frames after
// boot. The point filter eventually drops it (~2 s), and that transition is what
// produces the first release LVGL sees - without this guard it can be delivered
// as a click on whatever sits under the ghost. Both tap handlers therefore ignore
// events during the first 3 s after the first filtered touch frame (the anchor is
// taken in TouchPointFilter, see task-2/C2).
static uint32_t s_touch_ready_ms = 0;
static constexpr uint32_t kTapBootGuardMs = 3000;

// Subtitle text uses the display's *runtime* text font: Assets::MountAssets()
// loads the full-coverage cbin font from the assets partition (assets.cc:296-302,
// font_noto_sans_common_20_4 by default) and installs it via SetTextFont(), which
// the subtitle label follows (see SetTextFont below). The compiled-in
// font_noto_sans_basic_30_4 was rejected on evidence: it renders common
// characters such as 是/小/智 as tofu boxes, and it also meant handing LVGL a
// hand-built lv_font_t (a copied struct + fallback pointer) - the prime suspect
// for the task-19 panic. No custom font object is used any more.

class CustomLcdDisplay : public SpiLcdDisplay {
public:
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* setting_box_ = nullptr;
    lv_obj_t* face_panel_ = nullptr;
    bool face_ready_ = false;
    // Subtitle layer: a sibling of the face panel, so the face's 30 ms redraw
    // never invalidates the text area.
    lv_obj_t* subtitle_bar_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    // B2 mute UI: a 56x56 button in the empty left margin and a non-clickable
    // dark overlay over the 228x228 face (the panel is 480 wide, the face is
    // centred, so x=8..64 never covers it).
    lv_obj_t* mute_button_ = nullptr;
    lv_obj_t* mute_button_label_ = nullptr;
    lv_obj_t* mute_overlay_ = nullptr;
    lv_obj_t* mute_overlay_label_ = nullptr;
    MuteUiState mute_ui_state_ = MuteUiState::kNoEndpoint;
    bool mute_last_confirmed_ = false;  // server truth, for the failed-state visual
    // Evidence for B2 acceptance #2/#6: click -> "切换中…" repaint, and click ->
    // server-confirmed overlay repaint (total, split into render vs network).
    uint32_t mute_click_tick_ = 0;
    uint32_t mute_click_render_ms_ = 0;
    bool mute_click_pending_ = false;
    // UI v2 Phase B1 (A view): text-only information screen.
    lv_obj_t* ui_bar_ = nullptr;
    lv_obj_t* state_chip_ = nullptr;
    lv_obj_t* mute_text_ = nullptr;
    lv_obj_t* status_line_ = nullptr;
    lv_obj_t* hermes_icon_ = nullptr;
    lv_obj_t* hermes_name_ = nullptr;
    lv_obj_t* hermes_dot_ = nullptr;
    lv_obj_t* hermes_state_ = nullptr;
    lv_obj_t* hermes_avatar_ = nullptr;
    lv_obj_t* phase_b_bg_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* idle_wave_ = nullptr;
    lv_point_precise_t phase_b_wave_pts_[40] = {};
    int wave_phase_ = 0;
    lv_obj_t* mute_hit_ = nullptr;
    lv_obj_t* mute_icon_ = nullptr;
    lv_obj_t* view_hit_ = nullptr;
    lv_obj_t* view_label_ = nullptr;
    lv_obj_t* log_labels_[8] = {};
    lv_obj_t* hermes_hit_ = nullptr;
    lv_obj_t* cards_[6] = {};
    lv_obj_t* card_values_[6] = {};
    lv_obj_t* a_sep_ = nullptr;
    lv_obj_t* a_hlines_[3] = {};
    lv_obj_t* baseline_ = nullptr;
    lv_obj_t* a_metric_titles_[3] = {};
    lv_obj_t* a_metric_values_[3] = {};
    lv_obj_t* ascii_scope_ = nullptr;
    lv_obj_t* debug_line_ = nullptr;
    uint8_t ascii_phase_ = 0;
    lv_obj_t* dino_root_ = nullptr;
    lv_obj_t* dino_sprite_ = nullptr;
    lv_obj_t* dino_cactus_img_ = nullptr;
    lv_obj_t* dino_cactus_ = nullptr;
    int dino_cactus_x_ = 480;
    bool dino_legs_up_ = false;
    lv_obj_t* game_view_ = nullptr;
    lv_obj_t* game_dino_ = nullptr;
    lv_obj_t* game_cactus_ = nullptr;
    lv_obj_t* game_score_ = nullptr;
    lv_obj_t* game_ptero_ = nullptr;
    uint32_t game_score_value_ = 0;
    bool game_large_cactus_ = false;
    bool game_ptero_active_ = false;
    int game_ptero_x_ = 480;
    int game_ptero_y_ = 75;
    int game_cooldown_ = 300;
    int game_invincible_ = 0;
    uint32_t game_ticks_ = 0;
    uint64_t game_fps_start_us_ = 0;
    uint64_t game_dirty_px_ = 0;
    lv_obj_t* game_ground_a_ = nullptr;
    lv_obj_t* game_ground_b_ = nullptr;
    int game_ground_x_ = 0;
    int game_cactus_x_ = 480;
    int game_dino_y_ = 80;
    int game_vy_ = 0;
    bool game_jumping_ = false;
    lv_timer_t* game_timer_ = nullptr;
    uint8_t game_frame_ = 0;
    enum class PhaseBView { kA, kB, kC, kD };
    PhaseBView phase_b_view_ = PhaseBView::kA;
    lv_timer_t* phase_b_timer_ = nullptr;
    lv_timer_t* a_dino_timer_ = nullptr;
    lv_obj_t* media_overlay_ = nullptr;
    lv_obj_t* media_image_ = nullptr;
    lv_timer_t* media_timer_ = nullptr;
    std::unique_ptr<LvglImage> media_owner_;
    bool media_active_ = false;
    bool phase_b_built_ = false;
    bool dot_breathing_ = false;
    uint8_t phase_b_ticks_ = 0;
    UiLogLine phase_b_lines_[10];  // snapshot buffer: keep it off the LVGL timer stack
    lv_timer_t* type_timer_ = nullptr;
    std::string subtitle_text_;
    std::string subtitle_shown_;
    size_t subtitle_pos_ = 0;
    int subtitle_hold_ = 0;
    bool subtitle_visible_ = true;
    // NOTE: no LvglBuiltInFont / no fallback chain here on purpose. The label
    // points straight at the static const lv_font_t in flash, exactly like
    // InitializeLcdThemes() does with font_noto_emoji_30_4. A copied lv_font_t
    // plus a fallback pointer was the crash suspect in M7-S1B (task-19).
    using ClickHandler = std::function<void()>;
    using TapHandler = std::function<void()>;

    void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        DisplayLockGuard lock(this);
        if (!image) {
            if (preview_image_ != nullptr) lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            DismissMedia();
            return;
        }
        media_active_ = true;
        SpiLcdDisplay::SetPreviewImage(std::move(image));
        if (preview_image_ != nullptr) {
            lv_obj_set_pos(preview_image_, 0, 0);
            lv_obj_set_size(preview_image_, LV_HOR_RES, LV_VER_RES);
            auto* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(preview_image_));
            if (dsc != nullptr && dsc->header.w > 0 && dsc->header.h > 0) {
                const int32_t sw = (LV_HOR_RES * 256) / dsc->header.w;
                const int32_t sh = (LV_VER_RES * 256) / dsc->header.h;
                lv_image_set_scale(preview_image_, sw > sh ? sw : sh);  // cover
            }
            lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(preview_image_, MediaTapCb, LV_EVENT_CLICKED, this);
            lv_obj_move_foreground(preview_image_);
        }
        for (lv_obj_t* obj : {ui_bar_, baseline_, idle_wave_, hermes_avatar_, subtitle_bar_,
                              game_view_, dino_root_, a_sep_}) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* obj : {cards_[0], cards_[1], cards_[2], cards_[3], cards_[4], cards_[5]}) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* obj : log_labels_) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* obj : a_metric_titles_) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* obj : a_metric_values_) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t* obj : a_hlines_) {
            if (obj != nullptr) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (media_timer_ != nullptr) lv_timer_del(media_timer_);
        media_timer_ = lv_timer_create(MediaTimerCb, 8000, this);
    }

    void DismissMedia() {
        if (media_timer_ != nullptr) {
            lv_timer_del(media_timer_);
            media_timer_ = nullptr;
        }
        media_active_ = false;
        HideMedia();
        if (preview_image_ != nullptr) lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        if (content_ != nullptr) lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* obj : {phase_b_bg_, ui_bar_, baseline_}) {
            if (obj != nullptr) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        ApplyViewLayout();
        UpdateStatusLine();
    }

    static void MediaTapCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self != nullptr) self->DismissMedia();
    }

    static void MediaTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self != nullptr) {
            self->media_timer_ = nullptr;
            self->DismissMedia();
        }
    }

    void HideMedia() {
        if (media_timer_ != nullptr) {
            lv_timer_del(media_timer_);
            media_timer_ = nullptr;
        }
        if (media_overlay_ != nullptr && lv_obj_is_valid(media_overlay_)) {
            lv_obj_del(media_overlay_);
        }
        media_overlay_ = nullptr;
        media_image_ = nullptr;
        media_owner_.reset();
    }

    void SetPhaseBView(int view) {
        DisplayLockGuard lock(this);
        phase_b_view_ = view == 1 ? PhaseBView::kB
                       : (view == 2 ? PhaseBView::kC
                          : (view == 3 ? PhaseBView::kD : PhaseBView::kA));
        if (phase_b_built_) {
            ApplyViewLayout();
            UpdateCards();
            UpdateStatusLine();
            UpdateMuteUi();
        }
        ESP_LOGI("CHD_UI", "set view=%d", view);
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, int offset_x, int offset_y,
                       bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    void SetClickHandler(ClickHandler handler) {
        click_handler_ = std::move(handler);
    }

    // task-29: touch takes over from the BOOT button.
    void SetTapHandler(TapHandler handler) {
        tap_handler_ = std::move(handler);
    }

    // A tap counts only if it did not come from a swipe, and only if it landed on
    // the bare screen (not bubbled up from a child widget) - otherwise tapping the
    // face would also open a dialogue.
    static void ScreenClickCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        // task-1/A5: the boot ghost's first release must not become a click.
        if (lv_tick_elaps(s_touch_ready_ms) < kTapBootGuardMs) {
            ESP_LOGI("CHD_UI", "tap ignored: boot guard %u ms after touch init",
                     static_cast<unsigned>(lv_tick_elaps(s_touch_ready_ms)));
            return;
        }
        if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
            lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
            // Ignore taps on the face (it has its own reaction handler), and on
            // genuinely clickable child widgets. Plain labels/cards are just
            // surfaces: let them fall through to the talk toggle.
            if (self->face_panel_ != nullptr &&
                (target == self->face_panel_ || lv_obj_get_parent(target) == self->face_panel_)) {
                return;
            }
            if (lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE)) {
                return;
            }
        }
        if (lv_tick_elaps(self->last_gesture_ms_) < kTapAfterGestureGuardMs) {
            ESP_LOGI("CHD_UI", "tap ignored: %u ms after a swipe",
                     static_cast<unsigned>(lv_tick_elaps(self->last_gesture_ms_)));
            return;
        }
        DisplayLockGuard lock(self);
        if (self->tap_handler_) {
            self->tap_handler_();
        }
    }

    // Tapping the face plays a short local reaction and toggles the dialogue.
    // Since the BOOT button no longer does it (IO0 is shared with the TF card CMD
    // line), the face is the only touch entry point for starting/stopping a chat.
    static void FaceClickCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        // task-1/A5: the boot ghost's first release must not become a face tap.
        if (lv_tick_elaps(s_touch_ready_ms) < kTapBootGuardMs) {
            ESP_LOGI("CHD_UI", "tap ignored: boot guard %u ms after touch init (face)",
                     static_cast<unsigned>(lv_tick_elaps(s_touch_ready_ms)));
            return;
        }
        // task-2/C1: the face panel lets the legacy GESTURE bubble to the screen
        // (gesture_event_cb already acted on it) and still receives CLICKED on
        // release, so a swipe across the face would toggle the dialogue twice and
        // also cycle the expression. Same guard as ScreenClickCb.
        if (lv_tick_elaps(self->last_gesture_ms_) < kTapAfterGestureGuardMs) {
            ESP_LOGI("CHD_UI", "face tap ignored: %u ms after a swipe",
                     static_cast<unsigned>(lv_tick_elaps(self->last_gesture_ms_)));
            return;
        }
        DisplayLockGuard lock(self);
        if (!self->face_ready_ || self->face_panel_ == nullptr) {
            return;
        }
        static const face_emotion_t kReactions[] = {FACE_HAPPY, FACE_WINK, FACE_SURPRISED, FACE_LOVE};
        self->face_reaction_index_ = (self->face_reaction_index_ + 1) % 4;
        ESP_LOGI("CHD_UI", "face tap -> reaction %d + dialogue toggle", self->face_reaction_index_);
        face_set_emotion(kReactions[self->face_reaction_index_], true);
        lv_timer_t* timer = lv_timer_create(FaceReactionResetCb, 900, self);
        if (timer != nullptr) {
            lv_timer_set_repeat_count(timer, 1);  // one shot: run once, then stop
        }
        // task-1/A4: same action as a bare-screen tap (BOOT no longer toggles).
        // Called under the same DisplayLockGuard as ScreenClickCb, so both paths
        // have identical locking behaviour.
        if (self->tap_handler_) {
            self->tap_handler_();
        }
    }

    static void FaceReactionResetCb(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr || !self->face_ready_) {
            return;
        }
        DisplayLockGuard lock(self);
        face_set_emotion(FACE_NEUTRAL, true);
    }

    // The kawaii face is a plain drawing surface: make every object *under* the
    // face panel non-clickable so a tap anywhere in the face area reaches the
    // panel itself (and stops there). The panel keeps LV_OBJ_FLAG_CLICKABLE - it
    // is the object that owns FaceClickCb.
    static void MakeChildrenNonClickable(lv_obj_t* obj) {
        if (obj == nullptr) {
            return;
        }
        const uint32_t count = lv_obj_get_child_count(obj);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
            lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
            MakeChildrenNonClickable(child);
        }
    }

    // M7-S1 spike: server emotion name -> lvgl_kawaii_face expression.
    // Names follow the 21 canonical noto emoji emotions
    // (managed_components/78__xiaozhi-fonts/include/noto_emoji.h) plus the
    // device-side names this firmware sends ("neutral", "robot_2", "warning")
    // plus the C1a six-state emotion names routed explicitly below. Anything
    // unknown -> FACE_NEUTRAL (the same frame as idle).
    static face_emotion_t FaceEmotionFromName(const char* emotion) {
        static const struct {
            const char* name;
            face_emotion_t emotion;
        } kEmotionMap[] = {
            // Six persona states (C1a). The state -> emotion definition lives
            // on the server; this table only routes the received emotion name
            // to one of the 17 kawaii faces.
            {"idle", FACE_NEUTRAL},          {"neutral", FACE_NEUTRAL},
            {"robot_2", FACE_NEUTRAL},
            {"listening", FACE_HAPPY},       {"listen", FACE_HAPPY},
            {"happy", FACE_HAPPY},           {"laughing", FACE_HAPPY},
            {"delicious", FACE_HAPPY},       {"speaking", FACE_EXCITED},
            {"thinking", FACE_CONFUSED},     {"confused", FACE_CONFUSED},
            {"working", FACE_WORKING_HARD},  {"working_hard", FACE_WORKING_HARD},
            {"tool-running", FACE_WORKING_HARD},
            {"angry", FACE_WORKING_HARD},
            {"confirm-pending", FACE_SURPRISED},
            {"barge-in", FACE_SURPRISED},    {"surprised", FACE_SURPRISED},
            {"shocked", FACE_SURPRISED},
            // Other legacy names keep their previous reasonable mapping.
            {"funny", FACE_PLAYFUL},         {"sad", FACE_SAD},
            {"crying", FACE_CRY},            {"loving", FACE_LOVE},
            {"kissy", FACE_LOVE},            {"embarrassed", FACE_WORRIED},
            {"warning", FACE_WORRIED},
            {"winking", FACE_WINK},          {"cool", FACE_COOL},
            {"relaxed", FACE_COOL},          {"confident", FACE_SMIRK},
            {"sleepy", FACE_SLEEPY},         {"silly", FACE_SILLY},
        };
        if (emotion != nullptr) {
            for (const auto& entry : kEmotionMap) {
                if (strcmp(entry.name, emotion) == 0) {
                    return entry.emotion;
                }
            }
        }
        return FACE_NEUTRAL;
    }

    // The face component calls these around every LVGL access. esp_lvgl_port
    // uses a recursive mutex, so calling them from inside DisplayLockGuard (or
    // from the LVGL task that already owns the lock) is re-entrant and safe.
    static void FaceLvglLock() { (void)lvgl_port_lock(0); }
    static void FaceLvglUnlock() { lvgl_port_unlock(); }

    static void gesture_event_cb(lv_event_t * e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));// CustomLcdDisplay* self = (CustomLcdDisplay*)Board::GetInstance().GetDisplay();

        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        self->last_gesture_ms_ = lv_tick_get();
        if (dir == LV_DIR_NONE) {
            ESP_LOGE(TAG, "Gesture: dir == LV_DIR_NONE");
            return;
        }
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);      // 获取手势大概位置
        ESP_LOGI(TAG, "Gesture: %d at x=%d, y=%d", dir, point.x, point.y);

        DisplayLockGuard lock(self);
        auto& app = Application::GetInstance();
        auto dev_state = app.GetDeviceState();
        if (lv_obj_is_valid(self->setting_box_)) {      // 优先处理设置栏
            if (dir != LV_DIR_BOTTOM) {
                lv_obj_delete(self->setting_box_);
                self->setting_box_ = NULL;
                return;
            }
        }
        else {
            if ((dir == LV_DIR_BOTTOM) && (dev_state == kDeviceStateIdle)) {
                self->create_setting_box();
                return;
            }
        }

        switch (dir) {
            case LV_DIR_LEFT:
                ESP_LOGI(TAG, "Swipe Left detected");   // 执行向左滑动的逻辑，如切换页面
                if (dev_state == kDeviceStateIdle) {
                }
                break;
            case LV_DIR_RIGHT:
                ESP_LOGI(TAG, "Swipe Right detected");  // 执行向右滑动的逻辑  , 退出

                break;
            case LV_DIR_TOP:
                ESP_LOGI(TAG, "Swipe Up detected");
                app.ToggleChatState();
                break;
            case LV_DIR_BOTTOM:
                ESP_LOGI(TAG, "Swipe Down detected");
                // display->set_volume_slider(output_volume_);
                break;
            default:
                break;
        }
    }

    static void volume_slider_cb(lv_event_t* e) {
        lv_obj_t * volume_slider_t = (lv_obj_t *)lv_event_get_target(e);     // volume_slider_
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        DisplayLockGuard lock(self);
        int volume_value = lv_slider_get_value(volume_slider_t);
        auto audio_codec = Board::GetInstance().GetAudioCodec();
        if (audio_codec && (audio_codec->output_volume() != volume_value)) {
            audio_codec->SetOutputVolume(volume_value);
        }
    }

    void set_volume_slider(int volume_value) {                      // 同步音量图标
        DisplayLockGuard lock(this);
        ESP_LOGI(TAG, "set_volume_slider %d", volume_value);
        if (lv_obj_is_valid(volume_slider_)) {
            int slider_value = lv_slider_get_value(volume_slider_);
            ESP_LOGI(TAG, "lv_slider_get_value %d", slider_value);
            uint8_t value = 0;
            if (volume_value > 0) {
                if (volume_value > 100) {
                    value = 100;
                }
                else {
                    value = volume_value;
                }
            }
            if (value != slider_value) {
                ESP_LOGI(TAG, "set_volume_slider %d", value);
                lv_slider_set_value(volume_slider_, value, LV_ANIM_OFF);
            }
        }
    }

    virtual void SetupUI() override {
        ESP_LOGI(TAG, "SetupUI >>>>>>>>>");

        SpiLcdDisplay::SetupUI();   // Call parent SetupUI() first to create all lvgl objects

        DisplayLockGuard lock(this);
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();
        auto screen = lv_screen_active();

        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);                               // 将滚动条模式设置为 OFF，这样无论是否滚动，滚动条都不会显示
        lv_obj_set_scroll_dir(screen, LV_DIR_NONE);                                             // lv_obj_set_scroll_dir(screen, LV_DIR_HOR);
        lv_obj_add_event_cb(lv_scr_act(), gesture_event_cb, LV_EVENT_GESTURE, this);
        // task-29: single tap on the bare screen toggles the dialogue.
        lv_obj_add_event_cb(screen, ScreenClickCb, LV_EVENT_CLICKED, this);
        // task-1/A3: container_/content_ cover the whole screen and are CLICKABLE by
        // default, and CLICKED does not bubble - so a tap on empty space landed on
        // them and the screen callback above never ran. Register the same handler on
        // both; the target == current_target guard keeps taps on their children
        // (top/status/bottom bar, chat widgets) out.
        if (container_ != nullptr) {
            lv_obj_add_event_cb(container_, ScreenClickCb, LV_EVENT_CLICKED, this);
        }
        if (content_ != nullptr) {
            lv_obj_add_event_cb(content_, ScreenClickCb, LV_EVENT_CLICKED, this);
        }

        SetupFace(screen);
        SetupMuteUi(screen);
        SetupPhaseB(screen);
    }

    // ---- UI v2 Phase B1: A view (text-only, no face) ---------------------
    void SetupPhaseB(lv_obj_t* screen) {
        if (phase_b_built_) {
            return;
        }
        const lv_font_t* font = ActiveTextFont();
        const lv_font_t* small_font = &lv_font_montserrat_14;

        // Base bars are for the old status-icon layout; Phase B owns the top 44 px.
        if (top_bar_ != nullptr && lv_obj_is_valid(top_bar_)) {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr && lv_obj_is_valid(status_bar_)) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_box_ != nullptr && lv_obj_is_valid(emoji_box_)) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (container_ != nullptr && lv_obj_is_valid(container_)) {
            // The stock container is a full-screen white panel; hide it or the
            // paper background below never shows.
            lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (content_ != nullptr && lv_obj_is_valid(content_)) {
            // Old chat/emoji widgets would show through the text screen.
            lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        }

        phase_b_bg_ = lv_obj_create(screen);
        lv_obj_set_size(phase_b_bg_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(phase_b_bg_, 0, 0);
        lv_obj_set_style_radius(phase_b_bg_, 0, 0);
        lv_obj_set_style_bg_color(phase_b_bg_, lv_color_hex(0xF2EEE1), 0);
        lv_obj_set_style_bg_opa(phase_b_bg_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(phase_b_bg_, 0, 0);
        lv_obj_set_style_pad_all(phase_b_bg_, 0, 0);
        lv_obj_remove_flag(phase_b_bg_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(phase_b_bg_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(phase_b_bg_, 0);

        lv_obj_set_style_bg_color(screen, lv_color_hex(0xF2EEE1), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        ui_bar_ = lv_obj_create(screen);
        lv_obj_set_size(ui_bar_, LV_HOR_RES, 44);
        lv_obj_align(ui_bar_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(ui_bar_, 0, 0);
        lv_obj_set_style_bg_color(ui_bar_, lv_color_hex(0xF2EEE1), 0);
        lv_obj_set_style_bg_opa(ui_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ui_bar_, 1, 0);
        lv_obj_set_style_border_side(ui_bar_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(ui_bar_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_pad_all(ui_bar_, 0, 0);
        lv_obj_remove_flag(ui_bar_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ui_bar_, LV_OBJ_FLAG_CLICKABLE);  // consume taps on the bar text
        lv_obj_move_foreground(ui_bar_);

        state_chip_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(state_chip_, 8, 0);
        lv_obj_set_width(state_chip_, 72);
        lv_label_set_long_mode(state_chip_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(state_chip_, font, 0);
        lv_obj_set_style_text_color(state_chip_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(state_chip_, "启动中");
        lv_obj_add_flag(state_chip_, LV_OBJ_FLAG_HIDDEN);

        mute_text_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(mute_text_, 300, 10);
        lv_obj_set_width(mute_text_, 90);
        lv_label_set_long_mode(mute_text_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(mute_text_, font, 0);
        lv_obj_set_style_text_align(mute_text_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(mute_text_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(mute_text_, "未静音");
        // The top-right status text is the most obvious mute affordance; make
        // it a real control as well as the avatar at the bottom-right.
        lv_obj_add_flag(mute_text_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(mute_text_, 18);
        lv_obj_add_event_cb(mute_text_, MuteButtonCb, LV_EVENT_CLICKED, this);

        time_label_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(time_label_, 400, 10);
        lv_obj_set_width(time_label_, 72);
        lv_obj_set_style_text_font(time_label_, font, 0);
        lv_obj_set_style_text_color(time_label_, lv_color_hex(0x6B7372), 0);
        lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(time_label_, "--:--");

        // Hermes brand + link dot (second line, before RSSI/SRAM). The device
        // only knows its WS to the 309 server, not the Hermes backend health.
        hermes_icon_ = lv_label_create(ui_bar_);
        lv_obj_set_size(hermes_icon_, 16, 16);
        lv_obj_set_pos(hermes_icon_, 8, 26);
        lv_obj_set_style_text_font(hermes_icon_, small_font, 0);
        lv_obj_set_style_text_color(hermes_icon_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(hermes_icon_, ">_");
        lv_obj_add_flag(hermes_icon_, LV_OBJ_FLAG_HIDDEN);

        hermes_name_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(hermes_name_, 32, 10);
        lv_obj_set_width(hermes_name_, 100);
        lv_obj_set_style_text_font(hermes_name_, font, 0);
        lv_obj_set_style_text_color(hermes_name_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(hermes_name_, "Hermes");

        hermes_dot_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(hermes_dot_, 4, 10);
        lv_obj_set_style_text_font(hermes_dot_, font, 0);
        lv_obj_set_style_text_color(hermes_dot_, lv_color_hex(0x30D158), 0);
        lv_label_set_text(hermes_dot_, "●");

        StartDotBreathing();
        hermes_state_ = lv_label_create(ui_bar_);
        // 14 px ASCII only (common_20 cannot fit the 20 px second line); the
        // dot colour carries the same meaning. Chinese text is in the README.
        lv_obj_set_pos(hermes_state_, 136, 10);
        lv_obj_set_width(hermes_state_, 48);
        lv_obj_set_style_text_font(hermes_state_, font, 0);
        lv_obj_set_style_text_color(hermes_state_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(hermes_state_, "在线");
        for (lv_obj_t* obj : {hermes_dot_, hermes_name_, hermes_state_}) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(obj, HermesViewCb, LV_EVENT_CLICKED, this);
        }

        status_line_ = lv_label_create(ui_bar_);
        lv_obj_set_pos(status_line_, 144, 24);
        lv_obj_set_width(status_line_, 224);
        lv_label_set_long_mode(status_line_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(status_line_, small_font, 0);
        lv_obj_set_style_text_color(status_line_, lv_color_hex(0x6B7372), 0);
        lv_label_set_text(status_line_, "R --  S --K  A 0/0  D 0/0");
        lv_obj_add_flag(status_line_, LV_OBJ_FLAG_HIDDEN);

        // 44x44 transparent hit boxes, 24x24 visuals centred inside.
        mute_hit_ = lv_obj_create(ui_bar_);
        lv_obj_set_size(mute_hit_, 44, 44);
        lv_obj_set_pos(mute_hit_, 376, 0);
        lv_obj_set_style_radius(mute_hit_, 8, 0);
        lv_obj_set_style_bg_opa(mute_hit_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mute_hit_, 0, 0);
        lv_obj_set_style_pad_all(mute_hit_, 0, 0);
        lv_obj_remove_flag(mute_hit_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mute_hit_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mute_hit_, MuteButtonCb, LV_EVENT_CLICKED, this);
        mute_icon_ = lv_label_create(mute_hit_);
        lv_obj_set_style_text_font(mute_icon_, font, 0);
        lv_obj_set_style_text_color(mute_icon_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(mute_icon_, "静");
        lv_obj_center(mute_icon_);
        lv_obj_remove_flag(mute_icon_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(mute_hit_, LV_OBJ_FLAG_HIDDEN);

        view_hit_ = lv_obj_create(ui_bar_);
        lv_obj_set_size(view_hit_, 44, 44);
        lv_obj_set_pos(view_hit_, 428, 0);
        lv_obj_set_style_radius(view_hit_, 8, 0);
        lv_obj_set_style_bg_opa(view_hit_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(view_hit_, 0, 0);
        lv_obj_set_style_pad_all(view_hit_, 0, 0);
        lv_obj_remove_flag(view_hit_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(view_hit_, LV_OBJ_FLAG_CLICKABLE);
        view_label_ = lv_label_create(view_hit_);
        lv_obj_set_style_text_font(view_label_, font, 0);
        lv_obj_set_style_text_color(view_label_, lv_color_hex(0x202B2D), 0);
        lv_label_set_text(view_label_, "A");
        lv_obj_center(view_label_);
        lv_obj_remove_flag(view_label_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view_hit_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(view_hit_, [](lv_event_t* e) {
            (void)e;
            ESP_LOGI("CHD_UI", "view tap: A (B/C in B2)");
        }, LV_EVENT_CLICKED, nullptr);

        // Tap the Hermes brand to cycle A -> B -> C -> A (no NVS, boot A).
        hermes_hit_ = lv_obj_create(ui_bar_);
        lv_obj_set_size(hermes_hit_, 142, 44);
        lv_obj_set_pos(hermes_hit_, 0, 0);
        lv_obj_set_style_bg_opa(hermes_hit_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hermes_hit_, 0, 0);
        lv_obj_set_style_pad_all(hermes_hit_, 0, 0);
        lv_obj_remove_flag(hermes_hit_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hermes_hit_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hermes_hit_, HermesViewCb, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(hermes_hit_, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < 8; ++i) {
            log_labels_[i] = lv_label_create(screen);
            lv_obj_set_pos(log_labels_[i], 8, 44 + i * 33);
            lv_obj_set_size(log_labels_[i], 328, 33);
            lv_label_set_long_mode(log_labels_[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(log_labels_[i], font, 0);
            lv_obj_set_style_text_color(log_labels_[i], lv_color_hex(0x202B2D), 0);
            lv_label_set_text(log_labels_[i], "");
            lv_obj_move_foreground(log_labels_[i]);
        }

        // B view cards: 3x2, 152x62, gap 4, y44..172.
        static const char* const kCardTitles[6] = {"状态", "静音", "音量", "RSSI", "SRAM", "音频帧"};
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3;
            const int row = i / 3;
            cards_[i] = lv_obj_create(screen);
            lv_obj_set_pos(cards_[i], 8 + col * 156, 44 + row * 66);
            lv_obj_set_size(cards_[i], 152, 62);
            lv_obj_set_style_radius(cards_[i], 0, 0);
            lv_obj_set_style_bg_color(cards_[i], lv_color_hex(0xF7F3E8), 0);
            lv_obj_set_style_bg_opa(cards_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cards_[i], 1, 0);
            lv_obj_set_style_border_color(cards_[i], lv_color_hex(0x202B2D), 0);
            lv_obj_set_style_pad_all(cards_[i], 4, 0);
            lv_obj_remove_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(cards_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* title = lv_label_create(cards_[i]);
            lv_obj_set_pos(title, 0, 0);
            lv_obj_set_style_text_font(title, font, 0);
            lv_obj_set_style_text_color(title, lv_color_hex(0x6B7372), 0);
            lv_label_set_text(title, kCardTitles[i]);
            card_values_[i] = lv_label_create(cards_[i]);
            lv_obj_set_pos(card_values_[i], 0, 26);
            lv_obj_set_width(card_values_[i], 140);
            lv_label_set_long_mode(card_values_[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(card_values_[i], font, 0);
            lv_obj_set_style_text_color(card_values_[i], lv_color_hex(0x202B2D), 0);
            lv_label_set_text(card_values_[i], "--");
            lv_obj_move_foreground(cards_[i]);
        }

        static const char* const kAMetrics[3] = {"状态", "内存", "帧"};
        for (int i = 0; i < 3; ++i) {
            a_metric_titles_[i] = lv_label_create(screen);
            lv_obj_set_pos(a_metric_titles_[i], 4, 58 + i * 77);
            lv_obj_set_width(a_metric_titles_[i], 60);
            lv_obj_set_style_text_align(a_metric_titles_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(a_metric_titles_[i], font, 0);
            lv_obj_set_style_text_color(a_metric_titles_[i], lv_color_hex(0x6B7372), 0);
            lv_label_set_text(a_metric_titles_[i], kAMetrics[i]);
            a_metric_values_[i] = lv_label_create(screen);
            lv_obj_set_pos(a_metric_values_[i], 4, 78 + i * 77);
            lv_obj_set_width(a_metric_values_[i], 60);
            lv_obj_set_style_text_align(a_metric_values_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(a_metric_values_[i], i == 2 ? small_font : font, 0);
            lv_obj_set_style_text_color(a_metric_values_[i], lv_color_hex(0x202B2D), 0);
            lv_label_set_long_mode(a_metric_values_[i], LV_LABEL_LONG_CLIP);
            lv_label_set_text(a_metric_values_[i], "--");
            lv_obj_move_foreground(a_metric_values_[i]);
        }

        a_sep_ = lv_obj_create(screen);
        lv_obj_set_pos(a_sep_, 68, 44);
        lv_obj_set_size(a_sep_, 1, 231);
        lv_obj_set_style_bg_color(a_sep_, lv_color_hex(0xD8D5C6), 0);
        lv_obj_set_style_bg_opa(a_sep_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(a_sep_, 0, 0);
        lv_obj_set_style_pad_all(a_sep_, 0, 0);
        lv_obj_remove_flag(a_sep_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(a_sep_);
        for (int i = 0; i < 3; ++i) {
            a_hlines_[i] = lv_obj_create(screen);
            lv_obj_set_pos(a_hlines_[i], 0, 121 + i * 77);
            lv_obj_set_size(a_hlines_[i], 68, 1);
            lv_obj_set_style_bg_color(a_hlines_[i], lv_color_hex(0xD8D5C6), 0);
            lv_obj_set_style_bg_opa(a_hlines_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(a_hlines_[i], 0, 0);
            lv_obj_set_style_pad_all(a_hlines_[i], 0, 0);
            lv_obj_remove_flag(a_hlines_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_foreground(a_hlines_[i]);
        }
        baseline_ = lv_obj_create(screen);
        lv_obj_set_pos(baseline_, 0, 276);
        lv_obj_set_size(baseline_, LV_HOR_RES, 1);
        lv_obj_set_style_bg_color(baseline_, lv_color_hex(0xD8D5C6), 0);
        lv_obj_set_style_bg_opa(baseline_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(baseline_, 0, 0);
        lv_obj_set_style_pad_all(baseline_, 0, 0);
        lv_obj_remove_flag(baseline_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(baseline_);

        dino_root_ = lv_obj_create(screen);
        lv_obj_set_pos(dino_root_, 0, 273);
        lv_obj_set_size(dino_root_, LV_HOR_RES, 47);
        lv_obj_set_style_bg_opa(dino_root_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dino_root_, 0, 0);
        lv_obj_set_style_pad_all(dino_root_, 0, 0);
        lv_obj_remove_flag(dino_root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dino_root_, LV_OBJ_FLAG_CLICKABLE);
        dino_sprite_ = lv_image_create(dino_root_);
        lv_image_set_src(dino_sprite_, &chrome_dino_idle_dsc);
        lv_image_set_scale(dino_sprite_, 186);  // ~33x35 for the 44px A strip
        lv_obj_set_pos(dino_sprite_, 18, 9);
        lv_obj_set_style_image_recolor(dino_sprite_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_image_recolor_opa(dino_sprite_, LV_OPA_COVER, 0);
        dino_cactus_img_ = lv_image_create(dino_root_);
        lv_image_set_src(dino_cactus_img_, &chrome_cactus_small_dsc);
        lv_image_set_scale(dino_cactus_img_, 210);  // ~14x29
        lv_obj_set_pos(dino_cactus_img_, 480, 18);
        lv_obj_set_style_image_recolor(dino_cactus_img_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_image_recolor_opa(dino_cactus_img_, LV_OPA_COVER, 0);
        lv_obj_add_flag(dino_root_, LV_OBJ_FLAG_HIDDEN);

        game_view_ = lv_obj_create(screen);
        lv_obj_set_pos(game_view_, 0, 170);
        lv_obj_set_size(game_view_, LV_HOR_RES, 150);
        lv_obj_set_style_bg_color(game_view_, lv_color_hex(0xF4F3EA), 0);
        lv_obj_set_style_bg_opa(game_view_, LV_OPA_TRANSP, 0);  // paper is already behind
        lv_obj_set_style_border_width(game_view_, 0, 0);
        lv_obj_set_style_pad_all(game_view_, 0, 0);
        lv_obj_remove_flag(game_view_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(game_view_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(game_view_, GameTapCb, LV_EVENT_CLICKED, this);
        game_ground_a_ = lv_image_create(game_view_);
        lv_image_set_src(game_ground_a_, &chrome_ground_dsc);
        lv_obj_set_pos(game_ground_a_, 0, 127);
        lv_obj_set_style_image_recolor(game_ground_a_, lv_color_hex(0x6B7372), 0);
        lv_obj_set_style_image_recolor_opa(game_ground_a_, LV_OPA_COVER, 0);
        game_ground_b_ = lv_image_create(game_view_);
        lv_image_set_src(game_ground_b_, &chrome_ground_dsc);
        lv_obj_set_pos(game_ground_b_, 600, 127);
        lv_obj_set_style_image_recolor(game_ground_b_, lv_color_hex(0x6B7372), 0);
        lv_obj_set_style_image_recolor_opa(game_ground_b_, LV_OPA_COVER, 0);
        game_dino_ = lv_image_create(game_view_);
        lv_image_set_src(game_dino_, &chrome_dino_idle_dsc);
        lv_obj_set_pos(game_dino_, 50, 80);
        lv_obj_set_style_image_recolor(game_dino_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_image_recolor_opa(game_dino_, LV_OPA_COVER, 0);
        game_cactus_ = lv_image_create(game_view_);
        lv_image_set_src(game_cactus_, &chrome_cactus_small_dsc);
        lv_obj_set_pos(game_cactus_, 480, 92);
        lv_obj_set_style_image_recolor(game_cactus_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_image_recolor_opa(game_cactus_, LV_OPA_COVER, 0);
        game_score_ = lv_label_create(game_view_);
        lv_obj_set_pos(game_score_, 8, 8);
        lv_obj_set_style_text_font(game_score_, small_font, 0);
        lv_obj_set_style_text_color(game_score_, lv_color_hex(0x6B7372), 0);
        lv_label_set_text(game_score_, "00000");
        game_ptero_ = lv_image_create(game_view_);
        lv_image_set_src(game_ptero_, &chrome_ptero_dsc);
        lv_obj_set_pos(game_ptero_, 480, 75);
        lv_obj_set_style_image_recolor(game_ptero_, lv_color_hex(0x202B2D), 0);
        lv_obj_set_style_image_recolor_opa(game_ptero_, LV_OPA_COVER, 0);
        lv_obj_add_flag(game_ptero_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(game_view_, LV_OBJ_FLAG_HIDDEN);
        game_timer_ = lv_timer_create(GameTick, 25, this);

        ascii_scope_ = lv_label_create(screen);
        lv_obj_set_pos(ascii_scope_, 8, 262);
        lv_obj_set_width(ascii_scope_, LV_HOR_RES - 16);
        lv_obj_set_style_text_font(ascii_scope_, font, 0);
        lv_obj_set_style_text_color(ascii_scope_, lv_color_hex(0xE2703A), 0);
        lv_label_set_text(ascii_scope_, "");
        lv_obj_add_flag(ascii_scope_, LV_OBJ_FLAG_HIDDEN);  // ASCII waveform retired; debug line only

        debug_line_ = lv_label_create(screen);
        lv_obj_set_pos(debug_line_, 8, 288);
        lv_obj_set_width(debug_line_, LV_HOR_RES - 16);
        lv_obj_set_style_text_font(debug_line_, small_font, 0);
        lv_obj_set_style_text_color(debug_line_, lv_color_hex(0x6B7372), 0);
        lv_label_set_text(debug_line_, "");
        lv_obj_add_flag(debug_line_, LV_OBJ_FLAG_HIDDEN);

        hermes_avatar_ = lv_image_create(screen);
        lv_image_set_src(hermes_avatar_, &hermes_face_64x72_dsc);
        lv_obj_set_pos(hermes_avatar_, 408, 204);
        lv_obj_set_size(hermes_avatar_, 64, 72);
        lv_obj_set_style_image_recolor(hermes_avatar_, lv_color_hex(0x1F2B2D), 0);
        lv_obj_set_style_image_recolor_opa(hermes_avatar_, LV_OPA_COVER, 0);
        lv_obj_add_flag(hermes_avatar_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hermes_avatar_, MuteButtonCb, LV_EVENT_CLICKED, this);
        lv_obj_move_foreground(hermes_avatar_);

        idle_wave_ = lv_line_create(screen);
        for (int i = 0; i < 40; ++i) {
            phase_b_wave_pts_[i].x = i * 12;
            phase_b_wave_pts_[i].y = 22;
        }
        lv_line_set_points(idle_wave_, phase_b_wave_pts_, 40);
        lv_obj_set_pos(idle_wave_, 0, 276);
        lv_obj_set_size(idle_wave_, LV_HOR_RES, 44);
        lv_obj_set_style_line_width(idle_wave_, 2, 0);
        lv_obj_set_style_line_color(idle_wave_, lv_color_hex(0xE2703A), 0);
        lv_obj_remove_flag(idle_wave_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(idle_wave_);

        if (subtitle_bar_ != nullptr && lv_obj_is_valid(subtitle_bar_)) {
            lv_obj_set_width(subtitle_bar_, LV_HOR_RES);
            lv_obj_set_height(subtitle_bar_, 44);
            lv_obj_align(subtitle_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_radius(subtitle_bar_, 0, 0);
            lv_obj_set_style_bg_color(subtitle_bar_, lv_color_hex(0x202B2D), 0);
            lv_obj_set_style_bg_opa(subtitle_bar_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(subtitle_bar_, 0, 0);
            lv_obj_align(subtitle_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_move_foreground(subtitle_bar_);
            if (subtitle_label_ != nullptr && lv_obj_is_valid(subtitle_label_)) {
                lv_obj_set_width(subtitle_label_, LV_HOR_RES - 24);
                lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_center(subtitle_label_);
            }
        }

        if (face_panel_ != nullptr && lv_obj_is_valid(face_panel_)) {
            lv_obj_add_flag(face_panel_, LV_OBJ_FLAG_HIDDEN);
        }
        if (mute_overlay_ != nullptr && lv_obj_is_valid(mute_overlay_)) {
            lv_obj_add_flag(mute_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (mute_button_ != nullptr && lv_obj_is_valid(mute_button_)) {
            lv_obj_add_flag(mute_button_, LV_OBJ_FLAG_HIDDEN);
        }

        phase_b_built_ = true;
        ApplyPhaseBFonts(ActiveTextFont());
        ApplyViewLayout();
        UpdateStatusLine();
        UpdateMuteUi();
        phase_b_timer_ = lv_timer_create(PhaseBTick, 250, this);
        a_dino_timer_ = lv_timer_create(ADinoTick, 80, this);
    }

    const char* MuteShortText() const {
        switch (mute_ui_state_) {
            case MuteUiState::kNoEndpoint: return "待同步";
            case MuteUiState::kSyncing: return "同步中";
            case MuteUiState::kToggling: return "切换中";
            case MuteUiState::kMuted: return "已静音";
            case MuteUiState::kFailed: return "同步失败";
            default: return "未静音";
        }
    }

    const char* StateText() const {
        switch (Application::GetInstance().GetDeviceState()) {
            case kDeviceStateListening: return "在听";
            case kDeviceStateSpeaking: return "在说";
            case kDeviceStateConnecting:
            case kDeviceStateActivating: return "连接";
            case kDeviceStateWifiConfiguring: return "配网";
            default: return "空闲";
        }
    }

    static void HermesViewCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        if (lv_tick_elaps(s_touch_ready_ms) < kTapBootGuardMs) {
            ESP_LOGI("CHD_UI", "view tap ignored: boot guard");
            return;
        }
        if (lv_tick_elaps(self->last_gesture_ms_) < 400) {
            ESP_LOGI("CHD_UI", "view tap ignored: %u ms after swipe",
                     static_cast<unsigned>(lv_tick_elaps(self->last_gesture_ms_)));
            return;
        }
        DisplayLockGuard lock(self);
        switch (self->phase_b_view_) {
            case PhaseBView::kA: self->phase_b_view_ = PhaseBView::kB; break;
            case PhaseBView::kB: self->phase_b_view_ = PhaseBView::kC; break;
            case PhaseBView::kC: self->phase_b_view_ = PhaseBView::kD; break;
            default: self->phase_b_view_ = PhaseBView::kA; break;
        }
        self->ApplyViewLayout();
        ESP_LOGI("CHD_UI", "view=%d", static_cast<int>(self->phase_b_view_));
    }

    void ApplyViewLayout() {
        const bool is_b = phase_b_view_ == PhaseBView::kB;
        const bool is_c = phase_b_view_ == PhaseBView::kC;
        const bool is_d = phase_b_view_ == PhaseBView::kD;
        for (int i = 0; i < 6; ++i) {
            if (cards_[i] != nullptr && lv_obj_is_valid(cards_[i])) {
                if (is_b) {
                    lv_obj_remove_flag(cards_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(cards_[i]);
                } else {
                    lv_obj_add_flag(cards_[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        const int count = is_d ? 3 : (is_c ? 8 : (is_b ? 3 : 7));
        const int y0 = is_b ? 180 : 44;
        const int line_h = is_c ? 27 : (is_b ? 31 : 33);
        const int width = is_c ? 400 : 328;
        const int x0 = (is_b || is_c || is_d) ? 8 : 72;
        const bool is_a = !is_b && !is_c && !is_d;
        for (int i = 0; i < 3; ++i) {
            for (lv_obj_t* obj : {a_metric_titles_[i], a_metric_values_[i]}) {
                if (obj == nullptr || !lv_obj_is_valid(obj)) {
                    continue;
                }
                if (is_a) {
                    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        if (a_sep_ != nullptr && lv_obj_is_valid(a_sep_)) {
            if (is_a) lv_obj_remove_flag(a_sep_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(a_sep_, LV_OBJ_FLAG_HIDDEN);
        }
        for (auto* line : a_hlines_) {
            if (line == nullptr || !lv_obj_is_valid(line)) continue;
            if (is_a) lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        }
        if (ascii_scope_ != nullptr && lv_obj_is_valid(ascii_scope_)) {
            lv_obj_add_flag(ascii_scope_, LV_OBJ_FLAG_HIDDEN);
        }
        if (debug_line_ != nullptr && lv_obj_is_valid(debug_line_)) {
            if (is_c) lv_obj_remove_flag(debug_line_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(debug_line_, LV_OBJ_FLAG_HIDDEN);
        }
        if (game_view_ != nullptr && lv_obj_is_valid(game_view_)) {
            if (is_d) {
                lv_obj_remove_flag(game_view_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(game_view_);
            } else {
                lv_obj_add_flag(game_view_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (dino_root_ != nullptr && lv_obj_is_valid(dino_root_)) {
            if (is_a) {
                lv_obj_remove_flag(dino_root_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(dino_root_);
            } else {
                lv_obj_add_flag(dino_root_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (baseline_ != nullptr && lv_obj_is_valid(baseline_)) {
            if (is_d) lv_obj_add_flag(baseline_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(baseline_, LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < 8; ++i) {
            if (log_labels_[i] == nullptr || !lv_obj_is_valid(log_labels_[i])) {
                continue;
            }
            if (i < count) {
                lv_obj_set_pos(log_labels_[i], x0, y0 + i * line_h);
                lv_obj_set_size(log_labels_[i], width, line_h);
                lv_obj_remove_flag(log_labels_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(log_labels_[i]);
            } else {
                lv_obj_add_flag(log_labels_[i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(log_labels_[i], "");
            }
        }
        if (hermes_avatar_ != nullptr && lv_obj_is_valid(hermes_avatar_)) {
            // A/B: official 64x72 @(408,204); C: 56x64 @(416,212) so the
            // right-bottom avatar (and its mute tap) survives while the log
            // keeps x8..408. D (game) hides it.
            if (is_d) {
                lv_obj_add_flag(hermes_avatar_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(hermes_avatar_, LV_OBJ_FLAG_HIDDEN);
            }
            if (is_c) {
                lv_obj_set_pos(hermes_avatar_, 416, 212);
                lv_obj_set_size(hermes_avatar_, 56, 64);
                lv_image_set_scale(hermes_avatar_, 224);  // 56/64 = 0.875
            } else {
                lv_obj_set_pos(hermes_avatar_, 408, 204);
                lv_obj_set_size(hermes_avatar_, 64, 72);
                lv_image_set_scale(hermes_avatar_, 256);
            }
            lv_obj_move_foreground(hermes_avatar_);
        }
    }

    static void GameTapCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr || self->phase_b_view_ != PhaseBView::kD || self->game_jumping_) {
            return;
        }
        self->game_jumping_ = true;
        self->game_vy_ = -10;
    }

    static void GameTick(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr || self->phase_b_view_ != PhaseBView::kD) {
            return;
        }
        DisplayLockGuard lock(self);
        if (self->media_active_) {
            return;
        }
        if (self->media_overlay_ != nullptr) {
            return;  // full-screen media owns the panel
        }
        const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        const bool subtitle = self->subtitle_bar_ != nullptr && lv_obj_is_valid(self->subtitle_bar_) &&
                              !lv_obj_has_flag(self->subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        if (speaking || subtitle) {
            lv_obj_add_flag(self->game_view_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(self->game_view_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->game_view_);
        // Static ground: moving a 480x12 strip every frame was the main dirty
        // cost on this panel. Cactus/dino still move.
        self->game_cactus_x_ -= 6;
        if (self->game_cactus_x_ < -30) {
            self->game_cactus_x_ = 480;
            self->game_large_cactus_ = !self->game_large_cactus_;
            lv_image_set_src(self->game_cactus_,
                             self->game_large_cactus_ ? &chrome_cactus_large_dsc
                                                      : &chrome_cactus_small_dsc);
            lv_obj_set_y(self->game_cactus_, self->game_large_cactus_ ? 77 : 92);
        }
        lv_obj_set_x(self->game_cactus_, self->game_cactus_x_);
        self->game_score_value_++;
        if (self->game_ticks_ % 3 == 0) {
            char score[16];
            snprintf(score, sizeof(score), "%05u", (unsigned)self->game_score_value_);
            lv_label_set_text(self->game_score_, score);
        }
        if (!self->game_ptero_active_ && self->game_cooldown_ > 0) self->game_cooldown_--;
        if (!self->game_ptero_active_ && self->game_cooldown_ == 0) {
            self->game_ptero_active_ = true;
            self->game_ptero_x_ = 480;
            self->game_ptero_y_ = (self->game_score_value_ / 300) % 2 ? 90 : 75;
            lv_obj_set_pos(self->game_ptero_, self->game_ptero_x_, self->game_ptero_y_);
            lv_obj_remove_flag(self->game_ptero_, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->game_ptero_active_) {
            self->game_ptero_x_ -= 8;
            if (self->game_ptero_x_ < -50) {
                self->game_ptero_active_ = false;
                self->game_cooldown_ = 300;
                lv_obj_add_flag(self->game_ptero_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_x(self->game_ptero_, self->game_ptero_x_);
            }
        }

        const int dx = 50, dy = self->game_dino_y_;
        const int cw = self->game_large_cactus_ ? 25 : 17;
        const int chh = self->game_large_cactus_ ? 50 : 35;
        const int cy = self->game_large_cactus_ ? 77 : 92;
        auto hit = [](int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
            return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
        };
        auto dead = [&]() {
            if (hit(dx + 22, dy + 0, 17, 16, self->game_cactus_x_, cy, cw, chh)) return true;
            if (hit(dx + 1, dy + 18, 30, 9, self->game_cactus_x_, cy, cw, chh)) return true;
            if (hit(dx + 10, dy + 35, 14, 8, self->game_cactus_x_, cy, cw, chh)) return true;
            if (self->game_ptero_active_ &&
                hit(dx + 22, dy + 0, 17, 16, self->game_ptero_x_, self->game_ptero_y_, 46, 40)) return true;
            if (self->game_ptero_active_ &&
                hit(dx + 1, dy + 18, 30, 9, self->game_ptero_x_, self->game_ptero_y_, 46, 40)) return true;
            return false;
        };
        if (dead() && self->game_invincible_ == 0) {
            self->game_score_value_ = 0;
            self->game_cactus_x_ = 480;
            self->game_ptero_active_ = false;
            self->game_cooldown_ = 300;
            self->game_invincible_ = 30;
            lv_label_set_text(self->game_score_, "00000");
            lv_obj_add_flag(self->game_ptero_, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->game_invincible_ > 0) self->game_invincible_--;
        self->game_ticks_++;
        self->game_dirty_px_ += 44 * 47 + cw * chh + 480 * 12;
        const uint64_t elapsed = esp_timer_get_time() - self->game_fps_start_us_;
        if (self->game_fps_start_us_ != 0 && elapsed >= 60000000ULL) {
            const uint32_t fps = (uint32_t)(self->game_ticks_ * 1000000ULL / elapsed);
            ESP_LOGI("CHD_UI", "DINO 60s fps=%u dirty=%llu px", fps,
                     (unsigned long long)(self->game_dirty_px_ / self->game_ticks_));
            self->game_ticks_ = 0;
            self->game_dirty_px_ = 0;
            self->game_fps_start_us_ = esp_timer_get_time();
        }
        if (self->game_fps_start_us_ == 0) self->game_fps_start_us_ = esp_timer_get_time();
        self->game_frame_++;
        if (self->game_frame_ % 3 == 0) {
            lv_image_set_src(self->game_dino_,
                             (self->game_frame_ / 3) % 2 ? &chrome_dino_run2_dsc : &chrome_dino_run1_dsc);
        }
        if (self->game_jumping_) {
            self->game_dino_y_ += self->game_vy_;
            self->game_vy_ += 1;
            if (self->game_dino_y_ >= 80) {
                self->game_dino_y_ = 80;
                self->game_vy_ = 0;
                self->game_jumping_ = false;
            }
            lv_obj_set_y(self->game_dino_, self->game_dino_y_);
        }
    }

    void StartDotBreathing() {
        if (hermes_dot_ == nullptr || !lv_obj_is_valid(hermes_dot_)) {
            return;
        }
        if (dot_breathing_) {
            return;
        }
        dot_breathing_ = true;
        lv_anim_del(hermes_dot_, nullptr);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, hermes_dot_);
        lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
            lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), 0);
        });
        lv_anim_set_values(&anim, 70, 255);
        lv_anim_set_duration(&anim, 1400);
        lv_anim_set_playback_duration(&anim, 1400);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim);
    }

    void StopDotBreathing() {
        dot_breathing_ = false;
        if (hermes_dot_ != nullptr && lv_obj_is_valid(hermes_dot_)) {
            lv_anim_del(hermes_dot_, nullptr);
            lv_obj_set_style_opa(hermes_dot_, LV_OPA_COVER, 0);
        }
    }

    // Theme refreshes replace the runtime font and release the old one. Every
    // label that captured ActiveTextFont() at setup must be re-pointed, or its
    // style_text_font dangles and LVGL crashes in lv_font_get_glyph_width
    // (InstrFetchProhibited, seen 3.3 s after boot).
    void ApplyPhaseBFonts(const lv_font_t* font) {
        if (font == nullptr) {
            return;
        }
        auto set = [font](lv_obj_t* obj) {
            if (obj != nullptr && lv_obj_is_valid(obj)) {
                lv_obj_set_style_text_font(obj, font, 0);
            }
        };
        set(state_chip_);
        set(mute_text_);
        set(mute_icon_);
        set(view_label_);
        set(hermes_dot_);
        set(hermes_name_);
        set(hermes_state_);
        set(time_label_);
        for (auto* label : log_labels_) {
            set(label);
        }
        for (auto* obj : a_metric_titles_) set(obj);
        for (int i = 0; i < 3; ++i) {
            if (a_metric_values_[i] != nullptr && lv_obj_is_valid(a_metric_values_[i])) {
                lv_obj_set_style_text_font(a_metric_values_[i],
                    i == 2 ? &lv_font_montserrat_14 : font, 0);
            }
        }
        set(ascii_scope_);
        set(debug_line_);
        for (auto* card : cards_) {
            if (card == nullptr || !lv_obj_is_valid(card)) {
                continue;
            }
            const uint32_t children = lv_obj_get_child_count(card);
            for (uint32_t i = 0; i < children; ++i) {
                set(lv_obj_get_child(card, i));
            }
        }
    }

    void UpdateCards() {
        if (phase_b_view_ != PhaseBView::kB) {
            return;
        }
        auto& board = Board::GetInstance();
        auto* codec = board.GetAudioCodec();
        auto& wifi = WifiManager::GetInstance();
        const int rssi = wifi.IsConnected() ? wifi.GetRssi() : 0;
        const uint32_t sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        auto& as = Application::GetInstance().GetAudioService();
        char buf[64];
        const char* values[6] = {StateText(), MuteShortText(), "--", "--", "--", "--"};
        char vol[16] = "--";
        if (codec != nullptr) {
            snprintf(vol, sizeof(vol), "%d", codec->output_volume());
            values[2] = vol;
        }
        char rssi_buf[16];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d", rssi);
        values[3] = rssi_buf;
        char sram_buf[16];
        snprintf(sram_buf, sizeof(sram_buf), "%uK", static_cast<unsigned>(sram));
        values[4] = sram_buf;
        snprintf(buf, sizeof(buf), "A%u D%u", static_cast<unsigned>(as.GetInputFrameCount()),
                 static_cast<unsigned>(as.GetDecodedFrameCount()));
        values[5] = buf;
        for (int i = 0; i < 6; ++i) {
            if (card_values_[i] != nullptr && lv_obj_is_valid(card_values_[i])) {
                lv_label_set_text(card_values_[i], values[i]);
            }
        }
    }

    void UpdateStatusLine() {
        if (status_line_ == nullptr || !lv_obj_is_valid(status_line_)) {
            return;
        }
        const bool link = Application::GetInstance().IsServerLinkUp();
        if (hermes_avatar_ != nullptr && lv_obj_is_valid(hermes_avatar_)) {
            const bool muted = mute_ui_state_ == MuteUiState::kMuted ||
                               (mute_ui_state_ == MuteUiState::kFailed && mute_last_confirmed_);
            lv_obj_set_style_opa(hermes_avatar_, muted ? 77 : (link ? LV_OPA_COVER : 102), 0);
        }
        if (state_chip_ != nullptr && lv_obj_is_valid(state_chip_)) {
            const char* state_text = "空闲";
            switch (Application::GetInstance().GetDeviceState()) {
                case kDeviceStateListening: state_text = "在听"; break;
                case kDeviceStateSpeaking: state_text = "在说"; break;
                case kDeviceStateConnecting:
                case kDeviceStateActivating: state_text = "连接"; break;
                case kDeviceStateWifiConfiguring: state_text = "配网"; break;
                default: break;
            }
            lv_label_set_text(state_chip_, state_text);
        }
        if (time_label_ != nullptr && lv_obj_is_valid(time_label_)) {
            time_t now = time(nullptr);
            struct tm tm_buf;
            localtime_r(&now, &tm_buf);
            char tb[8];
            strftime(tb, sizeof(tb), "%H:%M", &tm_buf);
            lv_label_set_text(time_label_, tb);
        }
        if (hermes_dot_ != nullptr && lv_obj_is_valid(hermes_dot_)) {
            if (link) {
                StartDotBreathing();
            } else {
                StopDotBreathing();
            }
            lv_obj_set_style_text_color(hermes_dot_,
                                        lv_color_hex(link ? 0x30D158 : 0x8A8F8D), 0);
            lv_label_set_text(hermes_state_, link ? "在线" : "离线");
            if (link) {
                lv_obj_set_style_text_color(hermes_state_, lv_color_hex(0x202B2D), 0);
            } else {
                lv_obj_set_style_text_color(hermes_state_, lv_color_hex(0x8A8F8D), 0);
            }
        }
        auto& wifi = WifiManager::GetInstance();
        const int rssi = wifi.IsConnected() ? wifi.GetRssi() : 0;
        const uint32_t sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        auto& as = Application::GetInstance().GetAudioService();
        if (a_metric_values_[0] != nullptr && lv_obj_is_valid(a_metric_values_[0])) {
            lv_label_set_text(a_metric_values_[0], StateText());
            char mem[16];
            snprintf(mem, sizeof(mem), "%uK", static_cast<unsigned>(sram));
            lv_label_set_text(a_metric_values_[1], mem);
            char frames[24];
            snprintf(frames, sizeof(frames), "%u", static_cast<unsigned>(as.GetInputFrameCount()));
            lv_label_set_text(a_metric_values_[2], frames);
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "R %d  S %uK  A %u/%u  D %u/%u", rssi,
                 static_cast<unsigned>(sram), static_cast<unsigned>(as.GetInputFrameCount()),
                 static_cast<unsigned>(as.GetEncodedFrameCount()),
                 static_cast<unsigned>(as.GetDecodedFrameCount()),
                 static_cast<unsigned>(as.GetPlaybackFrameCount()));
        lv_label_set_text(status_line_, buf);
    }

    static void ADinoTick(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr || self->phase_b_view_ != PhaseBView::kA) {
            return;
        }
        DisplayLockGuard lock(self);
        if (self->media_active_ || self->media_overlay_ != nullptr) {
            if (self->dino_root_ != nullptr) lv_obj_add_flag(self->dino_root_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        const bool subtitle = self->subtitle_bar_ != nullptr && lv_obj_is_valid(self->subtitle_bar_) &&
                              !lv_obj_has_flag(self->subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        if (speaking || subtitle) {
            if (self->dino_root_ != nullptr) lv_obj_add_flag(self->dino_root_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (self->dino_root_ != nullptr) {
            lv_obj_remove_flag(self->dino_root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(self->dino_root_);
        }
        if (self->dino_sprite_ != nullptr) {
            self->dino_legs_up_ = !self->dino_legs_up_;
            lv_image_set_src(self->dino_sprite_,
                             self->dino_legs_up_ ? &chrome_dino_run2_dsc : &chrome_dino_run1_dsc);
        }
        if (self->dino_cactus_img_ != nullptr) {
            self->dino_cactus_x_ -= 5;
            if (self->dino_cactus_x_ < -20) self->dino_cactus_x_ = 480;
            lv_obj_set_x(self->dino_cactus_img_, self->dino_cactus_x_);
        }
    }

    static void PhaseBTick(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr) {
            return;
        }
        DisplayLockGuard lock(self);
        if (self->media_active_) {
            return;
        }
        UiLogLine* lines = self->phase_b_lines_;
        const size_t n = UiLog::GetInstance().Snapshot(lines, 10);
        const int count = self->phase_b_view_ == PhaseBView::kC
                              ? 8
                              : (self->phase_b_view_ == PhaseBView::kB ? 3 : 7);
        const size_t show = n < static_cast<size_t>(count) ? n : static_cast<size_t>(count);
        for (int i = 0; i < 8; ++i) {
            if (self->log_labels_[i] == nullptr || !lv_obj_is_valid(self->log_labels_[i])) {
                continue;
            }
            if (i >= count) {
                continue;  // hidden by ApplyViewLayout
            }
            if (i >= static_cast<int>(show)) {
                lv_label_set_text(self->log_labels_[i], "");
                continue;
            }
            const UiLogLine& line = lines[n - show + i];
            const uint32_t ms = line.uptime_ms;
            char buf[160];
            if (line.repeat > 0) {
                snprintf(buf, sizeof(buf), "%02u:%02u.%03u %s %s x%u", (unsigned)(ms / 60000) % 100,
                         (unsigned)(ms / 1000) % 60, (unsigned)ms % 1000, line.tag, line.text,
                         (unsigned)line.repeat + 1);
            } else {
                snprintf(buf, sizeof(buf), "%02u:%02u.%03u %s %s", (unsigned)(ms / 60000) % 100,
                         (unsigned)(ms / 1000) % 60, (unsigned)ms % 1000, line.tag, line.text);
            }
            lv_label_set_text(self->log_labels_[i], buf);
        }
        const bool is_c_view = self->phase_b_view_ == PhaseBView::kC;
        const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        const bool subtitle_shown =
            self->subtitle_bar_ != nullptr && lv_obj_is_valid(self->subtitle_bar_) &&
            !lv_obj_has_flag(self->subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        if (is_c_view) {
            const uint32_t up = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
            const uint32_t heap_k =
                static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            char dbg[96];
            snprintf(dbg, sizeof(dbg), "up %02u:%02u:%02u  heap %uK  A %u/D %u",
                     (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60),
                     (unsigned)heap_k,
                     (unsigned)Application::GetInstance().GetAudioService().GetInputFrameCount(),
                     (unsigned)Application::GetInstance().GetAudioService().GetDecodedFrameCount());
            lv_label_set_text(self->debug_line_, dbg);
            if (!speaking && !subtitle_shown) {
                lv_obj_remove_flag(self->debug_line_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(self->debug_line_, LV_OBJ_FLAG_HIDDEN);
            }
            if (self->idle_wave_ != nullptr && lv_obj_is_valid(self->idle_wave_)) {
                lv_obj_add_flag(self->idle_wave_, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (self->debug_line_ != nullptr && lv_obj_is_valid(self->debug_line_)) {
            lv_obj_add_flag(self->debug_line_, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->idle_wave_ != nullptr && lv_obj_is_valid(self->idle_wave_) && self->phase_b_view_ == PhaseBView::kB) {
            if (speaking || subtitle_shown) {
                lv_obj_add_flag(self->idle_wave_, LV_OBJ_FLAG_HIDDEN);
            } else {
                // P wave -> QRS -> T wave, two beats per 48-sample cycle.
                static const int8_t ecg[48] = {
                    22, 22, 22, 22, 21, 20, 21, 22,
                    22, 22, 24, 30, 36, 40, 12, 4,
                    22, 20, 18, 19, 21, 22, 22, 22,
                    22, 22, 22, 22, 21, 20, 21, 22,
                    22, 22, 23, 28, 34, 38, 14, 6,
                    22, 20, 18, 19, 21, 22, 22, 22};
                self->wave_phase_ = (self->wave_phase_ + 1) % 48;
                for (int i = 0; i < 40; ++i) {
                    self->phase_b_wave_pts_[i].y = ecg[(self->wave_phase_ + i) % 48];
                }
                lv_line_set_points(self->idle_wave_, self->phase_b_wave_pts_, 40);
                lv_obj_remove_flag(self->idle_wave_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (self->phase_b_view_ == PhaseBView::kB) {
            self->UpdateCards();
        }
        if (self->phase_b_view_ != PhaseBView::kB &&
            self->idle_wave_ != nullptr && lv_obj_is_valid(self->idle_wave_)) {
            lv_obj_add_flag(self->idle_wave_, LV_OBJ_FLAG_HIDDEN);
        }
        self->phase_b_ticks_++;
        if (self->phase_b_ticks_ >= 2) {
            self->phase_b_ticks_ = 0;
            self->UpdateStatusLine();
        }
    }

    // M7-S1: replace the stock centered emoji with the kawaii face, and add a
    // bottom subtitle bar as a *sibling* container (the face redraws every
    // 30 ms; keeping the text in a separate object means it is not invalidated).
    void SetupFace(lv_obj_t* screen) {
        if (face_panel_ != nullptr) {
            ESP_LOGW(TAG, "SetupFace() already ran, skipping");
            return;
        }

        // Hide the stock emoji layer and the stock bottom bar; this board draws
        // its own subtitle strip instead.
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        face_panel_ = lv_obj_create(screen);
        lv_obj_set_size(face_panel_, LV_HOR_RES, kFacePanelHeight);
        lv_obj_align(face_panel_, LV_ALIGN_TOP_MID, 0, kFaceIdleOffsetY);
        lv_obj_set_style_bg_opa(face_panel_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(face_panel_, 0, 0);
        lv_obj_set_style_pad_all(face_panel_, 0, 0);
        lv_obj_remove_flag(face_panel_, LV_OBJ_FLAG_SCROLLABLE);
        // Keep the face just above the background container so the status bars,
        // camera preview and the subtitle strip all render on top of it.
        lv_obj_move_to_index(face_panel_, 1);

        face_set_lvgl_lock_fns(FaceLvglLock, FaceLvglUnlock);
        face_config_t face_config = {
            .parent = face_panel_,
            .animation_speed = 30,
            .blink_interval = 3000,
            .auto_blink = true,
        };
        esp_err_t err = face_animation_init(&face_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "face_animation_init failed: %s", esp_err_to_name(err));
            lv_obj_delete(face_panel_);
            face_panel_ = nullptr;
            if (emoji_box_ != nullptr) {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

        SetupSubtitle(screen);

        // task-29: a tap on the face reacts locally; it must not bubble to the
        // screen (which would toggle the dialogue on top of the reaction).
        MakeChildrenNonClickable(face_panel_);  // keeps face_panel_ itself clickable
        lv_obj_add_event_cb(face_panel_, FaceClickCb, LV_EVENT_CLICKED, this);

        face_ready_ = true;
        face_set_emotion(FACE_NEUTRAL, false);
        ESP_LOGI(TAG, "kawaii face ready: panel=%dx%d face_sz=%d, subtitle %dx%d",
                 LV_HOR_RES, kFacePanelHeight, kFacePanelHeight, kSubtitleBarWidth,
                 kSubtitleBarRadius * 2);
    }

    // ---- B2 mute UI -------------------------------------------------------
    // Layout: 480x228 face panel; the kawaii face is min(480,228)=228 wide and
    // centred, leaving ~126 px empty on each side. The 56x56 button sits in the
    // left margin at (8,160), clear of the face and of the subtitle strip
    // (y228..312). The overlay is a child of the face panel, so it follows the
    // idle(+46)/speaking(0) slide without extra animation code.
    static void MuteButtonCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self == nullptr) {
            return;
        }
        if (lv_tick_elaps(s_touch_ready_ms) < kTapBootGuardMs) {
            ESP_LOGI("CHD_UI", "mute tap ignored: boot guard");
            return;
        }
        if (lv_tick_elaps(self->last_gesture_ms_) < kTapAfterGestureGuardMs) {
            ESP_LOGI("CHD_UI", "mute tap ignored: swipe");
            return;
        }
        DisplayLockGuard lock(self);
        const uint32_t started = lv_tick_get();
        self->mute_click_tick_ = started;
        self->mute_click_pending_ = true;
        // Never show "已静音" from a guess: until the server answers, only the
        // button says that a switch is in flight.
        self->mute_ui_state_ = MuteUiState::kToggling;
        self->UpdateMuteUi();
        lv_refr_now(nullptr);
        self->mute_click_render_ms_ = lv_tick_elaps(started);
        ESP_LOGI("CHD_UI", "mute_ui_ms=%u", (unsigned)self->mute_click_render_ms_);
        Application::GetInstance().ToggleMute();
    }

    void SetStatus(const char* status) override {
        SpiLcdDisplay::SetStatus(status);
        DisplayLockGuard lock(this);
        if (state_chip_ != nullptr && lv_obj_is_valid(state_chip_)) {
            // The stock display pushes HH:MM into SetStatus; the B1 bar owns
            // time on the right, so ignore clock strings here.
            if (status != nullptr && strchr(status, ':') == nullptr) {
                lv_label_set_text(state_chip_, status);
            }
        }
    }

    void SetupMuteUi(lv_obj_t* screen) {
        if (mute_button_ != nullptr) {
            return;
        }
        const lv_font_t* font = ActiveTextFont();

        mute_button_ = lv_obj_create(screen);
        lv_obj_set_size(mute_button_, 56, 56);
        // Top-left (not LEFT_MID): absolute y=160 keeps the button clear of the
        // face even while the panel slides.
        lv_obj_align(mute_button_, LV_ALIGN_TOP_LEFT, 8, 160);
        lv_obj_set_style_radius(mute_button_, 12, 0);
        lv_obj_set_style_bg_color(mute_button_, lv_color_hex(0x1E1E1E), 0);
        lv_obj_set_style_bg_opa(mute_button_, LV_OPA_70, 0);
        lv_obj_set_style_border_width(mute_button_, 2, 0);
        lv_obj_set_style_border_color(mute_button_, lv_color_white(), 0);
        lv_obj_set_style_border_opa(mute_button_, LV_OPA_40, 0);
        lv_obj_set_style_pad_all(mute_button_, 2, 0);
        lv_obj_remove_flag(mute_button_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mute_button_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(mute_button_);

        mute_button_label_ = lv_label_create(mute_button_);
        lv_obj_set_width(mute_button_label_, 52);
        lv_label_set_long_mode(mute_button_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(mute_button_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mute_button_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(mute_button_label_, font, 0);
        lv_obj_center(mute_button_label_);
        lv_obj_remove_flag(mute_button_label_, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(mute_button_label_, "静音");
        lv_obj_add_event_cb(mute_button_, MuteButtonCb, LV_EVENT_CLICKED, this);

        if (face_panel_ == nullptr || !lv_obj_is_valid(face_panel_)) {
            ESP_LOGW(TAG, "mute overlay skipped: no face panel");
            return;
        }
        mute_overlay_ = lv_obj_create(face_panel_);
        lv_obj_set_size(mute_overlay_, kFacePanelHeight, kFacePanelHeight);
        lv_obj_center(mute_overlay_);
        lv_obj_set_style_radius(mute_overlay_, 0, 0);
        lv_obj_set_style_border_width(mute_overlay_, 0, 0);
        lv_obj_set_style_pad_all(mute_overlay_, 0, 0);
        lv_obj_set_style_bg_color(mute_overlay_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mute_overlay_, static_cast<lv_opa_t>(255 * 55 / 100), 0);
        lv_obj_remove_flag(mute_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        // CRITICAL: a clickable overlay would swallow FaceClickCb. The overlay
        // is purely visual; taps must fall through to the face panel.
        lv_obj_remove_flag(mute_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(mute_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(mute_overlay_);

        mute_overlay_label_ = lv_label_create(mute_overlay_);
        lv_obj_set_width(mute_overlay_label_, kFacePanelHeight - 24);
        lv_label_set_long_mode(mute_overlay_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(mute_overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mute_overlay_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(mute_overlay_label_, font, 0);
        lv_obj_center(mute_overlay_label_);
        lv_obj_remove_flag(mute_overlay_label_, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(mute_overlay_label_, "已静音\n点左侧按钮解除");

        ESP_LOGI(TAG, "mute UI ready: button 56x56 @(8,160), overlay %dx%d",
                 kFacePanelHeight, kFacePanelHeight);
    }

    // Caller must hold the display lock.
    void UpdateMuteUi() {
        if (mute_button_label_ == nullptr || !lv_obj_is_valid(mute_button_label_)) {
            return;
        }
        const char* text = "静音";
        bool show_overlay = mute_last_confirmed_;
        switch (mute_ui_state_) {
            case MuteUiState::kNoEndpoint:
                text = "待同步";
                show_overlay = false;
                break;
            case MuteUiState::kSyncing:
                text = "同步中…";
                break;
            case MuteUiState::kToggling:
                text = "切换中…";
                break;
            case MuteUiState::kMuted:
                text = "解除静音";
                show_overlay = true;
                break;
            case MuteUiState::kUnmuted:
                text = "静音";
                show_overlay = false;
                break;
            case MuteUiState::kFailed:
                // Never claim "已静音" while the truth is unknown. A greyed
                // face may stay (hold is kept for safety) but its label says so.
                text = mute_last_confirmed_ ? "重试" : "失败\n重试";
                break;
        }
        lv_label_set_text(mute_button_label_, text);

        // Phase B A-view: permanent Chinese text + icon in the top bar.
        if (mute_text_ != nullptr && lv_obj_is_valid(mute_text_)) {
            const char* phase_b_text = "未静音";
            switch (mute_ui_state_) {
                case MuteUiState::kNoEndpoint:
                    phase_b_text = "待同步";
                    break;
                case MuteUiState::kSyncing:
                    phase_b_text = "同步中…";
                    break;
                case MuteUiState::kToggling:
                    phase_b_text = "切换中…";
                    break;
                case MuteUiState::kMuted:
                    phase_b_text = "已静音";
                    break;
                case MuteUiState::kUnmuted:
                    phase_b_text = "未静音";
                    break;
                case MuteUiState::kFailed:
                    phase_b_text = "同步失败";
                    break;
            }
            lv_label_set_text(mute_text_, phase_b_text);
        }
        if (hermes_avatar_ != nullptr && lv_obj_is_valid(hermes_avatar_)) {
            const bool muted = mute_ui_state_ == MuteUiState::kMuted ||
                               (mute_ui_state_ == MuteUiState::kFailed && mute_last_confirmed_);
            const bool link = Application::GetInstance().IsServerLinkUp();
            lv_obj_set_style_opa(hermes_avatar_, muted ? 77 : (link ? LV_OPA_COVER : 102), 0);
        }
        if (mute_overlay_label_ != nullptr && lv_obj_is_valid(mute_overlay_label_)) {
            // kMuted is the only state allowed to say "已静音".
            lv_label_set_text(mute_overlay_label_, mute_ui_state_ == MuteUiState::kFailed
                                                       ? "同步失败\n状态未知"
                                                       : "已静音\n点左侧按钮解除");
        }
        if (phase_b_built_) {
            show_overlay = false;  // text-only A view: no 228x228 overlay
        }
        if (mute_overlay_ != nullptr && lv_obj_is_valid(mute_overlay_)) {
            if (show_overlay) {
                lv_obj_remove_flag(mute_overlay_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(mute_overlay_);
            } else {
                lv_obj_add_flag(mute_overlay_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void SetMuteUiState(MuteUiState state) override {
        DisplayLockGuard lock(this);
        if (state == MuteUiState::kMuted) {
            mute_last_confirmed_ = true;
        } else if (state == MuteUiState::kUnmuted) {
            mute_last_confirmed_ = false;
        }
        mute_ui_state_ = state;
        UpdateMuteUi();
        ESP_LOGI("CHD_UI", "mute ui state=%d last_confirmed=%d", static_cast<int>(state),
                 mute_last_confirmed_ ? 1 : 0);

        // Total click -> final overlay on screen. Consume the marker on a final
        // state (or when there is no endpoint at all), never on the transient
        // labels themselves.
        const bool final_state = state == MuteUiState::kMuted ||
                                 state == MuteUiState::kUnmuted || state == MuteUiState::kFailed;
        if (mute_click_pending_ && final_state) {
            lv_refr_now(nullptr);
            const uint32_t total = lv_tick_elaps(mute_click_tick_);
            const uint32_t network =
                total >= mute_click_render_ms_ ? total - mute_click_render_ms_ : 0;
            ESP_LOGI("CHD_UI",
                     "mute ui total=%u ms (local render=%u ms, network+wait=%u ms)",
                     (unsigned)total, (unsigned)mute_click_render_ms_, (unsigned)network);
            mute_click_pending_ = false;
        } else if (state == MuteUiState::kNoEndpoint) {
            mute_click_pending_ = false;
        }
    }

    // Bottom subtitle strip: 464 px wide, content height (one line ~44 px, two
    // lines ~84 px), anchored 8 px above the screen bottom so growing upwards
    // never moves the face. Hidden and fully transparent while idle.
    void SetupSubtitle(lv_obj_t* screen) {
        // Static font in flash: no copy, no fallback pointer, no shared_ptr.
        // font_noto_sans_basic_30_4 is D-segment data (nm 0x3c24881c) whose
        // glyph callbacks resolve to flash code at 0x420edf6c / 0x420edd58.

        subtitle_bar_ = lv_obj_create(screen);
        lv_obj_set_width(subtitle_bar_, kSubtitleBarWidth);
        lv_obj_set_height(subtitle_bar_, LV_SIZE_CONTENT);
        lv_obj_align(subtitle_bar_, LV_ALIGN_BOTTOM_MID, 0, -kSubtitleBarMargin);
        lv_obj_set_style_radius(subtitle_bar_, kSubtitleBarRadius, 0);
        lv_obj_set_style_bg_color(subtitle_bar_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(subtitle_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(subtitle_bar_, 0, 0);
        lv_obj_set_style_pad_all(subtitle_bar_, kSubtitleBarPad, 0);
        lv_obj_remove_flag(subtitle_bar_, LV_OBJ_FLAG_SCROLLABLE);
        // A tap on the subtitle strip must still reach the screen (task-29), so
        // neither the strip nor its label may become the input target.
        lv_obj_remove_flag(subtitle_bar_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(subtitle_bar_);

        subtitle_label_ = lv_label_create(subtitle_bar_);
        lv_obj_set_width(subtitle_label_, kSubtitleBarWidth - 2 * kSubtitleBarPad);
        lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(subtitle_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(subtitle_label_, ActiveTextFont(), 0);
        lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(subtitle_label_, "");

        type_timer_ = lv_timer_create(TypewriterTick, kTypewriterPeriodMs, this);
        lv_timer_pause(type_timer_);
    }

    // Font of the stock UI (= the assets cbin font once assets are mounted, else
    // the compiled-in theme font). Both are real lv_font_t objects owned by the
    // framework, never a hand-copied struct.
    const lv_font_t* ActiveTextFont() const {
        auto theme = static_cast<LvglTheme*>(current_theme_);
        if (theme != nullptr && theme->text_font() != nullptr) {
            return theme->text_font()->font();
        }
        return LV_FONT_DEFAULT;
    }

    bool SetTextFont(std::shared_ptr<LvglFont> text_font) override {
        DisplayLockGuard lock(this);
        bool ok = SpiLcdDisplay::SetTextFont(text_font);
        if (subtitle_label_ != nullptr && lv_obj_is_valid(subtitle_label_)) {
            lv_obj_set_style_text_font(subtitle_label_, ActiveTextFont(), 0);
        }
        // The mute widgets use the same runtime font (the assets cbin font has
        // full Han coverage; the compiled-in basis font does not).
        if (mute_button_label_ != nullptr && lv_obj_is_valid(mute_button_label_)) {
            lv_obj_set_style_text_font(mute_button_label_, ActiveTextFont(), 0);
        }
        if (mute_overlay_label_ != nullptr && lv_obj_is_valid(mute_overlay_label_)) {
            lv_obj_set_style_text_font(mute_overlay_label_, ActiveTextFont(), 0);
        }
        ApplyPhaseBFonts(ActiveTextFont());
        return ok;
    }

    // Face never scales - its canvas geometry is latched in face_animation_init.
    // Speaking/idle only translates the panel (160 ms ease-out).
    void MoveFaceTo(int y) {
        if (face_panel_ == nullptr || !lv_obj_is_valid(face_panel_)) {
            return;
        }
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, face_panel_);
        lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
            lv_obj_set_y(static_cast<lv_obj_t*>(var), value);
        });
        lv_anim_set_values(&anim, lv_obj_get_y(face_panel_), y);
        lv_anim_set_duration(&anim, kFaceMoveMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_start(&anim);
    }

    static void SubtitleFadeDone(lv_anim_t* anim) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_anim_get_user_data(anim));
        if (self == nullptr) {
            return;
        }
        DisplayLockGuard lock(self);
        if (self->subtitle_bar_ != nullptr) {
            lv_obj_add_flag(self->subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (self->subtitle_label_ != nullptr) {
            lv_label_set_text(self->subtitle_label_, "");
        }
    }

    void ShowSubtitle() {
        if (!subtitle_visible_ || subtitle_bar_ == nullptr) {
            return;
        }
        lv_obj_remove_flag(subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(subtitle_bar_);
        MoveFaceTo(kFaceSpeakOffsetY);

        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, subtitle_bar_);
        lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
            lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), 0);
        });
        lv_anim_set_values(&anim, LV_OPA_TRANSP, phase_b_built_ ? LV_OPA_COVER : kSubtitleBarOpa);
        lv_anim_set_duration(&anim, kFaceMoveMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_start(&anim);
    }

    void FadeSubtitleOut() {
        MoveFaceTo(kFaceIdleOffsetY);
        if (subtitle_bar_ == nullptr) {
            return;
        }
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, subtitle_bar_);
        lv_anim_set_user_data(&anim, this);
        lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
            lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), 0);
        });
        lv_anim_set_values(&anim, lv_obj_get_style_bg_opa(subtitle_bar_, LV_PART_MAIN), LV_OPA_TRANSP);
        lv_anim_set_duration(&anim, 200);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&anim, SubtitleFadeDone);
        lv_anim_start(&anim);
    }

    // Typewriter: the protocol delivers a whole sentence (sentence_start) with no
    // per-character timing, so reveal ~11 chars/s and hold 2 s after the last one.
    static void TypewriterTick(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr) {
            lv_timer_pause(timer);
            return;
        }
        DisplayLockGuard lock(self);
        // Hard guard: the widgets can be gone (screen rebuilt / theme switched).
        // Touching a stale lv_obj_t here is exactly what panicked on the bench.
        if (self->subtitle_label_ == nullptr || !lv_obj_is_valid(self->subtitle_label_) ||
            self->subtitle_bar_ == nullptr || !lv_obj_is_valid(self->subtitle_bar_)) {
            ESP_LOGW(TAG, "subtitle widgets gone, stopping typewriter");
            self->subtitle_label_ = nullptr;
            self->subtitle_bar_ = nullptr;
            lv_timer_pause(timer);
            return;
        }
        if (self->subtitle_pos_ < self->subtitle_text_.size()) {
            const size_t index = self->subtitle_pos_;
            const auto lead = static_cast<unsigned char>(self->subtitle_text_[index]);
            size_t advance = 1;
            if ((lead & 0xF8) == 0xF0) {
                advance = 4;
            } else if ((lead & 0xF0) == 0xE0) {
                advance = 3;
            } else if ((lead & 0xE0) == 0xC0) {
                advance = 2;
            }
            self->subtitle_pos_ = std::min(self->subtitle_text_.size(), index + advance);
            // Copy into a member buffer first: never hand LVGL a pointer into a
            // temporary (lv_label_set_text copies, but this keeps it obvious).
            self->subtitle_shown_ = self->subtitle_text_.substr(0, self->subtitle_pos_);
            lv_label_set_text(self->subtitle_label_, self->subtitle_shown_.c_str());
            return;
        }
        if (self->subtitle_hold_ > 0) {
            --self->subtitle_hold_;
            return;
        }
        lv_timer_pause(timer);
        self->FadeSubtitleOut();
    }

    void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        const bool is_dialog = role != nullptr &&
                               (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0);
        // Only dialog text goes on the subtitle strip. "system" notices
        // (Wi-Fi/OTA/connection) keep the stock bottom bar - they can arrive
        // before/while SetupUI runs, and they are not speech.
        if (!is_dialog) {
            SpiLcdDisplay::SetChatMessage(role, content);
            return;
        }
        if (!face_ready_ || !subtitle_visible_) {
            SpiLcdDisplay::SetChatMessage(role, content);
            return;
        }
        if (subtitle_bar_ == nullptr) {
            ESP_LOGW(TAG, "subtitle bar missing, recreating");
            SetupSubtitle(lv_screen_active());
        }
        if (subtitle_bar_ == nullptr || subtitle_label_ == nullptr) {
            SpiLcdDisplay::SetChatMessage(role, content);
            return;
        }
        if (content == nullptr || content[0] == '\0') {
            return;  // empty text: nothing to show (stock behaviour)
        }
        subtitle_text_ = content;
        subtitle_shown_.clear();
        subtitle_pos_ = 0;
        subtitle_hold_ = kSubtitleHoldTicks;
        lv_label_set_text(subtitle_label_, "");
        ShowSubtitle();
        if (type_timer_ != nullptr) {
            lv_timer_reset(type_timer_);
            lv_timer_resume(type_timer_);
        }
    }

    void SetHideSubtitle(bool hide) override {
        DisplayLockGuard lock(this);
        subtitle_visible_ = !hide;
        if (type_timer_ != nullptr) {
            lv_timer_pause(type_timer_);
        }
        if (subtitle_bar_ != nullptr) {
            lv_obj_add_flag(subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(subtitle_bar_, LV_OPA_TRANSP, 0);
        }
        if (hide) {
            MoveFaceTo(kFaceIdleOffsetY);
        }
    }

    virtual bool SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (!face_ready_) {
            // Before the face exists (or if canvas init failed) keep stock behaviour.
            return SpiLcdDisplay::SetEmotion(emotion);
        }
        face_emotion_t mapped = FaceEmotionFromName(emotion);
        face_set_emotion(mapped, true);
        ESP_LOGI(TAG, "face emotion: %s -> %d", emotion != nullptr ? emotion : "(null)", (int)mapped);
        return true;
    }

    static void aec_switch_event_cb(lv_event_t * e) {
        CustomLcdDisplay* instance = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e)); // 1. 获取用户数据（即 this 指针）
        if (!instance) return;
        lv_obj_t * sw = static_cast<lv_obj_t*>(lv_event_get_target(e));                         // 2. 获取触发事件的对象（开关本身）
        bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);                                    // 3. 获取当前开关状态 // lv_obj_has_state 返回 true 表示处于 LV_STATE_CHECKED (打开)
        auto& app = Application::GetInstance();
        auto dev_state = app.GetDeviceState();
        if (dev_state == kDeviceStateIdle) {
            app.SetAecMode(is_on ? kAecOnDeviceSide : kAecOff);
            Settings settings("audio", true);
            settings.SetInt("aec_mode", app.GetAecMode());
        }
    }

    void create_setting_box(void) {      // 下滑，打开设置页面
        ESP_LOGW(TAG, "create_setting_box in");
        DisplayLockGuard lock(this);
        if (lv_obj_is_valid(setting_box_)) {
            ESP_LOGW(TAG, "create_setting_box already run");
            return;
        }

        auto& app = Application::GetInstance();

        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();

        setting_box_ = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(setting_box_);

        lv_obj_set_style_bg_color(setting_box_, lv_color_hex(0xF5F5F5), 0);
        lv_obj_set_style_bg_opa(setting_box_, LV_OPA_90, 0);
        lv_obj_set_size(setting_box_, lv_pct(100), lv_pct(100));
        lv_obj_add_flag(setting_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(setting_box_, LV_DIR_VER);        // 竖向滚动
        lv_obj_set_style_pad_bottom(setting_box_, 40, 0);       // 底部预留40空间
        lv_obj_align(setting_box_, LV_ALIGN_CENTER, 0, 0);
#ifdef USE_LCD_2_8
        static const int32_t grid_col[] = {LV_GRID_FR(1), LV_GRID_FR(14), LV_GRID_FR(14), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
#else
        static const int32_t grid_col[] = {LV_GRID_FR(2), LV_GRID_FR(4), LV_GRID_FR(4), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
#endif
        static const int32_t grid_row[] = {80, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};     // 第一行高度固定80
        lv_obj_set_grid_dsc_array(setting_box_, grid_col, grid_row);

        int grid_row_id = 0;            // --- 开关 1: AEC 语音打断 ---     // 第一行
        lv_obj_t *label_set = lv_label_create(setting_box_);
        lv_obj_set_style_pad_all(label_set, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_top(label_set, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(label_set, 2, 0);
        lv_obj_set_style_border_color(label_set, lv_color_hex(0x0000FF), 0);
        lv_obj_set_style_radius(label_set, 4, 0);
        lv_obj_set_style_text_font(label_set, text_font, LV_PART_MAIN);
        lv_label_set_text_static(label_set, "系统设置");
        lv_obj_set_grid_cell(label_set, LV_GRID_ALIGN_CENTER, 1, 2, LV_GRID_ALIGN_CENTER, grid_row_id++, 1);

        lv_obj_t *aec_box = lv_obj_create(setting_box_);
        lv_obj_set_size(aec_box, lv_pct(100), 50); // 宽度设为100%，自动占满父级单元格宽度
        lv_obj_set_style_bg_opa(aec_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(aec_box, 0, 0);
        lv_obj_set_style_border_width(aec_box, 0, 0);
        lv_obj_remove_flag(aec_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(aec_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *label_aec = lv_label_create(aec_box);
        lv_obj_set_style_text_font(label_aec, text_font, LV_PART_MAIN);
        lv_label_set_text_static(label_aec, "语音打断");
        lv_obj_align(label_aec, LV_ALIGN_LEFT_MID, 0, 0);

        auto aec_switch_ = lv_switch_create(aec_box);   // 开关
        lv_obj_set_size(aec_switch_, 50, 25);
        lv_obj_set_ext_click_area(aec_switch_, 10);
        lv_obj_align(aec_switch_, LV_ALIGN_RIGHT_MID, 0, 0);
#if CONFIG_USE_DEVICE_AEC
        if (AecMode::kAecOff != app.GetAecMode()) {
            lv_obj_add_state(aec_switch_, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(aec_switch_, aec_switch_event_cb, LV_EVENT_VALUE_CHANGED, this);
#endif
        lv_obj_set_grid_cell(aec_box, LV_GRID_ALIGN_STRETCH,  // 替换原来的LV_GRID_ALIGN_CENTER，自动拉伸填满单元格宽度
            1, 2, LV_GRID_ALIGN_CENTER, grid_row_id++, 1);

        // 第二行：音量控制
        auto volume_box_ = lv_obj_create(setting_box_);
        lv_obj_set_size(volume_box_, lv_pct(100), 50);
        lv_obj_set_style_bg_opa(volume_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(volume_box_, 0, 0);
        lv_obj_set_style_border_width(volume_box_, 0, 0);
        lv_obj_clear_flag(volume_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(volume_box_, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *lab_vol_min = lv_label_create(volume_box_);
        lv_obj_set_style_text_font(lab_vol_min, text_font, LV_PART_MAIN);
        lv_label_set_text_static(lab_vol_min, "音量");
        lv_obj_align(lab_vol_min, LV_ALIGN_LEFT_MID, 0, 0);

        volume_slider_ = lv_slider_create(volume_box_);
        lv_obj_set_size(volume_slider_, lv_pct(75), 10);
        lv_obj_set_ext_click_area(volume_slider_, 15);
        lv_obj_align_to(volume_slider_, lab_vol_min, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
        lv_slider_set_range(volume_slider_, 0, 100);
        int volume_value = 70;
        auto audio_codec = Board::GetInstance().GetAudioCodec();
        if (audio_codec) {
            volume_value = audio_codec->output_volume();
        }
        lv_slider_set_value(volume_slider_, volume_value, LV_ANIM_OFF);
        lv_obj_add_event_cb(volume_slider_, volume_slider_cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t *lab_vol_max = lv_label_create(volume_box_);
        lv_obj_set_style_text_font(lab_vol_max, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text_static(lab_vol_max, LV_SYMBOL_VOLUME_MAX);
        lv_obj_align(lab_vol_max, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_set_grid_cell(volume_box_, LV_GRID_ALIGN_STRETCH, 1, 2, LV_GRID_ALIGN_CENTER, grid_row_id++, 1);

        // ================= 第三行：亮度控制 =================
        auto brightness_box_ = lv_obj_create(setting_box_);
        lv_obj_set_size(brightness_box_, lv_pct(100), 50);
        lv_obj_set_style_bg_opa(brightness_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(brightness_box_, 0, 0);
        lv_obj_set_style_border_width(brightness_box_, 0, 0);
        lv_obj_clear_flag(brightness_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(brightness_box_, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *lab_bright_min = lv_label_create(brightness_box_);
        lv_obj_set_style_text_font(lab_bright_min, text_font, LV_PART_MAIN);
        lv_label_set_text_static(lab_bright_min, "亮度");
        lv_obj_align(lab_bright_min, LV_ALIGN_LEFT_MID, 0, 0);

        auto brightness_slider_ = lv_slider_create(brightness_box_);
        lv_obj_set_size(brightness_slider_, lv_pct(75), 10);
        lv_obj_set_ext_click_area(brightness_slider_, 15);
        lv_obj_align_to(brightness_slider_, lab_bright_min, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
        lv_slider_set_range(brightness_slider_, 1, 100);

        auto brightness = Board::GetInstance().GetBacklight()->brightness();
        lv_slider_set_value(brightness_slider_, brightness, LV_ANIM_OFF);
        lv_obj_add_event_cb(brightness_slider_, [](lv_event_t* e) {
            auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_t * slider_ = (lv_obj_t *)lv_event_get_target(e);
            if (self) {
                int slider_value_ = lv_slider_get_value(slider_);
                DisplayLockGuard lock(self);
                Board::GetInstance().GetBacklight()->SetBrightness(slider_value_);
            }
        }, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t *lab_bright_max = lv_label_create(brightness_box_);
        lv_obj_set_style_text_font(lab_bright_max, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text_static(lab_bright_max, LV_SYMBOL_PLUS);
        lv_obj_align(lab_bright_max, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_grid_cell(brightness_box_, LV_GRID_ALIGN_STRETCH, 1, 2, LV_GRID_ALIGN_CENTER, grid_row_id++, 1);

        ESP_LOGW(TAG, "create_setting_box out");
    }

private:
    ClickHandler click_handler_;
    TapHandler tap_handler_;
    uint32_t last_gesture_ms_ = 0;
    uint32_t face_reaction_index_ = 0;
    static constexpr uint32_t kTapAfterGestureGuardMs = 400;
};

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
// M7-S1B: official esp_emote_expression animation instead of the LVGL UI.
//
// Name resolution (verified, not guessed):
//  * the asset pack is built from
//    esp_emote_assets/320_240/emote.json, which defines 20 logical emote names
//    and maps each to an EAF file (happy->Happy.eaf, thinking->confused.eaf,
//    surprised->Happy.eaf, neutral->winking.eaf, idle->neutral.eaf, ...);
//  * scripts/spiffs_assets/build.py stores each of them as
//    {"name": <emote name>} , so the pack is indexed by the *logical name*;
//  * emote_load.c:192-199 looks it up with a case-sensitive strcmp against
//    that stored name.
// => SetEmotion("happy") hits the official animation directly; the EAF file
//    name/case is irrelevant. Only the names this firmware sends that the pack
//    does not define go through the alias table below (all "borrowed").
class ChdEmoteDisplay : public emote::EmoteDisplay {
public:
    ChdEmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                    int height)
        : emote::EmoteDisplay(panel, panel_io, width, height) {}

    bool SetEmotion(const char* emotion) override {
        return emote::EmoteDisplay::SetEmotion(EmotionToPackName(emotion));
    }

    static const char* EmotionToPackName(const char* emotion) {
        // 20 names defined by the official 320_240 layout (emote.json v1.0.2).
        static const char* const kNative[] = {
            "happy", "laughing", "funny", "loving", "embarrassed", "confident",
            "delicious", "sad", "crying", "sleepy", "silly", "angry", "surprised",
            "shocked", "thinking", "winking", "relaxed", "confused", "neutral", "idle",
        };
        // Extra names this firmware sends; none has a dedicated animation, so
        // each borrows the closest official one (documented in the board README).
        static const struct {
            const char* device;
            const char* pack;
        } kAlias[] = {
            {"listening", "idle"},        {"listen", "idle"},      {"speaking", "neutral"},
            {"robot_2", "neutral"},       {"working_hard", "thinking"},
            {"cool", "confident"},        {"kissy", "loving"},     {"warning", "shocked"},
        };
        if (emotion == nullptr) {
            return "neutral";
        }
        for (const char* name : kNative) {
            if (strcmp(name, emotion) == 0) {
                return emotion;  // exact official name: pass through
            }
        }
        for (const auto& entry : kAlias) {
            if (strcmp(entry.device, emotion) == 0) {
                return entry.pack;
            }
        }
        return "neutral";
    }
};
#endif  // CONFIG_USE_EMOTE_MESSAGE_STYLE

// ---------------------------------------------------------------------------
// Touch diagnosis (task-29 follow-up). Purely observational: it does not change
// any interaction behaviour, it only makes the touch path visible.
//   - are the raw I2C registers of the chip at 0x38 really a touch controller?
//   - does the panel report points on its own (TD_STATUS) when a finger is down?
//   - does LVGL's indev actually poll the panel (counted via the driver's
//     read_data wrapper; our own probe calls the original directly, so the two
//     are distinguishable)?
// ---------------------------------------------------------------------------
typedef enum { CHD_TOUCH_NONE = 0, CHD_TOUCH_GT911, CHD_TOUCH_FT5X06 } chd_touch_kind_t;
static chd_touch_kind_t s_touch_kind = CHD_TOUCH_NONE;
static esp_lcd_touch_handle_t s_touch = nullptr;
static lv_indev_t* s_touch_indev = nullptr;
static volatile uint32_t s_touch_indev_reads = 0;
static volatile uint32_t s_touch_poll_fails = 0;
static esp_err_t (*s_touch_read_orig)(esp_lcd_touch_handle_t) = nullptr;
static esp_lcd_panel_io_handle_t s_ft_io = nullptr;  // FT5x06 raw register access

#if CHD_TOUCH_PHANTOM_FILTER
// --- phantom-touch filter -------------------------------------------------
// The panel reports one point with constant coordinates from power-on and never
// a release, so LVGL sees a finger held down for ever (no CLICKED, ever). A real
// press must eventually move or lift: those two events reset the candidate, so
// only a point that has done neither for kPhantomHoldMs is suppressed.
static constexpr uint32_t kPhantomHoldMs = 1500;
static constexpr int32_t kPhantomMovePx = 2;
struct PhantomState {
    bool tracking = false;
    uint16_t first_x = 0;
    uint16_t first_y = 0;
    uint32_t first_ms = 0;
    bool ever_moved = false;
    bool ever_released = false;
    bool suppressing = false;
};
static PhantomState s_phantom;

static void TouchFilterApply(esp_lcd_touch_handle_t tp) {
    const uint32_t now = lv_tick_get();
    const uint8_t points = tp->data.points;
    if (points == 0) {
        // A release proves this is a real finger (or that the ghost is gone).
        if (s_phantom.suppressing) {
            ESP_LOGI("CHD_TOUCH", "phantom filter released (point lifted)");
        }
        s_phantom = PhantomState{};
        s_phantom.ever_released = true;
        return;
    }

    const esp_lcd_touch_point_data_t* p = &tp->data.coords[0];
    if (!s_phantom.tracking) {
        s_phantom = PhantomState{};
        s_phantom.tracking = true;
        s_phantom.first_x = p->x;
        s_phantom.first_y = p->y;
        s_phantom.first_ms = now;
        return;
    }

    const int32_t dx = static_cast<int32_t>(p->x) - static_cast<int32_t>(s_phantom.first_x);
    const int32_t dy = static_cast<int32_t>(p->y) - static_cast<int32_t>(s_phantom.first_y);
    if (dx > kPhantomMovePx || dx < -kPhantomMovePx || dy > kPhantomMovePx ||
        dy < -kPhantomMovePx) {
        s_phantom.ever_moved = true;
        s_phantom.first_x = p->x;
        s_phantom.first_y = p->y;
    }
    if (points > 1) {
        s_phantom.ever_moved = true;  // a second finger means somebody is there
    }

    const bool suspect = !s_phantom.ever_moved && !s_phantom.ever_released &&
                         (now - s_phantom.first_ms) >= kPhantomHoldMs;
    if (suspect != s_phantom.suppressing) {
        s_phantom.suppressing = suspect;
        if (suspect) {
            ESP_LOGW("CHD_TOUCH",
                     "phantom suppressed (x=%u y=%u, held=%u ms, strength=%u) - not reported "
                     "to LVGL",
                     p->x, p->y, static_cast<unsigned>(now - s_phantom.first_ms), p->strength);
        } else {
            ESP_LOGI("CHD_TOUCH", "phantom filter released (point moved / lifted)");
        }
    }
    if (suspect) {
        tp->data.points = 0;  // LVGL sees "no touch"
    }
}
#endif  // CHD_TOUCH_DIAG

#if CHD_TOUCH_LOG_INDEV
// Observes the one and only reader (LVGL's indev) from the inside: no extra I2C
// traffic, no race. Prints only when what LVGL will see changes, plus a 10 s
// heartbeat naming the object that is currently pressed.
static void TouchLogIndevState(esp_lcd_touch_handle_t tp) {
    static uint8_t last_pressed = 0xFF;
    static uint8_t last_points = 0xFF;
    static uint16_t last_x = 0xFFFF;
    static uint16_t last_y = 0xFFFF;
    static uint32_t last_beat_ms = 0;

    const uint8_t points = tp->data.points;
    const uint16_t x = (points > 0) ? tp->data.coords[0].x : 0;
    const uint16_t y = (points > 0) ? tp->data.coords[0].y : 0;
    lv_indev_t* indev = s_touch_indev;
    const uint8_t pressed =
        (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) ? 1 : 0;

    const bool changed = (pressed != last_pressed) || (points != last_points) || (x != last_x) ||
                         (y != last_y);
    const uint32_t now = lv_tick_get();
    if (changed) {
        last_pressed = pressed;
        last_points = points;
        last_x = x;
        last_y = y;
        last_beat_ms = now;
        // NOTE: the active object must NOT be sampled here - inside read_data the
        // indev has not processed this sample yet, so it is always NULL. It is
        // sampled by IndevActTimerCb() after the frame instead.
        ESP_LOGI("CHD_TOUCH", "indev state: pressed=%u points=%u x=%u y=%u", pressed, points, x, y);
    } else if (lv_tick_elaps(last_beat_ms) >= 10000) {
        last_beat_ms = now;
        ESP_LOGI("CHD_TOUCH", "indev heartbeat: pressed=%u points=%u x=%u y=%u state=%d", pressed,
                 points, x, y,
                 (indev != nullptr) ? static_cast<int>(lv_indev_get_state(indev)) : -1);
    }
}
#endif  // CHD_TOUCH_LOG_INDEV

// D2: no synthetic release. A finger that rests still must keep LVGL pressed
// until the panel itself stops reporting points; otherwise a >2 s press gets a
// fake RELEASED/CLICKED. `s_forced_releases` is kept at 0 only so the existing
// 5 s heartbeat line keeps its format.
static uint32_t s_forced_releases = 0;
static uint32_t s_filter_frames = 0;   // frames the filter saw
static uint8_t s_filter_last_raw = 0;  // points reported by the chip
static uint8_t s_filter_last_kept = 0; // points handed to LVGL
static uint32_t s_filter_stale = 0;    // held-still events (now HOLD, no release)
static uint32_t s_filter_oob = 0;      // drops: outside the display
static uint32_t s_filter_ghost = 0;    // reserved: ghost filter removed by D1

// task-1/C4: the touch mapping must not follow DISPLAY_MIRROR_* - those describe
// how the panel scan is flipped for the *picture*, and changing them would flip
// the display. Four-corner measurement on 2026-10-28 (second flash):
//   touch the *visible* top-left    -> raw(319,10) -> mapped(10,319) (bottom-left)
//   touch the *visible* bottom-left -> raw(0,44)   -> mapped(44,0)   (top-left)
// X is correct, Y is inverted: the correct mapping is Y = (DISPLAY_HEIGHT - 1) -
// raw_x. So touch gets its own mirror flags, derived from that measurement rather
// than from DISPLAY_*. Only the swap is shared (display and touch agree on it).
// Declared outside CHD_TOUCH_POINT_FILTER because the mapping log in
// InitializeTouch_ft6336() prints them as well.
static constexpr bool kTouchMirrorX = false;
static constexpr bool kTouchMirrorY = true;

#if CHD_TOUCH_POINT_FILTER
// --- point filter: raw -> display mapping, bounds, stale hold (D2)
static constexpr uint32_t kPointStaleMs = 2000;
// Sticky threshold: a contact that moves less than this between two samples is
// still the same resting finger. A real drag moves far more, so it re-arms the
// stale timer (30 px is unchanged from the previous tuning).
static constexpr int32_t kPointStalePx = 30;
// D3: how long a resting point may hold LVGL pressed before it is treated as a
// phantom (production 2026-10-29: a stuck point kept the indev pressed forever
// and swallowed the mute buttons and every screen tap).
static constexpr uint32_t kPointStaleForceMs = 2500;

// D1 (2026-10-28 cold-boot trace): the 60 s CHD_TOUCH_RAW capture contains zero
// contact frames (n>=1) and no (442,202) point, so this panel does not emit the
// historical boot ghost. The old permanent 60x60 anchor dead zone is therefore
// removed entirely; only the 3 s tap guard (kTapBootGuardMs) remains, which is
// what actually keeps the first boot frames from producing a click.
struct PointTrack {
    bool seen = false;
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t same_since = 0;
    bool dropped = false;
    bool stale_logged = false;  // D2: log the transition to held-stale once
    // D3 (2026-10-29): a point this stale was force-released and locked as a
    // phantom; it stays dropped until the panel reports no point (first again)
    // or a clearly different contact.
    bool locked = false;
    uint16_t lock_x = 0;
    uint16_t lock_y = 0;
    bool lock_logged = false;
};
static PointTrack s_point_track[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];


// The FT5x06 driver copies raw register values into tp->data and applies neither
// swap_xy nor mirror, so the mapping belongs here. The swap is shared with the
// display; the mirrors are touch-only (kTouchMirror*, see their comment above).
static void TouchMapRawToDisplay(uint16_t raw_x, uint16_t raw_y, uint16_t* out_x,
                                 uint16_t* out_y) {
    constexpr bool kSwap = DISPLAY_SWAP_XY;
    uint16_t mx = kSwap ? raw_y : raw_x;
    uint16_t my = kSwap ? raw_x : raw_y;
    if (kTouchMirrorX) {
        mx = static_cast<uint16_t>((DISPLAY_WIDTH - 1) - mx);
    }
    if (kTouchMirrorY) {
        my = static_cast<uint16_t>((DISPLAY_HEIGHT - 1) - my);
    }
    *out_x = mx;
    *out_y = my;
}

static void TouchPointFilter(esp_lcd_touch_handle_t tp) {
    const uint32_t now = lv_tick_get();
    // task-2/C2: anchor the tap boot guard to the first filtered sample, not to
    // lvgl_port_add_touch(): camera init can block the first frame by ~500 ms,
    // which left only ~490 ms between the ghost's first stale release and the
    // 3 s guard. Anchored here the margin is always ~1 s (stale = 2000 ms).
    // POINT_FILTER=0 builds never run this function, so s_touch_ready_ms stays 0
    // and the guard degrades to "3 s after tick 0" - acceptable.
    if (s_touch_ready_ms == 0) {
        s_touch_ready_ms = now;
    }
    const uint8_t raw_points = tp->data.points;
    uint8_t kept = 0;

    s_filter_frames = s_filter_frames + 1;
    s_filter_last_raw = raw_points;

    // `seen` means "this index had a point in the previous frame"; clear the
    // indices that are gone now, the loop below marks the live ones.
    for (uint8_t i = raw_points; i < CONFIG_ESP_LCD_TOUCH_MAX_POINTS; i++) {
        s_point_track[i].seen = false;
    }

    for (uint8_t i = 0; i < raw_points; i++) {
        const uint16_t raw_x = tp->data.coords[i].x;
        const uint16_t raw_y = tp->data.coords[i].y;
        uint16_t mx = 0;
        uint16_t my = 0;
        TouchMapRawToDisplay(raw_x, raw_y, &mx, &my);

        const bool in_bounds = (mx < DISPLAY_WIDTH) && (my < DISPLAY_HEIGHT);
        PointTrack& track = s_point_track[i];
        const bool first = !track.seen;  // no point at this index in the previous frame
        const bool moved = (mx > track.x + kPointStalePx) || (mx + kPointStalePx < track.x) ||
                           (my > track.y + kPointStalePx) || (my + kPointStalePx < track.y);
        const bool was_dropped = track.dropped;
        // task-1/A2: sticky drop. Only a first contact or real movement (> threshold)
        // re-arms the point (position, timer and drop flag). A point already judged
        // stale stays dropped while it jitters inside the threshold - otherwise the
        // +-16 px ghost kept resetting same_since and LVGL never saw a release.
        if (first || moved) {
            track.x = mx;
            track.y = my;
            track.same_since = now;
        }
        track.seen = true;

        // D3: keep a force-released phantom dropped until the panel reports no
        // point at this index for a frame (first contact again) or a clearly
        // different contact; otherwise the same stuck point would re-press LVGL
        // immediately and every tap would be swallowed again.
        const bool far_from_lock =
            track.locked &&
            ((mx > track.lock_x + kPointStalePx) || (mx + kPointStalePx < track.lock_x) ||
             (my > track.lock_y + kPointStalePx) || (my + kPointStalePx < track.lock_y));
        if (track.locked && (first || far_from_lock)) {
            track.locked = false;
            track.lock_logged = false;
            track.x = mx;
            track.y = my;
            track.same_since = now;
        }
        const bool stale = (now - track.same_since) >= kPointStaleMs;
        const bool stale_too_long = (now - track.same_since) >= kPointStaleForceMs;

        const char* reason = "keep";
        bool accept = true;
        if (!in_bounds) {
            accept = false;
            reason = "out-of-bounds";
            s_filter_oob = s_filter_oob + 1;
        } else if (track.locked) {
            // Already force-released: keep LVGL released while the phantom stays.
            accept = false;
            reason = "ghost-lock";
            if (!track.lock_logged) {
                track.lock_logged = true;
                ESP_LOGW("CHD_TOUCH",
                         "ghost lock: raw(%u,%u) stays dropped until release", raw_x, raw_y);
            }
        } else if (stale && stale_too_long) {
            // D3: the point has not moved for 2.5 s. Treat it as a phantom,
            // release LVGL now, and suppress the CLICKED that the release would
            // otherwise fire at this position (PRESS_LOST instead).
            accept = false;
            reason = "ghost-release";
            track.locked = true;
            track.lock_x = mx;
            track.lock_y = my;
            track.lock_logged = true;
            track.stale_logged = true;
            s_filter_stale = s_filter_stale + 1;
            s_filter_ghost = s_filter_ghost + 1;
            s_forced_releases = s_forced_releases + 1;
            if (s_touch_indev != nullptr) {
                lv_indev_wait_release(s_touch_indev);
            }
            ESP_LOGW("CHD_TOUCH",
                     "point raw(%u,%u) -> mapped(%u,%u) reason=ghost-release (held=%u ms) -> "
                     "forced release + ghost lock",
                     raw_x, raw_y, mx, my,
                     static_cast<unsigned>(now - track.same_since));
        } else if (stale) {
            // D2: a resting finger is still a finger for the first 2.5 s. Keep
            // feeding the point so LVGL stays pressed; only a real release (raw
            // points == 0) or the D3 timeout may end the press.
            reason = "stale";
            if (!track.stale_logged) {
                track.stale_logged = true;
                s_filter_stale = s_filter_stale + 1;
                ESP_LOGW("CHD_TOUCH",
                         "point raw(%u,%u) -> mapped(%u,%u) reason=stale (held=%u ms) -> "
                         "HOLD (LVGL stays pressed, no release)",
                         raw_x, raw_y, mx, my,
                         static_cast<unsigned>(now - track.same_since));
            }
        }
        if (!stale) {
            track.stale_logged = false;
        }

        // Only log on transitions, so a stuck point does not flood the console.
        if (!accept && !was_dropped) {
            ESP_LOGW("CHD_TOUCH",
                     "point raw(%u,%u) -> mapped(%u,%u) swap=%d mirror(x=%d,y=%d) -> DROP(%s, "
                     "held=%u ms)",
                     raw_x, raw_y, mx, my, static_cast<int>(DISPLAY_SWAP_XY),
                     static_cast<int>(kTouchMirrorX), static_cast<int>(kTouchMirrorY), reason,
                     static_cast<unsigned>(now - track.same_since));
        } else if (accept && was_dropped) {
            ESP_LOGI("CHD_TOUCH", "point raw(%u,%u) -> mapped(%u,%u) -> resumed", raw_x, raw_y, mx,
                     my);
        }
        track.dropped = !accept;

        if (accept) {
            tp->data.coords[kept].x = mx;
            tp->data.coords[kept].y = my;
            tp->data.coords[kept].strength = tp->data.coords[i].strength;
            tp->data.coords[kept].track_id = tp->data.coords[i].track_id;
            kept++;
        }
    }

    // D3: `kept == 0` now also covers a force-released phantom (see the lock
    // above). Falling back to the panel's own `raw points == 0` remains the
    // normal release path.

    s_filter_last_kept = kept;
    tp->data.points = kept;  // 0 = panel reported no usable point (real release)
}
#endif  // CHD_TOUCH_POINT_FILTER

// Runs as an LVGL timer, i.e. after the indev has processed the sample, so
// lv_indev_get_active_obj() is meaningful here (it is always NULL inside
// read_data). Logs only when the answer changes, plus a 10 s heartbeat.
static void IndevActTimerCb(lv_timer_t* timer) {
    (void)timer;
    static lv_obj_t* last_act = nullptr;
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    static uint32_t last_beat_ms = 0;
    static bool announced = false;
    if (!announced) {
        announced = true;
        ESP_LOGI("CHD_TOUCH", "post-frame act timer is running (first call)");
    }

    lv_obj_t* act = lv_indev_get_active_obj();
    const lv_indev_state_t state =
        (s_touch_indev != nullptr) ? lv_indev_get_state(s_touch_indev) : LV_INDEV_STATE_RELEASED;
    const uint32_t now = lv_tick_get();

    if (act != last_act || state != last_state) {
        const char* where = "none";
        if (act != nullptr) {
            if (act == lv_screen_active()) {
                where = "screen";
            } else if (lv_obj_get_parent(act) == lv_screen_active()) {
                where = "child-of-screen";
            } else {
                where = "nested";
            }
        }
        ESP_LOGI("CHD_TOUCH", "post-frame act=%p(%s) state=%s", static_cast<void*>(act), where,
                 (state == LV_INDEV_STATE_PRESSED) ? "PRESSED" : "RELEASED");
        last_act = act;
        last_state = state;
        last_beat_ms = now;
    }
    // Unconditional 5 s heartbeat: tells "timer alive but nothing changes" apart
    // from "timer never ran", and dumps the filter counters either way.
    if (lv_tick_elaps(last_beat_ms) >= 5000) {
        last_beat_ms = now;
        ESP_LOGI("CHD_TOUCH",
                 "post-frame heartbeat: act=%p(%s) state=%s | filter: frames=%u raw_points=%u "
                 "kept_last=%u stale=%u oob=%u ghost=%u forced=%u",
                 static_cast<void*>(act),
                 (act == nullptr) ? "none" : ((act == lv_screen_active()) ? "screen" : "child"),
                 (state == LV_INDEV_STATE_PRESSED) ? "PRESSED" : "RELEASED",
                 static_cast<unsigned>(s_filter_frames), static_cast<unsigned>(s_filter_last_raw),
                 static_cast<unsigned>(s_filter_last_kept), static_cast<unsigned>(s_filter_stale),
                 static_cast<unsigned>(s_filter_oob), static_cast<unsigned>(s_filter_ghost),
                 static_cast<unsigned>(s_forced_releases));
    }
}

// Installed over the driver callback: only LVGL's indev path goes through this,
// so counting here measures exactly what LVGL polls, and the phantom filter is
// applied once per poll inside the LVGL task (same lock).
static esp_err_t TouchCountedRead(esp_lcd_touch_handle_t tp) {
    s_touch_indev_reads = s_touch_indev_reads + 1;  // volatile: C++20 forbids ++
    const esp_err_t err =
        (s_touch_read_orig != nullptr) ? s_touch_read_orig(tp) : ESP_ERR_INVALID_STATE;
#if CHD_TOUCH_PHANTOM_FILTER
    if (err == ESP_OK) {
        TouchFilterApply(tp);
    }
#endif
#if CHD_TOUCH_POINT_FILTER
    if (err == ESP_OK) {
        TouchPointFilter(tp);
    }
#endif
#if CHD_TOUCH_LOG_INDEV
    if (err == ESP_OK) {
        TouchLogIndevState(tp);
    }
#endif
    return err;
}

// Raw register reads straight off the bus: proves what the chip really is and
// whether it sees a finger, independently of our driver and of LVGL.
// FT5x06 uses 8-bit register addresses, GT911 uses 16-bit ones, so the register
// numbers differ even though the transport is the same.
static void TouchReadRegs() {
    if (s_touch == nullptr || s_touch->io == nullptr) {
        ESP_LOGE("CHD_TOUCH", "no touch io handle - cannot read registers");
        return;
    }
    if (s_touch_kind == CHD_TOUCH_GT911) {
        ESP_LOGI("CHD_TOUCH", "register view: GT911 (16-bit regs)");
        uint8_t id[4] = {0};
        esp_err_t err = esp_lcd_panel_io_rx_param(s_touch->io, 0x8140, id, sizeof(id));
        if (err == ESP_OK) {
            ESP_LOGI("CHD_TOUCH", "GT911 0x8140 product-id = '%c%c%c%c' (0x%02x 0x%02x 0x%02x 0x%02x)",
                     id[0], id[1], id[2], id[3], id[0], id[1], id[2], id[3]);
        } else {
            ESP_LOGW("CHD_TOUCH", "GT911 0x8140 read failed: %s (0x%x)", esp_err_to_name(err),
                     static_cast<unsigned>(err));
        }
        uint8_t status = 0;
        err = esp_lcd_panel_io_rx_param(s_touch->io, 0x814E, &status, 1);
        if (err == ESP_OK) {
            ESP_LOGI("CHD_TOUCH", "GT911 0x814E status=0x%02x -> ready=%u points=%u", status,
                     static_cast<unsigned>((status >> 7) & 1), static_cast<unsigned>(status & 0x0F));
        } else {
            ESP_LOGW("CHD_TOUCH", "GT911 0x814E read failed: %s (0x%x)", esp_err_to_name(err),
                     static_cast<unsigned>(err));
        }
        return;
    }

    ESP_LOGI("CHD_TOUCH", "register view: FT5x06 (8-bit regs)");
    static const struct {
        const char* name;
        uint8_t reg;
    } kRegs[] = {
        {"0xA3 chip-id", 0xA3}, {"0xA8 cipher", 0xA8}, {"0x80 pd", 0x80}, {"0x00 mode", 0x00},
    };
    for (const auto& r : kRegs) {
        uint8_t v = 0;
        esp_err_t err = esp_lcd_panel_io_rx_param(s_touch->io, r.reg, &v, 1);
        if (err == ESP_OK) {
            ESP_LOGI("CHD_TOUCH", "FT5x06 %s = 0x%02x", r.name, v);
        } else {
            ESP_LOGW("CHD_TOUCH", "FT5x06 %s read failed: %s (0x%x)", r.name, esp_err_to_name(err),
                     static_cast<unsigned>(err));
        }
    }
    uint8_t td[6] = {0};
    esp_err_t err = esp_lcd_panel_io_rx_param(s_touch->io, 0x02, td, sizeof(td));
    if (err == ESP_OK) {
        ESP_LOGI("CHD_TOUCH", "FT5x06 TD_STATUS(0x02)=0x%02x -> points=%u", td[0],
                 static_cast<unsigned>(td[0] & 0x0F));
    } else {
        ESP_LOGW("CHD_TOUCH", "FT5x06 TD_STATUS(0x02) read failed: %s (0x%x)", esp_err_to_name(err),
                 static_cast<unsigned>(err));
    }
}

// 10 Hz raw TD_STATUS(0x02) + CTP_INT level poll. This bypasses the driver
// completely, so "does the chip itself see a finger" is answered without any
// interpretation from the driver or LVGL.
#if CHD_TOUCH_DIAG
static void TdStatusPollTask(void* arg) {
    (void)arg;
    ESP_LOGI("CHD_TOUCH", "TD_STATUS poll started (10 Hz, silent unless points>0)");
    int ticks = 0;
    int last_points = 0;
    while (true) {
        if (s_ft_io != nullptr) {
            uint8_t td[6] = {0};
            uint8_t mode = 0xFF;
            (void)lvgl_port_lock(0);
            const esp_err_t err = esp_lcd_panel_io_rx_param(s_ft_io, 0x02, td, sizeof(td));
            const esp_err_t merr = esp_lcd_panel_io_rx_param(s_ft_io, 0x00, &mode, 1);
            const uint8_t td_status = td[0];
            lvgl_port_unlock();
            if (err == ESP_OK) {
                const int points = td_status & 0x0F;
                if (points > 0) {
                    ESP_LOGI("CHD_TOUCH", "TD_STATUS points=%d (raw 0x%02x %02x %02x %02x %02x)",
                             points, td[0], td[1], td[2], td[3], td[4]);
                }
                last_points = points;
            } else {
                ESP_LOGW("CHD_TOUCH", "TD_STATUS read failed: %s (0x%x)", esp_err_to_name(err),
                         static_cast<unsigned>(err));
            }
            if (++ticks >= 50) {  // every 5 s
                ticks = 0;
                ESP_LOGI("CHD_TOUCH",
                         "10 Hz probe: mode=%s(0x%02x) GPIO%d(INT)=%d last_points=%d indev reads=%u",
                         (merr == ESP_OK && (mode & 0x07) == 0) ? "WORKING" : "NON-working", mode,
                         CHD_TOUCH_INT_GPIO, gpio_get_level(CHD_TOUCH_INT_GPIO), last_points,
                         s_touch_indev_reads);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif  // CHD_TOUCH_DIAG

// 200 ms raw poll, silent unless the panel reports points. The read is taken
// under the LVGL lock because the touch panel shares I2C port 1 with the audio
// codec and the LVGL indev reads it from the LVGL task.
#if CHD_TOUCH_DIAG
static void TouchDiagTask(void* arg) {
    (void)arg;
    ESP_LOGI("CHD_TOUCH", "diag poll started (200 ms, silent unless a point appears)");
    uint32_t last_reads = 0;
    int ticks = 0;
    while (true) {
        if (s_touch != nullptr && s_touch_read_orig != nullptr) {
            esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {};
            uint8_t n = 0;
            (void)lvgl_port_lock(0);
            esp_err_t err = s_touch_read_orig(s_touch);
            const esp_err_t gerr =
                (err == ESP_OK)
                    ? esp_lcd_touch_get_data(s_touch, points, &n, CONFIG_ESP_LCD_TOUCH_MAX_POINTS)
                    : err;
            lvgl_port_unlock();
            if (gerr == ESP_OK) {
                // Only changes are interesting: a steady ghost would otherwise
                // print dozens of identical lines per second.
                static uint8_t last_n = 0xFF;
                static uint16_t last_x = 0xFFFF, last_y = 0xFFFF;
                const uint8_t cur_n = n;
                const uint16_t cur_x = (n > 0) ? points[0].x : 0;
                const uint16_t cur_y = (n > 0) ? points[0].y : 0;
                if (cur_n != last_n || cur_x != last_x || cur_y != last_y) {
                    if (cur_n == 0) {
                        ESP_LOGI("CHD_TOUCH", "raw: no points (released)");
                    } else {
                        ESP_LOGI("CHD_TOUCH", "raw: n=%u x=%u y=%u strength=%u", cur_n, cur_x,
                                 cur_y, points[0].strength);
                    }
                    last_n = cur_n;
                    last_x = cur_x;
                    last_y = cur_y;
                }
            } else {
                s_touch_poll_fails = s_touch_poll_fails + 1;
            }
            if (err != ESP_OK) {
                ESP_LOGW("CHD_TOUCH", "poll read_data failed: %s (0x%x), fails=%u",
                         esp_err_to_name(err), static_cast<unsigned>(err), s_touch_poll_fails);
            }
        }
        if (++ticks >= 25) {
            ticks = 0;
            ESP_LOGI("CHD_TOUCH", "indev reads=%u (delta=%u over 5 s) poll fails=%u",
                     s_touch_indev_reads, s_touch_indev_reads - last_reads, s_touch_poll_fails);
            last_reads = s_touch_indev_reads;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
#endif  // CHD_TOUCH_DIAG

class ChdEsp32s3BoxBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    // No boot_button_ on purpose (task-29): BOOT (IO0) is also SD_CMD, and the
    // iot_button GPIO interrupt registered on IO0 fired while the SD bus was
    // transferring - which toggled the dialogue at random. The physical BOOT pin
    // is still used by the ROM for download mode (nothing in software touches
    // it), and the Wi-Fi config-mode fallback does not need a button either
    // (wifi_board.cc:112 no SSID, :164 60 s timeout).
    Display* display_;
    CustomLcdDisplay* touch_display_;
    Esp32Camera* camera_;
    PowerManager* power_manager_;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        // (-1 cpu freq, 180 s to dim, no shutdown). cpu_max_freq_ == -1 means
        // entering sleep mode here only dims the panel/backlight - it does NOT
        // stop wake-word detection, disable codec input or enable light sleep.
        power_save_timer_ = new PowerSaveTimer(-1, 180, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void ResetNvsFlash() {
        ESP_LOGI(TAG, "Resetting NVS flash");
        esp_err_t ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash");
        }
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize NVS flash");
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    // Touch tap = the only "talk" gesture now that the BOOT button is gone.
    // Also serves as the escape hatch the BOOT button used to provide: while the
    // device is still starting with Wi-Fi, a tap jumps into config mode.
    void HandleTap() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateStarting && GetNetworkType() == NetworkType::WIFI &&
            SsidManager::GetInstance().GetSsidList().empty()) {
            ESP_LOGI("CHD_UI", "tap -> EnterWifiConfigMode (no saved SSID)");
            static_cast<WifiBoard&>(GetCurrentBoard()).EnterWifiConfigMode();
            return;
        }
        ESP_LOGI("CHD_UI", "tap -> ToggleChatState (state=%d)", static_cast<int>(state));
        app.ToggleChatState();
        if (power_save_timer_ != nullptr) {
            power_save_timer_->WakeUp();
        }
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t   panel_io = nullptr;
        esp_lcd_panel_handle_t      panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode    = 0;
        io_config.pclk_hz     = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits      = 8;
        io_config.lcd_param_bits    = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        Settings settings("chd_esp_box3", false);   // 横屏模式0 竖屏1
        bool display_mode = static_cast<bool>(settings.GetInt("display_mode", 0));
        int width       = DISPLAY_WIDTH;
        int height      = DISPLAY_HEIGHT;
        bool mirror_x   = DISPLAY_MIRROR_X;
        bool mirror_y   = DISPLAY_MIRROR_Y;
        bool swap_xy    = DISPLAY_SWAP_XY;
        if (display_mode) {         // 竖屏模式
            width = DISPLAY_HEIGHT;
            height = DISPLAY_WIDTH;
            mirror_x = mirror_x ? false : true;
            // mirror_y = DISPLAY_MIRROR_Y;
            swap_xy = swap_xy ? false : true;
        }

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, swap_xy);
        esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        // The official emote engine takes over the panel and its flush callback:
        // it draws its own status bar / message area and there is no LVGL widget
        // tree at all, so no CustomLcdDisplay and no click handler.
        display_ = new ChdEmoteDisplay(panel, panel_io, width, height);
        touch_display_ = nullptr;
#else
        touch_display_ = new CustomLcdDisplay(panel_io, panel,
            width, height, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, mirror_x, mirror_y, swap_xy);
        display_ = touch_display_;
        touch_display_->SetTapHandler([this]() { HandleTap(); });
#endif
    }

    // 用户触控坐标处理函数，针对GT911触控芯片的坐标偏移问题进行修正
    static void user_touch_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                    uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
        for (int i = 0; i < *point_num; i++) {
            x[i] += 80; // ESP_LOGI(TAG, "x: %d, y: %d", x[i], y[i]);
        }
    }

    // task-29 round 3: one build, three observations, no interaction changes.
    // (1) scan the whole 7-bit range: "everything ACKs" means the bus is lying
    //     (SDA stuck / no pull-up), and then 0x38 means nothing either.
    void TouchScanI2cBus() {
        char list[256] = {0};
        size_t used = 0;
        int count = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (i2c_master_probe(i2c_bus_, addr, 20) == ESP_OK) {
                count++;
                const int written = snprintf(list + used, sizeof(list) - used, " 0x%02X", addr);
                if (written > 0 && used + static_cast<size_t>(written) < sizeof(list)) {
                    used += static_cast<size_t>(written);
                }
            }
        }
        ESP_LOGI("CHD_TOUCH", "i2c scan 0x08..0x77: %d device(s) ACK ->%s", count,
                 count > 0 ? list : " (none)");
        if (count > 6) {
            ESP_LOGW("CHD_TOUCH", "too many ACKs (%d) - the bus itself may be faulty (false ACKs)",
                     count);
        }
    }

    // (2) who answered at 0x38? FT6336 chip-id (0xA3) should read 0x64.
    void TouchIdentifyAddr38() {
        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_panel_io_i2c_config_t cfg = {};
        cfg.dev_addr = 0x38;
        cfg.control_phase_bytes = 1;
        cfg.dc_bit_offset = 0;
        cfg.lcd_cmd_bits = 8;
        cfg.flags.disable_control_phase = 1;
        cfg.scl_speed_hz = 400000;
        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus_, &cfg, &io);
        if (err != ESP_OK) {
            ESP_LOGW("CHD_TOUCH", "addr 0x38: cannot create i2c io: %s (0x%x)",
                     esp_err_to_name(err), static_cast<unsigned>(err));
            return;
        }
        static const struct {
            const char* name;
            uint8_t reg;
        } kRegs[] = {
            {"0xA3 chip-id", 0xA3}, {"0xA8 cipher", 0xA8}, {"0x80 pd", 0x80},
        };
        for (const auto& r : kRegs) {
            uint8_t v = 0;
            const esp_err_t e = esp_lcd_panel_io_rx_param(io, r.reg, &v, 1);
            if (e == ESP_OK) {
                ESP_LOGI("CHD_TOUCH", "addr 0x38 %s = 0x%02x", r.name, v);
            } else {
                ESP_LOGW("CHD_TOUCH", "addr 0x38 %s read failed: %s (0x%x)", r.name,
                         esp_err_to_name(e), static_cast<unsigned>(e));
            }
        }
        esp_lcd_panel_io_del(io);
    }

    bool InitializeTouch_gt911() {
        esp_lcd_touch_handle_t tp;
        Settings settings("chd_esp_box3", false); // 横屏模式0 竖屏1
        bool display_mode = static_cast<bool>(settings.GetInt("display_mode", 0));
        uint16_t width  = DISPLAY_WIDTH;
        uint16_t height = DISPLAY_HEIGHT;
        bool mirror_x   = DISPLAY_MIRROR_X;
        bool mirror_y   = DISPLAY_MIRROR_Y;
        bool swap_xy    = DISPLAY_SWAP_XY;
        if (display_mode) {         // 竖屏模式
            width       = DISPLAY_HEIGHT;
            height      = DISPLAY_WIDTH;
            mirror_x    = mirror_x ? false : true;
            mirror_y    = mirror_y;
            swap_xy     = swap_xy ? false : true;
        }

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = width,
            .y_max = height,
            // IO47 belongs to the LCD, not to the touch panel: leave it alone.
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = CHD_TOUCH_INT_GPIO,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        };

        if (display_mode == 0) { // 根据自己屏幕和显示模式调用不同函数处理坐标，解决GT911触控芯片在某些屏幕上的坐标偏移问题
            tp_cfg.process_coordinates = user_touch_process_coordinates;    // 横屏模式
        }

        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .flags = {
                .disable_control_phase = 1,
            }
	    };

        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_LOGI(TAG, "Initialize touch controller");

        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        ESP_LOGI("CHD_TOUCH", "GT911 esp_lcd_new_panel_io_i2c -> %s (0x%x)", esp_err_to_name(ret),
                 static_cast<unsigned>(ret));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2C panel IO: %s", esp_err_to_name(ret));
            return false;
        }

        ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
        ESP_LOGI("CHD_TOUCH", "esp_lcd_touch_new_i2c_gt911 -> %s (0x%x)", esp_err_to_name(ret),
                 static_cast<unsigned>(ret));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create GT911 touch controller: %s", esp_err_to_name(ret));
            esp_lcd_panel_io_del(tp_io_handle);
            return false;
        }

        s_touch_kind = CHD_TOUCH_GT911;
        s_touch = tp;
        s_touch_read_orig = tp->read_data;
        tp->read_data = TouchCountedRead;
        TouchReadRegs();
#if CHD_TOUCH_DIAG
        if (xTaskCreate(TouchDiagTask, "touch_diag", 4096, nullptr, 3, nullptr) != pdPASS) {
            ESP_LOGE("CHD_TOUCH", "cannot start touch diag task");
        }
#endif

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        if (touch_cfg.disp) {
            s_touch_indev = lvgl_port_add_touch(&touch_cfg);
            if (s_touch_indev != nullptr) {
                // task-2/C2: s_touch_ready_ms is anchored in TouchPointFilter.
                ESP_LOGI("CHD_TOUCH",
                         "driver = GT911 (addr 0x%02X), indev registered x=0..%u y=0..%u",
                         ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, static_cast<unsigned>(width - 1),
                         static_cast<unsigned>(height - 1));
            } else {
                ESP_LOGE("CHD_TOUCH", "driver = GT911, lvgl_port_add_touch returned nullptr");
            }
        } else {
            ESP_LOGE("CHD_TOUCH", "driver = GT911, but there is no LVGL display");
        }
        ESP_LOGI(TAG, "Touch panel initialized successfully");
        return true;
    }

    bool InitializeTouch_ft6336(void) {
        esp_lcd_touch_handle_t tp;
        Settings settings("chd_esp_box3", false);   // 横屏模式0 竖屏1
        bool display_mode = static_cast<bool>(settings.GetInt("display_mode", 0));
        // task-29: the raw panel is portrait and TouchMapRawToDisplay() already
        // applies swap/mirror, so x_max/y_max must describe the DISPLAY (480x320
        // landscape), not the panel. Keeping them portrait made the indev range
        // (320x480) disagree with the coordinates we hand over.
        uint16_t width  = DISPLAY_WIDTH;
        uint16_t height = DISPLAY_HEIGHT;
        // NOTE (task-1/C4): portrait mode stays a pre-existing limitation - the touch
        // mapping is a compile-time landscape transform (swap + Y mirror) and does
        // not follow display_mode; only x_max/y_max are swapped here. Not fixed.
        if (display_mode) {  // 竖屏模式
            width = DISPLAY_HEIGHT;
            height = DISPLAY_WIDTH;
        }

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = width,
            .y_max = height,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                // task-1/A1: FT5x06 does not implement set_swap_xy/set_mirror_*,
                // so the generic layer would transform the raw registers a second
                // time on top of TouchMapRawToDisplay(). Keep all three off: that
                // function is the single owner of the raw -> display mapping.
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags = {.disable_control_phase = 1,}
        };
        tp_io_config.scl_speed_hz = 400000;

        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_err_t tp_err = esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        ESP_LOGI("CHD_TOUCH", "esp_lcd_touch_new_i2c_ft5x06 -> %s (0x%x)", esp_err_to_name(tp_err),
                 static_cast<unsigned>(tp_err));
        assert(tp);

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };

        s_touch_kind = CHD_TOUCH_FT5X06;
        s_touch = tp;
        s_touch_read_orig = tp->read_data;
        tp->read_data = TouchCountedRead;
        s_ft_io = tp_io_handle;  // raw register access for the mode check below
        TouchReadRegs();

        // FT6336 only scans for touches in WORKING mode; in MONITOR mode it waits
        // for an INT edge. Read DEVICE_MODE and, if it is not working, force it -
        // this is the difference between "chip online" and "chip reports points".
        uint8_t mode = 0xFF;
        esp_err_t mode_err = esp_lcd_panel_io_rx_param(s_ft_io, 0x00, &mode, 1);
        ESP_LOGI("CHD_TOUCH", "DEVICE_MODE(0x00) before: %s (0x%02x) -> %s", esp_err_to_name(mode_err),
                 mode, (mode_err == ESP_OK && (mode & 0x07) == 0) ? "WORKING" : "NOT working");
        if (mode_err == ESP_OK && (mode & 0x07) != 0) {
            uint8_t working = 0x00;
            const esp_err_t werr = esp_lcd_panel_io_tx_param(s_ft_io, 0x00, &working, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            uint8_t after = 0xFF;
            const esp_err_t rerr = esp_lcd_panel_io_rx_param(s_ft_io, 0x00, &after, 1);
            ESP_LOGI("CHD_TOUCH", "force WORKING: write -> %s (0x%x), re-read -> %s (0x%x), now=0x%02x",
                     esp_err_to_name(werr), static_cast<unsigned>(werr), esp_err_to_name(rerr),
                     static_cast<unsigned>(rerr), after);
        }
        // Observe the INT line without letting the driver take it over (the
        // FT5x06 config keeps int_gpio_num = NC): input + pull-up, no interrupt.
        gpio_config_t int_observe = {};
        int_observe.pin_bit_mask = 1ULL << CHD_TOUCH_INT_GPIO;
        int_observe.mode = GPIO_MODE_INPUT;
        int_observe.pull_up_en = GPIO_PULLUP_ENABLE;
        int_observe.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_observe.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&int_observe);
        ESP_LOGI("CHD_TOUCH", "GPIO%d (CTP_INT) level at init = %d (input+pull-up, no ISR)",
                 CHD_TOUCH_INT_GPIO, gpio_get_level(CHD_TOUCH_INT_GPIO));
#if CHD_TOUCH_DIAG
        if (xTaskCreate(TdStatusPollTask, "td_status", 4096, nullptr, 3, nullptr) != pdPASS) {
            ESP_LOGE("CHD_TOUCH", "cannot start TD_STATUS poll task");
        }
#endif
        lv_display_t* disp = lv_display_get_default();
        const int32_t disp_h = (disp != nullptr) ? lv_display_get_horizontal_resolution(disp) : -1;
        const int32_t disp_v = (disp != nullptr) ? lv_display_get_vertical_resolution(disp) : -1;
        ESP_LOGI("CHD_TOUCH",
                 "touch mapping: driver flags swap_xy=0 mirror_x=0 mirror_y=0; "
                 "board TouchMapRawToDisplay swap_xy=%d mirror_x=%d mirror_y=%d; "
                 "x_max=%u y_max=%u int=%d rst=%d",
                 static_cast<int>(DISPLAY_SWAP_XY), static_cast<int>(kTouchMirrorX),
                 static_cast<int>(kTouchMirrorY), static_cast<unsigned>(tp_cfg.x_max),
                 static_cast<unsigned>(tp_cfg.y_max), static_cast<int>(tp_cfg.int_gpio_num),
                 static_cast<int>(tp_cfg.rst_gpio_num));
        ESP_LOGI("CHD_TOUCH", "ranges: display=%ldx%ld touch=%ux%u %s", static_cast<long>(disp_h),
                 static_cast<long>(disp_v), static_cast<unsigned>(tp_cfg.x_max),
                 static_cast<unsigned>(tp_cfg.y_max),
                 (disp_h == static_cast<int32_t>(tp_cfg.x_max) &&
                  disp_v == static_cast<int32_t>(tp_cfg.y_max))
                     ? "MATCH"
                     : "MISMATCH");
#if CHD_TOUCH_DIAG
        if (xTaskCreate(TouchDiagTask, "touch_diag", 4096, nullptr, 3, nullptr) != pdPASS) {
            ESP_LOGE("CHD_TOUCH", "cannot start touch diag task");
        }
#endif

        if (touch_cfg.disp) {
            s_touch_indev = lvgl_port_add_touch(&touch_cfg);
            if (s_touch_indev != nullptr) {
                // task-2/C2: s_touch_ready_ms is anchored in TouchPointFilter.
                ESP_LOGI("CHD_TOUCH",
                         "driver = FT5x06 (addr 0x%02X), indev registered x=0..%ld y=0..%ld",
                         ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, static_cast<long>(disp_h - 1),
                         static_cast<long>(disp_v - 1));
#if CHD_TOUCH_LOG_INDEV
                // Sample the active object after each frame (see IndevActTimerCb).
                (void)lvgl_port_lock(0);
                lv_timer_t* act_timer = lv_timer_create(IndevActTimerCb, 300, nullptr);
                lvgl_port_unlock();
                ESP_LOGI("CHD_TOUCH", "post-frame act timer created=%p (period 300 ms)",
                         static_cast<void*>(act_timer));
#endif
            } else {
                ESP_LOGE("CHD_TOUCH", "driver = FT5x06, lvgl_port_add_touch returned nullptr");
            }
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
            ESP_LOGE("CHD_TOUCH", "driver = FT5x06, but there is no LVGL display");
        }
        return true;
    }

    void InitializeTouch() {
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        // No LVGL display exists in the emote variant (emote owns the panel), so
        // lvgl_port_add_touch() has nothing to attach to. The emote UI has no
        // touch targets either - skip the whole probe.
        ESP_LOGI(TAG, "emote style: touch not registered with LVGL");
        return;
#endif
        // task-29 follow-up: probe both addresses before choosing a driver.
        // A successful probe of 0x38 used to select FT5x06 all by itself, but the
        // vendor factory image only ever mentions GT911 - so 0x38 was almost
        // certainly some other device sharing I2C port 1 (ES7210 is 0x40,
        // ES8311 0x18, so something else answered there).
        TouchScanI2cBus();
        TouchIdentifyAddr38();
        const esp_err_t probe_38 = i2c_master_probe(i2c_bus_, ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, 50);
        const esp_err_t probe_5d = i2c_master_probe(i2c_bus_, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 50);
        const esp_err_t probe_14 = i2c_master_probe(i2c_bus_, 0x14, 50);
        ESP_LOGI("CHD_TOUCH", "i2c probe: 0x38(FT5x06)=%s 0x5D(GT911)=%s 0x14(GT911 alt)=%s",
                 esp_err_to_name(probe_38), esp_err_to_name(probe_5d), esp_err_to_name(probe_14));

#if CHD_TOUCH_FORCE_GT911
        ESP_LOGI("CHD_TOUCH", "driver = GT911 (forced by CHD_TOUCH_FORCE_GT911=1)");
        if (InitializeTouch_gt911()) {
            return;
        }
        ESP_LOGE("CHD_TOUCH", "forced GT911 init FAILED - no touch events will arrive");
#else
        // 0xA3 = 0x64 at 0x38 proved the panel is an FT6336, so FT5x06 goes first.
        if (InitializeTouch_ft6336()) {
            return;
        }
        ESP_LOGW("CHD_TOUCH", "FT5x06 init failed, falling back to GT911");
        if (InitializeTouch_gt911()) {
            return;
        }
        ESP_LOGE("CHD_TOUCH", "neither FT5x06 nor GT911 initialised");
#endif
    }

    void InitializeCamera() {
        camera_config_t camera_config = {
            .pin_pwdn       = CAMERA_PIN_PWDN,
            .pin_reset      = CAMERA_PIN_RESET,
            .pin_xclk       = CAMERA_PIN_XCLK,
            .pin_sscb_sda   = BSP_I2C_SDA,
            .pin_sscb_scl   = BSP_I2C_SCL,

            .pin_d7         = CAMERA_PIN_D7,
            .pin_d6         = CAMERA_PIN_D6,
            .pin_d5         = CAMERA_PIN_D5,
            .pin_d4         = CAMERA_PIN_D4,
            .pin_d3         = CAMERA_PIN_D3,
            .pin_d2         = CAMERA_PIN_D2,
            .pin_d1         = CAMERA_PIN_D1,
            .pin_d0         = CAMERA_PIN_D0,
            .pin_vsync      = CAMERA_PIN_VSYNC,
            .pin_href       = CAMERA_PIN_HREF,
            .pin_pclk       = CAMERA_PIN_PCLK,

            .xclk_freq_hz   = CAMERA_XCLK_FREQ,
            .ledc_timer     = LEDC_TIMER_2,
            .ledc_channel   = LEDC_CHANNEL_0,
            .pixel_format   = PIXFORMAT_RGB565,
            .frame_size     = FRAMESIZE_VGA,         // FRAMESIZE_240X240,

            // .pixel_format = PIXFORMAT_JPEG,     // ov3660摄像头
            // .frame_size   = FRAMESIZE_QXGA,     // 2048x1536 FRAMESIZE_UXGA,     // 1600x1200  FRAMESIZE_UXGA,     // 1600x1200        FRAMESIZE_QVGA,     // 320x240
            // .jpeg_quality = 8,
            .fb_count     = 1,
            .fb_location  = CAMERA_FB_IN_PSRAM,
            .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
            .sccb_i2c_port = BSP_I2C_PORT,
        };

        camera_ = new Esp32Camera(camera_config);
        sensor_t* s = esp_camera_sensor_get();
        if (s == NULL) {
            ESP_LOGE(TAG, "chd camera esp_camera_sensor_get ERROR");
            return;
        }

        if (s->id.PID == OV3660_PID) {
            s->set_brightness(s, 1);   // up the blightness just a bit
            s->set_saturation(s, -2);  // lower the saturation
        }

        Settings settings("chd_esp_box3", false); // 考虑有的批次摄像头需要翻转
        bool camera_flipped = static_cast<bool>(settings.GetInt("camera-flipped", 0));
        camera_->SetHMirror(camera_flipped);
        camera_->SetVFlip(camera_flipped);

        camera_fb_t *fb = esp_camera_fb_get(); // 第一帧的数据不使用
        if (fb) {
            ESP_LOGI(TAG, "chd camera fb get OK");
            esp_camera_fb_return(fb);
        }
        else {
            ESP_LOGE(TAG, "chd camera fb get ERROR");
        }
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(BAT_CHARGING_PIN);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                // power_save_timer_->SetEnabled(false);
            } else {
                // power_save_timer_->SetEnabled(true);
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();    // 定义设备的属性
        mcp_server.AddTool("self.camera.set_camera_flipped", "翻转摄像头图像方向", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("chd_esp_box3", true);
            bool flipped = !static_cast<bool>(settings.GetInt("camera-flipped", 0));
            camera_->SetHMirror(flipped);
            camera_->SetVFlip(flipped);
            settings.SetInt("camera-flipped", flipped ? 1 : 0);
            return true;
        });

        mcp_server.AddTool(
            "self.AEC.set_mode",
            "设置AEC语音打断模式。当用户意图切换语音打断模式时或者用户觉得ai对话容易被打断时或者用户觉得无法实现对话打断时都使用此工具。\n"
            "参数：\n"
            "   `mode`: 语音打断模式，可选值只有`kAecOff`(关闭）和`kAecOnDeviceSide`（开启）\n"
            "返回值：\n"
            "   反馈状态信息，不需要确认，立即播报相关数据\n",
            PropertyList({
                Property("mode", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto mode = properties["mode"].value<std::string>();
                auto& app = Application::GetInstance();
                // vTaskDelay(pdMS_TO_TICKS(2000));
                if (mode == "kAecOff") {
                    app.SetAecMode(kAecOff);
                    return "{\"success\": true, \"message\": \"AEC对话打断模式已关闭\"}";
                }else {
                    auto& board = Board::GetInstance();
                    app.SetAecMode(kAecOnDeviceSide);

                    return "{\"success\": true, \"message\": \"AEC对话打断模式已开启\"}";
                }
            }
        );

        mcp_server.AddTool(
            "self.AEC.get_mode",
            "获取AEC语音打断模式状态。当用户意图获取语音打断模式状态时使用此工具。\n"
            "返回值：\n"
            "   反馈状态信息，不需要确认，立即播报相关数据\n",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                const bool is_currently_off = (app.GetAecMode() == kAecOff);
            if (is_currently_off) {
                    return "{\"success\": true, \"message\": \"AEC语音打断模式处于关闭状态\"}";
                }else {
                    return "{\"success\": true, \"message\": \"AEC语音打断模式处于开启状态\"}";
                }
            }
        );
        auto& app = Application::GetInstance();
        if (GetNetworkType() == NetworkType::WIFI) {
            mcp_server.AddTool("self.system.reconfigure_wifi",
                "End this conversation and enter WiFi configuration mode.\n"
                "**CAUTION** You must ask the user to confirm this action.",
                PropertyList(), [this](const PropertyList& properties) {
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                    return true;
                });
        }

        mcp_server.AddTool("self.screen.set_display_mode",
            "Set display mode to landscape or portrait: 1=portrait, 0=landscape\n"
            "Set display mode to landscape, display_mode = 0\n"
            "Set display mode to portrait, display_mode = 1\n"
            "最后提示：重新开机生效",
            PropertyList({
                Property("display_mode", kPropertyTypeInteger, 0, 1)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int is_portrait = static_cast<bool>(properties["display_mode"].value<int>());
                Settings settings("chd_esp_box3", true);
                settings.SetInt("display_mode", is_portrait ? 1 : 0);
                ESP_LOGE(TAG, "set display_mode %d changed, restart to apply", is_portrait ? 1 : 0);
                return true;
            }
        );
        mcp_server.AddTool("self.screen.get_display_mode",
            "Get display mode: 1=portrait, 0=landscape",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                Settings settings("chd_esp_box3", false);
                int display_mode = static_cast<int>(settings.GetInt("display_mode", 0));
                return display_mode;
            }
        );

        mcp_server.AddTool("self.screen.set_view",
            "Phase B view: 0=A (7 logs), 1=B (6 cards), 2=C (8 logs).",
            PropertyList({Property("view", kPropertyTypeInteger, 0, 3)}),
            [this](const PropertyList& properties) -> ReturnValue {
                auto* display = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
                if (display == nullptr) {
                    return false;
                }
                display->SetPhaseBView(properties["view"].value<int>());
                return true;
            }
        );

        // M7-S1 spike helper - DEBUG/SPIKE ONLY, not a product feature.
        // It exists so M7 acceptance can put the face into a known expression
        // on demand (4+ screenshots) instead of waiting for the LLM to happen
        // to emit that emotion. Step 2 decides whether to keep it; remove it
        // if state-driven expressions make it redundant.
#if CHD_SD_ENABLE
        // task-29: bench tool - keep the SD bus busy while watching the log for
        // spontaneous CHD_UI (dialogue) lines.
        mcp_server.AddTool(
            "self.debug.sd_storm",
            "Keep the TF card busy for N seconds (2-300) to check that SD traffic never "
            "triggers a dialogue toggle. Logs 'CHD_SD: storm' lines once per second.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 10, 2, 300)}),
            [](const PropertyList& properties) -> ReturnValue {
                const int seconds = properties["seconds"].value<int>();
                const BaseType_t ok = xTaskCreate(
                    SdStormTask, "sd_storm", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(seconds)), 2, nullptr);
                return ok == pdPASS;
            });
#endif
        mcp_server.AddTool("self.screen.set_emotion",
            "[debug/spike only - M7 face verification] Set the face expression directly.\n"
            "emotion: one of neutral/listening/happy/laughing/funny/sad/angry/crying/"
            "loving/embarrassed/surprised/shocked/thinking/speaking/winking/cool/"
            "relaxed/confident/sleepy/silly/confused. Unknown values fall back to neutral.",
            PropertyList({
                Property("emotion", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto emotion = properties["emotion"].value<std::string>();
                // Works for both styles: display_ is the kawaii CustomLcdDisplay
                // (default) or the emote ChdEmoteDisplay, which maps the name.
                if (display_ == nullptr) {
                    return false;
                }
                return display_->SetEmotion(emotion.c_str());
            }
        );
    }

public:
    ChdEsp32s3BoxBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 0),
        display_(nullptr), touch_display_(nullptr) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializePowerSaveTimer();
        InitializeTouch();
        InitializeCamera();
        InitializePowerManager();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
#if CHD_SD_ENABLE
        // Non-blocking: the probe runs in its own task so a slow/absent card
        // cannot delay startup, and a failure only logs a warning.
        xTaskCreate(SdSmokeTask, "chd_sd", 4096, nullptr, 2, nullptr);
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        DualNetworkBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(ChdEsp32s3BoxBoard);
