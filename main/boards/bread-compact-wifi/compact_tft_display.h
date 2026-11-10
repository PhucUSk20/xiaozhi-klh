#pragma once

#include "sdkconfig.h"

#if CONFIG_BREAD_COMPACT_WIFI_TFT_128X160
#include "display/display.h"
#include "display/lvgl_display/lvgl_image.h"
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
#include "compact_sd_gif.h"
#include "device_state.h"
#include "display/lvgl_display/gif/gifdec.h"
#endif

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <atomic>
#include <map>
#include <string>

class CompactTftDisplay : public Display {
public:
    CompactTftDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, bool mirror_x, bool mirror_y, bool swap_xy, bool initialize_lvgl);
    ~CompactTftDisplay() override;
    void SetupUI() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image);
    using Display::ShowNotification;
    void ShowNotification(const char* message, int duration_ms = 3000) override;
    bool ShowText(const std::string& text);
    void ShowEmoji();
    void ShowCaroBoard(const std::string& cells);
    void EndCaroBoard();
    void ShowGameCountdown(int digit);
    void ShowGameRound(int round, int total_rounds);
    void EndGameMode();
    bool DownloadImage(const std::string& url);
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
    bool ShowWordImage(const std::string& lesson_id, const std::string& key);
#endif
    bool IsDownloading() const { return downloading_.load(); }
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    const CompactSdGif* SdCard() const { return sd_card_.get(); }
    bool PlaySdGif(const std::string& name);
    void SetEmotion(const char* emotion) override;
    void SetRuntimeState(DeviceState state);
    void CycleIdleEmoji();
#endif

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    struct DownloadJob;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_WORD_IMAGES_SD
    struct SdWordImage {
        std::string file;
        std::string sha256;
    };
    std::map<std::string, SdWordImage> sd_word_images_;
    bool LoadSdWordImageIndex(char* failure, size_t failure_size);
    std::unique_ptr<LvglImage> LoadSdWordImage(const std::string& key, char* failure,
                                               size_t failure_size);
#endif
    static void DownloadTask(void* arg);
    lv_obj_t* image_layer_ = nullptr;
    lv_obj_t* fullscreen_image_ = nullptr;
    lv_obj_t* vocabulary_label_ = nullptr;
    bool text_visible_ = false;
    bool game_mode_ = false;  // Protected by the LVGL lock.
    lv_obj_t* game_panel_ = nullptr;
    lv_obj_t* game_title_ = nullptr;
    lv_obj_t* game_value_ = nullptr;
    lv_obj_t* game_hint_ = nullptr;
    void ShowGameScreen(const char* title, const char* value, const char* hint);
    struct CaroCell {
        lv_obj_t* circle = nullptr;
        lv_obj_t* x1 = nullptr;
        lv_obj_t* x2 = nullptr;
        lv_obj_t* num = nullptr;
    };
    lv_obj_t* caro_panel_ = nullptr;
    CaroCell caro_cells_[9];
    lv_point_precise_t caro_x1_points_[2] = {};
    lv_point_precise_t caro_x2_points_[2] = {};
    bool caro_visible_ = false;
    void BuildCaroPanelLocked();
    std::atomic<uint32_t> visual_generation_{0};
    std::unique_ptr<LvglImage> fullscreen_image_data_;
    std::atomic<bool> downloading_{false};
    std::atomic<uint32_t> image_generation_{0};
    lv_display_t* display_ = nullptr;
    lv_obj_t* error_label_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool owns_lvgl_port_ = false;
#if CONFIG_BREAD_COMPACT_WIFI_TFT_SDCARD
    static void SdGifTask(void* arg);
    void StopSdGifLocked();
    bool RenderSdGifFrame();
    std::unique_ptr<CompactSdGif> sd_card_;
    lv_obj_t* gif_widget_ = nullptr;
    gd_GIF* gif_decoder_ = nullptr;
    lv_image_dsc_t gif_descriptor_{};
#if CONFIG_BREAD_COMPACT_WIFI_TFT_ILI9486_480X320
    uint8_t* gif_raster_ = nullptr;
#endif
    lv_timer_t* gif_frame_timer_ = nullptr;
    size_t idle_emoji_index_ = 0;
    bool was_idle_ = true;
    static void GifFrameTimer(lv_timer_t* timer);
    std::atomic<bool> gif_loading_{false};
    std::string gif_path_;
    lv_timer_t* emotion_timer_ = nullptr;
    std::string desired_emotion_gif_ = "neutral.gif";
    std::string requested_emotion_ = "neutral";
    void UpdateEmotionLocked();
    std::string submitted_emotion_gif_;
    static void EmotionTimer(lv_timer_t* timer);
#endif
};
#endif
