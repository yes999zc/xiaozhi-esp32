#pragma once

#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include <cstring>
#include <string>

// A single 240x240 companion screen. The standard status bar remains in use,
// while the centre face and most recent subtitle replace the scrolling chat UI.
class DeskMateCubeDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override {
        if (setup_ui_called_) return;
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        if (!lock) return;
        auto* screen = lv_screen_active();
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        if (content_) lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        auto* brand = lv_label_create(screen);
        lv_label_set_text(brand, "DESK MATE");
        lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(brand, lv_color_hex(0x90d9b5), 0);
        lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 38);

        face_ = lv_obj_create(screen);
        lv_obj_set_pos(face_, 67, 62);
        lv_obj_set_size(face_, 106, 90);
        lv_obj_set_style_radius(face_, 30, 0);
        lv_obj_set_style_bg_color(face_, lv_color_hex(0xdaf4e7), 0);
        lv_obj_set_style_bg_opa(face_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(face_, 0, 0);
        lv_obj_set_style_pad_all(face_, 0, 0);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);
        left_eye_ = MakeFeature(face_, 29, 27, 11, 17);
        right_eye_ = MakeFeature(face_, 66, 27, 11, 17);
        mouth_ = MakeFeature(face_, 42, 59, 22, 5);

        mute_state_ = lv_label_create(screen);
        lv_obj_set_width(mute_state_, 200);
        lv_obj_set_style_text_align(mute_state_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mute_state_, lv_color_hex(0xbbe7cf), 0);
        lv_label_set_text(mute_state_, "待同步");
        lv_obj_align(mute_state_, LV_ALIGN_TOP_MID, 0, 157);

        auto* subtitle = lv_obj_create(screen);
        subtitle_card_ = subtitle;
        lv_obj_set_pos(subtitle, 10, 185);
        lv_obj_set_size(subtitle, 220, 45);
        lv_obj_set_style_radius(subtitle, 12, 0);
        lv_obj_set_style_bg_color(subtitle, lv_color_hex(0xf4f7f2), 0);
        lv_obj_set_style_bg_opa(subtitle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(subtitle, 0, 0);
        lv_obj_set_style_pad_all(subtitle, 4, 0);
        lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_SCROLLABLE);
        subtitle_ = lv_label_create(subtitle);
        lv_obj_set_width(subtitle_, 208);
        lv_obj_set_height(subtitle_, 37);
        lv_label_set_long_mode(subtitle_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(subtitle_, lv_color_hex(0x173e35), 0);
        lv_label_set_text(subtitle_, "你好，我是桌面伙伴");
        lv_obj_center(subtitle_);
        ApplyFonts();
        ApplyPalette();
    }

    void SetChatMessage(const char* role, const char* content) override {
        if (!role || !content) return;
        DisplayLockGuard lock(this);
        if (!lock || !subtitle_) return;
        if (*content) {
            size_t length = strnlen(content, 513);
            if (length > 512) {
                length = 512;
                while (length > 0 && (static_cast<unsigned char>(content[length]) & 0xc0) == 0x80) --length;
            }
            std::string text(content, length);
            lv_label_set_text(subtitle_, text.c_str());
        }
    }

    void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        if (lock && subtitle_) lv_label_set_text(subtitle_, "");
    }

    void RemoveChatMessage(const char*, const char*) override {}

    bool SetEmotion(const char* emotion) override {
        if (!emotion) return false;
        DisplayLockGuard lock(this);
        if (!lock || !face_) return false;
        const bool speaking = std::strcmp(emotion, "speaking") == 0;
        const bool sleepy = std::strcmp(emotion, "sleepy") == 0;
        const bool happy = std::strcmp(emotion, "happy") == 0 || std::strcmp(emotion, "loving") == 0;
        lv_obj_set_height(left_eye_, sleepy ? 5 : 17);
        lv_obj_set_height(right_eye_, sleepy ? 5 : 17);
        lv_obj_set_size(mouth_, speaking ? 13 : 22, speaking ? 12 : 5);
        lv_obj_set_x(mouth_, speaking ? 47 : 42);
        lv_obj_set_style_bg_color(face_, lv_color_hex(happy ? 0xc3f1d7 : 0xdaf4e7), 0);
        return true;
    }

    void SetMuteUiState(MuteUiState state) override {
        DisplayLockGuard lock(this);
        if (!lock || !mute_state_) return;
        const char* text = "待同步";
        switch (state) {
            case MuteUiState::kMuted: text = "麦克风已静音"; break;
            case MuteUiState::kUnmuted: text = "麦克风已开启"; break;
            case MuteUiState::kSyncing: text = "静音同步中"; break;
            case MuteUiState::kToggling: text = "静音切换中"; break;
            case MuteUiState::kFailed: text = "静音同步失败"; break;
            case MuteUiState::kNoEndpoint: break;
        }
        lv_label_set_text(mute_state_, text);
        lv_obj_set_style_text_color(mute_state_, lv_color_hex(
            state == MuteUiState::kMuted ? 0xffc48f : 0xbbe7cf), 0);
    }

    bool SetTextFont(std::shared_ptr<LvglFont> font) override {
        DisplayLockGuard lock(this);
        if (!lock) return false;
        bool ok = SpiLcdDisplay::SetTextFont(font);
        ApplyFonts();
        ApplyPalette();
        return ok;
    }

    void SetTheme(Theme* theme) override {
        if (!theme || !setup_ui_called_) return;
        DisplayLockGuard lock(this);
        if (!lock) return;
        SpiLcdDisplay::SetTheme(theme);
        ApplyFonts();
        ApplyPalette();
    }

    void SetHideSubtitle(bool hide) override {
        DisplayLockGuard lock(this);
        if (!lock || !subtitle_card_) return;
        if (hide) lv_obj_add_flag(subtitle_card_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(subtitle_card_, LV_OBJ_FLAG_HIDDEN);
    }

private:
    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* mute_state_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_obj_t* subtitle_card_ = nullptr;

    static lv_obj_t* MakeFeature(lv_obj_t* parent, int x, int y, int w, int h) {
        auto* obj = lv_obj_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x173e35), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    }

    void ApplyFonts() {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (!theme || !theme->text_font()) return;
        const lv_font_t* font = theme->text_font()->font();
        if (mute_state_) lv_obj_set_style_text_font(mute_state_, font, 0);
        if (subtitle_) lv_obj_set_style_text_font(subtitle_, font, 0);
    }

    void ApplyPalette() {
        auto background = lv_color_hex(0x173e35);
        auto foreground = lv_color_hex(0xe5f5eb);
        lv_obj_set_style_bg_color(lv_screen_active(), background, 0);
        if (container_) lv_obj_set_style_bg_color(container_, background, 0);
        if (top_bar_) lv_obj_set_style_bg_color(top_bar_, background, 0);
        for (auto* label : {network_label_, battery_label_, mute_label_, status_label_, notification_label_}) {
            if (label) lv_obj_set_style_text_color(label, foreground, 0);
        }
    }
};
