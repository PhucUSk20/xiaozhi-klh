#ifndef COMPACT_OLED_EMOJI_DISPLAY_H
#define COMPACT_OLED_EMOJI_DISPLAY_H

#include "display/oled_display.h"

#include <cstdint>
#include <functional>
#include <string>

class CompactOledEmojiDisplay : public OledDisplay {
public:
    CompactOledEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height, bool mirror_x, bool mirror_y,
                            bool emoji_only = true);
    ~CompactOledEmojiDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void SetEmotionCallback(std::function<void(const char*)> callback);
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowWifiConfig(const std::string& ssid);
    void ShowClockMode();
    void ShowEmojiMode();
    bool ShowControlQr(const std::string& url, uint32_t duration_ms = 15000);
    void ShowGameCountdown(int digit);
    void ShowGameRound(int round, int total_rounds);
    void EndGameMode();
    void ShowCaroBoard(const std::string& cells);
    void EndCaroBoard();

private:
    std::function<void(const char*)> emotion_callback_;
    std::string requested_emotion_ = "neutral";
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* wifi_panel_ = nullptr;
    lv_obj_t* wifi_title_label_ = nullptr;
    lv_obj_t* wifi_ssid_label_ = nullptr;
    lv_obj_t* clock_panel_ = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* game_panel_ = nullptr;
    lv_obj_t* caro_panel_ = nullptr;
    lv_obj_t* caro_cells_[9] = {};
    lv_obj_t* qr_panel_ = nullptr;
    lv_obj_t* qr_canvas_ = nullptr;
    lv_obj_t* qr_title_label_ = nullptr;
    lv_obj_t* qr_hint_label_ = nullptr;
    lv_obj_t* game_title_label_ = nullptr;
    lv_obj_t* game_round_label_ = nullptr;
    lv_obj_t* game_progress_ = nullptr;
    lv_draw_buf_t* canvas_draw_buf_ = nullptr;
    lv_draw_buf_t* qr_draw_buf_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;
    lv_timer_t* clock_timer_ = nullptr;
    lv_timer_t* qr_timer_ = nullptr;
    int previous_expression_ = 0;
    int current_expression_ = 0;
    int forced_expression_ = -1;
    uint32_t expression_started_ms_ = 0;
    float previous_look_x_ = 0.0f;
    float previous_look_y_ = 0.0f;
    float target_look_x_ = 0.0f;
    float target_look_y_ = 0.0f;
    uint32_t look_started_ms_ = 0;
    uint32_t next_look_ms_ = 0;
    bool blinking_ = false;
    uint32_t blink_started_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    bool game_mode_ = false;
    bool caro_mode_ = false;
    bool clock_mode_ = false;
    bool qr_mode_ = false;
    bool emoji_only_ = true;
    int game_round_ = 0;
    int game_total_rounds_ = 5;

    static void AnimationTimerCallback(lv_timer_t* timer);
    static void ClockTimerCallback(lv_timer_t* timer);
    static void QrTimerCallback(lv_timer_t* timer);
    static void DrawControlQr(const uint8_t* qrcode, void* user_data);
    bool SetupQrUi(lv_obj_t* screen);
    bool SetupCaroUi(lv_obj_t* screen);
    void RenderFrame();
    void RenderClock();
    void FillVerticalLine(int x, int start_y, int end_y);
};

#endif  // COMPACT_OLED_EMOJI_DISPLAY_H
