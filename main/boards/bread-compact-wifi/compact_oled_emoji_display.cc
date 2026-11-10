#include "compact_oled_emoji_display.h"
#include "compact_caro_board.h"

#include "assets/lang_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <stdexcept>

#include <strings.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>

extern "C" {
typedef const uint8_t* qrcode_wrapper_handle_t;

typedef struct {
    void (*display_func)(qrcode_wrapper_handle_t qrcode, void* user_data);
    int max_qrcode_version;
    int qrcode_ecc_level;
    void* user_data;
} qrcode_wrapper_config_t;

esp_err_t qrcode_wrapper_generate(qrcode_wrapper_config_t* cfg, const char* text);
int qrcode_wrapper_get_size(qrcode_wrapper_handle_t qrcode);
bool qrcode_wrapper_get_module(qrcode_wrapper_handle_t qrcode, int x, int y);
}

LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace {

constexpr char kTag[] = "CompactOledEmojiDisplay";
constexpr int kQrCodeSize = 62;
constexpr int kQrQuietZoneModules = 2;
constexpr int kQrcodeWrapperEccLow = 0;
constexpr uint32_t kFramePeriodMs = 40;
constexpr uint32_t kExpressionTransitionMs = 450;
constexpr uint32_t kLookTransitionMs = 300;
constexpr uint32_t kBlinkDurationMs = 180;

struct EyeShape {
    float offset_x;
    float offset_y;
    float width;
    float height;
    float slope_top;
    float slope_bottom;
    float radius_top;
    float radius_bottom;
};

struct FacePreset {
    const char* name;
    EyeShape left;
    EyeShape right;
};

#define EYE(ox, oy, w, h, st, sb, rt, rb) {(ox), (oy), (w), (h), (st), (sb), (rt), (rb)}

constexpr FacePreset kFacePresets[] = {
    {"normal", EYE(0, 0, 40, 40, 0.00f, 0.00f, 8, 8), EYE(0, 0, 40, 40, 0.00f, 0.00f, 8, 8)},
    {"happy", EYE(0, 1, 40, 11, 0.00f, 0.00f, 10, 1), EYE(0, 1, 40, 11, 0.00f, 0.00f, 10, 1)},
    {"sad", EYE(0, 4, 40, 16, -0.36f, 0.16f, 2, 8), EYE(0, 4, 40, 16, 0.36f, -0.16f, 2, 8)},
    {"angry", EYE(0, 0, 42, 22, 0.42f, 0.00f, 2, 8), EYE(0, 0, 42, 22, -0.42f, 0.00f, 2, 8)},
    {"surprised", EYE(0, 0, 32, 46, 0.00f, 0.00f, 16, 16), EYE(0, 0, 32, 46, 0.00f, 0.00f, 16, 16)},
    {"sleepy", EYE(0, 4, 42, 8, -0.18f, -0.18f, 4, 2), EYE(0, 4, 42, 8, 0.18f, 0.18f, 4, 2)},
    {"skeptic", EYE(0, 2, 42, 18, -0.22f, 0.00f, 4, 6), EYE(0, -2, 42, 30, 0.16f, 0.00f, 8, 8)},
    {"glee", EYE(0, 2, 38, 14, 0.00f, -0.28f, 12, 2), EYE(0, 2, 38, 14, 0.00f, 0.28f, 12, 2)},
    {"wink", EYE(0, 4, 42, 5, 0.00f, 0.00f, 2, 2), EYE(0, 0, 40, 40, 0.00f, 0.00f, 8, 8)},
    {"confused", EYE(0, -2, 40, 24, 0.32f, -0.12f, 6, 8), EYE(0, 3, 40, 18, -0.32f, 0.12f, 6, 8)},
    {"excited", EYE(0, 0, 46, 46, 0.00f, 0.00f, 14, 14), EYE(0, 0, 46, 46, 0.00f, 0.00f, 14, 14)},
    {"bored", EYE(0, 5, 44, 9, 0.00f, 0.00f, 1, 1), EYE(0, 5, 44, 9, 0.00f, 0.00f, 1, 1)},
    {"focused", EYE(1, 0, 36, 34, 0.10f, -0.10f, 4, 4), EYE(-1, 0, 36, 34, -0.10f, 0.10f, 4, 4)},
    {"nervous", EYE(0, 0, 36, 26, -0.18f, 0.18f, 5, 8), EYE(0, 0, 36, 26, 0.18f, -0.18f, 5, 8)},
    {"mischievous", EYE(0, 0, 42, 20, -0.35f, 0.10f, 4, 9), EYE(0, 0, 42, 20, 0.35f, -0.10f, 4, 9)},
    {"relaxed", EYE(0, 3, 40, 13, 0.12f, -0.12f, 10, 2), EYE(0, 3, 40, 13, -0.12f, 0.12f, 10, 2)},
    {"attentive", EYE(0, 0, 34, 40, 0.00f, 0.00f, 17, 17), EYE(0, 0, 34, 40, 0.00f, 0.00f, 17, 17)},
    {"chatty", EYE(0, 2, 38, 30, 0.00f, 0.00f, 18, 12), EYE(0, 2, 38, 30, 0.00f, 0.00f, 18, 12)},
};

#undef EYE

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(value, maximum));
}

float SmoothStep(float progress) {
    progress = Clamp(progress, 0.0f, 1.0f);
    return progress * progress * (3.0f - 2.0f * progress);
}

float Lerp(float from, float to, float progress) { return from + (to - from) * progress; }

float RandomRange(float minimum, float maximum) {
    const float normalized = static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX);
    return minimum + normalized * (maximum - minimum);
}

EyeShape InterpolateEye(const EyeShape& from, const EyeShape& to, float progress) {
    const float t = SmoothStep(progress);
    return {
        Lerp(from.offset_x, to.offset_x, t),     Lerp(from.offset_y, to.offset_y, t),
        Lerp(from.width, to.width, t),           Lerp(from.height, to.height, t),
        Lerp(from.slope_top, to.slope_top, t),   Lerp(from.slope_bottom, to.slope_bottom, t),
        Lerp(from.radius_top, to.radius_top, t), Lerp(from.radius_bottom, to.radius_bottom, t),
    };
}

int FindPreset(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        return 0;
    }

    struct EmotionMap {
        const char* emotion;
        const char* preset;
    };
    static constexpr EmotionMap kEmotionMap[] = {
        {"neutral", "normal"},      {"idle", "normal"},
        {"happy", "chatty"},        {"laughing", "chatty"},
        {"funny", "chatty"},        {"loving", "chatty"},
        {"embarrassed", "skeptic"}, {"confident", "focused"},
        {"delicious", "glee"},      {"sad", "sad"},
        {"crying", "sad"},          {"sleepy", "sleepy"},
        {"silly", "mischievous"},   {"angry", "angry"},
        {"surprised", "surprised"}, {"shocked", "nervous"},
        {"thinking", "focused"},    {"winking", "wink"},
        {"relaxed", "relaxed"},     {"confused", "confused"},
    };

    const char* requested = emotion;
    for (const auto& mapping : kEmotionMap) {
        if (strcasecmp(emotion, mapping.emotion) == 0) {
            requested = mapping.preset;
            break;
        }
    }
    for (size_t i = 0; i < std::size(kFacePresets); ++i) {
        if (strcasecmp(requested, kFacePresets[i].name) == 0) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

}  // namespace

CompactOledEmojiDisplay::CompactOledEmojiDisplay(esp_lcd_panel_io_handle_t panel_io,
                                                 esp_lcd_panel_handle_t panel, int width,
                                                 int height, bool mirror_x, bool mirror_y,
                                                 bool emoji_only)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y), emoji_only_(emoji_only) {}

CompactOledEmojiDisplay::~CompactOledEmojiDisplay() {
    DisplayLockGuard lock(this);
    if (animation_timer_ != nullptr) {
        lv_timer_delete(animation_timer_);
        animation_timer_ = nullptr;
    }
    if (clock_timer_ != nullptr) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }
    if (qr_timer_ != nullptr) {
        lv_timer_delete(qr_timer_);
        qr_timer_ = nullptr;
    }
    if (canvas_ != nullptr) {
        lv_obj_delete(canvas_);
        canvas_ = nullptr;
    }
    if (wifi_panel_ != nullptr) {
        lv_obj_delete(wifi_panel_);
        wifi_panel_ = nullptr;
    }
    if (clock_panel_ != nullptr) {
        lv_obj_delete(clock_panel_);
        clock_panel_ = nullptr;
    }
    if (game_panel_ != nullptr) {
        lv_obj_delete(game_panel_);
        game_panel_ = nullptr;
    }
    if (caro_panel_ != nullptr) {
        lv_obj_delete(caro_panel_);
        caro_panel_ = nullptr;
    }
    if (qr_panel_ != nullptr) {
        lv_obj_delete(qr_panel_);
        qr_panel_ = nullptr;
    }
    if (qr_draw_buf_ != nullptr) {
        lv_draw_buf_destroy(qr_draw_buf_);
        qr_draw_buf_ = nullptr;
    }
    if (canvas_draw_buf_ != nullptr) {
        lv_draw_buf_destroy(canvas_draw_buf_);
        canvas_draw_buf_ = nullptr;
    }
}

void CompactOledEmojiDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }
    if (!emoji_only_) {
        OledDisplay::SetupUI();
        DisplayLockGuard lock(this);
        SetupQrUi(lv_screen_active());
        SetupCaroUi(lv_screen_active());
        return;
    }
    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    canvas_draw_buf_ = lv_draw_buf_create(width_, height_, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
    if (canvas_draw_buf_ == nullptr) {
        return;
    }
    canvas_ = lv_canvas_create(screen);
    lv_canvas_set_draw_buf(canvas_, canvas_draw_buf_);
    lv_canvas_set_palette(canvas_, 0, lv_color32_make(0, 0, 0, 255));
    lv_canvas_set_palette(canvas_, 1, lv_color32_make(255, 255, 255, 255));
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    lv_obj_center(canvas_);

    wifi_panel_ = lv_obj_create(screen);
    lv_obj_remove_style_all(wifi_panel_);
    lv_obj_set_size(wifi_panel_, width_, height_);
    lv_obj_set_style_bg_color(wifi_panel_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_panel_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(wifi_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);

    wifi_title_label_ = lv_label_create(wifi_panel_);
    lv_label_set_text(wifi_title_label_, "Wi-Fi Setup");
    lv_obj_set_style_text_color(wifi_title_label_, lv_color_white(), 0);

    wifi_ssid_label_ = lv_label_create(wifi_panel_);
    lv_obj_set_width(wifi_ssid_label_, width_ - 4);
    lv_label_set_long_mode(wifi_ssid_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(wifi_ssid_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_ssid_label_, lv_color_white(), 0);

    clock_panel_ = lv_obj_create(screen);
    lv_obj_remove_style_all(clock_panel_);
    lv_obj_set_size(clock_panel_, width_, height_);
    lv_obj_set_style_bg_color(clock_panel_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(clock_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);

    clock_time_label_ = lv_label_create(clock_panel_);
    lv_obj_set_width(clock_time_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(clock_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(clock_time_label_, lv_color_white(), 0);
    // Make the time fill the otherwise unused upper portion of the 128x64
    // standby screen while preserving the compact date line at the bottom.
    lv_obj_set_style_text_font(clock_time_label_, &font_noto_sans_basic_30_4, 0);
    lv_obj_set_style_text_letter_space(clock_time_label_, 0, 0);
    lv_obj_set_style_transform_scale(clock_time_label_, 330, 0);
    lv_obj_align(clock_time_label_, LV_ALIGN_TOP_MID, 0, -2);

    clock_date_label_ = lv_label_create(clock_panel_);
    lv_obj_set_width(clock_date_label_, width_);
    lv_obj_set_style_text_align(clock_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(clock_date_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock_date_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(clock_date_label_, 1, 0);
    lv_obj_align(clock_date_label_, LV_ALIGN_BOTTOM_MID, 0, -1);

    game_panel_ = lv_obj_create(screen);
    lv_obj_remove_style_all(game_panel_);
    lv_obj_set_size(game_panel_, width_, height_);
    lv_obj_set_style_bg_color(game_panel_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(game_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);

    game_title_label_ = lv_label_create(game_panel_);
    lv_label_set_text(game_title_label_, "ROUND");
    lv_obj_set_style_text_color(game_title_label_, lv_color_white(), 0);
    lv_obj_align(game_title_label_, LV_ALIGN_TOP_MID, 0, 4);

    game_round_label_ = lv_label_create(game_panel_);
    lv_label_set_text(game_round_label_, "1 / 5");
    lv_obj_set_style_text_color(game_round_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(game_round_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(game_round_label_, LV_ALIGN_CENTER, 0, -2);

    game_progress_ = lv_bar_create(game_panel_);
    lv_obj_set_size(game_progress_, width_ - 20, 7);
    lv_obj_align(game_progress_, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_bar_set_range(game_progress_, 0, 100);
    lv_bar_set_value(game_progress_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(game_progress_, lv_color_make(48, 48, 48), LV_PART_MAIN);
    lv_obj_set_style_bg_color(game_progress_, lv_color_white(), LV_PART_INDICATOR);

    SetupCaroUi(screen);

    if (!SetupQrUi(screen)) {
        return;
    }

    const uint32_t now = lv_tick_get();
    expression_started_ms_ = now;
    look_started_ms_ = now;
    next_look_ms_ = now + 1200;
    next_blink_ms_ = now + 2200;
    animation_timer_ = lv_timer_create(AnimationTimerCallback, kFramePeriodMs, this);
    clock_timer_ = lv_timer_create(ClockTimerCallback, 1000, this);
    lv_timer_pause(clock_timer_);
    RenderFrame();
}

bool CompactOledEmojiDisplay::SetupQrUi(lv_obj_t* screen) {
    if (screen == nullptr || height_ < kQrCodeSize) {
        ESP_LOGW(kTag, "Control QR requires an OLED at least %d pixels high", kQrCodeSize);
        return false;
    }
    qr_panel_ = lv_obj_create(screen);
    lv_obj_remove_style_all(qr_panel_);
    lv_obj_set_size(qr_panel_, width_, height_);
    lv_obj_set_style_bg_color(qr_panel_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);
    qr_draw_buf_ = lv_draw_buf_create(kQrCodeSize, kQrCodeSize, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
    if (qr_draw_buf_ == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate control QR canvas");
        return false;
    }
    qr_canvas_ = lv_canvas_create(qr_panel_);
    lv_canvas_set_draw_buf(qr_canvas_, qr_draw_buf_);
    lv_canvas_set_palette(qr_canvas_, 0, lv_color32_make(255, 255, 255, 255));
    lv_canvas_set_palette(qr_canvas_, 1, lv_color32_make(0, 0, 0, 255));
    lv_canvas_fill_bg(qr_canvas_, lv_color_white(), LV_OPA_COVER);
    lv_obj_align(qr_canvas_, LV_ALIGN_LEFT_MID, 1, 0);
    qr_title_label_ = lv_label_create(qr_panel_);
    lv_label_set_text(qr_title_label_, "CONTROL");
    lv_obj_set_style_text_color(qr_title_label_, lv_color_black(), 0);
    lv_obj_align(qr_title_label_, LV_ALIGN_TOP_RIGHT, -3, 8);
    qr_hint_label_ = lv_label_create(qr_panel_);
    lv_obj_set_width(qr_hint_label_, 61);
    lv_label_set_text(qr_hint_label_, "Scan to\nopen");
    lv_obj_set_style_text_align(qr_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(qr_hint_label_, lv_color_black(), 0);
    lv_obj_align(qr_hint_label_, LV_ALIGN_RIGHT_MID, -2, 8);
    return true;
}

void CompactOledEmojiDisplay::ShowWifiConfig(const std::string& ssid) {
    DisplayLockGuard lock(this);
    if (wifi_panel_ == nullptr || wifi_ssid_label_ == nullptr) {
        return;
    }
    // The game owns the full display from its first round until completion.
    // Connection-status updates must not replace the round screen midway.
    if (game_mode_ || caro_mode_) {
        return;
    }
    qr_mode_ = false;
    if (qr_panel_ != nullptr)
        lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);
    if (qr_timer_ != nullptr) {
        lv_timer_delete(qr_timer_);
        qr_timer_ = nullptr;
    }
    lv_label_set_text(wifi_ssid_label_, ssid.c_str());
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    game_mode_ = false;
    clock_mode_ = false;
    lv_obj_remove_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    if (animation_timer_ != nullptr) {
        lv_timer_pause(animation_timer_);
    }
    if (clock_timer_ != nullptr) {
        lv_timer_pause(clock_timer_);
    }
}

void CompactOledEmojiDisplay::ShowClockMode() {
    DisplayLockGuard lock(this);
    if (clock_panel_ == nullptr || canvas_ == nullptr) {
        return;
    }
    if (game_mode_ || qr_mode_ || caro_mode_) {
        return;
    }
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    game_mode_ = false;
    clock_mode_ = true;
    if (animation_timer_ != nullptr) {
        lv_timer_pause(animation_timer_);
    }
    if (clock_timer_ != nullptr) {
        lv_timer_resume(clock_timer_);
    }
    RenderClock();
}

void CompactOledEmojiDisplay::ShowEmojiMode() {
    DisplayLockGuard lock(this);
    if (wifi_panel_ == nullptr || canvas_ == nullptr) {
        return;
    }
    // Application status transitions (including voice-state changes) can
    // request Emoji mode at any time. Ignore them while a game is active so
    // the ROUND panel remains visible for every throw.
    if (game_mode_ || qr_mode_ || caro_mode_) {
        return;
    }
    lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    game_mode_ = false;
    clock_mode_ = false;
    if (clock_timer_ != nullptr) {
        lv_timer_pause(clock_timer_);
    }
    if (animation_timer_ != nullptr) {
        lv_timer_resume(animation_timer_);
    }
    RenderFrame();
}

bool CompactOledEmojiDisplay::ShowControlQr(const std::string& url, uint32_t duration_ms) {
    DisplayLockGuard lock(this);
    if (url.empty() || qr_panel_ == nullptr || qr_canvas_ == nullptr || game_mode_ || caro_mode_) {
        ESP_LOGW(kTag, "Cannot show control QR (url=%s, panel=%p, canvas=%p, game=%d)",
                 url.empty() ? "empty" : "set", qr_panel_, qr_canvas_, game_mode_);
        return false;
    }
    qrcode_wrapper_config_t config = {
        .display_func = DrawControlQr,
        .max_qrcode_version = 5,
        .qrcode_ecc_level = kQrcodeWrapperEccLow,
        .user_data = this,
    };
    lv_display_enable_invalidation(display_, false);
    const esp_err_t result = qrcode_wrapper_generate(&config, url.c_str());
    lv_display_enable_invalidation(display_, true);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to encode control URL as QR (%u bytes)",
                 static_cast<unsigned int>(url.size()));
        return false;
    }
    if (canvas_ != nullptr)
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_panel_ != nullptr)
        lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    if (clock_panel_ != nullptr)
        lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    if (game_panel_ != nullptr)
        lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(qr_panel_, -1);
    lv_obj_invalidate(qr_panel_);
    qr_mode_ = true;
    game_mode_ = false;
    clock_mode_ = false;
    if (animation_timer_ != nullptr)
        lv_timer_pause(animation_timer_);
    if (clock_timer_ != nullptr)
        lv_timer_pause(clock_timer_);
    if (qr_timer_ != nullptr)
        lv_timer_delete(qr_timer_);
    qr_timer_ = lv_timer_create(QrTimerCallback, duration_ms, this);
    lv_timer_set_repeat_count(qr_timer_, 1);
    ESP_LOGI(kTag, "Control QR displayed for %u ms", static_cast<unsigned int>(duration_ms));
    return true;
}

void CompactOledEmojiDisplay::DrawControlQr(const uint8_t* qrcode, void* user_data) {
    auto* display = static_cast<CompactOledEmojiDisplay*>(user_data);
    if (display == nullptr || qrcode == nullptr || display->qr_canvas_ == nullptr) {
        return;
    }

    const int module_count = qrcode_wrapper_get_size(qrcode);
    const int total_modules = module_count + 2 * kQrQuietZoneModules;
    const int scale = kQrCodeSize / total_modules;
    if (module_count <= 0 || scale <= 0) {
        ESP_LOGE(kTag, "Control QR is too large for the OLED canvas (%d modules)", module_count);
        return;
    }

    const int rendered_size = total_modules * scale;
    const int offset = (kQrCodeSize - rendered_size) / 2;
    lv_canvas_fill_bg(display->qr_canvas_, lv_color_white(), LV_OPA_COVER);
    for (int y = 0; y < module_count; ++y) {
        for (int x = 0; x < module_count; ++x) {
            if (!qrcode_wrapper_get_module(qrcode, x, y)) {
                continue;
            }
            const int start_x = offset + (x + kQrQuietZoneModules) * scale;
            const int start_y = offset + (y + kQrQuietZoneModules) * scale;
            for (int py = 0; py < scale; ++py) {
                for (int px = 0; px < scale; ++px) {
                    lv_canvas_set_px(display->qr_canvas_, start_x + px, start_y + py,
                                     lv_color_black(), LV_OPA_COVER);
                }
            }
        }
    }
    lv_obj_invalidate(display->qr_canvas_);
}

void CompactOledEmojiDisplay::QrTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<CompactOledEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (display == nullptr)
        return;
    display->qr_timer_ = nullptr;
    display->qr_mode_ = false;
    if (display->qr_panel_ != nullptr)
        lv_obj_add_flag(display->qr_panel_, LV_OBJ_FLAG_HIDDEN);
    if (display->wifi_panel_ != nullptr)
        lv_obj_add_flag(display->wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    if (display->clock_panel_ != nullptr)
        lv_obj_add_flag(display->clock_panel_, LV_OBJ_FLAG_HIDDEN);
    if (display->game_panel_ != nullptr)
        lv_obj_add_flag(display->game_panel_, LV_OBJ_FLAG_HIDDEN);
    if (display->canvas_ != nullptr)
        lv_obj_remove_flag(display->canvas_, LV_OBJ_FLAG_HIDDEN);
    display->game_mode_ = false;
    display->clock_mode_ = false;
    if (display->clock_timer_ != nullptr)
        lv_timer_pause(display->clock_timer_);
    if (display->animation_timer_ != nullptr)
        lv_timer_resume(display->animation_timer_);
    if (display->emoji_only_)
        display->RenderFrame();
}

bool CompactOledEmojiDisplay::SetupCaroUi(lv_obj_t* screen) {
    if (screen == nullptr) {
        return false;
    }
    // Full-screen 3x3 board for the voice caro game. Cells are plain LVGL labels:
    // the server only ever sends digits and the letters X/O (all ASCII), so the
    // built-in fonts render everything without a custom glyph set or raw bitmap.
    caro_panel_ = lv_obj_create(screen);
    lv_obj_remove_style_all(caro_panel_);
    lv_obj_set_size(caro_panel_, width_, height_);
    lv_obj_set_style_bg_color(caro_panel_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(caro_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(caro_panel_, 0, 0);
    lv_obj_set_style_border_width(caro_panel_, 1, 0);
    lv_obj_set_style_border_color(caro_panel_, lv_color_white(), 0);
    lv_obj_set_flex_flow(caro_panel_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(caro_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(caro_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(caro_panel_, 0, 0);
    lv_obj_set_style_pad_column(caro_panel_, 0, 0);
    for (int i = 0; i < 9; ++i) {
        lv_obj_t* cell = lv_label_create(caro_panel_);
        lv_obj_set_size(cell, (width_ - 2) / 3, (height_ - 2) / 3);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(cell, lv_color_white(), 0);
        lv_obj_set_style_text_font(cell, &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_white(), 0);
        lv_label_set_text(cell, "1");
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        caro_cells_[i] = cell;
    }
    return true;
}

void CompactOledEmojiDisplay::ShowCaroBoard(const std::string& cells) {
    if (!IsValidCompactCaroBoard(cells))
        throw std::runtime_error("cells must contain 9 squares: their own digit, X or O");
    DisplayLockGuard lock(this);
    if (game_mode_)
        throw std::runtime_error("Rock Paper Scissors owns the OLED");
    if (caro_panel_ == nullptr) {
        throw std::runtime_error("Caro OLED is not ready");
    }
    for (int i = 0; i < 9; ++i) {
        if (caro_cells_[i] == nullptr) {
            continue;
        }
        const char text[2] = {cells[i], '\0'};
        lv_label_set_text(caro_cells_[i], text);
    }
    // Caro owns the full display while a game is on, same as the RPS "game_mode_"
    // guard -- block standby/clock/emoji/qr transitions from covering the board.
    qr_mode_ = false;
    if (qr_panel_ != nullptr)
        lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);
    if (qr_timer_ != nullptr) {
        lv_timer_delete(qr_timer_);
        qr_timer_ = nullptr;
    }
    if (canvas_ != nullptr)
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_panel_ != nullptr)
        lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    if (clock_panel_ != nullptr)
        lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    if (game_panel_ != nullptr)
        lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(caro_panel_, -1);
    lv_obj_invalidate(caro_panel_);
    caro_mode_ = true;
    game_mode_ = false;
    clock_mode_ = false;
    if (animation_timer_ != nullptr)
        lv_timer_pause(animation_timer_);
    if (clock_timer_ != nullptr)
        lv_timer_pause(clock_timer_);
}

void CompactOledEmojiDisplay::EndCaroBoard() {
    DisplayLockGuard lock(this);
    if (!caro_mode_) {
        return;
    }
    caro_mode_ = false;
    if (caro_panel_ != nullptr) {
        lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    // Return to the animated emoji face.
    if (canvas_ != nullptr)
        lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    if (animation_timer_ != nullptr)
        lv_timer_resume(animation_timer_);
    RenderFrame();
}

void CompactOledEmojiDisplay::ShowGameCountdown(int digit) {
    DisplayLockGuard lock(this);
    if (game_panel_ == nullptr || game_title_label_ == nullptr || game_round_label_ == nullptr ||
        game_progress_ == nullptr) {
        return;
    }
    caro_mode_ = false;
    if (caro_panel_ != nullptr)
        lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    qr_mode_ = false;
    if (qr_panel_ != nullptr)
        lv_obj_add_flag(qr_panel_, LV_OBJ_FLAG_HIDDEN);
    if (qr_timer_ != nullptr) {
        lv_timer_delete(qr_timer_);
        qr_timer_ = nullptr;
    }
    digit = std::clamp(digit, 1, 3);
    char digit_text[2] = {static_cast<char>('0' + digit), '\0'};
    lv_label_set_text(game_title_label_, "READY");
    lv_label_set_text(game_round_label_, digit_text);
    lv_obj_set_style_text_font(game_round_label_, &font_noto_sans_basic_30_4, 0);
    lv_obj_align(game_round_label_, LV_ALIGN_CENTER, 0, 3);
    lv_obj_add_flag(game_progress_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    game_mode_ = true;
    clock_mode_ = false;
    if (clock_timer_ != nullptr) {
        lv_timer_pause(clock_timer_);
    }
    if (animation_timer_ != nullptr) {
        lv_timer_resume(animation_timer_);
    }
}

void CompactOledEmojiDisplay::ShowGameRound(int round, int total_rounds) {
    DisplayLockGuard lock(this);
    if (game_panel_ == nullptr || game_title_label_ == nullptr || game_round_label_ == nullptr ||
        game_progress_ == nullptr) {
        return;
    }
    caro_mode_ = false;
    if (caro_panel_ != nullptr)
        lv_obj_add_flag(caro_panel_, LV_OBJ_FLAG_HIDDEN);
    game_total_rounds_ = std::max(total_rounds, 1);
    game_round_ = std::clamp(round, 1, game_total_rounds_);
    // Two signed 32-bit integers plus separators and the null terminator need
    // up to 24 bytes. Keep extra room so -Wformat-truncation stays satisfied.
    char round_text[32];
    snprintf(round_text, sizeof(round_text), "%d / %d", game_round_, game_total_rounds_);
    lv_label_set_text(game_title_label_, "ROUND");
    lv_label_set_text(game_round_label_, round_text);
    lv_obj_set_style_text_font(game_round_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(game_round_label_, LV_ALIGN_CENTER, 0, -2);
    lv_obj_remove_flag(game_progress_, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(game_progress_, game_round_ * 100 / game_total_rounds_, LV_ANIM_OFF);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    game_mode_ = true;
    clock_mode_ = false;
    if (clock_timer_ != nullptr) {
        lv_timer_pause(clock_timer_);
    }
    if (animation_timer_ != nullptr) {
        lv_timer_resume(animation_timer_);
    }
}

void CompactOledEmojiDisplay::EndGameMode() {
    DisplayLockGuard lock(this);
    if (!game_mode_) {
        return;
    }
    game_mode_ = false;
    if (game_panel_ != nullptr) {
        lv_obj_add_flag(game_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CompactOledEmojiDisplay::SetStatus(const char* status) {
    if (!emoji_only_) {
        OledDisplay::SetStatus(status);
        return;
    }
    // Standby is the wake-word waiting state. Keep the clock visible there;
    // every conversation state switches back to the full-screen animated face.
    if (status != nullptr && strcmp(status, Lang::Strings::STANDBY) == 0) {
        ShowClockMode();
    } else {
        ShowEmojiMode();
    }

    int forced_expression = -1;
    if (status != nullptr && strcmp(status, Lang::Strings::LISTENING) == 0) {
        forced_expression = FindPreset("attentive");
    } else if (status != nullptr && strcmp(status, Lang::Strings::SPEAKING) == 0) {
        forced_expression = FindPreset("chatty");
    }

    DisplayLockGuard lock(this);
    {
        if (status != nullptr && (strcmp(status, Lang::Strings::LISTENING) == 0 || clock_mode_)) {
            requested_emotion_ = "neutral";
        } else if (status != nullptr && strcmp(status, Lang::Strings::SPEAKING) == 0 &&
                   requested_emotion_ != "neutral") {
            // An LLM emotion can arrive before the speaking transition.
            forced_expression = -1;
        }
    }
    forced_expression_ = forced_expression;
    if (emotion_callback_) {
        if (forced_expression_ >= 0) {
            emotion_callback_(kFacePresets[forced_expression_].name);
        } else if (clock_mode_) {
            emotion_callback_("idle");
        } else if (requested_emotion_ != "neutral") {
            emotion_callback_(requested_emotion_.c_str());
        }
    }
    if (forced_expression_ >= 0 && forced_expression_ != current_expression_) {
        previous_expression_ = current_expression_;
        current_expression_ = forced_expression_;
        expression_started_ms_ = lv_tick_get();
    }
}

void CompactOledEmojiDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!emoji_only_)
        LvglDisplay::ShowNotification(notification, duration_ms);
}

void CompactOledEmojiDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    if (!emoji_only_)
        LvglDisplay::ShowNotification(notification, duration_ms);
}

void CompactOledEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    if (!emoji_only_)
        OledDisplay::SetChatMessage(role, content);
}

void CompactOledEmojiDisplay::SetTheme(Theme* theme) {
    if (!emoji_only_)
        OledDisplay::SetTheme(theme);
}

void CompactOledEmojiDisplay::UpdateStatusBar(bool update_all) {
    if (!emoji_only_)
        LvglDisplay::UpdateStatusBar(update_all);
}

void CompactOledEmojiDisplay::SetEmotionCallback(std::function<void(const char*)> callback) {
    DisplayLockGuard lock(this);
    emotion_callback_ = std::move(callback);
}

void CompactOledEmojiDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    {
        const bool neutral = emotion == nullptr || emotion[0] == '\0' ||
                             strcasecmp(emotion, "neutral") == 0 ||
                             strcasecmp(emotion, "normal") == 0 || strcasecmp(emotion, "idle") == 0;
        requested_emotion_ = neutral ? "neutral" : emotion;
        if (!neutral) {
            // Status expressions are defaults, not overrides for explicit emotions.
            forced_expression_ = -1;
        }
        ESP_LOGI("CompactEmotion", "Requested: %s; visible: %s", requested_emotion_.c_str(),
                 forced_expression_ >= 0 ? kFacePresets[forced_expression_].name
                                         : requested_emotion_.c_str());
    }
    if (emotion_callback_) {
        emotion_callback_(emoji_only_ && forced_expression_ >= 0
                              ? kFacePresets[forced_expression_].name
                              : emotion);
    }
    if (!emoji_only_) {
        OledDisplay::SetEmotion(emotion);
        return;
    }
    const int next_expression = forced_expression_ >= 0 ? forced_expression_ : FindPreset(emotion);
    if (next_expression == current_expression_) {
        return;
    }
    previous_expression_ = current_expression_;
    current_expression_ = next_expression;
    expression_started_ms_ = lv_tick_get();
}

void CompactOledEmojiDisplay::AnimationTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<CompactOledEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        if (!display->game_mode_ && !display->caro_mode_) {
            display->RenderFrame();
        }
    }
}

void CompactOledEmojiDisplay::ClockTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<CompactOledEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr && display->clock_mode_) {
        display->RenderClock();
    }
}

void CompactOledEmojiDisplay::RenderClock() {
    if (clock_time_label_ == nullptr || clock_date_label_ == nullptr) {
        return;
    }

    const time_t now = time(nullptr);
    tm local_time{};
    localtime_r(&now, &local_time);
    if (local_time.tm_year < 2025 - 1900) {
        lv_label_set_text(clock_time_label_, "--:--");
        lv_label_set_text(clock_date_label_, "SYNCING TIME");
        return;
    }

    char time_text[8];
    char date_text[12];
    strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
    static constexpr const char* kWeekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    const int day = std::clamp(local_time.tm_mday, 1, 31);
    const int month = std::clamp(local_time.tm_mon + 1, 1, 12);
    const int weekday = std::clamp(local_time.tm_wday, 0, 6);
    snprintf(date_text, sizeof(date_text), "%02d/%02d %s", day, month, kWeekdays[weekday]);
    lv_label_set_text(clock_time_label_, time_text);
    lv_obj_update_layout(clock_panel_);
    lv_obj_set_style_transform_pivot_x(clock_time_label_, lv_obj_get_width(clock_time_label_) / 2,
                                       0);
    lv_obj_set_style_transform_pivot_y(clock_time_label_, 0, 0);
    lv_label_set_text(clock_date_label_, date_text);
}

void CompactOledEmojiDisplay::FillVerticalLine(int x, int start_y, int end_y) {
    if (canvas_ == nullptr || x < 0 || x >= width_) {
        return;
    }
    if (start_y > end_y) {
        std::swap(start_y, end_y);
    }
    start_y = std::max(start_y, 0);
    end_y = std::min(end_y, height_ - 1);
    for (int y = start_y; y <= end_y; ++y) {
        lv_canvas_set_px(canvas_, x, y, lv_color_white(), LV_OPA_COVER);
    }
}

void CompactOledEmojiDisplay::RenderFrame() {
    if (canvas_ == nullptr) {
        return;
    }
    const uint32_t now = lv_tick_get();
    const float expression_progress = static_cast<float>(now - expression_started_ms_) /
                                      static_cast<float>(kExpressionTransitionMs);
    const EyeShape left =
        InterpolateEye(kFacePresets[previous_expression_].left,
                       kFacePresets[current_expression_].left, expression_progress);
    const EyeShape right =
        InterpolateEye(kFacePresets[previous_expression_].right,
                       kFacePresets[current_expression_].right, expression_progress);

    if (static_cast<int32_t>(now - next_look_ms_) >= 0) {
        const float progress = SmoothStep(static_cast<float>(now - look_started_ms_) /
                                          static_cast<float>(kLookTransitionMs));
        previous_look_x_ = Lerp(previous_look_x_, target_look_x_, progress);
        previous_look_y_ = Lerp(previous_look_y_, target_look_y_, progress);
        target_look_x_ = RandomRange(-0.85f, 0.85f);
        target_look_y_ = RandomRange(-0.65f, 0.65f);
        look_started_ms_ = now;
        next_look_ms_ = now + static_cast<uint32_t>(RandomRange(1300.0f, 2600.0f));
    }
    const float look_progress = SmoothStep(static_cast<float>(now - look_started_ms_) /
                                           static_cast<float>(kLookTransitionMs));
    const float look_x = Lerp(previous_look_x_, target_look_x_, look_progress);
    const float look_y = Lerp(previous_look_y_, target_look_y_, look_progress);

    if (!blinking_ && static_cast<int32_t>(now - next_blink_ms_) >= 0) {
        blinking_ = true;
        blink_started_ms_ = now;
    }
    float blink = 0.0f;
    if (blinking_) {
        const float progress =
            static_cast<float>(now - blink_started_ms_) / static_cast<float>(kBlinkDurationMs);
        if (progress >= 1.0f) {
            blinking_ = false;
            next_blink_ms_ = now + static_cast<uint32_t>(RandomRange(2200.0f, 5200.0f));
        } else if (progress < 0.5f) {
            blink = SmoothStep(progress * 2.0f);
        } else {
            blink = SmoothStep((1.0f - progress) * 2.0f);
        }
    }

    lv_display_enable_invalidation(display_, false);
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);

    auto draw_eye = [this, blink, look_x, look_y](float center_x, float center_y,
                                                  const EyeShape& eye) {
        const float look_scale_y =
            Clamp(1.0f - 0.25f * std::fabs(look_y) - 0.12f * std::fabs(look_x), 0.55f, 1.0f);
        const float open_height = eye.height * look_scale_y;
        const float height = Lerp(open_height, 2.0f, blink * blink);
        const float current_center_x = center_x + eye.offset_x + look_x * 9.0f;
        const float current_center_y = center_y + eye.offset_y + look_y * 7.0f;
        const float left_x = current_center_x - eye.width * 0.5f;
        const float right_x = current_center_x + eye.width * 0.5f;
        const float half_height = height * 0.5f;
        const float radius_top =
            std::min({eye.radius_top * (1.0f - blink), half_height, eye.width * 0.5f});
        const float radius_bottom =
            std::min({eye.radius_bottom * (1.0f - blink), half_height, eye.width * 0.5f});
        for (int x = static_cast<int>(std::floor(left_x));
             x <= static_cast<int>(std::ceil(right_x)); ++x) {
            const float local_x = (static_cast<float>(x) + 0.5f - left_x) / eye.width;
            if (local_x < 0.0f || local_x > 1.0f) {
                continue;
            }
            const float centered_x = local_x - 0.5f;
            float top = current_center_y - half_height + eye.slope_top * height * centered_x;
            float bottom = current_center_y + half_height + eye.slope_bottom * height * centered_x;
            if (radius_top > 0.5f && (static_cast<float>(x) < left_x + radius_top ||
                                      static_cast<float>(x) > right_x - radius_top)) {
                const float corner_x = static_cast<float>(x) < left_x + radius_top
                                           ? left_x + radius_top
                                           : right_x - radius_top;
                const float distance_x = static_cast<float>(x) + 0.5f - corner_x;
                top += radius_top -
                       std::sqrt(std::max(0.0f, radius_top * radius_top - distance_x * distance_x));
            }
            if (radius_bottom > 0.5f && (static_cast<float>(x) < left_x + radius_bottom ||
                                         static_cast<float>(x) > right_x - radius_bottom)) {
                const float corner_x = static_cast<float>(x) < left_x + radius_bottom
                                           ? left_x + radius_bottom
                                           : right_x - radius_bottom;
                const float distance_x = static_cast<float>(x) + 0.5f - corner_x;
                bottom -= radius_bottom - std::sqrt(std::max(0.0f, radius_bottom * radius_bottom -
                                                                       distance_x * distance_x));
            }
            FillVerticalLine(x, static_cast<int>(std::ceil(top)),
                             static_cast<int>(std::floor(bottom)));
        }
    };

    const bool is_attentive = strcasecmp(kFacePresets[current_expression_].name, "attentive") == 0;
    const bool is_chatty = strcasecmp(kFacePresets[current_expression_].name, "chatty") == 0;
    if (!is_attentive && !is_chatty) {
        draw_eye(40.0f, 32.0f, left);
        draw_eye(88.0f, 32.0f, right);
    } else {
        // The voice states form one continuous character animation. Listening
        // uses two soft round eyes; speaking morphs those same eyes into thick,
        // joyful domes instead of abruptly swapping to a different drawing.
        const bool was_attentive =
            strcasecmp(kFacePresets[previous_expression_].name, "attentive") == 0;
        const bool was_chatty = strcasecmp(kFacePresets[previous_expression_].name, "chatty") == 0;
        const float transition = SmoothStep(expression_progress);
        float speaking_mix = is_chatty ? 1.0f : 0.0f;
        if (is_chatty && was_attentive) {
            speaking_mix = transition;
        } else if (is_attentive && was_chatty) {
            speaking_mix = 1.0f - transition;
        }

        const float listening_motion = std::sin(static_cast<float>(now) * 0.0035f);
        const float talking_motion = std::sin(static_cast<float>(now) * 0.014f);
        const float bounce = Lerp(listening_motion * 0.35f, talking_motion * 0.65f, speaking_mix);
        const float talk_pulse = 0.5f + 0.5f * talking_motion;
        // Listening: large vertical ovals. Speaking: wider arched eye bands.
        // Together the pair spans roughly x=16..112 on the 128-pixel display.
        const float radius_x =
            Lerp(18.0f + listening_motion * 0.2f, 21.0f + talk_pulse * 0.5f, speaking_mix);
        const float radius_y =
            Lerp(19.0f + listening_motion * 0.25f, 17.0f + talk_pulse * 0.5f, speaking_mix);
        const float blink_amount = blink * (1.0f - speaking_mix);
        const float gaze_x = look_x * 2.0f * (1.0f - speaking_mix);
        const float gaze_y = look_y * 1.5f * (1.0f - speaking_mix);

        auto draw_voice_eye = [this, radius_x, radius_y, speaking_mix, blink_amount, bounce, gaze_x,
                               gaze_y](float base_x, int outer_direction) {
            const float center_x = base_x + gaze_x;
            const float center_y = 32.0f + bounce + gaze_y;
            const int extent_x = static_cast<int>(std::ceil(Lerp(radius_x, 25.0f, speaking_mix)));
            for (int dx = -extent_x; dx <= extent_x; ++dx) {
                // Listening reference: a tall filled oval. Clamp outside the
                // oval during the transition so the speaking eye grows smoothly.
                const bool has_listening_span = std::fabs(static_cast<float>(dx)) <= radius_x;
                const float listening_x = Clamp(static_cast<float>(dx) / radius_x, -1.0f, 1.0f);
                const float listening_curve =
                    std::sqrt(std::max(0.0f, 1.0f - listening_x * listening_x));
                const float ellipse_top = center_y - radius_y * listening_curve;
                const float ellipse_bottom = center_y + radius_y * listening_curve;

                // Speaking reference: sweep a round brush along a shallow arch.
                // A union of circles guarantees fully rounded ends and edges;
                // there are no intersecting outlines that can create spikes.
                const float left_arc_x = outer_direction < 0 ? -18.0f : -16.0f;
                const float right_arc_x = outer_direction < 0 ? 16.0f : 18.0f;
                float happy_top = center_y;
                float happy_bottom = center_y;
                bool has_happy_span = false;
                auto include_span = [&happy_top, &happy_bottom, &has_happy_span](float top,
                                                                                 float bottom) {
                    if (!has_happy_span) {
                        happy_top = top;
                        happy_bottom = bottom;
                        has_happy_span = true;
                    } else {
                        happy_top = std::min(happy_top, top);
                        happy_bottom = std::max(happy_bottom, bottom);
                    }
                };

                for (int sample_x = static_cast<int>(std::ceil(left_arc_x));
                     sample_x <= static_cast<int>(std::floor(right_arc_x)); ++sample_x) {
                    const float u =
                        (static_cast<float>(sample_x) - left_arc_x) / (right_arc_x - left_arc_x);
                    const float t = u * 2.0f - 1.0f;
                    const float edge_radius =
                        outer_direction < 0 ? Lerp(7.0f, 5.0f, u) : Lerp(5.0f, 7.0f, u);
                    // One quadratic curve produces one clean dome from end to
                    // end. A linearly changing brush radius shifts the crown
                    // slightly outward without introducing a second bend.
                    const float arch_curve = 1.0f - t * t;
                    const float brush_radius = edge_radius + 3.0f * arch_curve;
                    const float sample_y = center_y + 2.0f - 7.0f * arch_curve -
                                           static_cast<float>(outer_direction) * t;
                    const float distance_x = static_cast<float>(dx - sample_x);
                    if (std::fabs(distance_x) > brush_radius) {
                        continue;
                    }
                    const float vertical_radius = std::sqrt(
                        std::max(0.0f, brush_radius * brush_radius - distance_x * distance_x));
                    include_span(sample_y - vertical_radius, sample_y + vertical_radius);
                }

                if ((!has_listening_span && !has_happy_span) ||
                    (speaking_mix >= 0.999f && !has_happy_span) ||
                    (speaking_mix <= 0.001f && !has_listening_span)) {
                    continue;
                }
                if (!has_happy_span) {
                    happy_top = center_y;
                    happy_bottom = center_y;
                }
                float top = Lerp(ellipse_top, happy_top, speaking_mix);
                float bottom = Lerp(ellipse_bottom, happy_bottom, speaking_mix);

                // Keep the familiar quick blink while listening. It fades out
                // naturally during the morph so the speaking eyes stay cheerful.
                const float blink_y = center_y + 1.0f;
                top = Lerp(top, blink_y - 1.0f, blink_amount * blink_amount);
                bottom = Lerp(bottom, blink_y + 1.0f, blink_amount * blink_amount);
                FillVerticalLine(static_cast<int>(std::round(center_x)) + dx,
                                 static_cast<int>(std::ceil(top)),
                                 static_cast<int>(std::floor(bottom)));
            }
        };

        draw_voice_eye(37.0f, -1);
        draw_voice_eye(91.0f, 1);
    }
    lv_display_enable_invalidation(display_, true);
    lv_obj_invalidate(canvas_);
}
